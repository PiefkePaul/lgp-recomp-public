// Function discovery and CFG construction (P2.2). Algorithm: docs/knowledge/function-discovery.md.
#include "lgp/recomp/cfg.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <iterator>

#include "jump_table.hpp"
#include "lgp/recomp/a64_cf.hpp"

namespace lgp::recomp {

const char* function_source_name(FunctionSource s) {
    switch (s) {
        case FunctionSource::Fde: return "fde";
        case FunctionSource::Symbol: return "symbol";
        case FunctionSource::Entry: return "entry";
        case FunctionSource::InitArray: return "init_array";
        case FunctionSource::Config: return "config";
        case FunctionSource::CallTarget: return "call_target";
        case FunctionSource::TailTarget: return "tail_target";
        case FunctionSource::DataPointer: return "data_pointer";
        case FunctionSource::CodePointer: return "code_pointer";
        case FunctionSource::GapScan: return "gap_scan";
    }
    return "?";
}
const char* terminator_name(Terminator t) {
    switch (t) {
        case Terminator::Fallthrough: return "fallthrough";
        case Terminator::Branch: return "branch";
        case Terminator::CondBranch: return "cond_branch";
        case Terminator::Return: return "return";
        case Terminator::Trap: return "trap";
        case Terminator::IndirectJump: return "indirect_jump";
        case Terminator::JumpTable: return "jump_table";
        case Terminator::NoReturnCall: return "noreturn_call";
        case Terminator::Unknown: return "unknown";
    }
    return "?";
}
const char* confidence_name(Confidence c) {
    switch (c) {
        case Confidence::High: return "high";
        case Confidence::Medium: return "medium";
        case Confidence::Low: return "low";
    }
    return "?";
}
const char* diagnostic_kind_name(Diagnostic::Kind k) {
    switch (k) {
        case Diagnostic::Kind::UnresolvedIndirectJump: return "unresolved_indirect_jump";
        case Diagnostic::Kind::JumpTableUnbounded: return "jump_table_unbounded";
        case Diagnostic::Kind::CrossFunctionJump: return "cross_function_jump";
        case Diagnostic::Kind::CallIntoFunctionBody: return "call_into_function_body";
        case Diagnostic::Kind::FallsIntoNextFunction: return "falls_into_next_function";
        case Diagnostic::Kind::UnreachableCode: return "unreachable_code";
        case Diagnostic::Kind::UndecodableInstruction: return "undecodable_instruction";
        case Diagnostic::Kind::SeedDemoted: return "seed_demoted";
        case Diagnostic::Kind::JumpTableOverrideUnused: return "jump_table_override_unused";
        case Diagnostic::Kind::DataPointerIgnored: return "data_pointer_ignored";
        case Diagnostic::Kind::NoReturnInferred: return "noreturn_inferred";
        case Diagnostic::Kind::JumpTableOverrideRejected: return "jump_table_override_rejected";
        case Diagnostic::Kind::JumpTableOverrideOutsideFunction: return "jump_table_override_outside_function";
    }
    return "?";
}
bool is_config_diagnostic(Diagnostic::Kind k) {
    return k == Diagnostic::Kind::JumpTableOverrideUnused || k == Diagnostic::Kind::JumpTableOverrideRejected ||
           k == Diagnostic::Kind::JumpTableOverrideOutsideFunction;
}

uint32_t Function::instruction_count() const {
    uint64_t n = 0;
    for (const auto& b : blocks) n += (b.end - b.start) / 4;
    return static_cast<uint32_t>(n);
}

const Function* AnalysisResult::function_at(uint64_t entry) const {
    auto it = std::lower_bound(functions.begin(), functions.end(), entry,
                               [](const Function& f, uint64_t v) { return f.entry < v; });
    return (it != functions.end() && it->entry == entry) ? &*it : nullptr;
}
const Function* AnalysisResult::function_containing(uint64_t va) const {
    auto it = std::upper_bound(functions.begin(), functions.end(), va,
                               [](uint64_t v, const Function& f) { return v < f.entry; });
    if (it == functions.begin()) return nullptr;
    --it;
    return (va >= it->entry && va < it->end) ? &*it : nullptr;
}
bool AnalysisResult::is_entry(uint64_t va) const {
    return std::binary_search(function_entries.begin(), function_entries.end(), va);
}

namespace {

int priority(FunctionSource s) { return static_cast<int>(s); }  // enum order == priority (0 strongest)
bool weak_source(FunctionSource s) {
    return s == FunctionSource::DataPointer || s == FunctionSource::CodePointer || s == FunctionSource::GapScan;
}

std::string hex(uint64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(v));
    return buf;
}

struct CodeRange {
    uint64_t text_start, text_end;
    uint64_t header_end;      // (text_start, header_end) holds header words, not code; text_start itself is the entry
    uint64_t start() const { return text_start; }
    uint64_t end() const { return text_end; }
};

class Analyzer {
public:
    Analyzer(const LinkedImage& img, const AnalysisConfig& cfg) : img_(img), cfg_(cfg) {}

