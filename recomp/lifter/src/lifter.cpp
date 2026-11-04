// A64 → C++ emitter. Semantics follow the pseudocode of the Arm ARM (DDI 0487)
// for each instruction; comments name the less obvious corner cases.
#include "lgp/recomp/lifter.hpp"
#include "lgp/recomp/a64_simd.hpp"

#include <algorithm>
#include <bitset>
#include <cinttypes>
#include <cstdio>
#include <set>
#include <string_view>
#include <utility>

namespace lgp::recomp {
namespace {

using a64::AddrMode;
using a64::Extend;
using a64::Flow;
using a64::Inst;
using a64::Op;
using a64::Shift;

std::string hex(uint64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "0x%" PRIx64, v);
    return buf;
}

std::string c64(uint64_t v) { return "UINT64_C(" + hex(v) + ")"; }
std::string c32(uint64_t v) { return hex(v & 0xFFFFFFFFu) + "u"; }

std::string label(uint64_t va) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "L_%" PRIx64, va);
    return buf;
}

// Places where the guest registers in lifted-function locals must be written
// back to CpuState (before anything that can observe ctx: calls, returns,
// runtime hooks) or reloaded from it (after calls). Lifter::lift() replaces
// them once it knows which registers the function uses.
constexpr const char* kSyncOut = "/*@so*/";
constexpr const char* kSyncIn = "/*@si*/";
// The same on the rarely taken poll path of a back-edge: the fast path skips
// both, so registers stay dirty across it.
constexpr const char* kSyncOutPoll = "/*@sop*/";
constexpr const char* kSyncInPoll = "/*@sip*/";
// Followed by the loop header's address and "*/".
constexpr const char* kSyncLoopEntry = "/*@sl:";
// The same as kSyncOut/kSyncIn at a function boundary (BL, BLR, RET, tail
// call): without LiftOptions::flags_across_calls they leave out NZCV.
constexpr const char* kSyncOutCall = "/*@soc*/";
constexpr const char* kSyncInCall = "/*@sic*/";
// kSyncOutCall at RET: without LiftOptions::registers_across_calls it also
// leaves out the registers no caller reads after a call (AAPCS64).
constexpr const char* kSyncOutRet = "/*@sor*/";

constexpr uint64_t ones(int n) { return n >= 64 ? ~0ull : ((1ull << n) - 1); }

uint64_t ror_n(uint64_t v, int r, int esize) {
    r %= esize;
    if (r == 0) return v;
    return ((v >> r) | (v << (esize - r))) & ones(esize);
}

// DecodeBitMasks(N, imms, immr, immediate = FALSE) for the bitfield
// instructions: returns (wmask, tmask) replicated to the register width.
std::pair<uint64_t, uint64_t> bitfield_masks(bool sf, uint32_t imms, uint32_t immr) {
    const int len = sf ? 6 : 5;
    const int esize = 1 << len;
    const uint32_t levels = static_cast<uint32_t>(ones(len));
    const int s = static_cast<int>(imms & levels);
    const int r = static_cast<int>(immr & levels);
    const int d = static_cast<int>((s - r) & static_cast<int>(levels));
    const uint64_t welem = ror_n(ones(s + 1), r, esize);
    const uint64_t telem = ones(d + 1);
    return {welem, telem};  // esize equals the register width here, no replication needed
}

// Condition codes (ConditionHolds()).
std::string cond_expr(uint8_t cond) {
    std::string base;
    switch (cond >> 1) {
    case 0: base = "lgp_f.z"; break;
    case 1: base = "lgp_f.c"; break;
    case 2: base = "lgp_f.n"; break;
    case 3: base = "lgp_f.v"; break;
    case 4: base = "(lgp_f.c && !lgp_f.z)"; break;
    case 5: base = "(lgp_f.n == lgp_f.v)"; break;
    case 6: base = "(lgp_f.n == lgp_f.v && !lgp_f.z)"; break;
    case 7: return "true";  // AL and NV both mean "always" in A64
    }
    return (cond & 1) ? "!" + base : base;
}

// Condition for a bare `if (...)`: drop the outer parentheses of a single
// comparison so compilers do not warn about `if ((a == b))`.
std::string cond_if(uint8_t cond) {
    std::string e = cond_expr(cond);
    if (e.size() > 2 && e.front() == '(' && e.back() == ')') e = e.substr(1, e.size() - 2);
    return e;
}

class Emitter {
public:
    Emitter(const Lifter& lifter, const LiftFunction& fn, const LiftOptions& opts, LiftStats& stats)
        : lifter_(lifter), fn_(fn), opts_(opts), stats_(stats) {}

    void add(const Inst& i) { insts_.push_back(i); }
    void set_internal(std::set<uint64_t> s) { internal_ = std::move(s); }
    void set_block_starts(std::set<uint64_t> s) { block_starts_ = std::move(s); }
    void need_label(uint64_t va) { labels_.insert(va); }

    // One chunk of C++ per instruction (with its label and fallthrough jump).
    std::vector<std::string> emit_body();
    // Indexes of the instructions that may run after insts_[k] inside the function.
    std::vector<size_t> successors(size_t k) const;
    uint64_t pc_of(size_t k) const { return insts_[k].pc; }
    size_t index_of(uint64_t pc) const {
        auto it = std::lower_bound(insts_.begin(), insts_.end(), pc, [](const Inst& i, uint64_t v) { return i.pc < v; });
        return it != insts_.end() && it->pc == pc ? static_cast<size_t>(it - insts_.begin()) : SIZE_MAX;
    }

    // Pre-pass: which in-function addresses are jump targets.
    void collect_labels() {
        for (size_t k = 0; k < insts_.size(); ++k) {
            const Inst& i = insts_[k];
            switch (i.op) {
            case Op::B:
            case Op::BCond:
            case Op::Cbz:
            case Op::Cbnz:
            case Op::Tbz:
            case Op::Tbnz:
                if (internal_.count(i.target)) labels_.insert(i.target);
                break;
            case Op::Br: {
                auto it = fn_.indirect_targets.find(i.pc);
                if (it != fn_.indirect_targets.end())
                    for (uint64_t t : it->second)
                        if (internal_.count(t)) labels_.insert(t);
                break;
            }
            default: break;
            }
            // Fallthrough into a block that does not directly follow in emission order.
            if (falls_through(i)) {
                const uint64_t next = i.pc + 4;
                const bool next_emitted = k + 1 < insts_.size() && insts_[k + 1].pc == next;
                if (!next_emitted && internal_.count(next)) labels_.insert(next);
            }
        }
    }

private:
    static bool accesses_memory(const Inst& i) {
        switch (i.op) {
        case Op::LdrLit:
        case Op::LdSt:
        case Op::LdStPair:
        case Op::LdStExcl:
        case Op::LdStAcqRel:
        case Op::DcZva: return true;
        case Op::Simd: return a64::simd::is_ldst_structure(i.raw);  // LD1-LD4, ST1-ST4, LDnR
        default: return false;
        }
    }

    static bool falls_through(const Inst& i) {
        switch (i.flow) {
        case Flow::Normal:
        case Flow::CondBranch:
        case Flow::Call:
        case Flow::IndirectCall:
        case Flow::Syscall:
            return true;
        default: return false;
        }
    }

