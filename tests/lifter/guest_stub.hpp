// Test runtime for lifted code (stands in for runtime/core's guest.hpp until
// P2.6). Flat list of guest memory regions, single-threaded: the CAS
// functions are plain compare-and-write, which is exact while only one guest
// thread runs (ADR-0002 baton mode).
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

#include "lgp/lifted/cpu_state.hpp"

namespace lgp_test {

struct GuestRegion {
    uint64_t base;
    uint64_t size;
    uint8_t* data;
    bool writable;
};

struct Guest {
    GuestRegion regions[4];
    unsigned region_count = 0;
    lgp::CpuState* cpu = nullptr;
    uint64_t backedge_budget = 1'000'000;  // guards against runaway loops in lifted code
};

extern thread_local Guest* g_guest;

// Thrown by the memory hooks on a fault: the hooks must not return (see the
// contract in lifted.hpp), RecompiledExecutor catches it. Lifted code is built
// with LiftOptions::precise_fault_pc, so ctx->pc names the faulting instruction.
struct MemFault {
    uint64_t va;
};

[[noreturn]] inline void mem_fault(uint64_t va) { throw MemFault{va}; }

inline uint8_t* host_ptr(uint64_t va, unsigned bytes, bool write) {
    for (unsigned i = 0; i < g_guest->region_count; ++i) {
        const GuestRegion& r = g_guest->regions[i];
        if (va >= r.base && va - r.base <= r.size - bytes) {
            if (write && !r.writable) break;
            return r.data + (va - r.base);
        }
    }
    mem_fault(va);
}

template <typename T>
inline T load(uint64_t va) {
    T v;
    std::memcpy(&v, host_ptr(va, sizeof(T), false), sizeof(T));
    return v;
}

template <typename T>
inline void store(uint64_t va, T v) {
    std::memcpy(host_ptr(va, sizeof(T), true), &v, sizeof(T));
}

void svc(lgp::CpuState* ctx, uint32_t imm);
void call_indirect(lgp::CpuState* ctx, uint64_t va);

}  // namespace lgp_test

inline uint8_t lgp_mem_read8(uint64_t va) { return lgp_test::load<uint8_t>(va); }
inline uint16_t lgp_mem_read16(uint64_t va) { return lgp_test::load<uint16_t>(va); }
inline uint32_t lgp_mem_read32(uint64_t va) { return lgp_test::load<uint32_t>(va); }
inline uint64_t lgp_mem_read64(uint64_t va) { return lgp_test::load<uint64_t>(va); }
inline void lgp_mem_write8(uint64_t va, uint8_t v) { lgp_test::store(va, v); }
inline void lgp_mem_write16(uint64_t va, uint16_t v) { lgp_test::store(va, v); }
inline void lgp_mem_write32(uint64_t va, uint32_t v) { lgp_test::store(va, v); }
inline void lgp_mem_write64(uint64_t va, uint64_t v) { lgp_test::store(va, v); }

inline bool lgp_mem_cas32(uint64_t va, uint32_t expected, uint32_t desired) {
    if (lgp_mem_read32(va) != expected) return false;
    lgp_mem_write32(va, desired);
    return true;
}
inline bool lgp_mem_cas64(uint64_t va, uint64_t expected, uint64_t desired) {
    if (lgp_mem_read64(va) != expected) return false;
    lgp_mem_write64(va, desired);
    return true;
}
inline bool lgp_mem_cas128(uint64_t va, uint64_t exp_lo, uint64_t exp_hi, uint64_t new_lo, uint64_t new_hi) {
    if (lgp_mem_read64(va) != exp_lo || lgp_mem_read64(va + 8) != exp_hi) return false;
    lgp_mem_write64(va, new_lo);
    lgp_mem_write64(va + 8, new_hi);
    return true;
}

#define LGP_CALL_INDIRECT(va) lgp_test::call_indirect(ctx, (va))
#define LGP_JUMP_INDIRECT(va) lgp_test::call_indirect(ctx, (va))
#define LGP_SVC(imm) lgp_test::svc(ctx, (imm))
// A memory fault recorded earlier wins over the stop that follows it.
#define LGP_BRK(imm)                        \
    do {                                    \
        if (ctx->halt == lgp::kHaltNone) {  \
            ctx->halt = lgp::kHaltBrk;      \
            ctx->halt_info = (imm);         \
        }                                   \
    } while (0)
#define LGP_UNDEFINED(raw)                   \
    do {                                     \
        if (ctx->halt == lgp::kHaltNone) {   \
            ctx->halt = lgp::kHaltUndefined; \
            ctx->halt_info = (raw);          \
        }                                    \
    } while (0)
#define LGP_YIELD() ((void)0)
#define LGP_BARRIER() std::atomic_thread_fence(std::memory_order_seq_cst)
#define LGP_READ_SYSREG(enc) (static_cast<void>(enc), UINT64_C(0))
#define LGP_TRACE_BLOCK(pc) ((void)(pc))
#define LGP_BACKEDGE()                                 \
    do {                                               \
        if (--lgp_test::g_guest->backedge_budget == 0) { \
            ctx->halt = lgp::kHaltLimit;               \
            return;                                    \
        }                                              \
    } while (0)
