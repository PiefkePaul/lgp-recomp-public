// Host hooks on guest function entries (game mods, docs/knowledge/spiel-mods-hooks.md).
//
// Calls between lifted functions are direct C++ calls, so a function can only
// be intercepted where the recompiler left a door: `lgp_lift --hook`/`--mods`
// lifts the listed functions as `<name>_orig` plus a thin `<name>` that runs
// LGP_HOOKED(va, &<name>_orig). Direct calls, tail calls and the dispatch
// table all reach `<name>`, so a hook sees every call. The generated code
// announces these entries with LGP_HOOKABLE before main() (declare_hookable).
//
// A hook gets the guest registers and the original: it may run it (wrap),
// skip it (replace) or run it with changed arguments. Without a hook a
// hookable function costs one lookup in a small sorted table; functions the
// recompiler did not list cost nothing.
//
// Hooks are installed before guest code runs (like register_functions());
// lookups are not synchronised against set_hook()/clear_hooks(). A mod that
// is switched on and off while the game runs keeps its hook and decides
// inside it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "lgp/runtime/system.hpp"

namespace lgp::rt {

// ctx: the guest registers at the function entry (x30 = return address).
// original: the lifted function; `original(ctx)` runs it.
using HookFn = void (*)(CpuState* ctx, LiftedFn original, void* user);

// Called by generated code (LGP_HOOKABLE) during static initialisation.
void declare_hookable(const uint64_t* entries, size_t count);
// Whether the lifted code has a hook door at va.
bool is_hookable(uint64_t va);
// Sorted, without duplicates.
std::vector<uint64_t> hookable_entries();

// Installs (fn != nullptr) or removes (fn == nullptr) the hook at va; a later
// hook on the same address replaces the earlier one. False if va is not
// hookable: the code was lifted without that address, the hook would never
// run.
bool set_hook(uint64_t va, HookFn fn, void* user = nullptr);
size_t hook_count();
// Removes every hook (tests, a new process); the hookable entries stay.
void clear_hooks();

namespace detail {
struct HookSlot {
    uint64_t va;
    HookFn fn;
    void* user;
};
// Sorted by va; read by find_hook(), written by set_hook()/clear_hooks().
extern const HookSlot* g_hooks;
extern size_t g_hook_count;
}  // namespace detail

inline const detail::HookSlot* find_hook(uint64_t va) {
    size_t lo = 0, hi = detail::g_hook_count;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        const detail::HookSlot& s = detail::g_hooks[mid];
        if (s.va == va)
            return &s;
        if (s.va < va)
            lo = mid + 1;
        else
            hi = mid;
    }
    return nullptr;
}

// What LGP_HOOKED (guest.hpp) does, as a call: for runtime code that enters
// a hookable function by address.
inline void call_hooked(CpuState* ctx, uint64_t va, LiftedFn original) {
    if (const detail::HookSlot* s = find_hook(va)) [[unlikely]] {
        s->fn(ctx, original, s->user);
        return;
    }
    original(ctx);
}

}  // namespace lgp::rt