    AnalysisResult run() {
        collect_ranges();
        collect_seeds();
        collect_noreturn();
        explore_all();
        // noreturn propagation changes block termination: iterate until stable (bounded).
        for (int iter = 0; iter < 8; ++iter) {
            bool changed = propagate_noreturn();
            if (noreturn_inferred_) { noreturn_inferred_ = false; changed = true; }
            if (refute_noreturn()) changed = true;
            if (!changed) break;
            explore_all(true);
        }
        if (cfg_.scan_gaps) {
            gap_scan();
            explore_all();
        }
        return finish();
    }

private:
    const LinkedImage& img_;
    const AnalysisConfig& cfg_;
    std::vector<CodeRange> ranges_;
    std::map<uint64_t, FunctionSource> seeds_;
    std::map<uint64_t, uint64_t> fde_range_;        // start -> end (VA)
    std::set<uint64_t> noreturn_;                    // callee entries
    std::set<uint64_t> noreturn_fixed_;              // from the config (names, addresses): never refuted
    std::set<uint64_t> can_return_;                  // own code shows a return path: never guessed noreturn
    bool noreturn_inferred_ = false;                 // explore() added a noreturn callee: re-explore
    std::map<uint64_t, Function> functions_;         // explored, by entry
    std::set<uint64_t> todo_;                        // seeds not yet explored
    std::vector<Diagnostic> diags_;                               // events (demotion, inference, splits)
    std::map<uint64_t, std::vector<Diagnostic>> fn_diags_;        // per function, reset when it is re-explored
    uint64_t cur_fn_ = 0;
    bool in_explore_ = false;
    std::set<uint64_t> overrides_used_;

    // Diagnostics that describe the state of a function are stored per function and dropped when
    // the function is explored again (noreturn propagation, gap scan), so every explore pass does
    // not add another copy. Diagnostics that record an event stay global.
    void diag(Diagnostic::Kind k, uint64_t pc, std::string text) {
        bool event = k == Diagnostic::Kind::SeedDemoted || k == Diagnostic::Kind::NoReturnInferred ||
                     k == Diagnostic::Kind::JumpTableOverrideUnused || k == Diagnostic::Kind::DataPointerIgnored;
        if (in_explore_ && !event) fn_diags_[cur_fn_].push_back({k, pc, std::move(text)});
        else diags_.push_back({k, pc, std::move(text)});
    }

    const CodeRange* range_of(uint64_t va) const {
        for (const auto& r : ranges_)
            if (va >= r.text_start && va < r.text_end) return &r;
        return nullptr;
    }
    bool is_code(uint64_t va) const {
        const CodeRange* r = range_of(va);
        if (!r || (va & 3) != 0) return false;
        if (va > r->text_start && va < r->header_end) return false;
        return true;
    }

    void collect_ranges() {
        for (const auto& m : img_.modules) {
            const Segment& t = m.segment(SegmentKind::Text);
            CodeRange r;
            r.text_start = m.base + t.offset;
            r.text_end = r.text_start + t.size;
            r.header_end = r.text_start + m.header_code_exclusion;
            ranges_.push_back(r);
            for (auto [s, e] : m.fde_ranges) fde_range_[m.base + s] = m.base + e;
        }
    }

    void add_seed(uint64_t va, FunctionSource src) {
        if (!is_code(va)) return;
        auto it = seeds_.find(va);
        if (it == seeds_.end()) {
            seeds_[va] = src;
            todo_.insert(va);
        } else if (priority(src) < priority(it->second)) {
            it->second = src;
            auto f = functions_.find(va);
            if (f != functions_.end()) f->second.source = src;
        }
    }

    void collect_seeds() {
        for (uint32_t mi = 0; mi < img_.modules.size(); ++mi) {
            const Module& m = img_.modules[mi];
            // Entry: NRO/NSO .text start holds `b start`; the header words behind it are not code.
            add_seed(m.base + m.entry_offset, FunctionSource::Entry);
            for (uint64_t s : m.fde_starts) add_seed(m.base + s, FunctionSource::Fde);
            for (const auto& s : m.dynsym)
                if (s.defined() && s.type == kSttFunc) add_seed(m.base + s.value, FunctionSource::Symbol);
            if (m.init) add_seed(m.base + m.init, FunctionSource::InitArray);
            if (m.fini) add_seed(m.base + m.fini, FunctionSource::InitArray);
            for (uint64_t v : m.init_array) add_seed(m.base + v, FunctionSource::InitArray);
            for (uint64_t v : m.fini_array) add_seed(m.base + v, FunctionSource::InitArray);
        }
        for (uint64_t va : cfg_.extra_functions) {
            if (is_code(va)) add_seed(va, FunctionSource::Config);
            else diag(Diagnostic::Kind::DataPointerIgnored, va, "config function address is not code");
        }
        if (cfg_.seed_data_pointers) {
            for (const auto& m : img_.modules) {
                for (const auto& r : m.relocations) {
                    if (!r.applied || (r.type != kRAarch64Relative && r.type != kRAarch64Abs64)) continue;
                    const Segment* slot_seg = m.segment_at_offset(r.offset);
                    if (!slot_seg || slot_seg->kind == SegmentKind::Text) continue;
                    uint64_t v = m.read_u64(r.offset);
                    if (!is_code(v) || seeds_.count(v)) continue;
                    // A pointer strictly inside an FDE range is a label, not a function.
                    auto it = fde_range_.upper_bound(v);
                    if (it != fde_range_.begin()) {
                        --it;
                        if (v > it->first && v < it->second) {
                            diag(Diagnostic::Kind::DataPointerIgnored, v, "code pointer inside FDE range of " + hex(it->first));
                            continue;
                        }
                    }
                    add_seed(v, FunctionSource::DataPointer);
                }
            }
            seed_code_pointers();
        }
    }