    // ---- register access -------------------------------------------------
    static std::string x(uint8_t r) { return r == 31 ? std::string("UINT64_C(0)") : "lgp_x" + std::to_string(r); }
    static std::string xsp(uint8_t r) { return r == 31 ? std::string("lgp_sp") : x(r); }
    static std::string w(uint8_t r) {
        return r == 31 ? std::string("0u") : "static_cast<uint32_t>(lgp_x" + std::to_string(r) + ")";
    }
    static std::string wsp(uint8_t r) { return r == 31 ? std::string("static_cast<uint32_t>(lgp_sp)") : w(r); }
    // Operand of the instruction's width.
    static std::string rd(bool sf, uint8_t r) { return sf ? x(r) : w(r); }
    static std::string rdsp(bool sf, uint8_t r, bool sp) { return sp ? (sf ? xsp(r) : wsp(r)) : rd(sf, r); }

    // Write a result; 32-bit results are zero-extended into the X register.
    static std::string set(bool sf, uint8_t r, const std::string& expr, bool sp = false) {
        std::string dst;
        if (r == 31) {
            if (!sp) return "(void)(" + expr + ");";
            dst = "lgp_sp";
        } else {
            dst = "lgp_x" + std::to_string(r);
        }
        if (sf) return dst + " = " + expr + ";";
        return dst + " = static_cast<uint32_t>(" + expr + ");";
    }

    static const char* ty(bool sf) { return sf ? "uint64_t" : "uint32_t"; }
    static const char* sfx(bool sf) { return sf ? "64" : "32"; }
    static std::string cst(bool sf, uint64_t v) { return sf ? c64(v) : c32(v); }

    static std::string shifted(bool sf, const std::string& v, Shift s, uint8_t amount) {
        if (amount == 0) return v;
        const std::string a = std::to_string(amount);
        switch (s) {
        case Shift::Lsl: return std::string("static_cast<") + ty(sf) + ">(" + v + " << " + a + ")";
        case Shift::Lsr: return std::string("static_cast<") + ty(sf) + ">(" + v + " >> " + a + ")";
        case Shift::Asr: return std::string("lgp::lifted::asr") + sfx(sf) + "(" + v + ", " + a + ")";
        case Shift::Ror: return std::string("lgp::lifted::ror") + sfx(sf) + "(" + v + ", " + a + ")";
        }
        return v;
    }

    // ExtendReg(): extend a 64-bit register value, shift left, truncate to width.
    static std::string extended(bool sf, uint8_t rm, Extend e, uint8_t amount) {
        std::string v;
        const std::string r = x(rm);
        switch (e) {
        case Extend::Uxtb: v = "static_cast<uint64_t>(static_cast<uint8_t>(" + r + "))"; break;
        case Extend::Uxth: v = "static_cast<uint64_t>(static_cast<uint16_t>(" + r + "))"; break;
        case Extend::Uxtw: v = "static_cast<uint64_t>(static_cast<uint32_t>(" + r + "))"; break;
        case Extend::Uxtx: v = r; break;
        case Extend::Sxtb: v = "static_cast<uint64_t>(static_cast<int64_t>(static_cast<int8_t>(" + r + ")))"; break;
        case Extend::Sxth: v = "static_cast<uint64_t>(static_cast<int64_t>(static_cast<int16_t>(" + r + ")))"; break;
        case Extend::Sxtw: v = "static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(" + r + ")))"; break;
        case Extend::Sxtx: v = r; break;
        }
        if (amount) v = "(" + v + " << " + std::to_string(amount) + ")";
        return std::string("static_cast<") + ty(sf) + ">(" + v + ")";
    }

    // ---- control flow ------------------------------------------------------
    // Jump from the instruction at `from` to `target`. Backward jumps inside
    // the function are loop back-edges and get the LGP_BACKEDGE() hook.
    std::string transfer(uint64_t target, uint64_t from) const {
        if (internal_.count(target)) {
            if (target <= from && opts_.backedge_hook)
                return std::string("{ LGP_BACKEDGE_SYNC(") + kSyncOutPoll + ", " + kSyncInPoll + "); goto " + label(target) + "; }";
            if (is_loop_entry(from, target)) return "{ " + loop_entry_sync(target) + " goto " + label(target) + "; }";
            return "goto " + label(target) + ";";
        }
        if (lifter_.is_entry(target))
            return std::string("{ ") + kSyncOutCall + " LGP_TAIL_CALL(" + lifter_.function_name(target) + "); }";
        return std::string("{ ") + kSyncOut + " LGP_JUMP_INDIRECT(" + c64(target) + "); return; }";
    }

    // Marker for the registers to write back on a forward edge into a loop
    // (see Lifter::lift): the loop then starts with only its own dirty registers.
    static std::string loop_entry_sync(uint64_t header) { return std::string(kSyncLoopEntry) + hex(header) + "*/"; }

    std::string call(const Inst& i, uint64_t target) const {
        std::string s = "lgp_x30 = " + c64(i.pc + 4) + "; ";
        // ADR-0004: the host setjmp has to run in this frame, so BL setjmp is inlined.
        if (opts_.setjmp_entries.count(target))
            return s + "ctx->pc = " + c64(i.pc) + "; " + kSyncOut + " LGP_SETJMP(); " + kSyncIn + " LGP_HALT_CHECK();";
        s += kSyncOutCall;
        if (lifter_.is_entry(target))
            s += " " + lifter_.function_name(target) + "(ctx);";
        else
            s += " ctx->pc = " + c64(i.pc) + "; LGP_CALL_INDIRECT(" + c64(target) + ");";
        return s + " " + kSyncInCall + " LGP_HALT_CHECK();";
    }

    // ---- loads and stores -----------------------------------------------------
    static std::string read(unsigned bytes, const std::string& addr) {
        return "lgp_mem_read" + std::to_string(bytes * 8) + "(" + addr + ")";
    }
    static std::string write(unsigned bytes, const std::string& addr, const std::string& v) {
        const std::string t = "uint" + std::to_string(bytes * 8) + "_t";
        return "lgp_mem_write" + std::to_string(bytes * 8) + "(" + addr + ", static_cast<" + t + ">(" + v + "));";
    }

    // Load one GPR element into rt (sign/zero extension per the instruction).
    static std::string load_gpr(const Inst& i, uint8_t rt, const std::string& addr) {
        const unsigned bytes = 1u << i.size_log2;
        const std::string v = read(bytes, addr);
        if (!i.sign_extend) {
            if (rt == 31) return "(void)" + v + ";";
            return "lgp_x" + std::to_string(rt) + " = static_cast<uint64_t>(" + v + ");";
        }
        const std::string st = "int" + std::to_string(bytes * 8) + "_t";
        std::string e;
        if (i.dest64)
            e = "static_cast<uint64_t>(static_cast<int64_t>(static_cast<" + st + ">(" + v + ")))";
        else
            e = "static_cast<uint64_t>(static_cast<uint32_t>(static_cast<int32_t>(static_cast<" + st + ">(" + v + "))))";
        if (rt == 31) return "(void)" + e + ";";
        return "lgp_x" + std::to_string(rt) + " = " + e + ";";
    }

