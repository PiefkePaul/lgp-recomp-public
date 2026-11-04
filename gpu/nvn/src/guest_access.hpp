// Guest memory and argument access for HLE functions.
//
// HLE functions run inside lifted code and hold the HLE mutex, so they must
// not leave through the runtime's fault longjmp (it would skip the unlock and
// host destructors). Every access is checked against the page table instead;
// a bad pointer records a fault in the current FaultScope, reads return zero,
// writes are dropped, and the thunk halts the guest after the function
// returns (hle.cpp).
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>

#include "lgp/lifted/cpu_state.hpp"
#include "lgp/runtime/guest_mem.hpp"

namespace lgp::gpu::nvn::guest {

class FaultScope {
public:
    FaultScope() : previous_(current_) { current_ = this; }
    ~FaultScope() { current_ = previous_; }
    FaultScope(const FaultScope&) = delete;
    FaultScope& operator=(const FaultScope&) = delete;

    bool faulted() const { return faulted_; }
    uint64_t address() const { return address_; }

    static void record(uint64_t va) {
        if (current_ != nullptr && !current_->faulted_) {
            current_->faulted_ = true;
            current_->address_ = va;
        }
    }

private:
    static inline thread_local FaultScope* current_ = nullptr;
    FaultScope* previous_;
    bool faulted_ = false;
    uint64_t address_ = 0;
};

// [va, va+size) readable (or writable) by the guest, page by page.
inline bool accessible(uint64_t va, uint64_t size, bool write) {
    if (size == 0)
        return true;
    const uint8_t need = write ? rt::kPageWrite : rt::kPageRead;
    uint64_t pos = va;
    const uint64_t end = va + size;
    if (end < va)
        return false;
    while (pos < end) {
        const uint64_t page_end = (pos | (rt::kPageSize - 1)) + 1;
        const uint64_t chunk = (page_end < end ? page_end : end) - pos;
        if (!rt::access_allowed(pos, static_cast<unsigned>(chunk), need))
            return false;
        pos += chunk;
    }
    return true;
}

inline uint8_t* host(uint64_t va) { return rt::g_view.base + (va - rt::g_view.guest_base); }

inline bool read(void* dst, uint64_t va, uint64_t size) {
    if (!accessible(va, size, false)) {
        FaultScope::record(va);
        std::memset(dst, 0, size);
        return false;
    }
    std::memcpy(dst, host(va), size);
    return true;
}

inline bool write(uint64_t va, const void* src, uint64_t size) {
    if (!accessible(va, size, true)) {
        FaultScope::record(va);
        return false;
    }
    std::memcpy(host(va), src, size);
    // Keep write-watch bookkeeping (ADR-0001 point 4) consistent.
    for (uint64_t off = 0; off < size; off += rt::kPageSize)
        rt::note_write<false>(va + off, 1);
    rt::note_write<false>(va + size - 1, 1);
    return true;
}

template <typename T>
T load(uint64_t va) {
    static_assert(std::is_trivially_copyable_v<T>);
    T v;
    read(&v, va, sizeof(T));
    return v;
}

template <typename T>
void store(uint64_t va, const T& v) {
    static_assert(std::is_trivially_copyable_v<T>);
    write(va, &v, sizeof(T));
}

// NUL-terminated string of at most `max` bytes (longer: truncated).
inline std::string read_cstr(uint64_t va, size_t max = 256) {
    std::string s;
    for (size_t i = 0; i < max; ++i) {
        uint8_t c = 0;
        if (!read(&c, va + i, 1) || c == 0)
            break;
        s.push_back(static_cast<char>(c));
    }
    return s;
}

// AAPCS64: integer/pointer arguments in x0..x7, float arguments counted
// separately in s0..s7. Narrow integer arguments only define their low bits.
inline uint64_t ptr_arg(const CpuState* ctx, int i) { return ctx->x[i]; }
inline int32_t int_arg(const CpuState* ctx, int i) { return static_cast<int32_t>(ctx->x[i]); }
inline uint32_t uint_arg(const CpuState* ctx, int i) { return static_cast<uint32_t>(ctx->x[i]); }
inline bool bool_arg(const CpuState* ctx, int i) { return (ctx->x[i] & 0xFF) != 0; }
inline float float_arg(const CpuState* ctx, int i) {
    const uint32_t bits = static_cast<uint32_t>(ctx->vreg[i].lo);
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

}  // namespace lgp::gpu::nvn::guest
