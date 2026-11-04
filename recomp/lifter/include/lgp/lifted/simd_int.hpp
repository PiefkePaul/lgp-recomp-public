// Advanced SIMD integer operations (P2.4).
// Header-only helpers called by lifted code; see simd_common.hpp for lane access.
//
// Structure: lane operations are small functor structs with a static template
// member f (e.g. QAdd::f(ctx, a, b)); the map templates below apply one of
// them to `n` lanes (n = 1 for the scalar forms) and return a fresh V128 whose
// other lanes are zero. All inputs are taken by const reference and only read
// before the result is returned, so `lgp_v<d> = map(..., lgp_v<d>, ...)`
// is safe when registers alias.
//
// Element arithmetic follows the Arm ARM pseudocode (DDI 0487, C7.2): integer
// results are computed exactly and then truncated (wrapping forms) or
// saturated (SatQ, setting FPSR.QC). Wrap-around is always done in unsigned
// types to avoid signed-overflow UB; no __int128 is needed.
#pragma once

#include <bit>
#include <cstdint>
#include <limits>
#include <type_traits>

#include "lgp/lifted/simd_common.hpp"

namespace lgp::lifted::simd {

namespace idetail {

template <class T>
using U = std::make_unsigned_t<T>;
template <class T>
inline constexpr int kBits = static_cast<int>(sizeof(T) * 8);

template <class T>
inline T ones() {
    return static_cast<T>(static_cast<U<T>>(~U<T>(0)));
}
template <class T>
inline T wrap_add(T a, T b) {
    return static_cast<T>(static_cast<U<T>>(static_cast<uint64_t>(static_cast<U<T>>(a)) + static_cast<U<T>>(b)));
}
template <class T>
inline T wrap_sub(T a, T b) {
    return static_cast<T>(static_cast<U<T>>(static_cast<uint64_t>(static_cast<U<T>>(a)) - static_cast<U<T>>(b)));
}
template <class T>
inline T wrap_mul(T a, T b) {
    return static_cast<T>(
        static_cast<U<T>>(static_cast<uint64_t>(static_cast<U<T>>(a)) * static_cast<uint64_t>(static_cast<U<T>>(b))));
}
template <class T>
inline T wrap_neg(T a) {
    return wrap_sub(T(0), a);
}

template <class T>
inline T sat_max(CpuState* ctx) {
    set_qc(ctx);
    return std::numeric_limits<T>::max();
}
template <class T>
inline T sat_min(CpuState* ctx) {
    set_qc(ctx);
    return std::numeric_limits<T>::min();
}

// Left shift without saturation; s >= 0, bits shifted out are lost (s >= esize gives 0).
template <class T>
inline T shl(T x, int s) {
    if (s >= kBits<T>) return T(0);
    return static_cast<T>(static_cast<U<T>>(static_cast<uint64_t>(static_cast<U<T>>(x)) << s));
}

// Arithmetic (signed T) or logical (unsigned T) right shift, s >= 0, any size.
template <class T>
inline T shr_floor(T x, int s) {
    if (s >= kBits<T>) {
        if constexpr (std::is_signed_v<T>) return x < 0 ? T(-1) : T(0);
        return T(0);
    }
    return static_cast<T>(x >> s);
}

// (x + 2^(s-1)) >> s computed exactly, s >= 1: floor(x / 2^s) plus bit s-1 of x
// (of its infinite sign extension). Never overflows T.
template <class T>
inline T shr_round(T x, int s) {
    const T q = shr_floor(x, s);
    const int b = s - 1;
    T bit;
    if (b >= kBits<T>) {
        if constexpr (std::is_signed_v<T>)
            bit = x < 0 ? T(1) : T(0);
        else
            bit = T(0);
    } else {
        bit = static_cast<T>((x >> b) & 1);
    }
    return wrap_add(q, bit);
}

template <class T>
inline T shr(T x, int s, bool round) {
    return round ? shr_round(x, s) : shr_floor(x, s);
}

// Saturating left shift (SatQ(x << s)), s >= 0.
template <class T>
inline T qshl(CpuState* ctx, T x, int s) {
    if (x == 0) return T(0);
    bool over = s >= kBits<T>;
    T r = T(0);
    if (!over) {
        r = shl(x, s);
        over = static_cast<T>(r >> s) != x;
    }
    if (over) {
        if constexpr (std::is_signed_v<T>)
            return x < 0 ? sat_min<T>(ctx) : sat_max<T>(ctx);
        return sat_max<T>(ctx);
    }
    return r;
}

template <class T>
inline T qadd(CpuState* ctx, T a, T b) {
    if constexpr (sizeof(T) < 8) {
        return sat<T>(ctx, static_cast<int64_t>(a) + static_cast<int64_t>(b));
    } else if constexpr (std::is_signed_v<T>) {
        const uint64_t ua = static_cast<uint64_t>(a), ub = static_cast<uint64_t>(b), r = ua + ub;
        if (((ua ^ r) & (ub ^ r)) >> 63) return a < 0 ? sat_min<T>(ctx) : sat_max<T>(ctx);
        return static_cast<T>(r);
    } else {
        const T r = a + b;
        if (r < a) return sat_max<T>(ctx);
        return r;
    }
}

template <class T>
inline T qsub(CpuState* ctx, T a, T b) {
    if constexpr (sizeof(T) < 8) {
        return sat<T>(ctx, static_cast<int64_t>(a) - static_cast<int64_t>(b));
    } else if constexpr (std::is_signed_v<T>) {
        const uint64_t ua = static_cast<uint64_t>(a), ub = static_cast<uint64_t>(b), r = ua - ub;
        if (((ua ^ ub) & (ua ^ r)) >> 63) return a < 0 ? sat_min<T>(ctx) : sat_max<T>(ctx);
        return static_cast<T>(r);
    } else {
        if (a < b) {
            set_qc(ctx);
            return T(0);
        }
        return static_cast<T>(a - b);
    }
}

template <class T>
inline T mask_bits() {
    return ones<T>();
}

}  // namespace idetail

// ---- lane operations ----------------------------------------------------------------------

// Binary, element type T.
struct Add {
    template <class T>
    static T f(CpuState*, T a, T b) { return idetail::wrap_add(a, b); }
};
struct Sub {
    template <class T>
    static T f(CpuState*, T a, T b) { return idetail::wrap_sub(a, b); }
};
struct Mul {
    template <class T>
    static T f(CpuState*, T a, T b) { return idetail::wrap_mul(a, b); }
};
// Polynomial multiply over {0,1}, low 8 bits (PMUL).
struct Pmul {
    static uint8_t f(CpuState*, uint8_t a, uint8_t b) {
        unsigned r = 0;
        for (int i = 0; i < 8; ++i)
            if ((b >> i) & 1) r ^= static_cast<unsigned>(a) << i;
        return static_cast<uint8_t>(r);
    }
};
// Halving add/sub (esize <= 32).
struct HAdd {
    template <class T>
    static T f(CpuState*, T a, T b) { return static_cast<T>((static_cast<int64_t>(a) + static_cast<int64_t>(b)) >> 1); }
};
struct RHAdd {
    template <class T>
    static T f(CpuState*, T a, T b) {
        return static_cast<T>((static_cast<int64_t>(a) + static_cast<int64_t>(b) + 1) >> 1);
    }
};
struct HSub {
    template <class T>
    static T f(CpuState*, T a, T b) { return static_cast<T>((static_cast<int64_t>(a) - static_cast<int64_t>(b)) >> 1); }
};
struct QAdd {
    template <class T>
    static T f(CpuState* ctx, T a, T b) { return idetail::qadd(ctx, a, b); }
};
struct QSub {
    template <class T>
    static T f(CpuState* ctx, T a, T b) { return idetail::qsub(ctx, a, b); }
};
struct CmGt {
    template <class T>
    static T f(CpuState*, T a, T b) { return a > b ? idetail::ones<T>() : T(0); }
};
struct CmGe {
    template <class T>
    static T f(CpuState*, T a, T b) { return a >= b ? idetail::ones<T>() : T(0); }
};
struct CmEq {
    template <class T>
    static T f(CpuState*, T a, T b) { return a == b ? idetail::ones<T>() : T(0); }
};
struct CmTst {
    template <class T>
    static T f(CpuState*, T a, T b) { return (a & b) != 0 ? idetail::ones<T>() : T(0); }
};
// Shift by register (SSHL, USHL, SRSHL, URSHL, SQSHL, UQSHL, SQRSHL, UQRSHL):
// the shift is the signed low byte of the element of the second operand.
template <bool Round, bool Saturate>
struct ShlReg {
    template <class T>
    static T f(CpuState* ctx, T a, T b) {
        const int s = static_cast<int8_t>(static_cast<uint8_t>(static_cast<idetail::U<T>>(b) & 0xFFu));
        if (s >= 0) {
            if constexpr (Saturate)
                return idetail::qshl(ctx, a, s);
            else
                return idetail::shl(a, s);
        }
        return idetail::shr(a, -s, Round);
    }
};
struct Max {
    template <class T>
    static T f(CpuState*, T a, T b) { return a > b ? a : b; }
};
struct Min {
    template <class T>
    static T f(CpuState*, T a, T b) { return a < b ? a : b; }
};
// |a - b| modulo 2^esize.
struct Abd {
    template <class T>
    static T f(CpuState*, T a, T b) { return a > b ? idetail::wrap_sub(a, b) : idetail::wrap_sub(b, a); }
};
// Signed saturating doubling multiply returning high half (esize 16 or 32).
template <bool Round>
struct QDMulH {
    template <class T>
    static T f(CpuState* ctx, T a, T b) {
        static_assert(std::is_signed_v<T> && sizeof(T) <= 4);
        if (a == std::numeric_limits<T>::min() && b == std::numeric_limits<T>::min()) return idetail::sat_max<T>(ctx);
        const int64_t p = 2 * (static_cast<int64_t>(a) * static_cast<int64_t>(b)) +
                          (Round ? (int64_t{1} << (idetail::kBits<T> - 1)) : 0);
        return static_cast<T>(p >> idetail::kBits<T>);
    }
};
// SUQADD: signed accumulator d plus n read as unsigned, signed saturation (T signed).
struct SuQAdd {
    template <class T>
    static T f(CpuState* ctx, T d, T n) {
        static_assert(std::is_signed_v<T>);
        const uint64_t un = static_cast<idetail::U<T>>(n);
        if constexpr (sizeof(T) < 8) {
            return sat<T>(ctx, static_cast<int64_t>(d) + static_cast<int64_t>(un));
        } else {
            if (un > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) - static_cast<uint64_t>(d))
                return idetail::sat_max<T>(ctx);
            return static_cast<T>(static_cast<uint64_t>(d) + un);
        }
    }
};
// USQADD: unsigned accumulator d plus n read as signed, unsigned saturation (T unsigned).
struct UsQAdd {
    template <class T>
    static T f(CpuState* ctx, T d, T n) {
        static_assert(std::is_unsigned_v<T>);
        using S = std::make_signed_t<T>;
        const S sn = static_cast<S>(n);
        if constexpr (sizeof(T) < 8) {
            return sat<T>(ctx, static_cast<int64_t>(d) + static_cast<int64_t>(sn));
        } else {
            if (sn < 0) {
                const uint64_t mag = uint64_t{0} - static_cast<uint64_t>(sn);
                if (mag > d) {
                    set_qc(ctx);
                    return T(0);
                }
                return d - mag;
            }
            if (n > std::numeric_limits<T>::max() - d) return idetail::sat_max<T>(ctx);
            return d + n;
        }
    }
};

