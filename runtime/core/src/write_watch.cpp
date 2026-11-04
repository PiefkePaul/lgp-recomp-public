#include "lgp/runtime/write_watch.hpp"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <shared_mutex>

#include "lgp/runtime/guest.hpp"
#include "lgp/runtime/guest_mem.hpp"
#include "lgp/runtime/log.hpp"
#include "lgp/runtime/memory.hpp"

namespace lgp::rt {
namespace {

std::shared_mutex& watch_mutex() {
    static std::shared_mutex m;
    return m;
}
std::vector<WriteWatch>& watches() {
    static std::vector<WriteWatch> v;
    return v;
}
std::atomic<uint64_t> g_hits{0};
std::atomic<bool> g_armed{true};
std::atomic<bool> g_skip_zero{false};
std::atomic<uint64_t> g_max_lines{kWriteWatchMaxLines};

bool parse_number(const std::string& s, uint64_t* out) {
    if (s.empty() || s[0] == '-') return false;
    char* end = nullptr;
    *out = std::strtoull(s.c_str(), &end, 0);
    return end == s.c_str() + s.size();
}

}  // namespace

bool write_watch_build() { return LGP_GUEST_MEM_WRITE_WATCH != 0; }

void note_trapped_write(uint64_t va, unsigned size) {
    {
        std::shared_lock lock(watch_mutex());
        const bool hit = std::any_of(watches().begin(), watches().end(), [&](const WriteWatch& w) {
            return va < w.va + w.size && w.va < va + size;
        });
        if (!hit) return;
    }
    if (!g_armed.load(std::memory_order_relaxed)) return;
    // The bytes are written already (note_write runs after the store).
    const uint8_t* p = g_view.base + (va - g_view.guest_base);
    if (g_skip_zero.load(std::memory_order_relaxed) && std::all_of(p, p + size, [](uint8_t b) { return b == 0; }))
        return;
    const uint64_t max_lines = g_max_lines.load(std::memory_order_relaxed);
    const uint64_t n = g_hits.fetch_add(1, std::memory_order_relaxed);
    if (n >= max_lines) {
        if (n == max_lines) log(LogLevel::Warn, "watch: more than %" PRIu64 " stores, log stops", n);
        return;
    }
    char value[2 * 16 + 1] = {};
    const unsigned shown = std::min(size, 16u);
    for (unsigned i = 0; i < shown; ++i)  // most significant byte first, as a little-endian number
        std::snprintf(value + 2 * i, 3, "%02x", p[shown - 1 - i]);
    const CpuState* ctx = t_running_ctx;
    if (ctx != nullptr)
        log(LogLevel::Warn, "watch: 0x%" PRIx64 " size %u = 0x%s pc 0x%" PRIx64 " lr 0x%" PRIx64, va, size, value,
            ctx->pc, ctx->x[30]);
    else
        log(LogLevel::Warn, "watch: 0x%" PRIx64 " size %u = 0x%s (host thread)", va, size, value);
}

bool add_write_watch(GuestMemory& memory, uint64_t va, uint64_t size, std::string* error) {
    if (!write_watch_build()) {
        *error = "needs a build with -DLGP_GUEST_MEM_WRITE_WATCH=ON";
        return false;
    }
    if (size == 0 || !memory.contains(va, size)) {
        *error = "outside the guest address space";
        return false;
    }
    {
        std::unique_lock lock(watch_mutex());
        watches().push_back({va, size});
    }
    memory.set_trap(va, size, true);
    return true;
}

void clear_write_watches(GuestMemory& memory) {
    std::unique_lock lock(watch_mutex());
    for (const WriteWatch& w : watches()) memory.set_trap(w.va, w.size, false);
    watches().clear();
    g_hits.store(0, std::memory_order_relaxed);
    g_armed.store(true, std::memory_order_relaxed);
    g_skip_zero.store(false, std::memory_order_relaxed);
    g_max_lines.store(kWriteWatchMaxLines, std::memory_order_relaxed);
}

uint64_t write_watch_hits() { return g_hits.load(std::memory_order_relaxed); }

void set_write_watch_filter(bool skip_zero, uint64_t max_lines) {
    g_skip_zero.store(skip_zero, std::memory_order_relaxed);
    g_max_lines.store(max_lines != 0 ? max_lines : kWriteWatchMaxLines, std::memory_order_relaxed);
}
uint64_t write_watch_max_lines() { return g_max_lines.load(std::memory_order_relaxed); }
void set_write_watch_armed(bool armed) { g_armed.store(armed, std::memory_order_relaxed); }
bool write_watch_armed() { return g_armed.load(std::memory_order_relaxed); }

bool parse_write_watches(const std::string& spec, std::vector<WriteWatch>* out, std::string* error) {
    size_t start = 0;
    while (start <= spec.size()) {
        const size_t comma = std::min(spec.find(',', start), spec.size());
        const std::string item = spec.substr(start, comma - start);
        const size_t colon = item.find(':');
        WriteWatch w;
        if (!parse_number(item.substr(0, colon), &w.va) ||
            (colon != std::string::npos && (!parse_number(item.substr(colon + 1), &w.size) || w.size == 0))) {
            *error = "bad watch \"" + item + "\" (VA[:LEN])";
            return false;
        }
        out->push_back(w);
        start = comma + 1;
    }
    return true;
}

}  // namespace lgp::rt
