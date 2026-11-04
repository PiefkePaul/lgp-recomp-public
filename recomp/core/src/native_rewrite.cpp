// x18 renaming plan for native execution (see native_rewrite.hpp).
#include "lgp/recomp/native_rewrite.hpp"

#include <algorithm>
#include <bit>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace lgp::recomp {

using a64::Inst;
using a64::Op;

namespace {

constexpr uint32_t kX18 = 1u << 18;
constexpr uint32_t kArgs = 0x1FFu;                           // x0..x8
constexpr uint32_t kClobber = 0x3FFFFu | (1u << 30);         // x0..x17, x30 (x18 is ours)
constexpr uint32_t kExit = 0xFFu | (0xFFFu << 19) | kGprSp;  // x0..x7, x19..x30, SP

bool is_call(Op op) { return op == Op::Bl || op == Op::Blr; }

uint32_t map_reg(uint32_t mask, unsigned from, unsigned to) {
    if (!(mask & (1u << from))) return mask;
    return (mask & ~(1u << from)) | (1u << to);
}

// A decoded field may also hold immediate bits (the decoder fills fields the
// form does not use), so a field may stay as it was; gpr_use() decides which
// fields are operands.
bool field_ok(uint8_t a, uint8_t b, unsigned from, unsigned to) { return b == a || (a == from && b == to); }

// Same instruction apart from the register fields, which must be `a` with from -> to.
bool same_but_renamed(const Inst& a, const Inst& b, unsigned from, unsigned to) {
    return a.op == b.op && a.flow == b.flow && a.invalid == b.invalid && a.sf == b.sf && a.setflags == b.setflags &&
           field_ok(a.rd, b.rd, from, to) && field_ok(a.rn, b.rn, from, to) &&
           field_ok(a.rm, b.rm, from, to) && field_ok(a.ra, b.ra, from, to) &&
           field_ok(a.rt, b.rt, from, to) && field_ok(a.rt2, b.rt2, from, to) &&
           field_ok(a.rs, b.rs, from, to) && a.rd_sp == b.rd_sp && a.rn_sp == b.rn_sp && a.shift == b.shift &&
           a.amount == b.amount && a.extend == b.extend && a.cond == b.cond && a.nzcv_imm == b.nzcv_imm &&
           a.ccmp_imm == b.ccmp_imm && a.immr == b.immr && a.imms == b.imms && a.bit == b.bit && a.imm == b.imm &&
           a.size_log2 == b.size_log2 && a.load == b.load && a.sign_extend == b.sign_extend &&
           a.dest64 == b.dest64 && a.simd == b.simd && a.acquire == b.acquire && a.release == b.release &&
           a.pair == b.pair && a.mode == b.mode && a.offset == b.offset && a.target == b.target &&
           a.sysreg == b.sysreg && a.barrier == b.barrier;
}

// Instructions inside an exclusive window: an exclusive load followed, in
// straight-line code, by an exclusive store within 16 instructions (the
// census rule). Extra memory accesses there may clear the monitor.
std::vector<char> exclusive_windows(const std::vector<NativeInst>& code) {
    std::vector<char> in(code.size(), 0);
    for (size_t k = 0; k < code.size(); ++k) {
        const Inst& i = code[k].inst;
        if (i.op != Op::LdStExcl || !i.load) continue;
        for (size_t e = k + 1; e < code.size() && e <= k + 16; ++e) {
            if (code[e].inst.pc != code[e - 1].inst.pc + 4) break;
            if (code[e].inst.op == Op::LdStExcl && !code[e].inst.load) {
                std::fill(in.begin() + static_cast<std::ptrdiff_t>(k), in.begin() + static_cast<std::ptrdiff_t>(e + 1), 1);
                k = e;
                break;
            }
        }
    }
    return in;
}

// Block layout of a function's `code` (instructions in block order).
struct FlowIndex {
    std::vector<size_t> first;              // first[b] .. first[b + 1]: block b
    std::vector<std::vector<size_t>> succs; // successor blocks inside the function
    std::vector<size_t> block;              // block of every instruction
    explicit FlowIndex(const Function& f) {
        const size_t nb = f.blocks.size();
        first.assign(nb + 1, 0);
        for (size_t b = 0; b < nb; ++b) first[b + 1] = first[b] + (f.blocks[b].end - f.blocks[b].start) / 4;
        block.resize(first[nb]);
        for (size_t b = 0; b < nb; ++b)
            for (size_t k = first[b]; k < first[b + 1]; ++k) block[k] = b;
        succs.resize(nb);
        for (size_t b = 0; b < nb; ++b)
            for (uint64_t s : f.blocks[b].successors) {
                auto it = std::lower_bound(f.blocks.begin(), f.blocks.end(), s,
                                           [](const BasicBlock& x, uint64_t v) { return x.start < v; });
                if (it != f.blocks.end() && it->start == s) succs[b].push_back(static_cast<size_t>(it - f.blocks.begin()));
            }
    }
    template <class F>
    void next(size_t k, F&& fn) const {
        const size_t b = block[k];
        if (k + 1 < first[b + 1]) return fn(k + 1);
        for (size_t s : succs[b])
            if (first[s] < first[s + 1]) fn(first[s]);
    }
};

// How an instruction moves SP: by a known amount, or unknown (SP from a
// register, AND sp, ...).
std::optional<int64_t> sp_effect(const NativeInst& d) {
    const Inst& i = d.inst;
    if (!(d.use.writes & kGprSp)) return 0;
    if ((i.op == Op::AddImm || i.op == Op::SubImm) && i.rd == 31 && i.rd_sp && i.rn == 31 && i.rn_sp) {
        const int64_t v = static_cast<int64_t>(i.imm << i.amount);
        return i.op == Op::AddImm ? v : -v;
    }
    if ((i.op == Op::LdSt || i.op == Op::LdStPair) && i.rn == 31 &&
        (i.mode == a64::AddrMode::PreIndex || i.mode == a64::AddrMode::PostIndex))
        return i.offset;
    return std::nullopt;
}

bool pc_relative_x18(Op op) {
    return op == Op::Adr || op == Op::Adrp || op == Op::LdrLit || op == Op::Cbz || op == Op::Cbnz ||
           op == Op::Tbz || op == Op::Tbnz;
}

struct SpillPlan {
    std::vector<X18Spill> sites;
    uint64_t NativeRewrite::*reason = nullptr;  // why the web cannot be spilled
};

// Stack-slot plan for one web (native_rewrite.hpp, X18Spill). The web never
// spans a call, so on this thread nothing else writes below SP while it is
// live; the slots lie below the lowest SP of every point.
SpillPlan plan_spill(const X18Web& w, const std::vector<NativeInst>& code, const std::vector<uint32_t>& live_in,
                     const std::vector<uint32_t>& live_out, const FlowIndex& flow, const std::vector<char>& excl,
                     const std::vector<char>& gate, uint32_t candidates) {
    SpillPlan p;
    std::unordered_map<size_t, int64_t> delta;  // SP at each point, relative to the first one
    std::unordered_map<size_t, std::vector<std::pair<size_t, bool>>> adj;  // (other point, forward)
    for (size_t k : w.points) {
        if (!sp_effect(code[k])) { p.reason = &NativeRewrite::webs_sp_unknown; return p; }
        // A gate stub puts its frame below SP, over the slot.
        if (gate[k]) { p.reason = &NativeRewrite::webs_gate; return p; }
        if (!(live_out[k] & kX18)) continue;
        flow.next(k, [&](size_t j) {
            if (!(live_in[j] & kX18)) return;
            adj[k].push_back({j, true});
            adj[j].push_back({k, false});
        });
    }
    std::vector<size_t> todo{w.points.front()};
    delta[w.points.front()] = 0;
    while (!todo.empty()) {
        const size_t k = todo.back();
        todo.pop_back();
        for (const auto& [j, fwd] : adj[k]) {
            const int64_t d = fwd ? delta[k] + *sp_effect(code[k]) : delta[k] - *sp_effect(code[j]);
            const auto it = delta.find(j);
            if (it == delta.end()) {
                delta[j] = d;
                todo.push_back(j);
            } else if (it->second != d) {
                p.reason = &NativeRewrite::webs_sp_unknown;
                return p;
            }
        }
    }
    int64_t low = 0;
    for (size_t k : w.points) {
        if (!delta.count(k)) { p.reason = &NativeRewrite::webs_sp_unknown; return p; }
        low = std::min({low, delta[k], delta[k] + *sp_effect(code[k])});
    }
    const int64_t value = low - 16, save = low - 8;
    for (size_t k : w.points) {
        const int64_t before = delta[k], after = delta[k] + *sp_effect(code[k]);
        if (value - std::max(before, after) < -256) { p.reason = &NativeRewrite::webs_slot_far; return p; }
    }

    for (size_t k : w.sites) {
        const NativeInst& d = code[k];
        const Inst& i = d.inst;
        if (excl[k]) { p.reason = &NativeRewrite::webs_exclusive; return p; }
        const uint32_t named = d.use.reads | d.use.writes;
        if (i.op == Op::Blr || i.op == Op::Ret || native_stub_site(i, d.use)) {
            p.reason = &NativeRewrite::webs_form;
            return p;
        }
        X18Spill s;
        s.va = i.pc;
        s.word = i.raw;
        const int64_t before = delta[k], after = delta[k] + *sp_effect(d);
        s.slot_before = static_cast<int16_t>(value - before);
        s.slot_after = static_cast<int16_t>(value - after);
        s.save_before = static_cast<int16_t>(save - before);
        s.save_after = static_cast<int16_t>(save - after);
        const uint32_t dead = ~(live_in[k] | live_out[k] | named) & candidates & ~kX18 & ~kGprSp;
        static constexpr uint8_t kOrder[] = {16, 17, 9, 10, 11, 12, 13, 14, 15, 8, 7, 6, 5, 4, 3, 2, 1, 0,
                                             19, 20, 21, 22, 23, 24, 25, 26, 27, 28};
        bool found = false;
        for (uint8_t reg : kOrder)
            if (dead & (1u << reg)) {
                s.helper = reg;
                s.helper_dead = found = true;
                break;
            }
        if (!found) {
            if (i.op == Op::Br) { p.reason = &NativeRewrite::webs_form; return p; }  // no way to restore it
            for (uint8_t reg : kOrder)
                if (reg < 18 && !(named & (1u << reg))) {
                    s.helper = reg;
                    found = true;
                    break;
                }
        }
        if (!found || (!pc_relative_x18(i.op) && !rename_gpr(i.raw, i.pc, 18, s.helper))) {
            p.reason = &NativeRewrite::webs_form;
            return p;
        }
        p.sites.push_back(s);
    }
    return p;
}

}  // namespace