// Ternary (accumulate), first argument is the destination element.
struct Mla {
    template <class T>
    static T f(CpuState*, T d, T a, T b) { return idetail::wrap_add(d, idetail::wrap_mul(a, b)); }
};
struct Mls {
    template <class T>
    static T f(CpuState*, T d, T a, T b) { return idetail::wrap_sub(d, idetail::wrap_mul(a, b)); }
};
struct Aba {
    template <class T>
    static T f(CpuState* ctx, T d, T a, T b) { return idetail::wrap_add(d, Abd::f(ctx, a, b)); }
};
// Long saturating doubling multiplies on the widened elements (SQDMULL, SQDMLAL, SQDMLSL).
struct QDMull {
    template <class T>
    static T f(CpuState* ctx, T a, T b) {
        const T p = static_cast<T>(a * b);  // exact: a, b are sign-extended half-width values
        return idetail::qadd(ctx, p, p);
    }
};
struct QDMlal {
    template <class T>
    static T f(CpuState* ctx, T d, T a, T b) { return idetail::qadd(ctx, d, QDMull::f(ctx, a, b)); }
};
struct QDMlsl {
    template <class T>
    static T f(CpuState* ctx, T d, T a, T b) { return idetail::qsub(ctx, d, QDMull::f(ctx, a, b)); }
};