    // [from, to) is non-empty and holds only padding words.
    bool padding_between(uint64_t from, uint64_t to) const {
        if (to <= from) return false;
        for (uint64_t p = from; p < to; p += 4)
            if (!is_padding(img_.read_u32(p))) return false;
        return true;
    }

    // A pointer strictly inside an FDE range is a label, not a function.
    bool inside_fde(uint64_t v) const {
        auto it = fde_range_.upper_bound(v);
        if (it == fde_range_.begin()) return false;
        --it;
        return v > it->first && v < it->second;
    }

    // Function addresses built in code with `adrp xd, page` + `add xd, xd, #lo12` (callbacks
    // passed to thread creation or atexit; libnx: _EntryWrap, newlib: __libc_fini_array). Needed
    // where .eh_frame_hdr is missing; weak like a data pointer, so a wrong guess gets demoted.
    void seed_code_pointers() {
        for (const auto& r : ranges_) {
            for (uint64_t pc = r.text_start; pc + 4 < r.text_end; pc += 4) {
                if (!is_code(pc)) continue;
                const uint32_t adrp = img_.read_u32(pc);
                if ((adrp & 0x9F000000u) != 0x90000000u) continue;
                const uint32_t rd = adrp & 31;
                const uint64_t imm = (uint64_t{(adrp >> 5) & 0x7FFFF} << 2) | ((adrp >> 29) & 3);
                const int64_t off = static_cast<int64_t>(imm << 43) >> 31;  // sign-extend 21 bits, << 12
                const uint64_t page = (pc & ~uint64_t{0xFFF}) + static_cast<uint64_t>(off);
                const uint32_t add = img_.read_u32(pc + 4);
                // ADD (immediate, 64-bit, no shift) with rd == rn == the ADRP register.
                if ((add & 0xFFC00000u) != 0x91000000u || (add & 31) != rd || ((add >> 5) & 31) != rd) continue;
                const uint64_t v = page + ((add >> 10) & 0xFFF);
                if (!is_code(v) || seeds_.count(v) || inside_fde(v)) continue;
                add_seed(v, FunctionSource::CodePointer);
            }
        }
    }

    void collect_noreturn() {
        for (const auto& name : cfg_.noreturn_symbols) {
            if (auto va = img_.symbol_va(name)) {
                noreturn_.insert(*va);
                noreturn_fixed_.insert(*va);
            }
            // PLT-less imports resolve to the provider's entry, which is covered by symbol_va.
        }
        for (uint64_t va : cfg_.noreturn_addresses) {
            noreturn_.insert(va);
            noreturn_fixed_.insert(va);
        }
        // A BL as the very last instruction of an FDE range never returns. Only a guess: a cleanup
        // path can end in a call whose continuation the compiler dropped as unreachable (sdk: EH
        // landing pads ending in `bl` to a mutex-unlock thunk), so refute_noreturn() takes it back
        // when the callee's code returns.
        for (auto [s, e] : fde_range_) {
            if (e < s + 4 || !is_code(e - 4)) continue;
            CfInfo cf = classify(img_.read_u32(e - 4), e - 4);
            if (cf.kind == CfKind::Call && is_code(cf.target)) noreturn_.insert(cf.target);
        }
    }

    bool propagate_noreturn() {
        bool changed = false;
        for (auto& [entry, fn] : functions_) {
            if (fn.noreturn || fn.blocks.empty() || fn.has_unresolved_indirect || can_return_.count(entry)) continue;
            bool all_dead_ends = true;
            for (const auto& b : fn.blocks) {
                bool dead = false;
                switch (b.term) {
                    case Terminator::Trap: case Terminator::NoReturnCall: dead = true; break;
                    case Terminator::Branch:
                        if (b.successors.empty()) {
                            for (const auto& c : b.calls) if (c.tail && noreturn_.count(c.target)) dead = true;
                        } else dead = true;  // internal edge, not an exit
                        break;
                    case Terminator::Fallthrough: case Terminator::CondBranch: case Terminator::JumpTable: dead = true; break;
                    default: dead = false; break;
                }
                if (!dead) { all_dead_ends = false; break; }
            }
            // Only count functions that actually end somewhere (not just an empty loop).
            if (all_dead_ends && noreturn_.insert(entry).second) { fn.noreturn = true; changed = true; }
        }
        return changed;
    }