    static std::string load_simd(unsigned log2, uint8_t rt, const std::string& addr) {
        const std::string v = "lgp_v" + std::to_string(rt);
        if (log2 == 4)
            return v + ".lo = " + read(8, addr) + "; " + v + ".hi = " + read(8, "(" + addr + ") + 8") + ";";
        return v + ".lo = static_cast<uint64_t>(" + read(1u << log2, addr) + "); " + v + ".hi = 0;";
    }

    static std::string store_gpr(const Inst& i, uint8_t rt, const std::string& addr) {
        return write(1u << i.size_log2, addr, x(rt));
    }

    static std::string store_simd(unsigned log2, uint8_t rt, const std::string& addr) {
        const std::string v = "lgp_v" + std::to_string(rt);
        if (log2 == 4) return write(8, addr, v + ".lo") + " " + write(8, "(" + addr + ") + 8", v + ".hi");
        return write(1u << log2, addr, v + ".lo");
    }

    std::string element(const Inst& i, uint8_t rt, const std::string& addr) const {
        if (i.simd) return i.load ? load_simd(i.size_log2, rt, addr) : store_simd(i.size_log2, rt, addr);
        return i.load ? load_gpr(i, rt, addr) : store_gpr(i, rt, addr);
    }

    std::string emit_ldst(const Inst& i) const {
        std::string s = "uint64_t base = " + xsp(i.rn) + "; ";
        std::string addr;
        switch (i.mode) {
        case AddrMode::Offset:
        case AddrMode::PreIndex:
            s += "uint64_t addr = base + " + c64(static_cast<uint64_t>(i.offset)) + "; ";
            break;
        case AddrMode::PostIndex: s += "uint64_t addr = base; "; break;
        case AddrMode::RegOffset: s += "uint64_t addr = base + " + extended(true, i.rm, i.extend, i.amount) + "; "; break;
        case AddrMode::Literal: break;
        }
        s += element(i, i.rt, "addr");
        if (i.op == Op::LdStPair) s += " " + element(i, i.rt2, "addr + " + std::to_string(1u << i.size_log2));
        if (i.mode == AddrMode::PreIndex)
            s += " " + set(true, i.rn, "addr", true);
        else if (i.mode == AddrMode::PostIndex)
            s += " " + set(true, i.rn, "base + " + c64(static_cast<uint64_t>(i.offset)), true);
        return s;
    }

    std::string emit_exclusive(const Inst& i) const {
        const unsigned bytes = 1u << i.size_log2;
        const unsigned total = i.pair ? bytes * 2 : bytes;
        std::string s = "uint64_t addr = " + xsp(i.rn) + "; ";
        if (i.load) {
            s += "uint64_t v0 = static_cast<uint64_t>(" + read(bytes, "addr") + "); ";
            if (i.pair) s += "uint64_t v1 = static_cast<uint64_t>(" + read(bytes, "addr + " + std::to_string(bytes)) + "); ";
            s += "ctx->excl_addr = addr; ctx->excl_lo = v0; ";
            s += i.pair ? "ctx->excl_hi = v1; " : "ctx->excl_hi = 0; ";
            s += "ctx->excl_size = " + std::to_string(total) + "; ctx->excl_valid = 1; ";
            if (i.rt != 31) s += "lgp_x" + std::to_string(i.rt) + " = v0; ";
            if (i.pair && i.rt2 != 31) s += "lgp_x" + std::to_string(i.rt2) + " = v1; ";
            if (i.acquire) s += "LGP_BARRIER();";
            return s;
        }
        if (i.release) s = "LGP_BARRIER(); " + s;
        s += "uint32_t status = 1; ";
        s += "if (ctx->excl_valid && ctx->excl_addr == addr && ctx->excl_size == " + std::to_string(total) + ") { ";
        if (!i.pair) {
            if (bytes <= 4) {
                // Sub-word exclusives: compare-and-swap on the element size.
                const std::string t = "uint" + std::to_string(bytes * 8) + "_t";
                if (bytes == 4) {
                    s += "status = lgp_mem_cas32(addr, static_cast<uint32_t>(ctx->excl_lo), " + w(i.rt) + ") ? 0u : 1u; ";
                } else {
                    // No 8/16-bit CAS in the runtime contract: widen to the aligned
                    // 32-bit word that contains the element.
                    s += "uint64_t wa = addr & ~UINT64_C(3); unsigned sh = static_cast<unsigned>(addr & 3) * 8; ";
                    s += "uint32_t m = static_cast<uint32_t>(" + hex(ones(bytes * 8)) + "u) << sh; ";
                    s += "uint32_t cur = lgp_mem_read32(wa); ";
                    s += "if (((cur & m) >> sh) == static_cast<" + t + ">(ctx->excl_lo)) { ";
                    s += "uint32_t nv = (cur & ~m) | (static_cast<uint32_t>(static_cast<" + t + ">(" + x(i.rt) + ")) << sh); ";
                    s += "status = lgp_mem_cas32(wa, cur, nv) ? 0u : 1u; } ";
                }
            } else {
                s += "status = lgp_mem_cas64(addr, ctx->excl_lo, " + x(i.rt) + ") ? 0u : 1u; ";
            }
        } else if (bytes == 4) {
            s += "uint64_t expv = (ctx->excl_lo & 0xFFFFFFFFu) | (ctx->excl_hi << 32); ";
            s += "uint64_t newv = static_cast<uint64_t>(" + w(i.rt) + ") | (static_cast<uint64_t>(" + w(i.rt2) + ") << 32); ";
            s += "status = lgp_mem_cas64(addr, expv, newv) ? 0u : 1u; ";
        } else {
            s += "status = lgp_mem_cas128(addr, ctx->excl_lo, ctx->excl_hi, " + x(i.rt) + ", " + x(i.rt2) + ") ? 0u : 1u; ";
        }
        s += "} ctx->excl_valid = 0; ";
        s += set(false, i.rs, "status");
        return s;
    }

    std::string emit_inst(const Inst& i);

    const Lifter& lifter_;
    const LiftFunction& fn_;
    const LiftOptions& opts_;
    LiftStats& stats_;
    std::vector<Inst> insts_;
    std::set<uint64_t> internal_;
    std::set<uint64_t> block_starts_;
    std::set<uint64_t> labels_;

    std::set<std::pair<uint64_t, uint64_t>> loop_entries_;  // (from pc, header pc) edges from outside the loop

public:
    // Edges into a loop header from outside the loop, for loops that contain a
    // call (or SVC). Such a call writes back every dirty register; syncing on
    // entry instead leaves it only the loop's own. Loops without calls are left
    // alone: there an entry sync would only add stores. The natural loop of a
    // header is everything that reaches one of its back-edge sources without
    // passing the header.
    void compute_loops() {
        const size_t n = insts_.size();
        std::vector<std::vector<size_t>> preds(n);
        std::vector<std::vector<size_t>> succs(n);
        for (size_t k = 0; k < n; ++k) {
            succs[k] = successors(k);
            for (size_t m : succs[k])
                preds[m].push_back(k);
        }
        std::map<size_t, std::vector<bool>> body;  // header index -> members
        for (size_t k = 0; k < n; ++k)
            for (size_t h : succs[k]) {
                if (insts_[h].pc > insts_[k].pc) continue;
                std::vector<bool>& in = body[h];
                if (in.empty()) {
                    in.assign(n, false);
                    in[h] = true;
                }
                std::vector<size_t> work{k};
                while (!work.empty()) {
                    const size_t m = work.back();
                    work.pop_back();
                    if (in[m]) continue;
                    in[m] = true;
                    for (size_t p : preds[m])
                        work.push_back(p);
                }
            }
        for (const auto& [h, in] : body) {
            bool has_call = false;
            for (size_t k = 0; k < n && !has_call; ++k)
                has_call = in[k] && (insts_[k].flow == Flow::Call || insts_[k].flow == Flow::IndirectCall ||
                                     insts_[k].flow == Flow::Syscall);
            if (!has_call) continue;
            for (size_t p : preds[h])
                if (!in[p]) loop_entries_.insert({insts_[p].pc, insts_[h].pc});
        }
    }
    bool is_loop_entry(uint64_t from, uint64_t header) const { return loop_entries_.count({from, header}) != 0; }
};

