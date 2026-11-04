// Fast guest-memory access (ADR-0001, option A).
//
// One contiguous host block backs the guest address space from
// GuestView::guest_base upwards. A guest access becomes
//
//     host = base + (uint32_t)(va - guest_base)
//
// on every platform (on 32-bit hosts computed as bias + va, the same value). On 64-bit desktop hosts the block is a 4 GiB
// reservation, so even a truncated or wild address stays inside it and
// unmapped pages fault in hardware. On wasm32 the block is only as large as
// the guest layout, so the 32-bit offset can reach host memory: debug builds
// therefore compile with LGP_GUEST_MEM_CHECKED, which bounds- and
// permission-checks every access against the software page table.
//
// Build switches (compile definitions, set by runtime/core/CMakeLists.txt):
//   LGP_GUEST_MEM_CHECKED      bounds + permission check per access (Debug default)
//   LGP_GUEST_MEM_WRITE_WATCH  stores mark pages flagged kPageWatch as dirty
//                              (ADR-0001 point 4: GPU pools, off by default)
//                              and report stores to pages flagged kPageTrap
//                              (write watchpoints, lgp/runtime/write_watch.hpp)
//   LGP_GUEST_MEM64            no 32-bit truncation (memory64 build, untested)
//
// The switches change inline code, so every translation unit of one binary
// must see the same values. Use the target's compile definitions, never a
// local #define.
#pragma once

#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>

#ifndef LGP_GUEST_MEM_CHECKED
#define LGP_GUEST_MEM_CHECKED 0
#endif
#ifndef LGP_GUEST_MEM_WRITE_WATCH
#define LGP_GUEST_MEM_WRITE_WATCH 0
#endif
#ifndef LGP_GUEST_MEM64
#define LGP_GUEST_MEM64 0
#endif

