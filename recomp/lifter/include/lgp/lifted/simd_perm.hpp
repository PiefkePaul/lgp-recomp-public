// Advanced SIMD data movement: DUP/INS/UMOV/SMOV, permutes, EXT, TBL/TBX, modified immediates (P2.4).
// Header-only helpers called by lifted code; see simd_common.hpp for lane access.
//
// All helpers take their operands by value and return the result, so lifted
// code can assign straight to the destination even when it aliases a source.
#pragma once

#include "lgp/lifted/simd_common.hpp"

namespace lgp::lifted::simd {

// DUP: every lane of the 64- or 128-bit result is v.
template <class T>
inline V128 dup(T v, bool q) {
    V128 r = zero();
    for (unsigned i = 0; i < lanes_of<T>(q); ++i)
        set_lane<T>(r, i, v);
    return r;
}

// UZP1/UZP2: even (part 0) or odd (part 1) lanes of the concatenation b:a.
template <class T>
inline V128 uzp(V128 a, V128 b, unsigned part, bool q) {
    const unsigned n = lanes_of<T>(q);
    V128 r = zero();
    for (unsigned e = 0; e < n; ++e) {
        const unsigned s = 2 * e + part;
        set_lane<T>(r, e, s < n ? lane<T>(a, s) : lane<T>(b, s - n));
    }
    return r;
}

// TRN1/TRN2: interleave the even (part 0) or odd (part 1) lanes of a and b.
template <class T>
inline V128 trn(V128 a, V128 b, unsigned part, bool q) {
    const unsigned pairs = lanes_of<T>(q) / 2;
    V128 r = zero();
    for (unsigned p = 0; p < pairs; ++p) {
        set_lane<T>(r, 2 * p, lane<T>(a, 2 * p + part));
        set_lane<T>(r, 2 * p + 1, lane<T>(b, 2 * p + part));
    }
    return r;
}

// ZIP1/ZIP2: interleave the lower (part 0) or upper (part 1) halves of a and b.
template <class T>
inline V128 zip(V128 a, V128 b, unsigned part, bool q) {
    const unsigned pairs = lanes_of<T>(q) / 2;
    const unsigned base = part * pairs;
    V128 r = zero();
    for (unsigned p = 0; p < pairs; ++p) {
        set_lane<T>(r, 2 * p, lane<T>(a, base + p));
        set_lane<T>(r, 2 * p + 1, lane<T>(b, base + p));
    }
    return r;
}

// EXT: bytes [pos, pos + 8/16) of the concatenation b:a (pos < 8 for Q = 0).
inline V128 ext(V128 a, V128 b, unsigned pos, bool q) {
    const unsigned n = q ? 16u : 8u;
    V128 r = zero();
    for (unsigned i = 0; i < n; ++i) {
        const unsigned s = pos + i;
        set_lane<uint8_t>(r, i, s < n ? lane<uint8_t>(a, s) : lane<uint8_t>(b, s - n));
    }
    return r;
}

// TBL/TBX: byte lookup in a table of 1..4 registers. Out-of-range indices give
// 0 (TBL) or keep the byte of `fallback` (TBX: the old destination).
inline V128 tbx(const V128* table, unsigned regs, V128 idx, V128 fallback, bool q) {
    const unsigned n = q ? 16u : 8u;
    V128 r = zero();
    for (unsigned i = 0; i < n; ++i) {
        const unsigned k = lane<uint8_t>(idx, i);
        set_lane<uint8_t>(r, i, k < 16 * regs ? lane<uint8_t>(table[k / 16], k % 16) : lane<uint8_t>(fallback, i));
    }
    return r;
}
inline V128 tbl(const V128* table, unsigned regs, V128 idx, bool q) { return tbx(table, regs, idx, zero(), q); }

}  // namespace lgp::lifted::simd
