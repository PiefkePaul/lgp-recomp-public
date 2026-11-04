// Indirect branch targets of lifted code (ADR-0004, roadmap P2.5).
//
// Every recompiled function is registered in one direct-indexed table: slot
// (va - base) / 4 holds the host function for the guest function starting at
// va. A lookup is a subtraction, a bounds check and one load, which matters
// with ~109 000 BLR sites in LGP's main. The table covers the address range of
// all registered functions (main + sdk: ~18 MiB of slots with 4-byte wasm
// function indices, twice that on 64-bit desktop builds).
//
// An address without a function is an error, not a silent fallback: the
// target is recorded, logged once, and the guest halts with kHaltBadJump.
// unresolved_config() turns the recorded targets into `function 0x…` lines
// for the recompiler config (docs/knowledge/recompiler-config.md); the module
// is then recompiled with them.
//
// Registration happens before guest code runs; lookups are not synchronised
// against a concurrent register_functions().
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "lgp/runtime/system.hpp"

namespace lgp::rt {

// Adds functions to the table; the covered range grows as needed. A later
// entry for the same address replaces the earlier one.
void register_functions(const LiftedEntry* entries, size_t count);
// Empties the table and the record of unresolved targets (tests).
void reset_dispatch();

// Covered range and number of registered functions (diagnostics, tests).
struct DispatchInfo {
    uint64_t base = 0;
    uint64_t size = 0;  // bytes of guest address space covered
    size_t functions = 0;
};
DispatchInfo dispatch_info();
// Addresses with a registered function, ascending (native start: every one
// is a host function that needs a host-call stub).
std::vector<uint64_t> registered_functions();

namespace detail {
// Read-only for inline lookups; written by register_functions/reset_dispatch.
extern uint64_t g_dispatch_base;
extern uint64_t g_dispatch_slots;
extern LiftedFn* g_dispatch_table;

// Miss in the table: asks the resolver; if that fails too, records va as
// unresolved (branch at from_pc), logs it once and returns nullptr.
LiftedFn resolve_slow(uint64_t va, uint64_t from_pc);
}  // namespace detail

// The function registered at va, nullptr if there is none.
inline LiftedFn table_lookup(uint64_t va) {
    const uint64_t off = va - detail::g_dispatch_base;
    const uint64_t slot = off >> 2;
    if (slot < detail::g_dispatch_slots && (off & 3) == 0) [[likely]]
        return detail::g_dispatch_table[slot];
    return nullptr;
}

// Table first, then the resolver installed with set_indirect_resolver()
// (bring-up and tests); nullptr if neither knows va. For runtime code that
// enters guest code by address (thread entries, HLE callbacks).
LiftedFn lookup_function(uint64_t va);

// Lifted code resolves through lgp::rt::resolve_indirect() in guest.hpp,
// which turns a miss into kHaltBadJump.

struct UnresolvedTarget {
    uint64_t target = 0;
    uint64_t from_pc = 0;  // pc of the first branch that went there
    uint64_t count = 0;
};
// Sorted by target.
std::vector<UnresolvedTarget> unresolved_targets();
// One `function 0x…` line per unresolved target, with the branch as comment.
std::string unresolved_config();

}  // namespace lgp::rt