namespace lgp::rt {

static_assert(std::endian::native == std::endian::little, "guest and host must both be little-endian");

inline constexpr uint64_t kPageSize = 0x1000;
inline constexpr unsigned kPageShift = 12;

// Software page table entry: one byte per 4 KiB guest page.
enum PageFlag : uint8_t {
    kPageRead = 1,     // guest may read
    kPageWrite = 2,    // guest may write
    kPageWatch = 0x10, // stores set kPageDirty (LGP_GUEST_MEM_WRITE_WATCH)
    kPageDirty = 0x20,
    kPageTrap = 0x40,  // stores call note_trapped_write() (LGP_GUEST_MEM_WRITE_WATCH)
};

struct GuestView {
    uint8_t* base = nullptr;       // host address of guest VA `guest_base`
    uint64_t guest_base = 0;       // lowest guest VA of the address space
    uint64_t size = 0;             // bytes of guest VA space from guest_base
    uint8_t* page_flags = nullptr; // size >> kPageShift entries
    // base - guest_base in host pointer arithmetic. On 32-bit hosts (wasm32)
    // that wraps exactly like the truncated offset, so an access is a single
    // `bias + va` (see translate()).
    uintptr_t bias = 0;
};

// The guest address space of this process, published by GuestMemory.
extern GuestView g_view;

// Called on a faulting access in checked mode (and for misaligned atomics in
// every mode). A fault never returns to the accessor: lifted code does not
// check for errors after memory accesses (lifted.hpp contract), so the hook
// must leave by longjmp/exception, and without a hook the process aborts.
// run_lifted() in guest.hpp installs a hook that stops the guest thread.
// Thread-local.
using FaultHook = void (*)(uint64_t va, unsigned size, bool write, void* user);
struct FaultHandler {
    FaultHook hook = nullptr;
    void* user = nullptr;
};
FaultHandler set_fault_handler(FaultHandler handler);  // returns the previous one
[[noreturn, gnu::cold]] void report_fault(uint64_t va, unsigned size, bool write);
// A store to a page flagged kPageTrap (after the bytes are written). Defined
// in write_watch.cpp.
[[gnu::cold]] void note_trapped_write(uint64_t va, unsigned size);

inline uint64_t guest_offset(uint64_t va) {
#if LGP_GUEST_MEM64
    return va - g_view.guest_base;
#else
    return static_cast<uint32_t>(va - g_view.guest_base);
#endif
}

inline bool access_allowed(uint64_t va, unsigned size, uint8_t need) {
    const uint64_t off = va - g_view.guest_base;
    if (off >= g_view.size || g_view.size - off < size)
        return false;
    const uint64_t first = off >> kPageShift;
    const uint64_t last = (off + size - 1) >> kPageShift;
    return (g_view.page_flags[first] & need) == need && (g_view.page_flags[last] & need) == need;
}

template <bool Write>
inline uint8_t* translate(uint64_t va, unsigned size) {
#if LGP_GUEST_MEM_CHECKED
    if (!access_allowed(va, size, Write ? kPageWrite : kPageRead)) [[unlikely]]
        report_fault(va, size, Write);
    return g_view.base + (va - g_view.guest_base);
#else
    (void)size;
#if !LGP_GUEST_MEM64 && UINTPTR_MAX == 0xFFFFFFFFu
    // 32-bit host: base + (uint32_t)(va - guest_base) == bias + (uint32_t)va,
    // both modulo 2^32. The constant part of an address (adrp + offset) then
    // folds into one loop-invariant value; with the subtraction in between,
    // LLVM kept two extra adds per access in Wasm, on the critical path of
    // table lookups (CRC32 24 % slower, docs/knowledge/lifted-code-performance.md).
    return reinterpret_cast<uint8_t*>(g_view.bias + static_cast<uintptr_t>(va));
#else
    return g_view.base + guest_offset(va);
#endif
#endif
}

// Guest == false: a host-side write into guest memory (HLE): only the dirty
// bits of watched pages, no write-watch report (lgp/runtime/write_watch.hpp
// names lifted stores only).
template <bool Guest = true>
inline void note_write(uint64_t va, unsigned size) {
#if LGP_GUEST_MEM_WRITE_WATCH
    const uint64_t off = guest_offset(va);
    constexpr uint8_t kCheck = Guest ? (kPageWatch | kPageTrap) : kPageWatch;
    for (uint64_t page = off >> kPageShift; page <= (off + size - 1) >> kPageShift; ++page) {
        uint8_t& flags = g_view.page_flags[page];
        if (flags & kCheck) [[unlikely]] {
            if (flags & kPageWatch)
                std::atomic_ref<uint8_t>(flags).fetch_or(kPageDirty, std::memory_order_relaxed);
            if (Guest && (flags & kPageTrap)) {
                note_trapped_write(va, size);
                break;
            }
        }
    }
#else
    (void)va;
    (void)size;
#endif
}

template <typename T>
inline T load(uint64_t va) {
    T v;
    std::memcpy(&v, translate<false>(va, sizeof(T)), sizeof(T));
    return v;
}

template <typename T>
inline void store(uint64_t va, T v) {
    std::memcpy(translate<true>(va, sizeof(T)), &v, sizeof(T));
    note_write(va, sizeof(T));
}

// Exclusive pairs (LDXR/STXR) are lowered to compare-and-swap (ADR-0002).
// Exclusives must be naturally aligned on hardware; a misaligned one faults.
template <typename T>
inline bool compare_and_swap(uint64_t va, T expected, T desired) {
    uint8_t* p = translate<true>(va, sizeof(T));
    if (reinterpret_cast<uintptr_t>(p) % sizeof(T) != 0) [[unlikely]]
        report_fault(va, sizeof(T), true);
    const bool swapped = std::atomic_ref<T>(*reinterpret_cast<T*>(p))
                             .compare_exchange_strong(expected, desired, std::memory_order_seq_cst);
    if (swapped)
        note_write(va, sizeof(T));
    return swapped;
}

// 128-bit exclusive pair (LDXP/STXP of two X registers). Not lock-free on
// every host: it serialises through a striped lock, so it is atomic against
// other compare_and_swap_128 calls but not against plain 64-bit stores to the
// same location from another thread. Documented limitation.
bool compare_and_swap_128(uint64_t va, uint64_t exp_lo, uint64_t exp_hi, uint64_t new_lo, uint64_t new_hi);

}  // namespace lgp::rt