    // A function whose own code can return is not noreturn, whatever a heuristic guessed (FDE tail,
    // fall-into, propagation through a tail call). Treating a returning callee as noreturn cuts the
    // code after every call to it (the lifted caller then jumps to the return address through the
    // dispatch table and stops); the opposite mistake only lifts some unreachable code.
    bool refute_noreturn() {
        for (bool grew = true; grew;) {
            grew = false;
            for (const auto& [entry, fn] : functions_) {
                if (can_return_.count(entry) || noreturn_fixed_.count(entry) || !may_return(fn)) continue;
                can_return_.insert(entry);
                grew = true;
            }
        }
        bool changed = false;
        for (uint64_t entry : can_return_) {
            if (!noreturn_.erase(entry)) continue;
            changed = true;
            if (auto it = functions_.find(entry); it != functions_.end()) it->second.noreturn = false;
        }
        return changed;
    }

    bool may_return(const Function& fn) const {
        for (const auto& b : fn.blocks) {
            if (b.term == Terminator::Return || b.term == Terminator::IndirectJump) return true;
            if (b.term == Terminator::Branch && b.successors.empty())
                for (const auto& c : b.calls)
                    if (c.tail && !c.indirect && can_return_.count(c.target)) return true;
        }
        return false;
    }

    uint64_t bound_for(uint64_t entry) const {
        const CodeRange* r = range_of(entry);
        uint64_t bound = r ? r->end() : entry + 4;
        if (r && entry == r->text_start && r->header_end > entry) bound = entry + 4;  // entry stub before the header
        auto it = seeds_.upper_bound(entry);
        if (it != seeds_.end() && it->first < bound) bound = it->first;
        auto f = fde_range_.find(entry);
        if (f != fde_range_.end() && f->second > entry && f->second < bound) bound = f->second;
        return bound;
    }

    void explore_all(bool re_explore = false) {
        if (re_explore)
            for (auto& [e, f] : functions_) todo_.insert(e);
        while (!todo_.empty()) {
            uint64_t entry = *todo_.begin();
            todo_.erase(todo_.begin());
            if (!seeds_.count(entry)) continue;
            functions_.erase(entry);
            for (int attempt = 0; attempt < 64; ++attempt)
                if (explore(entry)) break;
        }
    }

    // Demote a weak seed that turned out to be inside the current function; returns true if demoted.
    bool demote(uint64_t seed_va, uint64_t from_pc, const char* why) {
        auto it = seeds_.find(seed_va);
        if (it == seeds_.end() || !weak_source(it->second)) return false;
        diag(Diagnostic::Kind::SeedDemoted, seed_va,
             std::string(function_source_name(it->second)) + " seed removed: " + why + " from " + hex(from_pc));
        seeds_.erase(it);
        todo_.erase(seed_va);
        functions_.erase(seed_va);
        fn_diags_.erase(seed_va);
        return true;
    }

    // External control-flow target (B / B.cond / jump table) leaving [entry, bound).
    // `conditional` = B.cond / jump-table edge: compilers never tail-call that way, so a weak seed
    // at such a target is a wrong seed and gets demoted.
    void external_target(Function& fn, uint64_t pc, uint64_t target, bool& restart, bool conditional = false) {
        if (!is_code(target)) {
            diag(Diagnostic::Kind::CrossFunctionJump, pc, "jump to non-code address " + hex(target));
            return;
        }
        if (seeds_.count(target)) {
            if (conditional && demote(target, pc, "conditional jump targets it")) restart = true;
            return;  // otherwise: tail call to a known function
        }
        // Inside an FDE range (not at its start): shared tail / cross jump.
        auto fit = fde_range_.upper_bound(target);
        if (fit != fde_range_.begin()) {
            --fit;
            if (target > fit->first && target < fit->second) {
                diag(Diagnostic::Kind::CrossFunctionJump, pc, "jump into body of " + hex(fit->first) + " at " + hex(target));
                return;
            }
        }
        // Inside an already explored function?
        auto f = functions_.upper_bound(target);
        if (f != functions_.begin()) {
            --f;
            if (target > f->first && target < f->second.end) {
                if (demote(f->first, pc, "jump target lies inside it")) { restart = true; return; }
                diag(Diagnostic::Kind::CrossFunctionJump, pc, "jump into body of " + hex(f->first) + " at " + hex(target));
                return;
            }
        }
        if (target >= fn.entry && target < fn.end) return;  // handled by caller
        add_seed(target, FunctionSource::TailTarget);
    }

    void call_target(uint64_t pc, uint64_t target) {
        if (!is_code(target)) return;
        if (seeds_.count(target)) return;
        auto fit = fde_range_.upper_bound(target);
        if (fit != fde_range_.begin()) {
            --fit;
            if (target > fit->first && target < fit->second) {
                diag(Diagnostic::Kind::CallIntoFunctionBody, pc, "call into body of " + hex(fit->first) + " at " + hex(target));
                return;
            }
        }
        auto f = functions_.upper_bound(target);
        if (f != functions_.begin()) {
            --f;
            if (target > f->first && target < f->second.end) {
                // The containing function has no FDE range: its end was guessed, so split it.
                diag(Diagnostic::Kind::CallIntoFunctionBody, pc,
                     "call into guessed body of " + hex(f->first) + " at " + hex(target) + "; splitting");
                todo_.insert(f->first);
            }
        }
        add_seed(target, FunctionSource::CallTarget);
    }

