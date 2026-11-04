// Census of the instruction sites a native-execution rewriter would have to
// handle (docs/analysis/56-originalcode-nativ-machbarkeit.md, step 0).
//
// Running original A64 code directly on an Apple or Android host needs every
// instruction patched that the host kernel does not allow or that means
// something else there: SVC, the thread registers (TPIDR_EL0/TPIDRRO_EL0),
// the counters, every use of x18 (platform register on Apple, ShadowCallStack
// on Android) and SP writes that may leave SP misaligned. This module only
// counts them, per module, over the functions the CFG found. The result holds
// counts only, never addresses or code bytes, so a run on game data may be
// shared.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "lgp/recomp/cfg.hpp"
#include "lgp/recomp/link.hpp"

namespace lgp::recomp {

namespace a64 {
struct Inst;
}

// General-purpose registers an instruction reads and writes: bit n = Xn/Wn
// (0..30), bit 31 = SP. XZR/WZR never appears. SIMD&FP registers do not
// count; the GPR operands of SIMD&FP instructions (load/store base, DUP/INS
// from a GPR, UMOV/SMOV, FP<->integer conversion) do.
struct GprUse {
    uint32_t reads = 0;
    uint32_t writes = 0;
};
GprUse gpr_use(const a64::Inst& inst);

inline constexpr uint32_t kGprSp = 1u << 31;

// A site other than x18 that native execution must patch: SVC, TPIDR*/counter
// access, SP written from a register or by an unaligned immediate.
bool native_stub_site(const a64::Inst& inst, const GprUse& use);

struct NativeCensus {
    std::string module;
    uint64_t functions = 0;
    uint64_t instructions = 0;   // inside functions (CFG blocks)
    uint64_t undecodable = 0;    // Op::Invalid inside blocks (not counted below)

    // x18 / w18
    uint64_t x18_sites = 0;            // instructions naming x18 at all
    uint64_t x18_reads = 0, x18_writes = 0;
    uint64_t x18_br = 0;               // BR x18 (jump table or tail call)
    uint64_t x18_br_jump_table = 0;    //   of which the CFG resolved as jump table
    uint64_t x18_blr = 0;              // BLR x18
    uint64_t x18_mem = 0;              // x18 as base or index of a load/store
    uint64_t x18_sp = 0;               // moves between SP and x18
    uint64_t x18_simd = 0;             // GPR operand of a SIMD&FP instruction
    uint64_t x18_in_exclusive = 0;     // inside an LDXR..STXR window
    uint64_t functions_with_x18 = 0;
    // Per function with x18 (intra-procedural liveness over the CFG; callers
    // and callees are assumed not to pass values in x18):
    uint64_t x18_live_at_entry = 0;      // read before any write on a path from the entry
    uint64_t x18_live_across_call = 0;   // live right after a BL/BLR (expects the callee to keep it)
    uint64_t x18_unresolved = 0;         // has an unresolved indirect jump (liveness not provable)
    uint64_t x18_no_free_register = 0;   // none of x9..x17 is unused in the function
    uint64_t x18_renamable = 0;          // none of the above: x18 can become a free scratch register
    uint64_t x18_renamable_sites = 0;    // x18 sites in those functions
    // Per live range (web) of x18, with the liveness of every GPR: a web can
    // take any of x0..x17/x19..x28 that is dead and unused across all its
    // points, so different webs of one function may take different registers.
    // Calls clobber x0..x17/x30 (not x18), exits keep x0..x7, x19..x30 and SP,
    // an exit through an unresolved indirect jump keeps everything.
    uint64_t x18_webs = 0;
    uint64_t x18_webs_renamable = 0;
    uint64_t x18_webs_open = 0;              // live at entry or at an unresolved indirect jump
    uint64_t x18_web_sites_renamable = 0;    // x18 sites in renamable webs
    uint64_t x18_web_sites_left = 0;         // x18 sites that still need a stub
    uint64_t x18_functions_all_renamable = 0;
    // The same with x0..x17 only (what plan_native_rewrite() uses by default,
    // native_rewrite.hpp: landing pads may read callee-saved registers).
    uint64_t x18_webs_renamable_caller_saved = 0;
    uint64_t x18_web_sites_renamable_caller_saved = 0;

    // Exceptions and system registers
    uint64_t svc = 0;
    std::map<uint32_t, uint64_t> svc_by_number;
    std::map<std::string, uint64_t> mrs, msr;   // by register name
    uint64_t dc_zva = 0, cache_maint = 0;
    uint64_t yield_wfe = 0;            // YIELD/WFE/WFI hints

    // SP
    uint64_t sp_from_register = 0;     // SP written from a register value (sub sp, sp, xN; mov sp, xN)
    uint64_t sp_imm_unaligned = 0;     // add/sub sp, sp, #imm with imm % 16 != 0

    // Exclusive monitor windows (LDXR .. STXR within a few instructions)
    uint64_t exclusive_windows = 0;
    uint64_t exclusive_windows_with_site = 0;   // a site to patch lies inside

    // Loops without SVC, store or call that only another thread can end
    // (approximation of a spin wait: no register read on entry is written in
    // the loop).
    uint64_t spin_loops = 0;
    uint64_t spin_loops_with_hint = 0;

    // Sites a rewriter must patch on Apple: x18 sites, SVC, TPIDR*/counter
    // accesses, SP from a register and unaligned SP immediates.
    uint64_t patch_sites() const;
};

// One entry per module, in load order.
std::vector<NativeCensus> native_census(const LinkedImage& image, const AnalysisResult& analysis);

const char* sysreg_name(uint32_t packed);

}  // namespace lgp::recomp
