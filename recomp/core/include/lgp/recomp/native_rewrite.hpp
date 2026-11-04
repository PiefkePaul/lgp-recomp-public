// Rewrite plan for running original A64 code natively (docs/analysis/56-originalcode-nativ-machbarkeit.md,
// stage B). First part: x18 renaming. Every live range (web) of x18 that has a
// register dead and unused across all its points gets that register instead,
// instruction by instruction, without a branch. Second part: the other webs
// keep their value in a stack slot below SP (a web never spans a call), each
// site becomes a branch to a stub (X18Spill); SVC and the thread/counter
// system registers go through a gate (GateSite). native_image.hpp turns the
// plan into code.
//
// The plan holds addresses and instruction words of the input. Run on game
// data, its output is derived game data and stays in $LGP_PRIVATE_DIR; only the
// counts may be shared.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "lgp/recomp/a64.hpp"
#include "lgp/recomp/cfg.hpp"
#include "lgp/recomp/link.hpp"
#include "lgp/recomp/native_census.hpp"

namespace lgp::recomp {

struct NativeInst {
    a64::Inst inst;
    GprUse use;
};

// Registers a web may take. Caller-saved x0..x17 are always safe: a call
// clobbers them, so no landing pad or caller expects their value. x19..x28 are
// only safe where the CFG sees every edge: C++ landing pads are not CFG edges
// and may read a callee-saved register the normal path treats as dead, so they
// are used only in functions whose FDE has no LSDA.
inline constexpr uint32_t kRenameCallerSaved = 0x3FFFFu & ~(1u << 18);
inline constexpr uint32_t kRenameCalleeSaved = 0x3FFu << 19;

struct X18Web {
    std::vector<size_t> sites;  // indices into the function's code that name x18
    std::vector<size_t> points; // every index where x18 is live or named
    uint32_t free = 0;          // registers dead and unused across all points (before the candidate mask)
    bool open = false;          // live at the entry (or an extra entry) or at an unresolved indirect jump
};

// Live ranges of x18 in one function. `code` holds the function's instructions
// in block order (as Function::blocks lists them). `extra_entries` are VAs
// inside the function that other code jumps or calls to; a web live there is
// open like one live at the entry.
// With `live_in`/`live_out` the GPR liveness before and after every
// instruction of `code` is returned as well.
std::vector<X18Web> x18_webs(const Function& f, const std::vector<NativeInst>& code,
                             const std::vector<uint64_t>& extra_entries = {},
                             std::vector<uint32_t>* live_in = nullptr, std::vector<uint32_t>* live_out = nullptr);

// `word` with GPR `from` replaced by `to` in every operand field that names it,
// and nothing else changed. Checked by decoding again: the result must decode to
// the same instruction with the same register use, `from` mapped to `to`.
// nullopt when the instruction does not name `from`, already names `to`, or the
// substitution cannot be proven (for example an immediate field that holds the
// same bits).
std::optional<uint32_t> rename_gpr(uint32_t word, uint64_t pc, unsigned from, unsigned to);

// A site of a web that keeps x18 in a stack slot. Offsets are relative to SP
// before (`_before`) and after (`_after`) the instruction, which may move SP
// (pre/post-index on SP, add/sub sp, sp, #imm).
struct X18Spill {
    uint64_t va = 0;
    uint32_t word = 0;          // original instruction
    int16_t slot_before = 0, slot_after = 0;  // the value of x18
    int16_t save_before = 0, save_after = 0;  // where `helper` is saved when it is live
    uint8_t helper = 0;         // register that stands in for x18 in the stub
    bool helper_dead = false;   // dead at the site: no save/restore
};

// SVC or MRS/MSR of TPIDR_EL0, TPIDRRO_EL0, CNTPCT/CNTVCT/CNTFRQ_EL0. `word`
// is the instruction after renaming (a renamed web may name the register).
struct GateSite {
    uint64_t va = 0;
    uint32_t word = 0;
};

struct NativeRewriteOptions {
    uint32_t candidates = kRenameCallerSaved;
    // Functions whose FDE has no LSDA have no landing pads: there x19..x28
    // may be used as well (the exit keeps them live, so only a register the
    // function saves and restores itself can be dead inside it).
    bool callee_saved_without_lsda = true;
    // false: no web is renamed; every web that could be is planned as if no
    // register were free (counted in webs_no_register). For tests of the stubs.
    bool rename = true;
};

struct NativeRewrite {
    std::string module;
    std::vector<std::pair<uint64_t, uint32_t>> words;  // VA -> new instruction word (renamed sites)
    std::vector<uint64_t> x18_stub_sites;              // x18 sites left for stubs
    std::vector<uint64_t> other_stub_sites;            // SVC, thread registers, counters, SP from a register
    std::vector<X18Spill> x18_spills;                  // x18 sites with a stack-slot stub
    std::vector<GateSite> gate_sites;                  // SVC / system register sites with a gate stub

    uint64_t functions_with_x18 = 0;
    uint64_t webs = 0, webs_renamed = 0, webs_open = 0, webs_no_register = 0;
    uint64_t webs_shared = 0;        // a point lies in more than one function: left for stubs
    uint64_t webs_encoding = 0;      // a site could not be re-encoded: left for stubs
    uint64_t sites_renamed = 0;
    uint64_t functions_callee_saved = 0;  // functions with x18 where x19..x28 were allowed (FDE without LSDA)

    // Second part: webs left after renaming.
    uint64_t webs_spilled = 0, sites_spilled = 0;
    uint64_t webs_sp_unknown = 0;     // SP not statically known at every point (SP from a register, paths disagree)
    uint64_t webs_slot_far = 0;       // slot beyond the 9-bit LDUR/STUR range of some point
    uint64_t webs_exclusive = 0;      // a site lies in an LDXR..STXR window
    uint64_t webs_gate = 0;           // a point is a gate site (its frame would cover the slot)
    uint64_t webs_form = 0;           // a site has no stub form (BLR/RET x18, BR x18 without a dead register,
                                      // a gate site naming x18, re-encoding failed)
    uint64_t gate_exclusive = 0;      // gate sites in an LDXR..STXR window: left unpatched
    uint64_t gate_x18 = 0;            // gate sites naming an x18 that is not renamed: left unpatched
    uint64_t sp_sites = 0;            // SP written from a register or unaligned immediate: checked, not patched
    // x18 sites without any patch (open, shared and every web counted above).
    std::vector<uint64_t> x18_unpatched;
};

// One entry per module, in load order.
std::vector<NativeRewrite> plan_native_rewrite(const LinkedImage& image, const AnalysisResult& analysis,
                                               const NativeRewriteOptions& opt = {});

}  // namespace lgp::recomp