    // Returns false if the function must be re-explored (a seed was demoted).
    bool explore(uint64_t entry) {
        struct Scope {
            Analyzer& a;
            Scope(Analyzer& an, uint64_t e) : a(an) { a.fn_diags_.erase(e); a.cur_fn_ = e; a.in_explore_ = true; }
            ~Scope() { a.in_explore_ = false; }
        } scope(*this, entry);
        Function fn;
        fn.entry = entry;
        fn.source = seeds_.at(entry);
        uint64_t bound = bound_for(entry);
        // Entry stub `b start` in front of the NRO/NSO header: when the branch lands right behind the
        // header on a word nobody else claims, stub and code are one function (libnx `_start: b 1f`,
        // clang crt0). The header words in between are skipped, never decoded.
        if (const CodeRange* r = range_of(entry); r && entry == r->text_start && r->header_end > entry) {
            CfInfo cf = classify(img_.read_u32(entry), entry);
            if (cf.kind == CfKind::Branch && cf.target == r->header_end && !seeds_.count(r->header_end)) {
                bound = r->end();
                auto it = seeds_.upper_bound(r->header_end);
                if (it != seeds_.end() && it->first < bound) bound = it->first;
            }
        }
        auto fr = fde_range_.find(entry);
        if (fr != fde_range_.end()) { fn.has_fde_range = true; fn.fde_end = fr->second; }
        fn.end = entry + 4;

        std::map<uint64_t, BasicBlock> blocks;
        std::vector<uint64_t> work{entry};
        std::set<uint64_t> indirect_pending;
        bool restart = false;

        auto split_at = [&](uint64_t at) -> bool {
            auto it = blocks.upper_bound(at);
            if (it == blocks.begin()) return false;
            --it;
            BasicBlock& b = it->second;
            if (at <= b.start || at >= b.end) return false;
            BasicBlock tail = b;
            tail.start = at;
            b.end = at;
            b.term = Terminator::Fallthrough;
            b.successors = {at};
            b.jump_table.reset();
            b.calls.erase(std::remove_if(b.calls.begin(), b.calls.end(), [&](const Call& c) { return c.pc >= at; }), b.calls.end());
            tail.calls.erase(std::remove_if(tail.calls.begin(), tail.calls.end(), [&](const Call& c) { return c.pc < at; }), tail.calls.end());
            blocks[at] = std::move(tail);
            return true;
        };

        auto in_fn = [&](uint64_t va) { return va >= entry && va < bound; };

        while (!restart) {
            if (work.empty()) {
                // Resolve pending indirect jumps once the worklist drains (they may add blocks).
                // Checked here, not after each new block: the last work item can be a block
                // that already exists, and the pending BRs must not be dropped then.
                if (indirect_pending.empty()) break;
                for (auto it = indirect_pending.begin(); it != indirect_pending.end() && !restart;) {
                    uint64_t br_pc = *it;
                    it = indirect_pending.erase(it);
                    resolve_br(fn, blocks, br_pc, bound, work, restart);
                }
                continue;
            }
            uint64_t start = work.back();
            work.pop_back();
            if (blocks.count(start) || split_at(start)) continue;
            BasicBlock b;
            b.start = start;
            uint64_t pc = start;
            bool done = false;
            while (!done) {
                if (pc >= bound) {
                    // Ran into the function bound without a terminator.
                    auto s = seeds_.find(bound);
                    if (s != seeds_.end() && weak_source(s->second) && demote(bound, pc - 4, "code falls through into it")) {
                        restart = true;
                        break;
                    }
                    // Compiled code never runs off the end of a function into the next one, so a
                    // BL right before a strong neighbour (FDE/symbol/call target) is a call that
                    // does not return (exit, abort, assertion failures without a known name).
                    if (s != seeds_.end() && !weak_source(s->second)) {
                        CfInfo last = classify(img_.read_u32(pc - 4), pc - 4);
                        if (last.kind == CfKind::Call && is_code(last.target) && last.target != entry &&
                            !can_return_.count(last.target)) {
                            if (noreturn_.insert(last.target).second) {
                                noreturn_inferred_ = true;
                                diag(Diagnostic::Kind::NoReturnInferred, pc - 4,
                                     "callee " + hex(last.target) + " marked noreturn: code would fall into " + hex(bound));
                            }
                            b.term = Terminator::NoReturnCall;
                            done = true;
                            break;
                        }
                    }
                    b.term = Terminator::Branch;
                    b.calls.push_back(Call{pc - 4, bound, false, true, 0});
                    diag(Diagnostic::Kind::FallsIntoNextFunction, pc - 4, "falls through into " + hex(bound));
                    if (s == seeds_.end() && is_code(bound) && !is_padding(img_.read_u32(bound)))
                        add_seed(bound, FunctionSource::TailTarget);
                    done = true;
                    break;
                }
                if (pc != start && blocks.count(pc)) {
                    b.term = Terminator::Fallthrough;
                    b.successors = {pc};
                    done = true;
                    break;
                }
                if (!is_code(pc)) {
                    // Header words behind the entry stub (or the end of the segment): not code.
                    b.term = Terminator::Unknown;
                    diag(Diagnostic::Kind::UndecodableInstruction, pc, "control flow runs into non-code bytes");
                    done = true;
                    break;
                }
                uint32_t insn = img_.read_u32(pc);
                CfInfo cf = classify(insn, pc);
                pc += 4;
                switch (cf.kind) {
                    case CfKind::None:
                    case CfKind::Svc:
                        break;
                    case CfKind::Trap:
                        b.term = Terminator::Trap;
                        done = true;
                        break;
                    case CfKind::Return:
                        b.term = Terminator::Return;
                        done = true;
                        break;
                    case CfKind::Branch:
                        b.term = Terminator::Branch;
                        // Without an FDE the bound is only the next seed. A forward B whose
                        // target lies behind alignment padding is a tail call to the next
                        // function (newlib's __call_exitprocs stub: `b __libc_fini_array`).
                        if (!fn.has_fde_range && in_fn(cf.target) && cf.target > pc && (cf.target & 7) == 0 &&
                            !seeds_.count(cf.target) && padding_between(pc, cf.target)) {
                            add_seed(cf.target, FunctionSource::TailTarget);
                            restart = true;
                            done = true;
                            break;
                        }
                        if (in_fn(cf.target)) {
                            b.successors = {cf.target};
                            work.push_back(cf.target);
                        } else {
                            b.calls.push_back(Call{pc - 4, cf.target, false, true, 0});
                            external_target(fn, pc - 4, cf.target, restart);
                        }
                        done = true;
                        break;
                    case CfKind::CondBranch:
                        b.term = Terminator::CondBranch;
                        b.successors = {pc};
                        work.push_back(pc);
                        if (in_fn(cf.target)) {
                            if (cf.target != pc) b.successors.push_back(cf.target);
                            work.push_back(cf.target);
                        } else {
                            b.calls.push_back(Call{pc - 4, cf.target, false, true, 0});
                            external_target(fn, pc - 4, cf.target, restart, true);
                        }
                        done = true;
                        break;
                    case CfKind::Call:
                        b.calls.push_back(Call{pc - 4, cf.target, false, false, 0});
                        call_target(pc - 4, cf.target);
                        if (noreturn_.count(cf.target)) {
                            b.term = Terminator::NoReturnCall;
                            done = true;
                        }
                        break;
                    case CfKind::IndirectCall:
                        b.calls.push_back(Call{pc - 4, 0, true, false, 0});
                        break;
                    case CfKind::IndirectBranch:
                        b.term = Terminator::IndirectJump;
                        indirect_pending.insert(pc - 4);
                        done = true;
                        break;
                    case CfKind::Unknown:
                        b.term = Terminator::Unknown;
                        diag(Diagnostic::Kind::UndecodableInstruction, pc - 4, "undecodable instruction");
                        done = true;
                        break;
                }
            }
            if (restart) break;
            b.end = pc;
            blocks[start] = std::move(b);
        }
        if (restart) return false;

        // Function end and noreturn.
        for (auto& [s, blk] : blocks) fn.end = std::max(fn.end, blk.end);
        fn.blocks.reserve(blocks.size());
        for (auto& [s, blk] : blocks) {
            if (blk.term == Terminator::IndirectJump) fn.has_unresolved_indirect = true;
            fn.blocks.push_back(std::move(blk));
        }
        if (fn.has_fde_range && fn.fde_end > fn.end) {
            // Bytes between the last reachable block and the FDE end: padding or unreachable code.
            bool only_padding = true;
            for (uint64_t pc = fn.end; pc + 4 <= fn.fde_end; pc += 4)
                if (!is_padding(img_.read_u32(pc))) { only_padding = false; break; }
            if (!only_padding)
                diag(Diagnostic::Kind::UnreachableCode, fn.end, "unreachable code before FDE end " + hex(fn.fde_end));
            fn.end = fn.fde_end;
        }
        if (noreturn_.count(entry)) fn.noreturn = true;
        const Module* owner = nullptr;
        if (const Symbol* sym = img_.symbol_at(entry, &owner)) fn.name = sym->name;
        else {
            char buf[32];
            std::snprintf(buf, sizeof buf, "fn_%llx", static_cast<unsigned long long>(entry));
            fn.name = buf;
        }
        functions_[entry] = std::move(fn);
        return true;
    }