// Unary.
struct Abs {
    template <class T>
    static T f(CpuState*, T a) { return a < 0 ? idetail::wrap_neg(a) : a; }
};
struct Neg {
    template <class T>
    static T f(CpuState*, T a) { return idetail::wrap_neg(a); }
};
struct QAbs {
    template <class T>
    static T f(CpuState* ctx, T a) {
        if (a == std::numeric_limits<T>::min()) return idetail::sat_max<T>(ctx);
        return a < 0 ? static_cast<T>(-a) : a;
    }
};
struct QNeg {
    template <class T>
    static T f(CpuState* ctx, T a) {
        if (a == std::numeric_limits<T>::min()) return idetail::sat_max<T>(ctx);
        return static_cast<T>(-a);
    }
};
struct Cls {
    template <class T>
    static T f(CpuState*, T a) {
        const uint64_t v = static_cast<uint64_t>(static_cast<int64_t>(a));
        return static_cast<T>(std::countl_zero((v ^ (v << 1)) | 1) - (64 - idetail::kBits<T>));
    }
};
struct Clz {
    template <class T>
    static T f(CpuState*, T a) {
        return static_cast<T>(std::countl_zero(static_cast<uint64_t>(static_cast<idetail::U<T>>(a))) -
                              (64 - idetail::kBits<T>));
    }
};
struct Cnt {
    static uint8_t f(CpuState*, uint8_t a) { return static_cast<uint8_t>(std::popcount(a)); }
};
struct Not {
    static uint8_t f(CpuState*, uint8_t a) { return static_cast<uint8_t>(~a); }
};
struct Rbit {
    static uint8_t f(CpuState*, uint8_t a) {
        unsigned r = 0;
        for (int i = 0; i < 8; ++i)
            r |= ((a >> i) & 1u) << (7 - i);
        return static_cast<uint8_t>(r);
    }
};
struct CmGt0 {
    template <class T>
    static T f(CpuState*, T a) { return a > 0 ? idetail::ones<T>() : T(0); }
};
struct CmGe0 {
    template <class T>
    static T f(CpuState*, T a) { return a >= 0 ? idetail::ones<T>() : T(0); }
};
struct CmEq0 {
    template <class T>
    static T f(CpuState*, T a) { return a == 0 ? idetail::ones<T>() : T(0); }
};
struct CmLe0 {
    template <class T>
    static T f(CpuState*, T a) { return a <= 0 ? idetail::ones<T>() : T(0); }
};
struct CmLt0 {
    template <class T>
    static T f(CpuState*, T a) { return a < 0 ? idetail::ones<T>() : T(0); }
};