std::optional<uint32_t> rename_gpr(uint32_t word, uint64_t pc, unsigned from, unsigned to) {
    if (from > 30 || to > 30 || from == to) return std::nullopt;
    const Inst old_inst = a64::decode(word, pc);
    if (old_inst.op == Op::Invalid) return std::nullopt;
    const GprUse old_use = gpr_use(old_inst);
    const uint32_t named = old_use.reads | old_use.writes;
    if (!(named & (1u << from)) || (named & (1u << to))) return std::nullopt;
    const GprUse want{map_reg(old_use.reads, from, to), map_reg(old_use.writes, from, to)};

    // Operand fields at bits 0, 5, 10 and 16. Try the smallest set of fields
    // holding `from`; an immediate that happens to hold the same bits is never
    // needed for the register use to match, so the smallest set leaves it alone.
    static constexpr int kShift[4] = {0, 5, 10, 16};
    unsigned holders[4];
    int n = 0;
    for (int s : kShift)
        if (((word >> s) & 31u) == from) holders[n++] = static_cast<unsigned>(s);
    std::optional<uint32_t> best;
    int best_size = 5, best_count = 0;
    for (unsigned set = 1; set < (1u << n); ++set) {
        const int size = std::popcount(set);
        if (size > best_size) continue;
        uint32_t w = word;
        for (int k = 0; k < n; ++k)
            if (set & (1u << k)) w = (w & ~(31u << holders[k])) | (to << holders[k]);
        const Inst ni = a64::decode(w, pc);
        const GprUse nu = gpr_use(ni);
        if (nu.reads != want.reads || nu.writes != want.writes || !same_but_renamed(old_inst, ni, from, to)) continue;
        if (size < best_size) {
            best_size = size;
            best_count = 0;
        }
        ++best_count;
        best = w;
    }
    if (best_count != 1) return std::nullopt;  // none, or ambiguous
    return best;
}