    void resolve_br(Function& fn, std::map<uint64_t, BasicBlock>& blocks, uint64_t br_pc, uint64_t bound,
                    std::vector<uint64_t>& work, bool& restart) {
        auto bit = blocks.upper_bound(br_pc);
        if (bit == blocks.begin()) return;
        --bit;
        BasicBlock& b = bit->second;
        // Config override first.
        for (const auto& o : cfg_.jump_table_overrides) {
            if (o.br_pc != br_pc) continue;
            JumpTable t;
            t.br_pc = br_pc; t.table_va = o.table_va; t.entry_size = o.entry_size; t.count = o.count;
            t.base_va = o.entry_size == 8 ? 0 : (o.base_va ? o.base_va : o.table_va);
            t.entries_signed = o.entries_signed; t.shift = o.entry_size == 8 ? 0 : o.shift;
            t.bound_known = true; t.confidence = Confidence::High;
            for (uint32_t i = 0; i < o.count; ++i) {
                uint64_t at = t.table_va + static_cast<uint64_t>(i) * t.entry_size, v = 0;
                try {
                    switch (t.entry_size) {
                        case 1: v = img_.read_u8(at); if (t.entries_signed) v = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int8_t>(v))); break;
                        case 2: v = (img_.read_u32(at & ~uint64_t(3)) >> ((at & 3) * 8)) & 0xFFFF; if (t.entries_signed) v = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int16_t>(v))); break;
                        case 4: v = img_.read_u32(at); if (t.entries_signed) v = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(v))); break;
                        default: v = img_.read_u64(at); break;
                    }
                } catch (const std::out_of_range&) { break; }
                t.targets.push_back(t.entry_size == 8 ? v : t.base_va + (v << t.shift));
            }
            overrides_used_.insert(br_pc);
            // A wrong entry/shift/base sends the targets anywhere (shift=2 on byte offsets: in
            // front of the module). Such an override is not applied; the heuristic decides.
            std::string bad;
            if (t.targets.size() != o.count) bad = "table entry " + std::to_string(t.targets.size()) + " is unmapped";
            for (size_t i = 0; i < t.targets.size() && bad.empty(); ++i)
                if ((t.targets[i] & 3) != 0 || !img_.in_text(t.targets[i]))
                    bad = "entry " + std::to_string(i) + " -> " + hex(t.targets[i]) + " is " +
                          ((t.targets[i] & 3) ? "misaligned" : "outside .text");
            if (!bad.empty()) {
                diag(Diagnostic::Kind::JumpTableOverrideRejected, br_pc,
                     "jumptable override not applied: " + bad + " (check entry=, shift=, base=, signed)");
                break;
            }
            for (size_t i = 0; i < t.targets.size(); ++i)
                if (t.targets[i] < fn.entry || t.targets[i] >= bound) {
                    diag(Diagnostic::Kind::JumpTableOverrideOutsideFunction, br_pc,
                         "jumptable override entry " + std::to_string(i) + " -> " + hex(t.targets[i]) +
                             " leaves the function " + hex(fn.entry) + "-" + hex(bound) + " (tail jump?)");
                    break;
                }
            apply_table(fn, b, std::move(t), bound, work, restart);
            return;
        }
        detail::IndirectResolution r = detail::resolve_indirect(img_, blocks, br_pc, fn.entry, bound, cfg_.max_jump_table_scan);
        switch (r.kind) {
            case detail::IndirectResolution::Kind::Direct:
                b.term = Terminator::Branch;
                if (r.target >= fn.entry && r.target < bound) {
                    b.successors = {r.target};
                    work.push_back(r.target);
                } else {
                    b.calls.push_back(Call{br_pc, r.target, false, true, r.via_slot});
                    external_target(fn, br_pc, r.target, restart);
                }
                break;
            case detail::IndirectResolution::Kind::Table:
                if (!r.table.bound_known)
                    diag(Diagnostic::Kind::JumpTableUnbounded, br_pc,
                         "jump table at " + hex(r.table.table_va) + " without cmp bound; scanned " +
                             std::to_string(r.table.count) + " entries");
                apply_table(fn, b, std::move(r.table), bound, work, restart);
                break;
            case detail::IndirectResolution::Kind::Unresolved:
                diag(Diagnostic::Kind::UnresolvedIndirectJump, br_pc, "BR not resolved: " + r.note);
                break;
        }
    }

    void apply_table(Function& fn, BasicBlock& b, JumpTable t, uint64_t bound, std::vector<uint64_t>& work, bool& restart) {
        b.term = Terminator::JumpTable;
        b.successors.clear();
        std::set<uint64_t> uniq;
        for (uint64_t tgt : t.targets) {
            if (tgt >= fn.entry && tgt < bound) {
                if (uniq.insert(tgt).second) { b.successors.push_back(tgt); work.push_back(tgt); }
            } else {
                external_target(fn, t.br_pc, tgt, restart, true);
                if (!restart) diag(Diagnostic::Kind::CrossFunctionJump, t.br_pc, "jump table target " + hex(tgt) + " outside function");
                if (restart) return;
            }
        }
        b.jump_table = static_cast<uint32_t>(fn.jump_tables.size());
        fn.jump_tables.push_back(std::move(t));
    }

    void gap_scan() {
        for (const auto& r : ranges_) {
            uint64_t pc = r.start();
            while (pc < r.end()) {
                if (pc > r.text_start && pc < r.header_end) { pc = r.header_end; continue; }
                // Inside an explored function: skip to its end.
                auto f = functions_.upper_bound(pc);
                if (f != functions_.begin()) {
                    auto prev = std::prev(f);
                    if (pc >= prev->first && pc < prev->second.end) { pc = prev->second.end; continue; }
                }
                uint64_t gap_end = r.end();
                if (f != functions_.end() && f->first < gap_end) gap_end = f->first;
                auto s = seeds_.lower_bound(pc);
                if (s != seeds_.end() && s->first < gap_end) gap_end = s->first;
                if (gap_end <= pc) {
                    // A seed that never became a function: skip to the next explored function.
                    pc = (f != functions_.end()) ? f->first : r.end();
                    if (f != functions_.end() && f->first <= gap_end) pc = f->second.end;
                    continue;
                }
                uint64_t p = pc;
                bool only_nops = true;
                while (p < gap_end && is_padding(img_.read_u32(p))) {
                    if (img_.read_u32(p) != kNop) only_nops = false;
                    p += 4;
                }
                if (p < gap_end) {
                    // Alignment padding always ends on an address aligned to at least 8. NOPs that
                    // lead to a merely 4-aligned word are code: the linker turns a BL to an undefined
                    // weak symbol into a NOP (libnx: `bl __libnx_exception_handler`), so such a
                    // function starts with a NOP.
                    if (only_nops && (p & 7) != 0) p = pc;
                    add_seed(p, FunctionSource::GapScan);
                }
                pc = gap_end;
            }
        }
    }

    AnalysisResult finish() {
        AnalysisResult res;
        for (const auto& o : cfg_.jump_table_overrides)
            if (!overrides_used_.count(o.br_pc))
                diag(Diagnostic::Kind::JumpTableOverrideUnused, o.br_pc, "jump table override never reached a BR");
        res.functions.reserve(functions_.size());
        for (auto& [e, f] : functions_) res.functions.push_back(std::move(f));
        for (const auto& f : res.functions) res.function_entries.push_back(f.entry);
        res.diagnostics = std::move(diags_);
        for (auto& [e, v] : fn_diags_) res.diagnostics.insert(res.diagnostics.end(), v.begin(), v.end());
        std::sort(res.diagnostics.begin(), res.diagnostics.end(),
                  [](const Diagnostic& a, const Diagnostic& b) { return a.pc < b.pc; });
        AnalysisStats& st = res.stats;
        st.functions = res.functions.size();
        for (const auto& f : res.functions) {
            st.blocks += f.blocks.size();
            st.instructions += f.instruction_count();
            st.by_source[static_cast<int>(f.source)]++;
            st.covered_bytes += f.end - f.entry;
            if (f.noreturn) ++st.noreturn_functions;
            st.jump_tables += f.jump_tables.size();
            for (const auto& t : f.jump_tables) if (t.bound_known) ++st.jump_tables_bounded;
            for (const auto& b : f.blocks) {
                if (b.term == Terminator::IndirectJump) ++st.indirect_jumps_unresolved;
                for (const auto& c : b.calls) {
                    if (c.indirect) ++st.indirect_calls;
                    else if (c.tail) ++st.tail_calls;
                    else ++st.direct_calls;
                }
            }
        }
        for (const auto& r : ranges_) {
            st.text_bytes += r.text_end - r.text_start;
            for (uint64_t pc = r.header_end ? r.header_end : r.text_start; pc + 4 <= r.text_end; pc += 4)
                if (is_padding(img_.read_u32(pc))) st.padding_bytes += 4;
        }
        return res;
    }
};

}  // namespace

