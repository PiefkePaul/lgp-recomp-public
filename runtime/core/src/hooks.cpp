#include "lgp/runtime/hooks.hpp"

#include <algorithm>

namespace lgp::rt {

namespace detail {
const HookSlot* g_hooks = nullptr;
size_t g_hook_count = 0;
}  // namespace detail

namespace {

// Function-local: declare_hookable() runs from other translation units'
// static initialisers, in no defined order relative to this one.
std::vector<uint64_t>& hookable() {
    static std::vector<uint64_t> v;
    return v;
}

std::vector<detail::HookSlot>& slots() {
    static std::vector<detail::HookSlot> v;
    return v;
}

void publish() {
    detail::g_hooks = slots().data();
    detail::g_hook_count = slots().size();
}

}  // namespace

void declare_hookable(const uint64_t* entries, size_t count) {
    std::vector<uint64_t>& v = hookable();
    v.insert(v.end(), entries, entries + count);
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
}

bool is_hookable(uint64_t va) {
    const std::vector<uint64_t>& v = hookable();
    return std::binary_search(v.begin(), v.end(), va);
}

std::vector<uint64_t> hookable_entries() { return hookable(); }

bool set_hook(uint64_t va, HookFn fn, void* user) {
    if (!is_hookable(va))
        return false;
    std::vector<detail::HookSlot>& v = slots();
    auto it = std::lower_bound(v.begin(), v.end(), va,
                               [](const detail::HookSlot& s, uint64_t a) { return s.va < a; });
    if (fn == nullptr) {
        if (it != v.end() && it->va == va)
            v.erase(it);
    } else if (it != v.end() && it->va == va) {
        it->fn = fn;
        it->user = user;
    } else {
        v.insert(it, {va, fn, user});
    }
    publish();
    return true;
}

size_t hook_count() { return slots().size(); }

void clear_hooks() {
    slots().clear();
    publish();
}

}  // namespace lgp::rt