// Shifts by immediate (s is the decoded shift amount).
struct Shr {  // SSHR/USHR, 1 <= s <= esize
    template <class T>
    static T f(CpuState*, T a, int s) { return idetail::shr_floor(a, s); }
};
struct Rshr {  // SRSHR/URSHR
    template <class T>
    static T f(CpuState*, T a, int s) { return idetail::shr_round(a, s); }
};
struct ShlI {  // SHL, SSHLL/USHLL, SHLL; 0 <= s < esize
    template <class T>
    static T f(CpuState*, T a, int s) { return idetail::shl(a, s); }
};
struct QShlI {  // SQSHL/UQSHL (immediate)
    template <class T>
    static T f(CpuState* ctx, T a, int s) { return idetail::qshl(ctx, a, s); }
};
struct QShlU {  // SQSHLU: signed input, unsigned saturation (T signed, result bits unsigned)
    template <class T>
    static T f(CpuState* ctx, T a, int s) {
        if (a < 0) {
            set_qc(ctx);
            return T(0);
        }
        return static_cast<T>(idetail::qshl(ctx, static_cast<idetail::U<T>>(a), s));
    }
};
struct Sra {  // SSRA/USRA: d + (a >> s)
    template <class T>
    static T f(CpuState*, T d, T a, int s) { return idetail::wrap_add(d, idetail::shr_floor(a, s)); }
};
struct Rsra {
    template <class T>
    static T f(CpuState*, T d, T a, int s) { return idetail::wrap_add(d, idetail::shr_round(a, s)); }
};
struct Sri {  // shift right and insert (T unsigned), 1 <= s <= esize
    template <class T>
    static T f(CpuState*, T d, T a, int s) {
        if (s >= idetail::kBits<T>) return d;
        const T m = static_cast<T>(idetail::ones<T>() >> s);
        return static_cast<T>((d & static_cast<T>(~m)) | (static_cast<T>(a >> s) & m));
    }
};
struct Sli {  // shift left and insert (T unsigned), 0 <= s < esize
    template <class T>
    static T f(CpuState*, T d, T a, int s) {
        const T m = idetail::shl(idetail::ones<T>(), s);
        return static_cast<T>((d & static_cast<T>(~m)) | (idetail::shl(a, s) & m));
    }
};