std::vector<std::string> AnalysisConfig::default_noreturn_symbols() {
    return {"abort", "exit", "_exit", "_Exit", "quick_exit", "__stack_chk_fail", "__cxa_throw",
            "__cxa_rethrow", "__cxa_bad_cast", "__cxa_bad_typeid", "__cxa_call_unexpected",
            "_Unwind_Resume", "_Unwind_RaiseException", "__assert_fail", "__assert_func", "__libnx_exit",
            "svcExitProcess", "svcExitThread", "diagAbortWithResult", "__nx_exit",
            "_ZSt9terminatev", "_ZSt10unexpectedv", "_ZN2nn4diag6detail17AbortImplEPKcS3_S3_i",
            "_ZN2nn4diag6detail17AbortImplEPKcS3_S3_iPKNS_6ResultEPKcz",
            "_ZN2nn4diag6detail17AbortImplEPKcS3_S3_iPKNS_6ResultES3_z",
            "_ZN2nn4diag6detail17AbortImplEPKcS3_S3_iPKNS_6ResultES3_St9__va_list",
            "nnDiagAbort", "nnDiagAbortWithResult"};
}

AnalysisResult analyze(const LinkedImage& image, const AnalysisConfig& cfg) {
    Analyzer a(image, cfg);
    return a.run();
}

}  // namespace lgp::recomp