std::string Emitter::emit_inst(const Inst& i) {
    const bool sf = i.sf;
    const std::string T = ty(sf);
    const std::string S = sfx(sf);

    switch (i.op) {
    // ---- data processing, immediate --------------------------------------
    case Op::Adr:
    case Op::Adrp: return set(true, i.rd, c64(i.target));

    case Op::AddImm:
    case Op::SubImm: {
        const uint64_t imm = i.imm << i.amount;
        const bool sub = i.op == Op::SubImm;
        std::string r;
        const std::string a = rdsp(sf, i.rn, true);
        if (i.setflags)
            r = "lgp::lifted::adds" + S + "(&lgp_f, " + a + ", " + cst(sf, sub ? ~imm : imm) + ", " + (sub ? "1" : "0") + ")";
        else
            r = "static_cast<" + T + ">(" + a + (sub ? " - " : " + ") + cst(sf, imm) + ")";
        return set(sf, i.rd, r, i.rd_sp);
    }

    case Op::AndImm:
    case Op::OrrImm:
    case Op::EorImm:
    case Op::AndsImm: {
        const char* op = i.op == Op::OrrImm ? " | " : i.op == Op::EorImm ? " ^ " : " & ";
        std::string r = "static_cast<" + T + ">(" + rd(sf, i.rn) + op + cst(sf, i.imm) + ")";
        if (i.setflags) r = "lgp::lifted::logic_flags" + S + "(&lgp_f, " + r + ")";
        return set(sf, i.rd, r, i.rd_sp);
    }

    case Op::Movz: return set(sf, i.rd, cst(sf, i.imm << i.amount));
    case Op::Movn: return set(sf, i.rd, cst(sf, ~(i.imm << i.amount)));
    case Op::Movk: {
        const uint64_t mask = ~(0xFFFFull << i.amount);
        return set(sf, i.rd, "static_cast<" + T + ">((" + rd(sf, i.rd) + " & " + cst(sf, mask) + ") | " +
                                 cst(sf, i.imm << i.amount) + ")");
    }

    case Op::Sbfm:
    case Op::Ubfm:
    case Op::Bfm: {
        const auto [wmask, tmask] = bitfield_masks(sf, i.imms, i.immr);
        const int width = sf ? 64 : 32;
        const unsigned r = i.immr & (width - 1);
        const unsigned s = i.imms & (width - 1);
        const std::string src = rd(sf, i.rn);
        const std::string rot = r ? "lgp::lifted::ror" + S + "(" + src + ", " + std::to_string(r) + ")" : src;
        std::string e = "{ " + T + " rot = " + rot + "; ";
        if (i.op == Op::Ubfm) {
            e += set(sf, i.rd, "static_cast<" + T + ">(rot & " + cst(sf, wmask & tmask) + ")");
        } else if (i.op == Op::Sbfm) {
            e += T + " top = static_cast<" + T + ">(" + cst(sf, 0) + " - ((" + src + " >> " + std::to_string(s) + ") & 1)); ";
            e += T + " bot = static_cast<" + T + ">(rot & " + cst(sf, wmask) + "); ";
            e += set(sf, i.rd, "static_cast<" + T + ">((top & " + cst(sf, ~tmask) + ") | (bot & " + cst(sf, tmask) + "))");
        } else {
            e += T + " dst = " + rd(sf, i.rd) + "; ";
            e += T + " bot = static_cast<" + T + ">((dst & " + cst(sf, ~wmask) + ") | (rot & " + cst(sf, wmask) + ")); ";
            e += set(sf, i.rd, "static_cast<" + T + ">((dst & " + cst(sf, ~tmask) + ") | (bot & " + cst(sf, tmask) + "))");
        }
        return e + " }";
    }

    case Op::Extr: {
        const unsigned lsb = i.imms;
        if (lsb == 0) return set(sf, i.rd, rd(sf, i.rm));
        const unsigned width = sf ? 64 : 32;
        return set(sf, i.rd, "static_cast<" + T + ">((" + rd(sf, i.rm) + " >> " + std::to_string(lsb) + ") | (" +
                                 rd(sf, i.rn) + " << " + std::to_string(width - lsb) + "))");
    }

    // ---- data processing, register -----------------------------------------
    case Op::AndReg:
    case Op::BicReg:
    case Op::OrrReg:
    case Op::OrnReg:
    case Op::EorReg:
    case Op::EonReg:
    case Op::AndsReg:
    case Op::BicsReg: {
        std::string b = shifted(sf, rd(sf, i.rm), i.shift, i.amount);
        const bool invert = i.op == Op::BicReg || i.op == Op::OrnReg || i.op == Op::EonReg || i.op == Op::BicsReg;
        if (invert) b = "static_cast<" + T + ">(~" + b + ")";
        const char* op = (i.op == Op::OrrReg || i.op == Op::OrnReg)   ? " | "
                         : (i.op == Op::EorReg || i.op == Op::EonReg) ? " ^ "
                                                                      : " & ";
        std::string r = "static_cast<" + T + ">(" + rd(sf, i.rn) + op + b + ")";
        if (i.setflags) r = "lgp::lifted::logic_flags" + S + "(&lgp_f, " + r + ")";
        return set(sf, i.rd, r);
    }

    case Op::AddReg:
    case Op::SubReg:
    case Op::AddExt:
    case Op::SubExt: {
        const bool sub = i.op == Op::SubReg || i.op == Op::SubExt;
        const bool ext = i.op == Op::AddExt || i.op == Op::SubExt;
        const std::string a = ext ? rdsp(sf, i.rn, true) : rd(sf, i.rn);
        const std::string b = ext ? extended(sf, i.rm, i.extend, i.amount) : shifted(sf, rd(sf, i.rm), i.shift, i.amount);
        std::string e = "{ " + T + " a = " + a + "; " + T + " b = " + b + "; ";
        std::string r;
        if (i.setflags)
            r = "lgp::lifted::adds" + S + "(&lgp_f, a, " + (sub ? "static_cast<" + T + ">(~b), 1" : "b, 0") + ")";
        else
            r = "static_cast<" + T + ">(a " + (sub ? "-" : "+") + " b)";
        return e + set(sf, i.rd, r, ext && i.rd_sp) + " }";
    }

    case Op::Adc:
    case Op::Sbc: {
        const bool sub = i.op == Op::Sbc;
        std::string b = rd(sf, i.rm);
        if (sub) b = "static_cast<" + T + ">(~" + b + ")";
        std::string e = "{ " + T + " a = " + rd(sf, i.rn) + "; " + T + " b = " + b + "; ";
        std::string r;
        if (i.setflags)
            r = "lgp::lifted::adds" + S + "(&lgp_f, a, b, lgp_f.c)";
        else
            r = "static_cast<" + T + ">(a + b + lgp_f.c)";
        return e + set(sf, i.rd, r) + " }";
    }

    case Op::Ccmn:
    case Op::Ccmp: {
        const std::string b = i.ccmp_imm ? cst(sf, i.imm) : rd(sf, i.rm);
        std::string e = "if (" + cond_if(i.cond) + ") { " + T + " a = " + rd(sf, i.rn) + "; " + T + " b = " + b + "; ";
        if (i.op == Op::Ccmp)
            e += "lgp::lifted::adds" + S + "(&lgp_f, a, static_cast<" + T + ">(~b), 1); ";
        else
            e += "lgp::lifted::adds" + S + "(&lgp_f, a, b, 0); ";
        return e + "} else { lgp::lifted::set_nzcv_imm(&lgp_f, " + std::to_string(i.nzcv_imm) + "); }";
    }

    case Op::Csel:
    case Op::Csinc:
    case Op::Csinv:
    case Op::Csneg: {
        std::string other = rd(sf, i.rm);
        if (i.op == Op::Csinc) other = "static_cast<" + T + ">(" + other + " + 1)";
        if (i.op == Op::Csinv) other = "static_cast<" + T + ">(~" + other + ")";
        if (i.op == Op::Csneg) other = "static_cast<" + T + ">(" + cst(sf, 0) + " - " + other + ")";
        return set(sf, i.rd, "(" + cond_expr(i.cond) + " ? " + rd(sf, i.rn) + " : " + other + ")");
    }

    case Op::Rbit: return set(sf, i.rd, "lgp::lifted::rbit" + S + "(" + rd(sf, i.rn) + ")");
    case Op::Rev16: return set(sf, i.rd, "lgp::lifted::rev16_" + S + "(" + rd(sf, i.rn) + ")");
    case Op::Rev32: return set(true, i.rd, "lgp::lifted::rev32_64(" + x(i.rn) + ")");
    case Op::Rev: return set(sf, i.rd, "lgp::lifted::bswap" + S + "(" + rd(sf, i.rn) + ")");
    case Op::Clz: return set(sf, i.rd, "lgp::lifted::clz" + S + "(" + rd(sf, i.rn) + ")");
    case Op::Cls: return set(sf, i.rd, "lgp::lifted::cls" + S + "(" + rd(sf, i.rn) + ")");

    case Op::Udiv: return set(sf, i.rd, "lgp::lifted::udiv" + S + "(" + rd(sf, i.rn) + ", " + rd(sf, i.rm) + ")");
    case Op::Sdiv: return set(sf, i.rd, "lgp::lifted::sdiv" + S + "(" + rd(sf, i.rn) + ", " + rd(sf, i.rm) + ")");
    case Op::Lslv:
    case Op::Lsrv:
    case Op::Asrv:
    case Op::Rorv: {
        const std::string amt = "static_cast<unsigned>(" + rd(sf, i.rm) + " & " + (sf ? "63u" : "31u") + ")";
        const std::string n = rd(sf, i.rn);
        std::string r;
        if (i.op == Op::Lslv) r = "static_cast<" + T + ">(" + n + " << " + amt + ")";
        if (i.op == Op::Lsrv) r = "static_cast<" + T + ">(" + n + " >> " + amt + ")";
        if (i.op == Op::Asrv) r = "lgp::lifted::asr" + S + "(" + n + ", " + amt + ")";
        if (i.op == Op::Rorv) r = "lgp::lifted::ror" + S + "(" + n + ", " + amt + ")";
        return set(sf, i.rd, r);
    }
    case Op::Crc32:
    case Op::Crc32c: {
        const unsigned bytes = 1u << i.size_log2;
        const std::string data = i.size_log2 == 3 ? x(i.rm) : "static_cast<uint64_t>(" + w(i.rm) + ")";
        return set(false, i.rd, std::string("lgp::lifted::") + (i.op == Op::Crc32 ? "crc32" : "crc32c") + "(" + w(i.rn) +
                                    ", " + data + ", " + std::to_string(bytes) + ")");
    }

    case Op::Madd:
    case Op::Msub:
        return set(sf, i.rd, "static_cast<" + T + ">(" + rd(sf, i.ra) + (i.op == Op::Madd ? " + " : " - ") +
                                 "static_cast<" + T + ">(" + rd(sf, i.rn) + " * " + rd(sf, i.rm) + "))");
    case Op::Smaddl:
    case Op::Smsubl: {
        const std::string p = "static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(" + w(i.rn) +
                              ")) * static_cast<int64_t>(static_cast<int32_t>(" + w(i.rm) + ")))";
        return set(true, i.rd, "static_cast<uint64_t>(" + x(i.ra) + (i.op == Op::Smaddl ? " + " : " - ") + p + ")");
    }
    case Op::Umaddl:
    case Op::Umsubl: {
        const std::string p = "(static_cast<uint64_t>(" + w(i.rn) + ") * static_cast<uint64_t>(" + w(i.rm) + "))";
        return set(true, i.rd, "static_cast<uint64_t>(" + x(i.ra) + (i.op == Op::Umaddl ? " + " : " - ") + p + ")");
    }
    case Op::Smulh: return set(true, i.rd, "lgp::lifted::smulh(" + x(i.rn) + ", " + x(i.rm) + ")");
    case Op::Umulh: return set(true, i.rd, "lgp::lifted::umulh(" + x(i.rn) + ", " + x(i.rm) + ")");

    // ---- branches ------------------------------------------------------------------
    case Op::B: return transfer(i.target, i.pc);
    case Op::BCond: return "if (" + cond_if(i.cond) + ") " + transfer(i.target, i.pc);
    case Op::Cbz: return "if (" + rd(sf, i.rt) + " == 0) " + transfer(i.target, i.pc);
    case Op::Cbnz: return "if (" + rd(sf, i.rt) + " != 0) " + transfer(i.target, i.pc);
    case Op::Tbz:
    case Op::Tbnz:
        return "if (((" + x(i.rt) + " >> " + std::to_string(i.bit) + ") & 1) " + (i.op == Op::Tbz ? "==" : "!=") +
               " 0) " + transfer(i.target, i.pc);
    case Op::Bl: return call(i, i.target);
    case Op::Blr:
        // Read the target before x30 is written (BLR x30).
        return "uint64_t t = " + x(i.rn) + "; lgp_x30 = " + c64(i.pc + 4) + "; ctx->pc = " + c64(i.pc) +
               "; " + kSyncOutCall + " LGP_CALL_INDIRECT(t); " + kSyncInCall + " LGP_HALT_CHECK();";
    case Op::Br: {
        std::string s = "uint64_t t = " + x(i.rn) + "; ";
        auto it = fn_.indirect_targets.find(i.pc);
        if (it != fn_.indirect_targets.end() && !it->second.empty()) {
            s += "switch (t) { ";
            std::set<uint64_t> seen;
            for (uint64_t t : it->second) {
                if (!seen.insert(t).second) continue;
                s += "case " + c64(t) + ": " + transfer(t, i.pc) + " ";
            }
            s += "default: break; } ";
        }
        return s + "ctx->pc = " + c64(i.pc) + "; " + kSyncOut + " LGP_JUMP_INDIRECT(t); return;";
    }
    case Op::Ret:
        // ADR-0004 option A: RET is a native return; the caller continues at its BL + 4.
        return std::string(kSyncOutRet) + " return;";

    // ---- exceptions and system --------------------------------------------------------
    case Op::Svc:
        return "ctx->pc = " + c64(i.pc + 4) + "; " + kSyncOut + " LGP_SVC(" + std::to_string(i.imm) + "); " + kSyncIn +
               " LGP_HALT_CHECK();";
    case Op::Brk: return "ctx->pc = " + c64(i.pc) + "; " + kSyncOut + " LGP_BRK(" + std::to_string(i.imm) + "); return;";
    case Op::Hint: {
        const uint32_t crm_op2 = (i.raw >> 5) & 0x7F;
        if (crm_op2 == 1 || crm_op2 == 2 || crm_op2 == 3) return "LGP_YIELD();";  // YIELD, WFE, WFI
        return ";";  // NOP, SEV, SEVL and unallocated hints
    }
    case Op::Clrex: return "ctx->excl_valid = 0;";
    case Op::Dmb:
    case Op::Dsb: return "LGP_BARRIER();";
    case Op::Isb: return ";";
    case Op::Mrs:
        switch (i.sysreg) {
        case a64::sr::kNzcv: return set(true, i.rt, "static_cast<uint64_t>(lgp::lifted::pack_flags(lgp_f))");
        case a64::sr::kFpcr: return set(true, i.rt, "static_cast<uint64_t>(lgp_fpcr)");
        case a64::sr::kFpsr: return set(true, i.rt, "static_cast<uint64_t>(ctx->fpsr)");
        case a64::sr::kTpidrEl0: return set(true, i.rt, "ctx->tpidr_el0");
        case a64::sr::kTpidrroEl0: return set(true, i.rt, "ctx->tpidrro_el0");
        case a64::sr::kCtrEl0: return set(true, i.rt, "lgp::lifted::kCtrEl0");
        case a64::sr::kDczidEl0: return set(true, i.rt, "lgp::lifted::kDczidEl0");
        default: return set(true, i.rt, "static_cast<uint64_t>(LGP_READ_SYSREG(" + std::to_string(i.sysreg) + "))");
        }
    case Op::Msr:
        switch (i.sysreg) {
        case a64::sr::kNzcv: return "lgp::lifted::unpack_flags(&lgp_f, " + x(i.rt) + ");";
        case a64::sr::kFpcr: return "lgp_fpcr = static_cast<uint32_t>(" + x(i.rt) + ") & lgp::lifted::kFpcrMask;";
        case a64::sr::kFpsr: return "ctx->fpsr = static_cast<uint32_t>(" + x(i.rt) + ") & lgp::lifted::kFpsrMask;";
        case a64::sr::kTpidrEl0: return "ctx->tpidr_el0 = " + x(i.rt) + ";";
        default: break;
        }
        break;
    case Op::DcZva:
        return "uint64_t a = " + x(i.rt) +
               " & ~(lgp::lifted::kDcZvaBytes - 1); for (unsigned k = 0; k < lgp::lifted::kDcZvaBytes; k += 8) "
               "lgp_mem_write64(a + k, 0);";
    case Op::CacheMaint: return ";";

    // ---- loads and stores --------------------------------------------------------------
    case Op::LdrLit: return element(i, i.rt, c64(i.target));
    case Op::PrfmLit:
    case Op::Prfm: return ";";
    case Op::LdSt:
    case Op::LdStPair: return emit_ldst(i);
    case Op::LdStExcl: return emit_exclusive(i);
    case Op::LdStAcqRel: {
        if (i.load) return "uint64_t addr = " + xsp(i.rn) + "; " + load_gpr(i, i.rt, "addr") + " LGP_BARRIER();";
        return "LGP_BARRIER(); uint64_t addr = " + xsp(i.rn) + "; " + store_gpr(i, i.rt, "addr");
    }

    case Op::Simd: {
        a64::simd::Lifted l = a64::simd::lift(i.raw, i.pc, true);
        if (l.invalid != a64::Invalid::None) break;
        ++stats_.by_op[std::string("simd.") + l.name];
        return std::move(l.code);
    }

    case Op::Invalid:
    case Op::kCount: break;
    }
    ++stats_.unsupported;
    stats_.unsupported_pcs.push_back(i.pc);
    return "ctx->pc = " + c64(i.pc) + "; " + kSyncOut + " LGP_UNDEFINED(" + c32(i.raw) + "); return;";
}