std::vector<X18Web> x18_webs(const Function& f, const std::vector<NativeInst>& code,
                             const std::vector<uint64_t>& extra_entries, std::vector<uint32_t>* live_in,
                             std::vector<uint32_t>* live_out) {
    const size_t nb = f.blocks.size();
    std::vector<size_t> first(nb + 1, 0);
    for (size_t b = 0; b < nb; ++b) first[b + 1] = first[b] + (f.blocks[b].end - f.blocks[b].start) / 4;
    auto block_of = [&](uint64_t va) -> long {
        auto it = std::lower_bound(f.blocks.begin(), f.blocks.end(), va,
                                   [](const BasicBlock& b, uint64_t v) { return b.start < v; });
        if (it == f.blocks.end() || it->start != va) return -1;
        return static_cast<long>(it - f.blocks.begin());
    };
    std::vector<std::vector<size_t>> succs(nb);
    std::vector<uint32_t> exit_live(nb, 0);
    for (size_t b = 0; b < nb; ++b) {
        bool outside = f.blocks[b].successors.empty();
        for (uint64_t s : f.blocks[b].successors) {
            const long k = block_of(s);
            if (k >= 0) succs[b].push_back(static_cast<size_t>(k));
            else outside = true;
        }
        if (!outside || first[b + 1] == first[b]) continue;
        const Inst& last = code[first[b + 1] - 1].inst;
        // An indirect jump the CFG could not resolve may land anywhere.
        exit_live[b] = (last.op == Op::Br && f.has_unresolved_indirect) ? ~0u : kExit;
    }
    auto step = [&](size_t k, uint32_t live) {
        const GprUse& u = code[k].use;
        if (is_call(code[k].inst.op)) return (live & ~kClobber) | kArgs | u.reads;
        return (live & ~u.writes) | u.reads;
    };
    std::vector<uint32_t> in(code.size(), 0), out(code.size(), 0), block_in(nb, 0);
    for (bool changed = true; changed;) {
        changed = false;
        for (size_t bb = nb; bb-- > 0;) {
            uint32_t live = exit_live[bb];
            for (size_t s : succs[bb]) live |= block_in[s];
            for (size_t k = first[bb + 1]; k-- > first[bb];) {
                out[k] = live;
                live = step(k, live);
                in[k] = live;
            }
            if (live != block_in[bb]) {
                block_in[bb] = live;
                changed = true;
            }
        }
    }

    std::vector<size_t> parent(code.size());
    for (size_t k = 0; k < code.size(); ++k) parent[k] = k;
    auto find = [&](size_t k) {
        while (parent[k] != k) k = parent[k] = parent[parent[k]];
        return k;
    };
    auto node = [&](size_t k) { return ((in[k] | out[k] | code[k].use.reads | code[k].use.writes) & kX18) != 0; };
    for (size_t b = 0; b < nb; ++b)
        for (size_t k = first[b]; k < first[b + 1]; ++k) {
            if (!(out[k] & kX18)) continue;
            auto join = [&](size_t j) { if (in[j] & kX18) parent[find(j)] = find(k); };
            if (k + 1 < first[b + 1]) join(k + 1);
            else
                for (size_t s : succs[b])
                    if (first[s] < first[s + 1]) join(first[s]);
        }

    std::unordered_set<size_t> entries;
    if (const long e = block_of(f.entry); e >= 0) entries.insert(first[static_cast<size_t>(e)]);
    for (uint64_t va : extra_entries)
        for (size_t b = 0; b < nb; ++b)
            if (va >= f.blocks[b].start && va < f.blocks[b].end) entries.insert(first[b] + (va - f.blocks[b].start) / 4);

    std::vector<long> web_of(code.size(), -1);
    std::vector<X18Web> webs;
    std::vector<uint32_t> forbid;
    for (size_t b = 0; b < nb; ++b)
        for (size_t k = first[b]; k < first[b + 1]; ++k) {
            if (!node(k)) continue;
            const size_t r = find(k);
            if (web_of[r] < 0) {
                web_of[r] = static_cast<long>(webs.size());
                webs.emplace_back();
                forbid.push_back(0);
            }
            const size_t wi = static_cast<size_t>(web_of[r]);
            X18Web& w = webs[wi];
            forbid[wi] |= in[k] | out[k] | code[k].use.reads | code[k].use.writes;
            if (is_call(code[k].inst.op) && (out[k] & kX18)) forbid[wi] |= kClobber;  // the callee clobbers these
            w.points.push_back(k);
            if ((code[k].use.reads | code[k].use.writes) & kX18) w.sites.push_back(k);
            if (entries.count(k) && (in[k] & kX18)) w.open = true;
            if (k + 1 == first[b + 1] && exit_live[b] == ~0u && (out[k] & kX18)) w.open = true;
        }
    for (size_t wi = 0; wi < webs.size(); ++wi) webs[wi].free = ~forbid[wi] & ~kGprSp & ~kX18;
    if (live_in) *live_in = std::move(in);
    if (live_out) *live_out = std::move(out);
    return webs;
}

