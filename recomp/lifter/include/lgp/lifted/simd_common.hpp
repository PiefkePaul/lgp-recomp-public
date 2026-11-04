// Lane access and common building blocks for lifted SIMD&FP code (P2.4).
//
// A V register is lgp::V128 {lo, hi}; lanes are numbered from the least
// significant end, as in the Arm ARM (element 0 = bits [esize-1:0]). The
// helpers assume a little-endian host (x86-64, arm64, wasm32 all are).
//
// Writes of SIMD&FP results to a V register always clear the bits above the
// written width (AArch64 semantics); only INS, LD1 (single lane) and friends
// keep the other lanes.
#pragma once

#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

#include "lgp/lifted/cpu_state.hpp"

namespace lgp::lifted::simd {

static_assert(std::endian::native == std::endian::little, "lifted SIMD code assumes a little-endian host");

template <class T>
inline T lane(const V128& v, unsigned i) {
    T t;
    std::memcpy(&t, reinterpret_cast<const unsigned char*>(&v) + i * sizeof(T), sizeof(T));
    return t;
}

template <class T>
inline void set_lane(V128& v, unsigned i, T t) {
    std::memcpy(reinterpret_cast<unsigned char*>(&v) + i * sizeof(T), &t, sizeof(T));
}

template <class T>
inline constexpr unsigned lanes_of(bool q) {
    return (q ? 16u : 8u) / static_cast<unsigned>(sizeof(T));
}

inline constexpr V128 zero() { return V128{0, 0}; }
// Clears the upper 64 bits for 64-bit (Q = 0) vector results.
inline V128 narrow_q(V128 v, bool q) {
    if (!q) v.hi = 0;
    return v;
}
// Scalar SIMD&FP register values (zero-extended to 128 bits on write).
inline V128 from_u64(uint64_t v) { return V128{v, 0}; }
inline V128 from_u32(uint32_t v) { return V128{v, 0}; }
inline V128 from_u16(uint16_t v) { return V128{v, 0}; }
inline V128 from_u8(uint8_t v) { return V128{v, 0}; }

// FPSR.QC: cumulative saturation flag, set by saturating integer operations.
inline constexpr uint32_t kFpsrQc = 1u << 27;
inline void set_qc(CpuState* ctx) { ctx->fpsr |= kFpsrQc; }

// Signed/unsigned saturation of a wide intermediate to the lane type T,
// setting QC on saturation.
template <class T, class W>
inline T sat(CpuState* ctx, W v) {
    static_assert(sizeof(W) >= sizeof(T));
    using L = std::numeric_limits<T>;
    if constexpr (std::is_signed_v<W>) {
        if (v > static_cast<W>(L::max())) {
            set_qc(ctx);
            return L::max();
        }
        if (v < static_cast<W>(L::min())) {
            set_qc(ctx);
            return L::min();
        }
    } else {
        if (v > static_cast<W>(L::max())) {
            set_qc(ctx);
            return L::max();
        }
    }
    return static_cast<T>(v);
}

// Element-wise maps over all lanes of T (Q selects 64 or 128 bits).
template <class T, class F>
inline V128 map1(const V128& a, bool q, F f) {
    V128 r = zero();
    for (unsigned i = 0; i < lanes_of<T>(q); ++i)
        set_lane<T>(r, i, f(lane<T>(a, i)));
    return r;
}

template <class T, class F>
inline V128 map2(const V128& a, const V128& b, bool q, F f) {
    V128 r = zero();
    for (unsigned i = 0; i < lanes_of<T>(q); ++i)
        set_lane<T>(r, i, f(lane<T>(a, i), lane<T>(b, i)));
    return r;
}

template <class T, class F>
inline V128 map3(const V128& a, const V128& b, const V128& c, bool q, F f) {
    V128 r = zero();
    for (unsigned i = 0; i < lanes_of<T>(q); ++i)
        set_lane<T>(r, i, f(lane<T>(a, i), lane<T>(b, i), lane<T>(c, i)));
    return r;
}

}  // namespace lgp::lifted::simd