std::vector<size_t> Emitter::successors(size_t k) const {
    std::vector<size_t> out;
    const Inst& i = insts_[k];
    auto add = [&](uint64_t pc) {
        if (!internal_.count(pc)) return;
        const size_t n = index_of(pc);
        if (n != SIZE_MAX) out.push_back(n);
    };
    if (falls_through(i)) add(i.pc + 4);
    switch (i.op) {
    case Op::B:
    case Op::BCond:
    case Op::Cbz:
    case Op::Cbnz:
    case Op::Tbz:
    case Op::Tbnz: add(i.target); break;
    case Op::Br: {
        auto it = fn_.indirect_targets.find(i.pc);
        if (it != fn_.indirect_targets.end())
            for (uint64_t t : it->second)
                add(t);
        break;
    }
    default: break;
    }
    return out;
}

std::vector<std::string> Emitter::emit_body() {
    std::vector<std::string> chunks;
    for (size_t k = 0; k < insts_.size(); ++k) {
        std::string out;
        const Inst& i = insts_[k];
        if (labels_.count(i.pc)) out += label(i.pc) + ":;\n";
        if (opts_.block_trace && block_starts_.count(i.pc)) out += "    LGP_TRACE_BLOCK(" + c64(i.pc) + ");\n";
        ++stats_.instructions;
        ++stats_.by_op[a64::op_name(i.op)];
        char comment[64];
        std::snprintf(comment, sizeof(comment), "    /* %" PRIx64 ": %08x %s */ ", i.pc, i.raw, a64::op_name(i.op));
        out += comment;
        out += "{ ";
        if (opts_.precise_fault_pc && accesses_memory(i)) out += "ctx->pc = " + c64(i.pc) + "; " + kSyncOut + " ";
        if (opts_.patch_sites.count(i.pc) && i.flow == Flow::Normal) {
            // The patch path syncs like the poll path of a back-edge: the
            // unpatched path skips both, so its registers stay dirty.
            stats_.patch_sites.insert(i.pc);
            // LGP_PATCH() false: the host function only looked, the
            // instruction still runs (observing patches, LGP_PATCH_LOG),
            // with ctx->pc back on it for a fault or a write watch.
            const std::string inst = emit_inst(i);
            out += "if (LGP_PATCH_ACTIVE(" + c64(i.pc) + ")) [[unlikely]] { ctx->pc = " + c64(i.pc + 4) + "; " +
                   kSyncOutPoll + " const bool lgp_replaced = LGP_PATCH(" + c64(i.pc) + "); " + kSyncInPoll +
                   " LGP_HALT_CHECK(); if (!lgp_replaced) { ctx->pc = " + c64(i.pc) + "; " + inst + " } } else { ";
            out += inst + " } }\n";
        } else {
            if (opts_.patch_sites.count(i.pc)) stats_.rejected_patch_sites.push_back(i.pc);
            out += emit_inst(i) + " }\n";
        }

        if (falls_through(i)) {
            const uint64_t next = i.pc + 4;
            const bool next_emitted = k + 1 < insts_.size() && insts_[k + 1].pc == next;
            if (!next_emitted) out += "    " + transfer(next, i.pc) + "\n";
            else if (is_loop_entry(i.pc, next)) out += "    " + loop_entry_sync(next) + "\n";
        }
        chunks.push_back(std::move(out));
    }
    return chunks;
}

