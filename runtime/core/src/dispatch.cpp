#include "lgp/runtime/dispatch.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <map>
#include <mutex>

#include "lgp/runtime/log.hpp"

namespace lgp::rt {

namespace detail {
uint64_t g_dispatch_base = 0;
uint64_t g_dispatch_slots = 0;
LiftedFn* g_dispatch_table = nullptr;
}  // namespace detail

namespace {

std::vector<LiftedFn> g_table;
size_t g_functions = 0;

std::mutex g_unresolved_mutex;
std::map<uint64_t, UnresolvedTarget> g_unresolved;

void publish() {
    detail::g_dispatch_table = g_table.empty() ? nullptr : g_table.data();
    detail::g_dispatch_slots = g_table.size();
}

}  // namespace

void register_functions(const LiftedEntry* entries, size_t count) {
    if (count == 0)
        return;
    uint64_t lo = UINT64_MAX;
    uint64_t hi = 0;
    for (size_t i = 0; i < count; ++i) {
        if ((entries[i].va & 3) != 0 || entries[i].fn == nullptr) {
            log(LogLevel::Error, "dispatch: invalid entry va=0x%" PRIx64, entries[i].va);
            continue;
        }
        lo = std::min(lo, entries[i].va);
        hi = std::max(hi, entries[i].va + 4);
    }
    if (lo >= hi)
        return;
    if (!g_table.empty()) {
        lo = std::min(lo, detail::g_dispatch_base);
        hi = std::max(hi, detail::g_dispatch_base + 4 * g_table.size());
    }
    if (g_table.empty() || lo != detail::g_dispatch_base || (hi - lo) / 4 != g_table.size()) {
        std::vector<LiftedFn> grown((hi - lo) / 4, nullptr);
        if (!g_table.empty())
            std::copy(g_table.begin(), g_table.end(), grown.begin() + (detail::g_dispatch_base - lo) / 4);
        g_table = std::move(grown);
        detail::g_dispatch_base = lo;
    }
    for (size_t i = 0; i < count; ++i) {
        if ((entries[i].va & 3) != 0 || entries[i].fn == nullptr)
            continue;
        LiftedFn& slot = g_table[(entries[i].va - lo) / 4];
        if (slot == nullptr)
            ++g_functions;
        slot = entries[i].fn;
    }
    publish();
}

void reset_dispatch() {
    g_table.clear();
    g_table.shrink_to_fit();
    g_functions = 0;
    detail::g_dispatch_base = 0;
    publish();
    std::lock_guard<std::mutex> lock(g_unresolved_mutex);
    g_unresolved.clear();
}

DispatchInfo dispatch_info() { return {detail::g_dispatch_base, 4 * g_table.size(), g_functions}; }

std::vector<uint64_t> registered_functions() {
    std::vector<uint64_t> out;
    out.reserve(g_functions);
    for (size_t i = 0; i < g_table.size(); ++i)
        if (g_table[i] != nullptr)
            out.push_back(detail::g_dispatch_base + 4 * i);
    return out;
}

LiftedFn lookup_function(uint64_t va) {
    if (LiftedFn fn = table_lookup(va))
        return fn;
    IndirectResolver resolve = indirect_resolver();
    return resolve != nullptr ? resolve(va) : nullptr;
}

namespace detail {

LiftedFn resolve_slow(uint64_t va, uint64_t from) {
    if (IndirectResolver resolve = indirect_resolver()) {
        if (LiftedFn fn = resolve(va))
            return fn;
    }
    bool first = false;
    {
        std::lock_guard<std::mutex> lock(g_unresolved_mutex);
        UnresolvedTarget& t = g_unresolved[va];
        first = t.count == 0;
        if (first) {
            t.target = va;
            t.from_pc = from;
        }
        ++t.count;
    }
    if (first)
        log(LogLevel::Error,
            "unresolved indirect branch target 0x%" PRIx64 " (from pc 0x%" PRIx64
            "); add `function 0x%" PRIx64 "` to the recompiler config",
            va, from, va);
    return nullptr;
}

}  // namespace detail

std::vector<UnresolvedTarget> unresolved_targets() {
    std::lock_guard<std::mutex> lock(g_unresolved_mutex);
    std::vector<UnresolvedTarget> out;
    out.reserve(g_unresolved.size());
    for (const auto& [va, t] : g_unresolved)
        out.push_back(t);
    return out;
}

std::string unresolved_config() {
    std::string out;
    char line[160];
    for (const UnresolvedTarget& t : unresolved_targets()) {
        std::snprintf(line, sizeof(line), "function 0x%08" PRIx64 "  # unresolved indirect target, from pc 0x%08" PRIx64
                      ", %" PRIu64 "x\n", t.target, t.from_pc, t.count);
        out += line;
    }
    return out;
}

}  // namespace lgp::rt