// Narrowing: f<TN>(ctx, wide element ...) returns the narrow element.
struct Xtn {
    template <class TN, class TW>
    static TN f(CpuState*, TW a) { return static_cast<TN>(a); }
};
struct QXtn {  // SQXTN/UQXTN/SQXTUN, depending on the signedness of TW and TN
    template <class TN, class TW>
    static TN f(CpuState* ctx, TW a) { return sat<TN>(ctx, a); }
};
template <bool Round>
struct ShrN {  // SHRN/RSHRN (TW unsigned)
    template <class TN, class TW>
    static TN f(CpuState*, TW a, int s) { return static_cast<TN>(idetail::shr(a, s, Round)); }
};
template <bool Round>
struct QShrN {  // SQ(R)SHRN, UQ(R)SHRN, SQ(R)SHRUN
    template <class TN, class TW>
    static TN f(CpuState* ctx, TW a, int s) { return sat<TN>(ctx, idetail::shr(a, s, Round)); }
};
template <bool Round>
struct AddHn {  // (R)ADDHN: high half of a + b (+ rounding), modulo 2^(2*esize)
    template <class TN, class TW>
    static TN f(CpuState*, TW a, TW b) {
        TW s = idetail::wrap_add(a, b);
        if (Round) s = idetail::wrap_add(s, static_cast<TW>(TW(1) << (idetail::kBits<TN> - 1)));
        return static_cast<TN>(s >> idetail::kBits<TN>);
    }
};
template <bool Round>
struct SubHn {
    template <class TN, class TW>
    static TN f(CpuState*, TW a, TW b) {
        TW s = idetail::wrap_sub(a, b);
        if (Round) s = idetail::wrap_add(s, static_cast<TW>(TW(1) << (idetail::kBits<TN> - 1)));
        return static_cast<TN>(s >> idetail::kBits<TN>);
    }
};

// ---- maps over lanes ----------------------------------------------------------------------
// n = number of result lanes (1 for scalar forms); lanes >= n are zero.

template <class T, class Op>
inline V128 v1(CpuState* ctx, const V128& a, unsigned n) {
    V128 r = zero();
    for (unsigned i = 0; i < n; ++i)
        set_lane<T>(r, i, static_cast<T>(Op::f(ctx, lane<T>(a, i))));
    return r;
}

template <class T, class Op>
inline V128 v2(CpuState* ctx, const V128& a, const V128& b, unsigned n) {
    V128 r = zero();
    for (unsigned i = 0; i < n; ++i)
        set_lane<T>(r, i, static_cast<T>(Op::f(ctx, lane<T>(a, i), lane<T>(b, i))));
    return r;
}

template <class T, class Op>
inline V128 v3(CpuState* ctx, const V128& d, const V128& a, const V128& b, unsigned n) {
    V128 r = zero();
    for (unsigned i = 0; i < n; ++i)
        set_lane<T>(r, i, static_cast<T>(Op::f(ctx, lane<T>(d, i), lane<T>(a, i), lane<T>(b, i))));
    return r;
}