// Guest state a lifted function touches, found by scanning the emitted code
// for the names the emitter uses: lgp_x<N>, lgp_sp, lgp_f (NZCV), lgp_fpcr,
// lgp_v<N>. One bit each: 0-30 = x0-x30, then SP, NZCV, FPCR, v0-v31.
constexpr int kBitSp = 31, kBitFlags = 32, kBitFpcr = 33, kBitV0 = 34, kStateBits = 66;
using StateMask = std::bitset<kStateBits>;
const StateMask kAllState = StateMask().set();

StateMask bit_of(int b) { return StateMask().set(static_cast<size_t>(b)); }

struct StateUse {
    StateMask used;
    StateMask written;
};

bool ident_char(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

StateUse scan_state(const std::string& code) {
    StateUse u;
    // Assignment `name = ...` (not `==`); the emitter writes X registers only this
    // way or, for the flags, by passing &lgp_f to a helper. V registers are also
    // written per half (`.lo = `, `.hi |= `), through a pointer (&lgp_v<N>, the
    // structure loads) and by set_lane(lgp_v<N>, ...).
    auto assigned = [&](size_t end) { return code.compare(end, 3, " = ") == 0; };
    auto v_written = [&](size_t start, size_t end) {
        if (start > 0 && code[start - 1] == '&') return true;
        if (code.compare(end, 3, ".lo") == 0 || code.compare(end, 3, ".hi") == 0) end += 3;
        if (end + 2 < code.size() && code[end] == ' ' && code[end + 2] == '=' && code[end + 1] != '=' &&
            code[end + 1] != '!' && code[end + 1] != '<' && code[end + 1] != '>')
            return true;  // compound assignment `|=`, `&=`, ...
        if (code.compare(end, 3, " = ") == 0) return true;
        const size_t call = code.rfind("set_lane<", start);
        return call != std::string::npos && code.find('(', call) == start - 1;
    };
    for (size_t p = code.find("lgp_"); p != std::string::npos; p = code.find("lgp_", p + 4)) {
        if (p > 0 && ident_char(code[p - 1])) continue;
        size_t e = p + 4;
        while (e < code.size() && ident_char(code[e]))
            ++e;
        const std::string_view name(code.data() + p + 4, e - p - 4);
        int bit = -1;
        bool write = assigned(e);
        const bool numbered = name.size() >= 2 && name.find_first_not_of("0123456789", 1) == std::string_view::npos;
        if (numbered && name[0] == 'x') {
            const int r = std::stoi(std::string(name.substr(1)));
            if (r < 31) bit = r;
        } else if (numbered && name[0] == 'v') {
            const int r = std::stoi(std::string(name.substr(1)));
            if (r < 32) {
                bit = kBitV0 + r;
                write = v_written(p, e);
            }
        } else if (name == "sp") {
            bit = kBitSp;
        } else if (name == "f") {
            bit = kBitFlags;
            write = p > 0 && code[p - 1] == '&';
        } else if (name == "fpcr") {
            bit = kBitFpcr;
        }
        if (bit < 0) continue;
        u.used.set(static_cast<size_t>(bit));
        if (write) u.written.set(static_cast<size_t>(bit));
    }
    return u;
}

// Code that copies the state in `mask` between the locals and CpuState.
std::string sync_code(const StateMask& mask, bool out) {
    std::string s;
    auto put = [&](const std::string& local, const std::string& field) {
        s += out ? field + " = " + local + "; " : local + " = " + field + "; ";
    };
    for (int r = 0; r < 31; ++r)
        if (mask[r]) put("lgp_x" + std::to_string(r), "ctx->x[" + std::to_string(r) + "]");
    if (mask[kBitSp]) put("lgp_sp", "ctx->sp");
    if (mask[kBitFlags])
        for (const char* f : {"n", "z", "c", "v"})
            put(std::string("lgp_f.") + f, std::string("ctx->") + f);
    if (mask[kBitFpcr]) put("lgp_fpcr", "ctx->fpcr");
    for (int r = 0; r < 32; ++r)
        if (mask[kBitV0 + r]) put("lgp_v" + std::to_string(r), "ctx->vreg[" + std::to_string(r) + "]");
    if (!s.empty()) s.pop_back();
    return s;
}

// The chunk reloads every register from CpuState (after a call or SVC).
bool has_sync_in(const std::string& chunk) {
    return chunk.find(kSyncIn) != std::string::npos || chunk.find(kSyncInCall) != std::string::npos;
}

void replace_all(std::string& s, const std::string& from, const std::string& to) {
    std::string out;
    out.reserve(s.size());
    size_t last = 0;
    for (size_t p = s.find(from); p != std::string::npos; p = s.find(from, last)) {
        out.append(s, last, p - last);
        out += to;
        last = p + from.size();
    }
    out.append(s, last, std::string::npos);
    s = std::move(out);
}

}  // namespace

