// Census of native-execution patch sites (see native_census.hpp).
#include "lgp/recomp/native_census.hpp"

#include <algorithm>
#include <set>
#include <utility>

#include "lgp/recomp/a64.hpp"
#include "lgp/recomp/a64_simd.hpp"
#include "lgp/recomp/native_rewrite.hpp"

namespace lgp::recomp {

namespace {

using a64::Inst;
using a64::Op;

uint32_t bits(uint32_t v, int hi, int lo) { return (v >> lo) & ((1u << (hi - lo + 1)) - 1); }

struct Use {
    GprUse u;
    void r(uint32_t reg) { if (reg < 31) u.reads |= 1u << reg; }
    void w(uint32_t reg) { if (reg < 31) u.writes |= 1u << reg; }
    // Rn/Rd fields where 31 means SP.
    void r_sp(uint32_t reg, bool sp) { if (reg == 31) { if (sp) u.reads |= kGprSp; } else r(reg); }
    void w_sp(uint32_t reg, bool sp) { if (reg == 31) { if (sp) u.writes |= kGprSp; } else w(reg); }
};

// GPR operands of SIMD&FP data processing and structure loads/stores.
void simd_use(const Inst& i, Use& u) {
    const uint32_t r = i.raw;
    const uint32_t rd = bits(r, 4, 0), rn = bits(r, 9, 5);
    const std::string cls = a64::simd::class_name(r);
    if (a64::simd::is_ldst_structure(r)) {
        u.r_sp(rn, true);
        if ((r >> 23) & 1) {  // post-index: base written back, Rm != 31 is the increment register
            u.w_sp(rn, true);
            u.r(bits(r, 20, 16));
        }
        return;
    }
    if (cls == "copy") {
        const bool op = (r >> 29) & 1;
        const uint32_t imm4 = bits(r, 14, 11);
        if (!op && (imm4 == 0b0001 || imm4 == 0b0011)) u.r(rn);  // DUP (general), INS (general)
        else if (!op && (imm4 == 0b0101 || imm4 == 0b0111)) u.w(rd);  // SMOV, UMOV
        return;
    }
    if (cls == "fp_int_conv") {
        const uint32_t opcode = bits(r, 18, 16);
        if (opcode == 0b010 || opcode == 0b011 || opcode == 0b111) u.r(rn);  // SCVTF, UCVTF, FMOV to FP
        else u.w(rd);                                                         // FCVT*, FMOV to GPR
        return;
    }
    if (cls == "fp_fixed_conv") {
        const uint32_t opcode = bits(r, 18, 16);
        if (opcode == 0b010 || opcode == 0b011) u.r(rn);
        else u.w(rd);
    }
}

bool is_yield_hint(uint32_t raw) {
    return raw == 0xD503203Fu || raw == 0xD503205Fu || raw == 0xD503207Fu;  // YIELD, WFE, WFI
}

bool is_load(const Inst& i) {
    switch (i.op) {
    case Op::LdrLit: return true;
    case Op::LdSt:
    case Op::LdStPair:
    case Op::LdStExcl:
    case Op::LdStAcqRel: return i.load;
    default: return false;
    }
}

bool patched_sysreg(uint32_t sr) {
    namespace s = a64::sr;
    return sr == s::kTpidrEl0 || sr == s::kTpidrroEl0 || sr == s::kCntpctEl0 || sr == s::kCntvctEl0 ||
           sr == s::kCntfrqEl0;
}

struct SpKind {
    bool from_register = false;
    bool imm_unaligned = false;
};

SpKind sp_write_kind(const Inst& i, const GprUse& u) {
    SpKind k;
    if (!(u.writes & kGprSp)) return k;
    switch (i.op) {
    case Op::AddImm:
    case Op::SubImm:
        if (i.rn != 31) k.from_register = true;  // mov sp, xN / add sp, xN, #imm
        else if (((i.imm << i.amount) & 0xF) != 0) k.imm_unaligned = true;
        break;
    case Op::AddExt:
    case Op::SubExt: k.from_register = true; break;
    default: break;  // AND sp, xN, #mask and load/store writeback keep the alignment of their input
    }
    return k;
}

struct Decoded {
    Inst inst;
    GprUse use;
};

// Liveness of x18 inside one function: backward data flow over its blocks.
// Exits (returns, tail calls, jumps out of the function) count as x18 dead,
// which is the assumption under test: x18 never carries a value between
// functions. Calls do not kill x18; a live x18 right after a call means the
// code relies on the callee preserving it.
void x18_liveness(const Function& f, const std::vector<Decoded>& code, NativeCensus& c) {
    constexpr uint32_t kX18 = 1u << 18;
    const size_t nb = f.blocks.size();
    std::vector<size_t> first(nb + 1, 0);  // index of each block's first instruction in `code`
    for (size_t b = 0; b < nb; ++b) first[b + 1] = first[b] + (f.blocks[b].end - f.blocks[b].start) / 4;
    auto block_of = [&](uint64_t va) -> long {
        auto it = std::lower_bound(f.blocks.begin(), f.blocks.end(), va,
                                   [](const BasicBlock& b, uint64_t v) { return b.start < v; });
        if (it == f.blocks.end() || it->start != va) return -1;
        return static_cast<long>(it - f.blocks.begin());
    };
    std::vector<std::vector<long>> succs(nb);
    for (size_t b = 0; b < nb; ++b)
        for (uint64_t s : f.blocks[b].successors) {
            const long k = block_of(s);
            if (k >= 0) succs[b].push_back(k);
        }

    std::vector<bool> live_in(nb, false);
    bool changed = true, across_call = false;
    while (changed) {
        changed = false;
        across_call = false;
        for (size_t bb = nb; bb-- > 0;) {
            bool live = false;
            for (long s : succs[bb]) live = live || live_in[static_cast<size_t>(s)];
            for (size_t k = first[bb + 1]; k-- > first[bb];) {
                const Inst& i = code[k].inst;
                if ((i.op == Op::Bl || i.op == Op::Blr) && live) across_call = true;
                if (code[k].use.writes & kX18) live = false;
                if (code[k].use.reads & kX18) live = true;
            }
            if (live != live_in[bb]) {
                live_in[bb] = live;
                changed = true;
            }
        }
    }
    const long entry = block_of(f.entry);
    const bool at_entry = entry >= 0 && live_in[static_cast<size_t>(entry)];

    uint32_t used = 0;
    uint64_t sites = 0;
    for (const Decoded& d : code) {
        used |= d.use.reads | d.use.writes;
        if ((d.use.reads | d.use.writes) & kX18) ++sites;
    }
    bool free_reg = false;
    for (unsigned r = 9; r <= 17; ++r) free_reg = free_reg || !(used & (1u << r));

    if (at_entry) ++c.x18_live_at_entry;
    if (across_call) ++c.x18_live_across_call;
    if (f.has_unresolved_indirect) ++c.x18_unresolved;
    if (!free_reg) ++c.x18_no_free_register;
    if (!at_entry && !across_call && !f.has_unresolved_indirect && free_reg) {
        ++c.x18_renamable;
        c.x18_renamable_sites += sites;
    }
}

// Live ranges (webs) of x18 with full GPR liveness (native_rewrite.hpp).
void x18_web_counts(const Function& f, const std::vector<Decoded>& code, NativeCensus& c) {
    std::vector<NativeInst> insts;
    insts.reserve(code.size());
    for (const Decoded& d : code) insts.push_back({d.inst, d.use});
    bool all = true;
    for (const X18Web& w : x18_webs(f, insts)) {
        ++c.x18_webs;
        if (w.open) ++c.x18_webs_open;
        if (!w.open && (w.free & (kRenameCallerSaved | kRenameCalleeSaved))) {
            ++c.x18_webs_renamable;
            c.x18_web_sites_renamable += w.sites.size();
        } else {
            all = false;
            c.x18_web_sites_left += w.sites.size();
        }
        if (!w.open && (w.free & kRenameCallerSaved)) {
            ++c.x18_webs_renamable_caller_saved;
            c.x18_web_sites_renamable_caller_saved += w.sites.size();
        }
    }
    if (all) ++c.x18_functions_all_renamable;
}

}  // namespace

bool native_stub_site(const Inst& i, const GprUse& u) {
    if (i.op == Op::Svc) return true;
    if ((i.op == Op::Mrs || i.op == Op::Msr) && patched_sysreg(i.sysreg)) return true;
    const SpKind sp = sp_write_kind(i, u);
    return sp.from_register || sp.imm_unaligned;
}

GprUse gpr_use(const Inst& i) {
    Use u;
    switch (i.op) {
    case Op::Invalid:
    case Op::B:
    case Op::BCond:
    case Op::Svc:
    case Op::Brk:
    case Op::Hint:
    case Op::Clrex:
    case Op::Dsb:
    case Op::Dmb:
    case Op::Isb:
    case Op::PrfmLit:
    case Op::kCount: break;
    case Op::Adr:
    case Op::Adrp:
    case Op::Movz:
    case Op::Movn: u.w(i.rd); break;
    case Op::Movk: u.r(i.rd); u.w(i.rd); break;
    case Op::AddImm:
    case Op::SubImm:
        u.r_sp(i.rn, i.rn_sp);
        u.w_sp(i.rd, i.rd_sp);
        break;
    case Op::AndImm:
    case Op::OrrImm:
    case Op::EorImm:
    case Op::AndsImm:
        u.r(i.rn);
        u.w_sp(i.rd, i.rd_sp);
        break;
    case Op::Sbfm:
    case Op::Ubfm:
    case Op::Rbit:
    case Op::Rev16:
    case Op::Rev32:
    case Op::Rev:
    case Op::Clz:
    case Op::Cls: u.r(i.rn); u.w(i.rd); break;
    case Op::Bfm: u.r(i.rn); u.r(i.rd); u.w(i.rd); break;
    case Op::Extr:
    case Op::AndReg:
    case Op::BicReg:
    case Op::OrrReg:
    case Op::OrnReg:
    case Op::EorReg:
    case Op::EonReg:
    case Op::AndsReg:
    case Op::BicsReg:
    case Op::AddReg:
    case Op::SubReg:
    case Op::Adc:
    case Op::Sbc:
    case Op::Csel:
    case Op::Csinc:
    case Op::Csinv:
    case Op::Csneg:
    case Op::Udiv:
    case Op::Sdiv:
    case Op::Lslv:
    case Op::Lsrv:
    case Op::Asrv:
    case Op::Rorv:
    case Op::Crc32:
    case Op::Crc32c:
    case Op::Smulh:
    case Op::Umulh: u.r(i.rn); u.r(i.rm); u.w(i.rd); break;
    case Op::AddExt:
    case Op::SubExt:
        u.r_sp(i.rn, i.rn_sp);
        u.r(i.rm);
        u.w_sp(i.rd, i.rd_sp);
        break;
    case Op::Ccmn:
    case Op::Ccmp:
        u.r(i.rn);
        if (!i.ccmp_imm) u.r(i.rm);
        break;
    case Op::Madd:
    case Op::Msub:
    case Op::Smaddl:
    case Op::Smsubl:
    case Op::Umaddl:
    case Op::Umsubl: u.r(i.rn); u.r(i.rm); u.r(i.ra); u.w(i.rd); break;
    case Op::Bl: u.w(30); break;
    case Op::Cbz:
    case Op::Cbnz:
    case Op::Tbz:
    case Op::Tbnz: u.r(i.rt); break;
    case Op::Br:
    case Op::Ret: u.r(i.rn); break;
    case Op::Blr: u.r(i.rn); u.w(30); break;
    case Op::Mrs: u.w(i.rt); break;
    case Op::Msr:
    case Op::DcZva:
    case Op::CacheMaint: u.r(i.rt); break;
    case Op::LdrLit: if (!i.simd) u.w(i.rt); break;
    case Op::Prfm:
        u.r_sp(i.rn, true);
        if (i.mode == a64::AddrMode::RegOffset) u.r(i.rm);
        break;
    case Op::LdSt:
    case Op::LdStPair:
        u.r_sp(i.rn, true);
        if (i.mode == a64::AddrMode::RegOffset) u.r(i.rm);
        if (i.mode == a64::AddrMode::PreIndex || i.mode == a64::AddrMode::PostIndex) u.w_sp(i.rn, true);
        if (!i.simd) {
            if (i.load) {
                u.w(i.rt);
                if (i.op == Op::LdStPair) u.w(i.rt2);
            } else {
                u.r(i.rt);
                if (i.op == Op::LdStPair) u.r(i.rt2);
            }
        }
        break;
    case Op::LdStExcl:
        u.r_sp(i.rn, true);
        if (i.load) {
            u.w(i.rt);
            if (i.pair) u.w(i.rt2);
        } else {
            u.r(i.rt);
            if (i.pair) u.r(i.rt2);
            u.w(i.rs);
        }
        break;
    case Op::LdStAcqRel:
        u.r_sp(i.rn, true);
        if (i.load) u.w(i.rt);
        else u.r(i.rt);
        break;
    case Op::Simd: simd_use(i, u); break;
    }
    return u.u;
}

const char* sysreg_name(uint32_t packed) {
    namespace s = a64::sr;
    switch (packed) {
    case s::kNzcv: return "nzcv";
    case s::kFpcr: return "fpcr";
    case s::kFpsr: return "fpsr";
    case s::kTpidrEl0: return "tpidr_el0";
    case s::kTpidrroEl0: return "tpidrro_el0";
    case s::kCtrEl0: return "ctr_el0";
    case s::kDczidEl0: return "dczid_el0";
    case s::kCntfrqEl0: return "cntfrq_el0";
    case s::kCntpctEl0: return "cntpct_el0";
    case s::kCntvctEl0: return "cntvct_el0";
    default: return "other";
    }
}

uint64_t NativeCensus::patch_sites() const {
    uint64_t n = x18_sites + svc + sp_from_register + sp_imm_unaligned;
    for (const char* name : {"tpidr_el0", "tpidrro_el0", "cntpct_el0", "cntvct_el0", "cntfrq_el0"}) {
        if (auto it = mrs.find(name); it != mrs.end()) n += it->second;
        if (auto it = msr.find(name); it != msr.end()) n += it->second;
    }
    return n;
}

std::vector<NativeCensus> native_census(const LinkedImage& image, const AnalysisResult& analysis) {
    std::vector<NativeCensus> out(image.modules.size());
    for (size_t m = 0; m < image.modules.size(); ++m) out[m].module = image.modules[m].name;

    auto module_of = [&](uint64_t va) -> int {
        for (size_t m = 0; m < image.modules.size(); ++m) {
            const Module& mod = image.modules[m];
            if (va >= mod.base && va - mod.base < mod.image.size()) return static_cast<int>(m);
        }
        return -1;
    };
    auto decode_at = [&](uint64_t va) {
        Decoded d;
        d.inst = a64::decode(image.read_u32(va), va);
        d.use = gpr_use(d.inst);
        return d;
    };
    constexpr uint32_t kX18 = 1u << 18;

    for (const Function& f : analysis.functions) {
        const int mi = module_of(f.entry);
        if (mi < 0) continue;
        NativeCensus& c = out[static_cast<size_t>(mi)];
        ++c.functions;

        // Instructions of the function in address order.
        std::vector<Decoded> code;
        for (const BasicBlock& b : f.blocks)
            for (uint64_t pc = b.start; pc < b.end; pc += 4) code.push_back(decode_at(pc));
        std::set<uint64_t> table_brs;
        for (const JumpTable& jt : f.jump_tables) table_brs.insert(jt.br_pc);

        std::vector<bool> site(code.size(), false);
        bool fn_x18 = false;
        for (size_t k = 0; k < code.size(); ++k) {
            const Inst& i = code[k].inst;
            const GprUse& u = code[k].use;
            if (i.op == Op::Invalid) {
                ++c.undecodable;
                continue;
            }
            ++c.instructions;
            const bool x18 = ((u.reads | u.writes) & kX18) != 0;
            if (x18) {
                site[k] = true;
                fn_x18 = true;
                ++c.x18_sites;
                if (u.reads & kX18) ++c.x18_reads;
                if (u.writes & kX18) ++c.x18_writes;
                if (i.op == Op::Br && i.rn == 18) {
                    ++c.x18_br;
                    if (table_brs.count(i.pc)) ++c.x18_br_jump_table;
                }
                if (i.op == Op::Blr && i.rn == 18) ++c.x18_blr;
                const bool mem = i.op == Op::LdSt || i.op == Op::LdStPair || i.op == Op::LdStExcl ||
                                 i.op == Op::LdStAcqRel || i.op == Op::Prfm;
                if (mem && (i.rn == 18 || (i.mode == a64::AddrMode::RegOffset && i.rm == 18))) ++c.x18_mem;
                if ((u.reads | u.writes) & kGprSp) ++c.x18_sp;
                if (i.op == Op::Simd) ++c.x18_simd;
            }
            switch (i.op) {
            case Op::Svc:
                site[k] = true;
                ++c.svc;
                ++c.svc_by_number[static_cast<uint32_t>(i.imm)];
                break;
            case Op::Mrs:
                ++c.mrs[sysreg_name(i.sysreg)];
                if (patched_sysreg(i.sysreg)) site[k] = true;
                break;
            case Op::Msr:
                ++c.msr[sysreg_name(i.sysreg)];
                if (patched_sysreg(i.sysreg)) site[k] = true;
                break;
            case Op::DcZva: ++c.dc_zva; break;
            case Op::CacheMaint: ++c.cache_maint; break;
            case Op::Hint: if (is_yield_hint(i.raw)) ++c.yield_wfe; break;
            default: break;
            }
            const SpKind sp = sp_write_kind(i, u);
            if (sp.from_register) { site[k] = true; ++c.sp_from_register; }
            if (sp.imm_unaligned) { site[k] = true; ++c.sp_imm_unaligned; }
        }
        if (fn_x18) {
            ++c.functions_with_x18;
            x18_liveness(f, code, c);
            x18_web_counts(f, code, c);
        }

        // Exclusive windows: an exclusive load followed, in straight-line code,
        // by an exclusive store within 16 instructions.
        for (size_t k = 0; k < code.size(); ++k) {
            const Inst& i = code[k].inst;
            if (i.op != Op::LdStExcl || !i.load) continue;
            for (size_t e = k + 1; e < code.size() && e <= k + 16; ++e) {
                const Inst& n = code[e].inst;
                if (n.pc != code[e - 1].inst.pc + 4) break;
                if (n.op == Op::LdStExcl && !n.load) {
                    ++c.exclusive_windows;
                    bool any = false;
                    for (size_t s = k; s <= e; ++s) {
                        if (!site[s]) continue;
                        any = true;
                        if ((code[s].use.reads | code[s].use.writes) & kX18) ++c.x18_in_exclusive;
                    }
                    if (any) ++c.exclusive_windows_with_site;
                    k = e;
                    break;
                }
                if (n.flow != a64::Flow::Normal && n.flow != a64::Flow::CondBranch) break;
            }
        }

        // Spin-wait candidates: back edges whose loop body only loads,
        // computes, branches and waits, and writes no register it reads on entry.
        std::set<std::pair<uint64_t, uint64_t>> loops;
        for (const BasicBlock& b : f.blocks)
            for (uint64_t s : b.successors)
                if (s <= b.start && s >= f.entry && b.end - s <= 64 * 4) loops.insert({s, b.end});
        for (const auto& [head, end] : loops) {
            uint32_t defined = 0, live_in = 0, written = 0;
            bool ok = true, load = false, hint = false;
            for (uint64_t pc = head; pc < end && ok; pc += 4) {
                const Decoded d = decode_at(pc);
                const Inst& i = d.inst;
                switch (i.op) {
                case Op::Invalid:
                case Op::Bl:
                case Op::Blr:
                case Op::Br:
                case Op::Ret:
                case Op::Svc:
                case Op::Brk:
                case Op::Mrs:
                case Op::Msr:
                case Op::DcZva:
                case Op::CacheMaint:
                case Op::Clrex:
                case Op::Simd: ok = false; break;
                case Op::LdSt:
                case Op::LdStPair:
                case Op::LdStExcl:
                case Op::LdStAcqRel:
                    if (!i.load) ok = false;
                    break;
                case Op::Hint: if (is_yield_hint(i.raw)) hint = true; break;
                default: break;
                }
                if (!ok) break;
                if (is_load(i)) load = true;
                live_in |= d.use.reads & ~defined;
                defined |= d.use.writes;
                written |= d.use.writes;
            }
            if (ok && load && (live_in & written) == 0) {
                ++c.spin_loops;
                if (hint) ++c.spin_loops_with_hint;
            }
        }
    }
    return out;
}

}  // namespace lgp::recomp