template <class T, class Op>
inline V128 v1i(CpuState* ctx, const V128& a, unsigned n, int s) {
    V128 r = zero();
    for (unsigned i = 0; i < n; ++i)
        set_lane<T>(r, i, static_cast<T>(Op::f(ctx, lane<T>(a, i), s)));
    return r;
}

template <class T, class Op>
inline V128 v2i(CpuState* ctx, const V128& d, const V128& a, unsigned n, int s) {
    V128 r = zero();
    for (unsigned i = 0; i < n; ++i)
        set_lane<T>(r, i, static_cast<T>(Op::f(ctx, lane<T>(d, i), lane<T>(a, i), s)));
    return r;
}

// Pairwise over the concatenation b:a (a supplies the low half of the result), n result lanes.
template <class T, class Op>
inline V128 pw(CpuState* ctx, const V128& a, const V128& b, unsigned n) {
    V128 r = zero();
    const unsigned h = n / 2;
    for (unsigned i = 0; i < h; ++i) {
        set_lane<T>(r, i, static_cast<T>(Op::f(ctx, lane<T>(a, 2 * i), lane<T>(a, 2 * i + 1))));
        set_lane<T>(r, h + i, static_cast<T>(Op::f(ctx, lane<T>(b, 2 * i), lane<T>(b, 2 * i + 1))));
    }
    return r;
}

// Long: narrow lanes first..first+n-1 of a and b, widened to TW, n wide result lanes.
template <class TN, class TW, class Op>
inline V128 l2(CpuState* ctx, const V128& a, const V128& b, unsigned first, unsigned n) {
    V128 r = zero();
    for (unsigned i = 0; i < n; ++i)
        set_lane<TW>(r, i,
                     static_cast<TW>(Op::f(ctx, static_cast<TW>(lane<TN>(a, first + i)),
                                           static_cast<TW>(lane<TN>(b, first + i)))));
    return r;
}

template <class TN, class TW, class Op>
inline V128 l3(CpuState* ctx, const V128& d, const V128& a, const V128& b, unsigned first, unsigned n) {
    V128 r = zero();
    for (unsigned i = 0; i < n; ++i)
        set_lane<TW>(r, i,
                     static_cast<TW>(Op::f(ctx, lane<TW>(d, i), static_cast<TW>(lane<TN>(a, first + i)),
                                           static_cast<TW>(lane<TN>(b, first + i)))));
    return r;
}

template <class TN, class TW, class Op>
inline V128 l1i(CpuState* ctx, const V128& a, unsigned first, unsigned n, int s) {
    V128 r = zero();
    for (unsigned i = 0; i < n; ++i)
        set_lane<TW>(r, i, static_cast<TW>(Op::f(ctx, static_cast<TW>(lane<TN>(a, first + i)), s)));
    return r;
}

// Wide: wide lanes of a with widened narrow lanes of b (SADDW etc.).
template <class TN, class TW, class Op>
inline V128 w2(CpuState* ctx, const V128& a, const V128& b, unsigned first, unsigned n) {
    V128 r = zero();
    for (unsigned i = 0; i < n; ++i)
        set_lane<TW>(r, i, static_cast<TW>(Op::f(ctx, lane<TW>(a, i), static_cast<TW>(lane<TN>(b, first + i)))));
    return r;
}

// Narrow results: n narrow lanes written to the low half (part = false, upper
// half cleared) or to the upper half with the low half of d kept (part = true).
inline V128 put_part(const V128& d, const V128& narrow, bool part) {
    return part ? V128{d.lo, narrow.lo} : narrow;
}

template <class TW, class TN, class Op>
inline V128 n1(CpuState* ctx, const V128& d, const V128& a, unsigned n, bool part) {
    V128 r = zero();
    for (unsigned i = 0; i < n; ++i)
        set_lane<TN>(r, i, Op::template f<TN>(ctx, lane<TW>(a, i)));
    return put_part(d, r, part);
}

