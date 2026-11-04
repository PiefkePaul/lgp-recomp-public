// A64 → C++ lifter (roadmap P2.3: integer, loads/stores, branches, flags).
//
// Input is one function as the CFG (P2.2, docs/design/p2-interfaces.md)
// describes it: entry plus address ranges of its basic blocks. Output is the
// C++ definition `void <name>(lgp::CpuState* ctx)` built on
// lgp/lifted/lifted.hpp. Instructions the lifter does not support become a
// LGP_UNDEFINED() call and are counted; lifting itself never fails.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "lgp/recomp/a64.hpp"

namespace lgp::recomp {

struct LiftBlock {
    uint64_t start = 0;
    uint64_t end = 0;  // exclusive
};

struct LiftFunction {
    uint64_t entry = 0;
    std::vector<LiftBlock> blocks;  // any order; must not overlap
    // Resolved targets of BR instructions (jump tables, P2.5), keyed by the
    // BR's address. Targets inside the function become a switch with gotos.
    std::map<uint64_t, std::vector<uint64_t>> indirect_targets;
};

struct LiftOptions {
    std::string name_prefix = "lgp_fn_";
    bool block_trace = false;   // emit LGP_TRACE_BLOCK(pc) at every block start
    bool backedge_hook = true;  // emit LGP_BACKEDGE() on backward jumps inside a function
    // Write ctx->pc before every memory access, so a faulting lgp_mem_* hook
    // sees the exact pc of the faulting instruction. Costs a store per access;
    // meant for tests and debugging builds.
    bool precise_fault_pc = false;
    // Keep the guest registers, NZCV and FPCR a function uses in C++ locals and
    // write them back to CpuState only where something can observe it: calls,
    // SVCs, returns, tail calls, halts and the slow path of LGP_BACKEDGE_SYNC
    // (with precise_fault_pc also before every memory access). Without
    // precise_fault_pc, CpuState holds stale registers when a memory access
    // faults. Off: every access goes through ctx (the P2.3 code shape).
    bool register_locals = true;
    // NZCV across function boundaries. true: exact A64 semantics, the flags are
    // written back and reloaded at BL/BLR, RET and tail calls like any other
    // register. false (AAPCS64): the flags are undefined at function entry and
    // after a call, so compiled code never reads them there; they are neither
    // written back nor reloaded at those places, and a function starts with
    // zero flags instead of loading them from CpuState. Saves 12-20 % per call
    // (docs/knowledge/lifted-code-performance.md). SVC, BRK, indirect jumps to
    // non-entries and the setjmp call keep exact flags. Only with
    // register_locals.
    bool flags_across_calls = true;
    // Registers at RET. true: exact, every register the function changed is
    // written back. false (AAPCS64): the corruptible x9-x17, v4-v7 and v16-v31
    // are not (no caller reads them after a call; results are in x0-x7 and
    // v0-v3). Breaks hand-written code that returns values in those
    // registers. Only with register_locals.
    bool registers_across_calls = true;
    // Entries of the guest's setjmp (ADR-0004 host variant): a BL to one of
    // them becomes LGP_SETJMP() in the calling function. The setjmp function
    // itself and longjmp are replaced by the code generator, not lifted.
    std::set<uint64_t> setjmp_entries;
    // Patch sites of game mods (lgp::mods, `patch` lines): at each address the
    // lifted code asks LGP_PATCH_ACTIVE(va) first and, while a mod has a host
    // function there, runs LGP_PATCH(va) instead of the instruction (registers
    // written back before, reloaded after). Only instructions that fall
    // through to pc + 4 without a branch (Flow::Normal); Lifter::lift() leaves
    // other sites unpatched and lists them in LiftStats::rejected_patch_sites.
    std::set<uint64_t> patch_sites;
};

struct LiftStats {
    uint64_t instructions = 0;
    uint64_t unsupported = 0;
    std::map<std::string, uint64_t> by_op;  // op_name → count (supported and unsupported)
    std::vector<uint64_t> unsupported_pcs;
    std::set<uint64_t> patch_sites;             // LiftOptions::patch_sites emitted
    std::vector<uint64_t> rejected_patch_sites;  // not Flow::Normal: left as they are
};

// Reads one little-endian instruction word; returns false outside the image.
using CodeReader = std::function<bool(uint64_t va, uint32_t& insn)>;

class Lifter {
public:
    // function_entries: every known function entry (BL targets become direct
    // calls only when listed here).
    Lifter(CodeReader reader, std::vector<uint64_t> function_entries, LiftOptions options = {});

    std::string function_name(uint64_t entry) const;
    bool is_entry(uint64_t va) const;

    // Forward declarations of all known functions.
    std::string declarations() const;
    // Definition of one function.
    std::string lift(const LiftFunction& fn, LiftStats& stats) const;

private:
    CodeReader reader_;
    std::vector<uint64_t> entries_;  // sorted, unique
    LiftOptions options_;
};

}  // namespace lgp::recomp