std::vector<NativeRewrite> plan_native_rewrite(const LinkedImage& image, const AnalysisResult& analysis,
                                               const NativeRewriteOptions& opt) {
    std::vector<NativeRewrite> out(image.modules.size());
    for (size_t m = 0; m < image.modules.size(); ++m) out[m].module = image.modules[m].name;
    auto module_of = [&](uint64_t va) -> int {
        for (size_t m = 0; m < image.modules.size(); ++m) {
            const Module& mod = image.modules[m];
            if (va >= mod.base && va - mod.base < mod.image.size()) return static_cast<int>(m);
        }
        return -1;
    };
    auto decode_at = [&](uint64_t va) {
        NativeInst d;
        d.inst = a64::decode(image.read_u32(va), va);
        d.use = gpr_use(d.inst);
        return d;
    };

    // Instructions that belong to more than one function, function entries, and
    // targets that other functions jump or call to inside a function body.
    std::unordered_map<uint64_t, uint32_t> owners;
    std::unordered_set<uint64_t> fn_entries;
    for (const Function& f : analysis.functions) {
        fn_entries.insert(f.entry);
        for (const BasicBlock& b : f.blocks)
            for (uint64_t pc = b.start; pc < b.end; pc += 4) ++owners[pc];
    }
    std::unordered_set<uint64_t> foreign_targets;
    for (const Function& f : analysis.functions) {
        auto inside = [&](uint64_t va) {
            return std::any_of(f.blocks.begin(), f.blocks.end(),
                               [&](const BasicBlock& b) { return va >= b.start && va < b.end; });
        };
        for (const BasicBlock& b : f.blocks) {
            for (uint64_t s : b.successors)
                if (!inside(s) && !fn_entries.count(s)) foreign_targets.insert(s);
            for (uint64_t pc = b.start; pc < b.end; pc += 4) {
                const Inst i = a64::decode(image.read_u32(pc), pc);
                if (i.op == Op::Bl && !fn_entries.count(i.target)) foreign_targets.insert(i.target);
            }
        }
    }

    for (const Function& f : analysis.functions) {
        const int mi = module_of(f.entry);
        if (mi < 0) continue;
        NativeRewrite& r = out[static_cast<size_t>(mi)];
        std::vector<NativeInst> code;
        for (const BasicBlock& b : f.blocks)
            for (uint64_t pc = b.start; pc < b.end; pc += 4) code.push_back(decode_at(pc));
        bool fn_x18 = false;
        std::vector<size_t> stub_points;  // SVC / system register / SP sites of this function
        for (size_t k = 0; k < code.size(); ++k) {
            const NativeInst& d = code[k];
            if ((d.use.reads | d.use.writes) & kX18) fn_x18 = true;
            if (d.inst.op != Op::Invalid && native_stub_site(d.inst, d.use)) {
                r.other_stub_sites.push_back(d.inst.pc);
                stub_points.push_back(k);
            }
        }
        const std::vector<char> excl = exclusive_windows(code);
        std::vector<char> gate(code.size(), 0);
        // Gate sites; a site naming x18 waits for the webs (it may be renamed).
        std::vector<size_t> gate_x18_points;
        for (size_t k : stub_points) {
            const Inst& i = code[k].inst;
            if (i.op != Op::Svc && i.op != Op::Mrs && i.op != Op::Msr) {
                ++r.sp_sites;
                continue;
            }
            if (excl[k]) {
                ++r.gate_exclusive;
                continue;
            }
            gate[k] = 1;
            if ((code[k].use.reads | code[k].use.writes) & kX18) gate_x18_points.push_back(k);
            else r.gate_sites.push_back({i.pc, i.raw});
        }
        if (!fn_x18) continue;
        ++r.functions_with_x18;

        uint32_t candidates = opt.candidates;
        if (opt.callee_saved_without_lsda) {
            const Module& mod = image.modules[static_cast<size_t>(mi)];
            const uint64_t off = f.entry - mod.base;
            if (std::binary_search(mod.fde_starts.begin(), mod.fde_starts.end(), off) &&
                !std::binary_search(mod.lsda_fde_starts.begin(), mod.lsda_fde_starts.end(), off)) {
                candidates |= kRenameCalleeSaved;
                ++r.functions_callee_saved;
            }
        }

        std::vector<uint64_t> extra;
        for (const BasicBlock& b : f.blocks)
            for (uint64_t pc = b.start; pc < b.end; pc += 4)
                if (pc != f.entry && foreign_targets.count(pc)) extra.push_back(pc);

        std::vector<uint32_t> live_in, live_out;
        const std::vector<X18Web> webs = x18_webs(f, code, extra, &live_in, &live_out);
        const FlowIndex flow(f);
        std::unordered_map<uint64_t, uint32_t> renamed;  // this function's renamed sites
        for (const X18Web& w : webs) {
            ++r.webs;
            auto leave = [&](uint64_t& counter) {
                ++counter;
                for (size_t k : w.sites) {
                    r.x18_stub_sites.push_back(code[k].inst.pc);
                    r.x18_unpatched.push_back(code[k].inst.pc);
                }
            };
            // Renaming failed: try a stack slot (part 2).
            auto spill = [&](uint64_t& counter) {
                ++counter;
                for (size_t k : w.sites) r.x18_stub_sites.push_back(code[k].inst.pc);
                SpillPlan sp = plan_spill(w, code, live_in, live_out, flow, excl, gate, candidates);
                if (!sp.reason) {
                    ++r.webs_spilled;
                    r.sites_spilled += sp.sites.size();
                    r.x18_spills.insert(r.x18_spills.end(), sp.sites.begin(), sp.sites.end());
                    return;
                }
                ++(r.*sp.reason);
                for (size_t k : w.sites) r.x18_unpatched.push_back(code[k].inst.pc);
            };
            if (w.open) { leave(r.webs_open); continue; }
            const bool shared =
                std::any_of(w.points.begin(), w.points.end(), [&](size_t k) { return owners[code[k].inst.pc] > 1; });
            const uint32_t cand = opt.rename ? w.free & candidates : 0;
            if (!cand) {
                if (shared) leave(r.webs_no_register);
                else spill(r.webs_no_register);
                continue;
            }
            if (shared) { leave(r.webs_shared); continue; }
            const unsigned reg = static_cast<unsigned>(std::countr_zero(cand));
            std::vector<std::pair<uint64_t, uint32_t>> words;
            bool ok = true;
            for (size_t k : w.sites) {
                const auto nw = rename_gpr(code[k].inst.raw, code[k].inst.pc, 18, reg);
                if (!nw) { ok = false; break; }
                words.push_back({code[k].inst.pc, *nw});
            }
            if (!ok) { spill(r.webs_encoding); continue; }
            ++r.webs_renamed;
            r.sites_renamed += words.size();
            for (const auto& [va, word] : words) renamed[va] = word;
            r.words.insert(r.words.end(), words.begin(), words.end());
        }
        for (size_t k : gate_x18_points) {
            const auto it = renamed.find(code[k].inst.pc);
            if (it != renamed.end()) r.gate_sites.push_back({code[k].inst.pc, it->second});
            else ++r.gate_x18;
        }
    }
    for (NativeRewrite& r : out) {
        std::sort(r.words.begin(), r.words.end());
        std::sort(r.x18_stub_sites.begin(), r.x18_stub_sites.end());
        std::sort(r.other_stub_sites.begin(), r.other_stub_sites.end());
        r.other_stub_sites.erase(std::unique(r.other_stub_sites.begin(), r.other_stub_sites.end()),
                                 r.other_stub_sites.end());
        std::sort(r.x18_unpatched.begin(), r.x18_unpatched.end());
        auto by_va = [](const auto& a, const auto& b) { return a.va < b.va; };
        auto same_va = [](const auto& a, const auto& b) { return a.va == b.va; };
        std::sort(r.gate_sites.begin(), r.gate_sites.end(), by_va);
        r.gate_sites.erase(std::unique(r.gate_sites.begin(), r.gate_sites.end(), same_va), r.gate_sites.end());
        std::sort(r.x18_spills.begin(), r.x18_spills.end(), by_va);
    }
    return out;
}

}  // namespace lgp::recomp