Lifter::Lifter(CodeReader reader, std::vector<uint64_t> function_entries, LiftOptions options)
    : reader_(std::move(reader)), entries_(std::move(function_entries)), options_(std::move(options)) {
    std::sort(entries_.begin(), entries_.end());
    entries_.erase(std::unique(entries_.begin(), entries_.end()), entries_.end());
}

std::string Lifter::function_name(uint64_t entry) const {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%" PRIx64, entry);
    return options_.name_prefix + buf;
}

bool Lifter::is_entry(uint64_t va) const { return std::binary_search(entries_.begin(), entries_.end(), va); }

std::string Lifter::declarations() const {
    std::string out;
    for (uint64_t e : entries_)
        out += "void " + function_name(e) + "(lgp::CpuState* ctx);\n";
    return out;
}

std::string Lifter::lift(const LiftFunction& fn, LiftStats& stats) const {
    std::vector<LiftBlock> blocks = fn.blocks;
    std::sort(blocks.begin(), blocks.end(), [](const LiftBlock& a, const LiftBlock& b) { return a.start < b.start; });

    Emitter em(*this, fn, options_, stats);
    std::set<uint64_t> internal;
    std::set<uint64_t> starts;
    for (const LiftBlock& b : blocks) {
        starts.insert(b.start);
        for (uint64_t pc = b.start; pc < b.end; pc += 4) {
            uint32_t raw = 0;
            if (!reader_(pc, raw)) raw = 0;  // outside the image: decodes as UDF #0 (Op::Invalid)
            em.add(a64::decode(raw, pc));
            internal.insert(pc);
        }
    }
    em.set_internal(std::move(internal));
    em.set_block_starts(std::move(starts));
    em.collect_labels();
    if (options_.register_locals) em.compute_loops();

    // The entry block may not be the lowest address of the function.
    const bool entry_jump = !blocks.empty() && blocks.front().start != fn.entry;
    if (entry_jump) em.need_label(fn.entry);
    std::vector<std::string> chunks = em.emit_body();

    // Guest registers live in locals of the lifted function (register_locals)
    // or are references into CpuState. Locals let the compiler keep them in
    // host registers across guest memory accesses, which may alias anything.
    std::vector<StateUse> use(chunks.size());
    StateUse all;
    for (size_t k = 0; k < chunks.size(); ++k) {
        use[k] = scan_state(chunks[k]);
        all.used |= use[k].used;
        all.written |= use[k].written;
    }
    std::string decl;
    const bool loc = options_.register_locals;
    const char* ref = loc ? " " : "& ";
    for (int r = 0; r < 31; ++r)
        if (all.used[r])
            decl += std::string("    uint64_t") + ref + "lgp_x" + std::to_string(r) + " = ctx->x[" + std::to_string(r) + "];\n";
    if (all.used[kBitSp]) decl += std::string("    uint64_t") + ref + "lgp_sp = ctx->sp;\n";
    // Without flags_across_calls the flags are undefined at entry (AAPCS64).
    const bool abi_flags = loc && !options_.flags_across_calls;
    if (all.used[kBitFlags])
        decl += !loc        ? "    lgp::CpuState& lgp_f = *ctx;\n"
                : abi_flags ? "    lgp::lifted::Nzcv lgp_f{};\n"
                            : "    lgp::lifted::Nzcv lgp_f{ctx->n, ctx->z, ctx->c, ctx->v};\n";
    if (all.used[kBitFpcr]) decl += std::string("    uint32_t") + ref + "lgp_fpcr = ctx->fpcr;\n";
    for (int r = 0; r < 32; ++r)
        if (all.used[kBitV0 + r])
            decl += std::string("    lgp::V128") + ref + "lgp_v" + std::to_string(r) + " = ctx->vreg[" + std::to_string(r) + "];\n";
    // Back-edge counter of this invocation for LGP_BACKEDGE_SYNC (lifted.hpp).
    bool backedges = false;
    for (const std::string& c : chunks)
        backedges = backedges || c.find("LGP_BACKEDGE_SYNC(") != std::string::npos;
    if (backedges) decl += "    [[maybe_unused]] uint32_t lgp_backedges = 0;\n    LGP_BACKEDGE_ENTRY();\n";

    // Function boundaries. With flags_across_calls = false NZCV never crosses
    // them. With registers_across_calls = false (AAPCS64) RET does not write
    // back the corruptible x9-x17, v4-v7 and v16-v31: callers do not read them
    // after a call (results are in x0-x7 and v0-v3). SVC, BRK, setjmp and
    // indirect jumps to non-entries still sync everything.
    const StateMask call_mask = abi_flags ? ~bit_of(kBitFlags) : kAllState;
    StateMask ret_mask = call_mask;  // written back at RET
    if (loc && !options_.registers_across_calls) {
        for (int r = 9; r <= 17; ++r)
            ret_mask.reset(static_cast<size_t>(r));
        for (int r = 4; r < 32; ++r)
            if (r < 8 || r >= 16) ret_mask.reset(static_cast<size_t>(kBitV0 + r));
    }

    // Write back only what may differ from CpuState ("dirty"): forward data flow
    // over the instructions, a register is dirty after a write until the next
    // sync that reloads it. Unreached instructions count as all-dirty.
    std::vector<StateMask> dirty_in(chunks.size());
    std::vector<bool> seen(chunks.size(), false);
    if (loc && !chunks.empty()) {
        const size_t entry = em.index_of(fn.entry);
        std::vector<size_t> work;
        if (entry != SIZE_MAX) {
            seen[entry] = true;
            work.push_back(entry);
        }
        while (!work.empty()) {
            const size_t k = work.back();
            work.pop_back();
            StateMask out = dirty_in[k] | use[k].written;
            if (has_sync_in(chunks[k])) out.reset();
            for (size_t n : em.successors(k)) {
                // Forward edges into a loop write back whatever the loop itself does
                // not leave dirty, so they add nothing here.
                const bool loop_entry = em.is_loop_entry(em.pc_of(k), em.pc_of(n));
                if (loop_entry) {
                    if (!seen[n]) {
                        seen[n] = true;
                        work.push_back(n);
                    }
                    continue;
                }
                if (seen[n] && (dirty_in[n] | out) == dirty_in[n]) continue;
                seen[n] = true;
                dirty_in[n] |= out;
                work.push_back(n);
            }
        }
    }
    const std::string sync_in = loc ? sync_code(all.used, false) : std::string();
    const std::string sync_in_call = loc ? sync_code(all.used & call_mask, false) : std::string();
    std::string body;
    for (size_t k = 0; k < chunks.size(); ++k) {
        const StateMask dirty = ((seen[k] ? dirty_in[k] : kAllState) | use[k].written) & all.written;
        const std::string sync_out = loc ? sync_code(dirty, true) : std::string();
        const std::string sync_out_call = loc ? sync_code(dirty & call_mask, true) : std::string();
        const std::string sync_out_ret = loc ? sync_code(dirty & ret_mask, true) : std::string();
        // State after the chunk's call or SVC, for a loop entry behind it.
        StateMask after = dirty;
        if (has_sync_in(chunks[k])) after.reset();
        for (size_t p = chunks[k].find(kSyncLoopEntry); p != std::string::npos; p = chunks[k].find(kSyncLoopEntry, p)) {
            const size_t end = chunks[k].find("*/", p);
            const uint64_t header = std::stoull(chunks[k].substr(p + 6, end - p - 6), nullptr, 16);
            const size_t h = em.index_of(header);
            const StateMask keep = h != SIZE_MAX && seen[h] ? dirty_in[h] : kAllState;
            const std::string code = loc ? sync_code(after & ~keep, true) : std::string();
            chunks[k].replace(p, end + 2 - p, code);
            p += code.size();
        }
        replace_all(chunks[k], kSyncOut, sync_out);
        replace_all(chunks[k], kSyncIn, sync_in);
        replace_all(chunks[k], kSyncOutCall, sync_out_call);
        replace_all(chunks[k], kSyncOutRet, sync_out_ret);
        replace_all(chunks[k], kSyncInCall, sync_in_call);
        replace_all(chunks[k], kSyncOutPoll, sync_out);
        replace_all(chunks[k], kSyncInPoll, sync_in);
        body += chunks[k];
    }

    std::string out = "void " + function_name(fn.entry) + "(lgp::CpuState* ctx) {\n";
    out += decl;
    if (entry_jump) out += "    goto " + label(fn.entry) + ";\n";
    out += body;
    out += "}\n";
    return out;
}

}  // namespace lgp::recomp