template <class TW, class TN, class Op>
inline V128 n1i(CpuState* ctx, const V128& d, const V128& a, unsigned n, bool part, int s) {
    V128 r = zero();
    for (unsigned i = 0; i < n; ++i)
        set_lane<TN>(r, i, Op::template f<TN>(ctx, lane<TW>(a, i), s));
    return put_part(d, r, part);
}

template <class TW, class TN, class Op>
inline V128 n2(CpuState* ctx, const V128& d, const V128& a, const V128& b, unsigned n, bool part) {
    V128 r = zero();
    for (unsigned i = 0; i < n; ++i)
        set_lane<TN>(r, i, Op::template f<TN>(ctx, lane<TW>(a, i), lane<TW>(b, i)));
    return put_part(d, r, part);
}

// SADDLP/UADDLP (acc = false) and SADALP/UADALP (acc = true), n wide result lanes.
template <class TN, class TW>
inline V128 paddl(const V128& d, const V128& a, unsigned n, bool acc) {
    V128 r = zero();
    for (unsigned i = 0; i < n; ++i) {
        TW s = idetail::wrap_add(static_cast<TW>(lane<TN>(a, 2 * i)), static_cast<TW>(lane<TN>(a, 2 * i + 1)));
        if (acc) s = idetail::wrap_add(s, lane<TW>(d, i));
        set_lane<TW>(r, i, s);
    }
    return r;
}

// Across lanes: ADDV, [SU]MAXV, [SU]MINV over n lanes; scalar result.
template <class T, class Op>
inline V128 across(CpuState* ctx, const V128& a, unsigned n) {
    T acc = lane<T>(a, 0);
    for (unsigned i = 1; i < n; ++i)
        acc = static_cast<T>(Op::f(ctx, acc, lane<T>(a, i)));
    V128 r = zero();
    set_lane<T>(r, 0, acc);
    return r;
}

// [SU]ADDLV: sum of n narrow lanes in the wide type; scalar result.
template <class TN, class TW>
inline V128 acrossl(const V128& a, unsigned n) {
    TW acc = TW(0);
    for (unsigned i = 0; i < n; ++i)
        acc = idetail::wrap_add(acc, static_cast<TW>(lane<TN>(a, i)));
    V128 r = zero();
    set_lane<TW>(r, 0, acc);
    return r;
}

// Lane idx of v in every lane (operand of the by-element forms).
template <class T>
inline V128 bcast(const V128& v, unsigned idx) {
    const T x = lane<T>(v, idx);
    V128 r = zero();
    for (unsigned i = 0; i < 16 / sizeof(T); ++i)
        set_lane<T>(r, i, x);
    return r;
}

// REV16/REV32/REV64: reverse the elements in each container; cmask = lanes per container - 1.
template <class T>
inline V128 rev(const V128& a, unsigned n, unsigned cmask) {
    V128 r = zero();
    for (unsigned i = 0; i < n; ++i)
        set_lane<T>(r, i, lane<T>(a, i ^ cmask));
    return r;
}

// PMULL/PMULL2: 8x8 -> 16 bit polynomial products of the selected half.
inline V128 pmull8(const V128& a, const V128& b, bool part) {
    V128 r = zero();
    const unsigned first = part ? 8 : 0;
    for (unsigned i = 0; i < 8; ++i) {
        const unsigned x = lane<uint8_t>(a, first + i), y = lane<uint8_t>(b, first + i);
        unsigned p = 0;
        for (int k = 0; k < 8; ++k)
            if ((y >> k) & 1) p ^= x << k;
        set_lane<uint16_t>(r, i, static_cast<uint16_t>(p));
    }
    return r;
}

// PMULL/PMULL2 (1Q): 64x64 -> 128 bit polynomial product (Crypto extension).
inline V128 pmull64(const V128& a, const V128& b, bool part) {
    const uint64_t x = part ? a.hi : a.lo, y = part ? b.hi : b.lo;
    uint64_t lo = 0, hi = 0;
    for (int k = 0; k < 64; ++k) {
        if ((y >> k) & 1) {
            lo ^= x << k;
            if (k) hi ^= x >> (64 - k);
        }
    }
    return V128{lo, hi};
}

}  // namespace lgp::lifted::simd
