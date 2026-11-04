// Patch sites of game mods: host functions that replace single guest
// instructions while a mod is on (lgp::mods `patch` lines). The lifter emits
// LGP_PATCH_ACTIVE(va) / LGP_PATCH(va) only at the addresses it was given
// (LiftOptions::patch_sites); everywhere else this costs nothing.
//
// A host function sees every guest register in ctx (written back before the
// call, reloaded after it), ctx->pc = va + 4, and may change registers and
// guest memory. Setting ctx->halt stops the guest thread as an SVC would.
//
// Thread-safe: set_patch() may run while guest threads execute (the menu
// turns a mod on or off); a guest thread sees the change at its next pass
// through the site.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "lgp/lifted/cpu_state.hpp"

namespace lgp::rt {

class GuestMemory;

using PatchFn = void (*)(CpuState* ctx, void* user);

// Installs fn at the patch site va (replacing one installed there before);
// fn == nullptr removes it. observe = true: fn runs before the instruction,
// which still executes (logging, counting); otherwise fn replaces it.
void set_patch(uint64_t va, PatchFn fn, void* user = nullptr, bool observe = false);
// Removes every patch.
void clear_patches();
// Number of installed patches.
size_t patch_count();

namespace detail {
extern std::atomic<uint32_t> g_patches;  // installed patches: 0 keeps the sites to one load
bool patch_installed(uint64_t va);
}  // namespace detail

inline bool patch_active(uint64_t va) {
    return detail::g_patches.load(std::memory_order_acquire) != 0 && detail::patch_installed(va);
}
// Runs the host function at va. True if it replaced the instruction, false
// if the instruction must still run (an observing patch, or one removed
// meanwhile). The first pass through each installed patch is logged ("patch:
// first pass through 0x…"), so a patch that never runs shows in the log.
bool run_patch(CpuState* ctx, uint64_t va);
// (va, passes) of every installed patch, by address.
std::vector<std::pair<uint64_t, uint64_t>> patch_hits();

// Value patches, the first building blocks of game mods (and the diagnosis
// switches LGP_PATCH_IMM / LGP_POKE of the boot program):
//
// set_immediate_patch() makes the move-wide instruction at va (MOVZ, MOVN,
// MOVK, or MOV with a bitmask immediate = ORR Rd, ZR, #imm) write `value` to its destination register instead, all of it (a MOVK
// that completes a MOVZ+MOVK pair then yields the whole new constant). The
// lifted code must have va as a patch site (lgp_lift --patch-site). Reads the
// instruction from `memory` (the process's guest memory: before Kernel::run()
// there is no active kernel yet); false and *error if it is not a move-wide
// instruction or not mapped.
bool set_immediate_patch(GuestMemory& memory, uint64_t va, uint64_t value, std::string* error);
// The value MOVZ, MOVN or MOV (bitmask immediate) writes to its register *rd
// (a W register's value zero-extended), for a mod that patches such a site
// but keeps the game's value while it is off. False for any other instruction
// (MOVK too: it only completes a value).
bool decode_move_immediate(uint32_t insn, unsigned* rd, uint64_t* value);
// Writes the bytes of `value` (4 or 8 of them, little endian) to guest memory
// at va, also into read-only segments (rodata constants). False and *error if
// va is not mapped.
bool poke_guest(GuestMemory& memory, uint64_t va, uint64_t value, unsigned bytes, std::string* error);

// Diagnosis (boot program LGP_PATCH_LOG=VA,…): an observing patch at the
// patch site va that logs x0–x30, sp and s0–s31 (as float) on its first
// `passes` passes ("patch log 0x…: x0=… … sp=… s0=…"), e.g. at a function
// entry for `this`.
void set_register_log(uint64_t va, unsigned passes = 8);

struct ValuePatch {
    uint64_t va = 0;
    uint64_t value = 0;
    unsigned bytes = 4;  // 8 for a value written as 0x with more than 8 digits or d:...
};
// "VA=VALUE[,VA=VALUE...]". VALUE: an integer (0x… hex or decimal), f:X for
// the bits of the float X, f:A/B for those of A / B (f:21/9), d:X or d:A/B for
// a double. False and *error on anything else.
bool parse_value_patches(const std::string& spec, std::vector<ValuePatch>* out, std::string* error);

}  // namespace lgp::rt
