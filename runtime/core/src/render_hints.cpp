#include "lgp/runtime/render_hints.hpp"

#include <atomic>
#include <mutex>
#include <utility>

namespace lgp::rt {

namespace {
std::mutex g_mutex;
std::vector<RepeatHint> g_hints;
std::atomic<bool> g_any{false};  // the draw path checks this without the lock
}  // namespace

void set_repeat_hints(std::vector<RepeatHint> hints) {
    std::lock_guard lock(g_mutex);
    g_hints = std::move(hints);
    g_any.store(!g_hints.empty(), std::memory_order_release);
}

bool repeat_hint(uint32_t width, uint32_t height, bool* u, bool* v) {
    if (!g_any.load(std::memory_order_acquire)) return false;
    std::lock_guard lock(g_mutex);
    for (const RepeatHint& h : g_hints)
        if (h.width == width && h.height == height) {
            *u = h.u;
            *v = h.v;
            return true;
        }
    return false;
}

}  // namespace lgp::rt
