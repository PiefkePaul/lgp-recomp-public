#include "lgp/runtime/guest_mem.hpp"

#include <array>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace lgp::rt {

GuestView g_view;

namespace {

thread_local FaultHandler t_fault;

std::array<std::mutex, 64> g_cas128_locks;

}  // namespace

FaultHandler set_fault_handler(FaultHandler handler) {
    const FaultHandler previous = t_fault;
    t_fault = handler;
    return previous;
}

void report_fault(uint64_t va, unsigned size, bool write) {
    if (t_fault.hook != nullptr)
        t_fault.hook(va, size, write, t_fault.user);
    std::fprintf(stderr, "lgp runtime: guest %s fault at 0x%" PRIx64 " (%u bytes)%s\n", write ? "write" : "read", va,
                 size, t_fault.hook != nullptr ? ", fault hook returned" : ", no fault hook installed");
    std::abort();
}

bool compare_and_swap_128(uint64_t va, uint64_t exp_lo, uint64_t exp_hi, uint64_t new_lo, uint64_t new_hi) {
    uint8_t* p = translate<true>(va, 16);
    if (reinterpret_cast<uintptr_t>(p) % 16 != 0)
        report_fault(va, 16, true);
    std::lock_guard lock(g_cas128_locks[(va >> 4) % g_cas128_locks.size()]);
    uint64_t cur[2];
    std::memcpy(cur, p, sizeof(cur));
    if (cur[0] != exp_lo || cur[1] != exp_hi)
        return false;
    const uint64_t next[2] = {new_lo, new_hi};
    std::memcpy(p, next, sizeof(next));
    note_write(va, 16);
    return true;
}

}  // namespace lgp::rt
