// Scalar floating-point semantics of A64 for lifted code (P2.4): FPCR
// rounding modes, flush-to-zero (FZ), default NaN (DN), alternative half
// precision (AHP), and the Arm NaN propagation rules. Shared by the scalar FP
// instructions and the per-lane vector FP instructions.
//
// Values are passed as raw bit patterns (uint16_t half, uint32_t single,
// uint64_t double), never as host floats across function boundaries, so no
// host can quiet or canonicalize a NaN on the way.
//
// Two paths per arithmetic operation:
//   - fast: FPCR.RMode = RN and FPCR.FZ = 0 (the normal case). The host's
//     IEEE-754 arithmetic gives the Arm result for every non-NaN outcome;
//     NaN results are rebuilt with the Arm rules (operand order, quieting,
//     DN, positive default NaN 0x7FC00000 where x86 would give 0xFFC00000,
//     WebAssembly any NaN).
//   - soft: any other rounding mode or FZ = 1. A bit-exact software
//     implementation of the Arm ARM pseudocode (FPUnpack, FPRound).
// Conversions, FRINT*, estimates and comparisons always use exact integer
// code, which does not depend on the host's rounding mode.
//
// Not modelled: the cumulative IEEE exception flags in FPSR (IOC, DZC, OFC,
// UFC, IXC, IDC). The Switch's A57 has no FP exception traps, so they are
// only visible through MRS FPSR; see docs/knowledge/a64-fp-simd-semantik.md.
//
// Requires a build without FMA contraction and fast-math (CMake target
// lgp_lifted_headers sets -ffp-contract=off).
#pragma once

#include <bit>
#include <cmath>
#include <cstdint>
#if LGP_FP_STATS
#include <atomic>
#endif
#include <type_traits>
#include <utility>  // std::swap (MSVC STL only declares it in <type_traits>)

#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace lgp::lifted::fp {

// ---- FPCR -------------------------------------------------------------------
inline constexpr uint32_t kFpcrAhp = 1u << 26;
inline constexpr uint32_t kFpcrDn = 1u << 25;
inline constexpr uint32_t kFpcrFz = 1u << 24;
inline constexpr uint32_t kFpcrRModeShift = 22;
// LGP_FP_HOST_NAN (CMake option, default on for lgp_game only): the host fast paths return the
// host's NaN instead of rebuilding the Arm NaN (operand priority, quieting,
// positive default NaN). NaN-ness, comparisons and conversions stay exact;
// only the sign and payload bits of a NaN result can differ (x86 produces a
// negative default NaN, WebAssembly leaves them unspecified). The per-result
// NaN test made vector FP code in WebAssembly up to 4.5 times slower
// (docs/knowledge/lifted-code-performance.md). FPCR.DN = 1 then takes the
// software path, which handles it exactly. All translation units of one
// program must agree on it (the helpers are inline).
#ifndef LGP_FP_HOST_NAN
#define LGP_FP_HOST_NAN 0
#endif
inline constexpr bool kHostNan = LGP_FP_HOST_NAN != 0;
// Diagnosis: LGP_FP_HOST_NAN_CLASSES limits host NaN results to some operation
// classes (bit mask of HostNanClass); the other classes take the exact Arm NaN
// path again. Bisects which operation makes a host NaN visible (arm64 money
// text in the bag, 2026-10-28). Default: all classes.
#ifndef LGP_FP_HOST_NAN_CLASSES
#define LGP_FP_HOST_NAN_CLASSES 0xffffffffu
#endif
enum HostNanClass : uint32_t {
    kHostNanAdd = 1u << 0,     // FADD (scalar)
    kHostNanSub = 1u << 1,     // FSUB (scalar)
    kHostNanMul = 1u << 2,     // FMUL (scalar)
    kHostNanDiv = 1u << 3,     // FDIV (scalar)
    kHostNanSqrt = 1u << 4,    // FSQRT (scalar)
    kHostNanVector = 1u << 5,  // whole-vector + - * / (simd_fp.hpp)
};
inline constexpr bool host_nan(uint32_t cls) { return kHostNan && (uint32_t{LGP_FP_HOST_NAN_CLASSES} & cls) != 0; }
inline constexpr uint32_t kFpcrSoftMask = kFpcrFz | (3u << kFpcrRModeShift) | (kHostNan ? kFpcrDn : 0u);

// Rounding modes; 0..3 equal the FPCR.RMode encoding.
enum Rounding : uint8_t {
    kTieEven = 0,  // RN
    kPosInf = 1,   // RP
    kNegInf = 2,   // RM
    kZero = 3,     // RZ
    kTieAway = 4,  // FCVTA*/FRINTA
    kOdd = 5,      // FCVTXN
};

inline Rounding fpcr_rounding(uint32_t fpcr) { return static_cast<Rounding>((fpcr >> kFpcrRModeShift) & 3); }
// LGP_FP_STATS (CMake option): count how many FP operations take the
// software path and which FPCR values sent them there (P6 measurement).
#if LGP_FP_STATS
struct FpStats {
    std::atomic<uint64_t> ops{0}, soft{0};
    std::atomic<uint32_t> fpcr_seen{0};  // OR of the FPCR values on the software path
};
inline FpStats g_fp_stats;
#endif

inline bool fast_path(uint32_t fpcr) {
    const bool fast = (fpcr & kFpcrSoftMask) == 0;
#if LGP_FP_STATS
    g_fp_stats.ops.fetch_add(1, std::memory_order_relaxed);
    if (!fast) {
        g_fp_stats.soft.fetch_add(1, std::memory_order_relaxed);
        g_fp_stats.fpcr_seen.fetch_or(fpcr, std::memory_order_relaxed);
    }
#endif
    return fast;
}

// A whole vector on the host fast path counts as one operation per used lane,
// like the lane-by-lane path. The vector code tests FPCR without fast_path()
// so that a fallback to that path is not counted twice.
inline void count_fast_lanes([[maybe_unused]] unsigned lanes) {
#if LGP_FP_STATS
    g_fp_stats.ops.fetch_add(lanes, std::memory_order_relaxed);
#endif
}

// ---- formats ------------------------------------------------------------------
template <class U>
struct Fmt;
template <>
struct Fmt<uint16_t> {
    static constexpr int N = 16, E = 5, F = 10;
};
template <>
struct Fmt<uint32_t> {
    static constexpr int N = 32, E = 8, F = 23;
    using Host = float;
};
template <>
struct Fmt<uint64_t> {
    static constexpr int N = 64, E = 11, F = 52;
    using Host = double;
};

template <class U>
inline constexpr U sign_bit() {
    return static_cast<U>(U{1} << (Fmt<U>::N - 1));
}
template <class U>
inline constexpr U exp_mask() {
    return static_cast<U>(((U{1} << Fmt<U>::E) - 1) << Fmt<U>::F);
}
template <class U>
inline constexpr U frac_mask() {
    return static_cast<U>((U{1} << Fmt<U>::F) - 1);
}
template <class U>
inline constexpr U quiet_bit() {
    return static_cast<U>(U{1} << (Fmt<U>::F - 1));
}
template <class U>
inline constexpr U default_nan() {
    return static_cast<U>(exp_mask<U>() | quiet_bit<U>());
}
template <class U>
inline constexpr U infinity(bool sign) {
    return static_cast<U>((sign ? sign_bit<U>() : U{0}) | exp_mask<U>());
}
template <class U>
inline constexpr U zero(bool sign) {
    return sign ? sign_bit<U>() : U{0};
}
template <class U>
inline constexpr U max_normal(bool sign) {
    return static_cast<U>(zero<U>(sign) | (exp_mask<U>() - (U{1} << Fmt<U>::F)) | frac_mask<U>());
}

template <class U>
inline constexpr bool is_nan(U v) {
    return (v & exp_mask<U>()) == exp_mask<U>() && (v & frac_mask<U>()) != 0;
}
template <class U>
inline constexpr bool is_snan(U v) {
    return is_nan(v) && (v & quiet_bit<U>()) == 0;
}
template <class U>
inline constexpr U quiet(U v) {
    return static_cast<U>(v | quiet_bit<U>());
}
template <class U>
inline constexpr U neg(U v) {
    return static_cast<U>(v ^ sign_bit<U>());
}
template <class U>
inline constexpr U abs(U v) {
    return static_cast<U>(v & ~sign_bit<U>());
}

template <class U>
inline typename Fmt<U>::Host to_host(U v) {
    return std::bit_cast<typename Fmt<U>::Host>(v);
}
template <class U>
inline U from_host(typename Fmt<U>::Host h) {
    return std::bit_cast<U>(h);
}

// ---- NaN propagation (FPProcessNaN/FPProcessNaNs/FPProcessNaNs3) ---------------
template <class U>
inline U process_nan(uint32_t fpcr, U a) {
    if (fpcr & kFpcrDn) return default_nan<U>();
    return quiet(a);
}

// Result for two operands of which at least one is a NaN (or an invalid
// operation produced a NaN: then the default NaN).
template <class U>
inline U nan2(uint32_t fpcr, U a, U b) {
    if (is_snan(a)) return process_nan(fpcr, a);
    if (is_snan(b)) return process_nan(fpcr, b);
    if (is_nan(a)) return process_nan(fpcr, a);
    if (is_nan(b)) return process_nan(fpcr, b);
    return default_nan<U>();
}

template <class U>
inline U nan3(uint32_t fpcr, U a, U b, U c) {
    if (is_snan(a)) return process_nan(fpcr, a);
    if (is_snan(b)) return process_nan(fpcr, b);
    if (is_snan(c)) return process_nan(fpcr, c);
    if (is_nan(a)) return process_nan(fpcr, a);
    if (is_nan(b)) return process_nan(fpcr, b);
    if (is_nan(c)) return process_nan(fpcr, c);
    return default_nan<U>();
}

// =============================================================================
// Exact software arithmetic (soft path)
// =============================================================================
namespace soft {

// 128-bit unsigned integer, portable (no __int128 requirement).
struct U128 {
    uint64_t hi = 0, lo = 0;
};
inline bool is_zero(U128 a) { return (a.hi | a.lo) == 0; }
inline int clz(U128 a) { return a.hi ? std::countl_zero(a.hi) : 64 + std::countl_zero(a.lo); }
inline U128 shl(U128 a, int s) {
    if (s <= 0) return a;
    if (s >= 128) return {};
    if (s >= 64) return {a.lo << (s - 64), 0};
    return {(a.hi << s) | (a.lo >> (64 - s)), a.lo << s};
}
// Logical shift right; bits shifted out are ORed into `sticky`.
inline U128 shr(U128 a, int s, bool& sticky) {
    if (s <= 0) return a;
    if (s >= 128) {
        sticky |= !is_zero(a);
        return {};
    }
    if (s >= 64) {
        sticky |= a.lo != 0 || (s > 64 && (a.hi << (128 - s)) != 0);
        return {0, a.hi >> (s - 64)};
    }
    sticky |= (a.lo << (64 - s)) != 0;
    return {a.hi >> s, (a.lo >> s) | (a.hi << (64 - s))};
}
inline U128 add(U128 a, U128 b) {
    U128 r{a.hi + b.hi, a.lo + b.lo};
    if (r.lo < a.lo) ++r.hi;
    return r;
}
inline U128 sub(U128 a, U128 b) {
    U128 r{a.hi - b.hi, a.lo - b.lo};
    if (a.lo < b.lo) --r.hi;
    return r;
}
inline bool less(U128 a, U128 b) { return a.hi < b.hi || (a.hi == b.hi && a.lo < b.lo); }
inline U128 mul64(uint64_t a, uint64_t b) {
    const uint64_t a0 = a & 0xFFFFFFFFu, a1 = a >> 32, b0 = b & 0xFFFFFFFFu, b1 = b >> 32;
    const uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    const uint64_t mid = (p00 >> 32) + (p01 & 0xFFFFFFFFu) + (p10 & 0xFFFFFFFFu);
    return {p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32), (mid << 32) | (p00 & 0xFFFFFFFFu)};
}

enum class Type : uint8_t { Zero, Number, Inf, QNaN, SNaN };

// FPUnpack(): value = (-1)^sign * mant * 2^exp for Type::Number.
struct Value {
    Type type = Type::Zero;
    bool sign = false;
    uint64_t mant = 0;
    int exp = 0;
};

template <class U>
inline Value unpack(U v, uint32_t fpcr) {
    constexpr int E = Fmt<U>::E, F = Fmt<U>::F;
    Value r;
    r.sign = (v & sign_bit<U>()) != 0;
    const uint64_t e = (v & exp_mask<U>()) >> F;
    const uint64_t f = v & frac_mask<U>();
    const int bias = (1 << (E - 1)) - 1;
    if (e == 0) {
        // Denormal inputs are flushed with FZ (single and double only in ARMv8.0).
        if (f == 0 || (Fmt<U>::N != 16 && (fpcr & kFpcrFz))) return r;
        r.type = Type::Number;
        r.mant = f;
        r.exp = 1 - bias - F;
        return r;
    }
    if (e == (1u << E) - 1 && !(Fmt<U>::N == 16 && (fpcr & kFpcrAhp))) {
        if (f == 0) {
            r.type = Type::Inf;
        } else {
            r.type = (f & quiet_bit<U>()) ? Type::QNaN : Type::SNaN;
        }
        return r;
    }
    r.type = Type::Number;
    r.mant = f | (uint64_t{1} << F);
    r.exp = static_cast<int>(e) - bias - F;
    return r;
}

// An exact (or sticky-marked) nonzero real value: (-1)^sign * (m + sticky) * 2^exp,
// where sticky means "plus something strictly between 0 and 1 unit of m's last bit".
struct Exact {
    bool sign = false;
    U128 m;
    int exp = 0;
    bool sticky = false;
};

// FPRound()/FPRoundCV(): rounds a nonzero value to format U.
template <class U>
inline U round(const Exact& x, uint32_t fpcr, Rounding mode) {
    constexpr int N = Fmt<U>::N, E = Fmt<U>::E, F = Fmt<U>::F;
    const bool ahp = N == 16 && (fpcr & kFpcrAhp);
    // Normalize: MSB at bit 127.
    const int lz = clz(x.m);
    const U128 m = shl(x.m, lz);
    // value = 1.xxx * 2^e
    const int e = x.exp + (127 - lz);
    const int minimum_exp = 2 - (1 << (E - 1));
    if (N != 16 && (fpcr & kFpcrFz) && e < minimum_exp) return zero<U>(x.sign);

    int biased_exp = e - minimum_exp + 1;
    int keep = F + 1;  // significand bits kept, including the leading one
    if (biased_exp <= 0) {
        keep -= 1 - biased_exp;  // denormal: shift right by minimum_exp - e
        biased_exp = 0;
    }
    uint64_t int_mant = 0;
    U128 rest;  // the discarded bits as a fraction of one unit (MSB = one half)
    bool sticky = x.sticky;
    if (keep > 0) {
        bool unused = false;
        int_mant = shr(m, 128 - keep, unused).lo;
        rest = shl(m, keep);
    } else {
        rest = shr(m, -keep, sticky);
    }
    const bool half_or_more = (rest.hi >> 63) != 0;
    const bool below_half_nonzero = (rest.hi << 1) != 0 || rest.lo != 0 || sticky;
    const bool inexact = half_or_more || below_half_nonzero;

    bool round_up = false;
    switch (mode) {
    case kTieEven: round_up = half_or_more && (below_half_nonzero || (int_mant & 1)); break;
    case kPosInf: round_up = inexact && !x.sign; break;
    case kNegInf: round_up = inexact && x.sign; break;
    case kZero: break;
    case kTieAway: round_up = half_or_more; break;
    case kOdd:
        if (inexact) int_mant |= 1;
        break;
    }
    if (round_up) {
        ++int_mant;
        if (biased_exp == 0 && int_mant == (uint64_t{1} << F)) biased_exp = 1;  // denormal rounded up to normal
        if (int_mant == (uint64_t{1} << (F + 1))) {
            ++biased_exp;
            int_mant >>= 1;
        }
    }

    const U sign = zero<U>(x.sign);
    if (!ahp) {
        if (biased_exp >= (1 << E) - 1) {
            bool to_inf = false;
            switch (mode) {
            case kTieEven:
            case kTieAway: to_inf = true; break;
            case kPosInf: to_inf = !x.sign; break;
            case kNegInf: to_inf = x.sign; break;
            default: break;
            }
            return to_inf ? infinity<U>(x.sign) : max_normal<U>(x.sign);
        }
    } else if (biased_exp >= (1 << E)) {
        return static_cast<U>(sign | ((U{1} << (N - 1)) - 1));
    }
    return static_cast<U>(sign | (static_cast<U>(biased_exp) << F) | (static_cast<U>(int_mant) & frac_mask<U>()));
}

inline Exact exact_of(const Value& v) {
    Exact x;
    x.sign = v.sign;
    x.m = {0, v.mant};
    x.exp = v.exp;
    return x;
}

// Exact sum of two (possibly sticky) values; returns false for an exact zero.
inline bool add_exact(Exact a, Exact b, Exact& out) {
    // Left-justify both to bit 126 (one bit of headroom for the carry).
    auto justify = [](Exact& x) {
        if (is_zero(x.m)) return;
        const int s = clz(x.m) - 1;
        if (s > 0) {
            x.m = shl(x.m, s);
            x.exp -= s;
        } else if (s < 0) {
            x.m = shr(x.m, -s, x.sticky);
            x.exp -= s;
        }
    };
    justify(a);
    justify(b);
    if (is_zero(a.m)) {
        out = b;
        return !is_zero(b.m);
    }
    if (is_zero(b.m)) {
        out = a;
        return true;
    }
    if (a.exp < b.exp || (a.exp == b.exp && less(a.m, b.m))) std::swap(a, b);
    // |a| >= |b| (up to sticky): align b to a.
    bool sticky = b.sticky;
    const U128 bm = shr(b.m, a.exp - b.exp, sticky);
    out.exp = a.exp;
    out.sign = a.sign;
    if (a.sign == b.sign) {
        out.m = add(a.m, bm);
        out.sticky = a.sticky || sticky;
    } else {
        // a - (bm + sticky): borrow one unit so the result stays exact-with-sticky.
        out.m = sub(a.m, bm);
        if (sticky) out.m = sub(out.m, U128{0, 1});
        out.sticky = sticky || a.sticky;
        if (is_zero(out.m) && !out.sticky) return false;
    }
    return true;
}

inline Exact mul_exact(const Value& a, const Value& b) {
    Exact x;
    x.sign = a.sign != b.sign;
    x.m = mul64(a.mant, b.mant);
    x.exp = a.exp + b.exp;
    return x;
}

// Quotient a/b with at least 66 significant bits plus sticky.
inline Exact div_exact(const Value& a, const Value& b) {
    Exact x;
    x.sign = a.sign != b.sign;
    // Normalize both mantissas to 64 bits (MSB at 63).
    const int la = std::countl_zero(a.mant), lb = std::countl_zero(b.mant);
    const uint64_t ma = a.mant << la, mb = b.mant << lb;
    // Long division of ma * 2^66 by mb, bit by bit.
    U128 rem{0, ma};
    U128 q;
    const int qbits = 67;
    U128 d{0, mb};
    // First compare: quotient bit for 2^0 position relative to ma/mb in [0.5, 2).
    for (int i = 0; i < qbits; ++i) {
        q = shl(q, 1);
        if (!less(rem, d)) {
            rem = sub(rem, d);
            q.lo |= 1;
        }
        rem = shl(rem, 1);
    }
    // q = floor(ma/mb * 2^(qbits-1)).
    x.m = q;
    x.sticky = !is_zero(rem);
    x.exp = (a.exp - la) - (b.exp - lb) - (qbits - 1);
    return x;
}

// Square root of a positive value with 64+ significant bits plus sticky.
inline Exact sqrt_exact(const Value& a) {
    Exact x;
    // Make the exponent even and the radicand large: r = mant * 2^s with s even exponent.
    int e = a.exp;
    U128 r{0, a.mant};
    const int lz = clz(r);
    int s = lz - 2;  // leave room
    if (((e - s) & 1) != 0) --s;
    r = shl(r, s);
    e -= s;
    // Integer square root of r (bit by bit), result has ~63 bits.
    U128 res, rem;
    for (int i = 63; i >= 0; --i) {
        // Bring down two bits.
        rem = shl(rem, 2);
        const uint64_t two = i >= 32 ? (r.hi >> (2 * (i - 32))) & 3 : (r.lo >> (2 * i)) & 3;
        rem.lo |= two;
        const U128 trial = add(shl(res, 2), U128{0, 1});
        res = shl(res, 1);
        if (!less(rem, trial)) {
            rem = sub(rem, trial);
            res.lo |= 1;
        }
    }
    x.m = res;
    x.sticky = !is_zero(rem);
    x.exp = e / 2;
    return x;
}

template <class U>
inline U zero_result(uint32_t fpcr, Rounding mode) {
    (void)fpcr;
    return zero<U>(mode == kNegInf);
}

template <class U>
inline U add(uint32_t fpcr, U op1, U op2, bool subtract) {
    const Value a = unpack(op1, fpcr);
    Value b = unpack(op2, fpcr);
    if (a.type >= Type::QNaN || b.type >= Type::QNaN) return nan2(fpcr, op1, op2);
    if (subtract) b.sign = !b.sign;
    const Rounding mode = fpcr_rounding(fpcr);
    if (a.type == Type::Inf && b.type == Type::Inf && a.sign != b.sign) return default_nan<U>();
    if (a.type == Type::Inf) return infinity<U>(a.sign);
    if (b.type == Type::Inf) return infinity<U>(b.sign);
    if (a.type == Type::Zero && b.type == Type::Zero && a.sign == b.sign) return zero<U>(a.sign);
    Exact sum;
    if (!add_exact(a.type == Type::Zero ? Exact{} : exact_of(a), b.type == Type::Zero ? Exact{} : exact_of(b), sum))
        return zero_result<U>(fpcr, mode);
    return round<U>(sum, fpcr, mode);
}

template <class U>
inline U mul(uint32_t fpcr, U op1, U op2, bool mulx) {
    const Value a = unpack(op1, fpcr), b = unpack(op2, fpcr);
    if (a.type >= Type::QNaN || b.type >= Type::QNaN) return nan2(fpcr, op1, op2);
    const bool sign = a.sign != b.sign;
    if ((a.type == Type::Inf && b.type == Type::Zero) || (a.type == Type::Zero && b.type == Type::Inf)) {
        // FMULX: 2.0 with the product's sign instead of the default NaN.
        if (mulx) return static_cast<U>(zero<U>(sign) | (U{1} << (Fmt<U>::N - 2)));
        return default_nan<U>();
    }
    if (a.type == Type::Inf || b.type == Type::Inf) return infinity<U>(sign);
    if (a.type == Type::Zero || b.type == Type::Zero) return zero<U>(sign);
    return round<U>(mul_exact(a, b), fpcr, fpcr_rounding(fpcr));
}

template <class U>
inline U div(uint32_t fpcr, U op1, U op2) {
    const Value a = unpack(op1, fpcr), b = unpack(op2, fpcr);
    if (a.type >= Type::QNaN || b.type >= Type::QNaN) return nan2(fpcr, op1, op2);
    const bool sign = a.sign != b.sign;
    if ((a.type == Type::Inf && b.type == Type::Inf) || (a.type == Type::Zero && b.type == Type::Zero))
        return default_nan<U>();
    if (a.type == Type::Inf || b.type == Type::Zero) return infinity<U>(sign);
    if (a.type == Type::Zero || b.type == Type::Inf) return zero<U>(sign);
    return round<U>(div_exact(a, b), fpcr, fpcr_rounding(fpcr));
}

template <class U>
inline U sqrt(uint32_t fpcr, U op) {
    const Value a = unpack(op, fpcr);
    if (a.type >= Type::QNaN) return process_nan(fpcr, op);
    if (a.type == Type::Zero) return zero<U>(a.sign);
    if (a.sign) return default_nan<U>();
    if (a.type == Type::Inf) return infinity<U>(false);
    return round<U>(sqrt_exact(a), fpcr, fpcr_rounding(fpcr));
}

// FPMulAdd(addend, op1, op2) with an optional final halving (FRSQRTS) and a
// special result for inf * 0 (FRECPS/FRSQRTS); `special` = 0 for plain FMA.
template <class U>
inline U muladd(uint32_t fpcr, U addend, U op1, U op2, bool halve = false, U special = 0, bool has_special = false) {
    const Value c = unpack(addend, fpcr), a = unpack(op1, fpcr), b = unpack(op2, fpcr);
    const bool inf_zero = (a.type == Type::Inf && b.type == Type::Zero) || (a.type == Type::Zero && b.type == Type::Inf);
    if (c.type >= Type::QNaN || a.type >= Type::QNaN || b.type >= Type::QNaN) {
        if (c.type == Type::QNaN && inf_zero) return default_nan<U>();
        return nan3(fpcr, addend, op1, op2);
    }
    const Rounding mode = fpcr_rounding(fpcr);
    const bool sign_p = a.sign != b.sign;
    const bool inf_p = a.type == Type::Inf || b.type == Type::Inf;
    const bool zero_p = a.type == Type::Zero || b.type == Type::Zero;
    if (inf_zero) return has_special ? special : default_nan<U>();
    if (c.type == Type::Inf && inf_p && c.sign != sign_p) return default_nan<U>();
    if ((c.type == Type::Inf && !c.sign) || (inf_p && !sign_p)) return infinity<U>(false);
    if ((c.type == Type::Inf && c.sign) || (inf_p && sign_p)) return infinity<U>(true);
    if (c.type == Type::Zero && zero_p && c.sign == sign_p) return zero<U>(c.sign);
    Exact p;
    if (!zero_p) p = mul_exact(a, b);
    Exact sum;
    if (!add_exact(c.type == Type::Zero ? Exact{} : exact_of(c), p, sum)) return zero_result<U>(fpcr, mode);
    if (halve) --sum.exp;
    return round<U>(sum, fpcr, mode);
}

}  // namespace soft

// =============================================================================
// Arithmetic (fast path + soft path)
// =============================================================================
template <class U>
inline U add(uint32_t fpcr, U a, U b) {
    if (fast_path(fpcr)) {
        const auto r = to_host(a) + to_host(b);
        if (host_nan(kHostNanAdd) || r == r) return from_host<U>(r);
        return nan2(fpcr, a, b);
    }
    return soft::add(fpcr, a, b, false);
}

template <class U>
inline U sub(uint32_t fpcr, U a, U b) {
    if (fast_path(fpcr)) {
        const auto r = to_host(a) - to_host(b);
        if (host_nan(kHostNanSub) || r == r) return from_host<U>(r);
        return nan2(fpcr, a, b);
    }
    return soft::add(fpcr, a, b, true);
}

template <class U>
inline U mul(uint32_t fpcr, U a, U b) {
    if (fast_path(fpcr)) {
        const auto r = to_host(a) * to_host(b);
        if (host_nan(kHostNanMul) || r == r) return from_host<U>(r);
        return nan2(fpcr, a, b);
    }
    return soft::mul(fpcr, a, b, false);
}

// FMULX: like FMUL, but (+-inf) * (+-0) = +-2.0.
template <class U>
inline U mulx(uint32_t fpcr, U a, U b) {
    if (fast_path(fpcr)) {
        const auto r = to_host(a) * to_host(b);
        if (r == r) return from_host<U>(r);
        if (!is_nan(a) && !is_nan(b))
            return static_cast<U>(((a ^ b) & sign_bit<U>()) | (U{1} << (Fmt<U>::N - 2)));  // inf * 0
        return nan2(fpcr, a, b);
    }
    return soft::mul(fpcr, a, b, true);
}

template <class U>
inline U div(uint32_t fpcr, U a, U b) {
    if (fast_path(fpcr)) {
        const auto r = to_host(a) / to_host(b);
        if (host_nan(kHostNanDiv) || r == r) return from_host<U>(r);
        return nan2(fpcr, a, b);
    }
    return soft::div(fpcr, a, b);
}

template <class U>
inline U sqrt(uint32_t fpcr, U a) {
    if (fast_path(fpcr)) {
        const auto r = std::sqrt(to_host(a));
        if (host_nan(kHostNanSqrt) || r == r) return from_host<U>(r);
        return is_nan(a) ? process_nan(fpcr, a) : default_nan<U>();
    }
    return soft::sqrt(fpcr, a);
}

// FPMulAdd(addend, op1, op2) = addend + op1 * op2 with a single rounding.
// FMADD/FMLA call it directly; FMSUB/FNMADD/FNMSUB/FMLS negate operands first
// (the negated operand is what NaN propagation sees, as in the pseudocode).
template <class U>
inline U muladd(uint32_t fpcr, U addend, U op1, U op2) {
    if (fast_path(fpcr)) {
        const auto r = std::fma(to_host(op1), to_host(op2), to_host(addend));
        if (r == r) return from_host<U>(r);
        // QNaN addend with inf * 0 gives the default NaN, not the addend.
        const bool inf_zero = ((op1 & ~sign_bit<U>()) == exp_mask<U>() && (op2 & ~sign_bit<U>()) == 0) ||
                              ((op1 & ~sign_bit<U>()) == 0 && (op2 & ~sign_bit<U>()) == exp_mask<U>());
        if (is_nan(addend) && !is_snan(addend) && inf_zero) return default_nan<U>();
        return nan3(fpcr, addend, op1, op2);
    }
    return soft::muladd(fpcr, addend, op1, op2);
}

// FRECPS: 2.0 - op1 * op2 (fused); inf * 0 gives +2.0.
template <class U>
inline U recip_step(uint32_t fpcr, U op1, U op2) {
    constexpr U two = static_cast<U>(U{1} << (Fmt<U>::N - 2));
    return soft::muladd(fpcr, two, neg(op1), op2, false, two, true);
}

// FRSQRTS: (3.0 - op1 * op2) / 2 (fused); inf * 0 gives +1.5.
template <class U>
inline U rsqrt_step(uint32_t fpcr, U op1, U op2) {
    constexpr int F = Fmt<U>::F;
    constexpr U bias = static_cast<U>((U{1} << (Fmt<U>::E - 1)) - 1);
    constexpr U three = static_cast<U>(((bias + 1) << F) | (U{1} << (F - 1)));
    constexpr U one_half = static_cast<U>((bias << F) | (U{1} << (F - 1)));
    return soft::muladd(fpcr, three, neg(op1), op2, true, one_half, true);
}

// ---- max/min (FPMax, FPMin, FPMaxNum, FPMinNum) -----------------------------
template <class U>
inline U max(uint32_t fpcr, U op1, U op2) {
    const soft::Value a = soft::unpack(op1, fpcr), b = soft::unpack(op2, fpcr);
    if (a.type >= soft::Type::QNaN || b.type >= soft::Type::QNaN) return nan2(fpcr, op1, op2);
    if (a.type == soft::Type::Zero && b.type == soft::Type::Zero) return zero<U>(a.sign && b.sign);
    // Compare as ordered values; flushed denormals compare as zero.
    const U x = a.type == soft::Type::Zero ? zero<U>(a.sign) : op1;
    const U y = b.type == soft::Type::Zero ? zero<U>(b.sign) : op2;
    if constexpr (Fmt<U>::N == 16) {
        return x;  // not reachable: no half-precision arithmetic in ARMv8.0
    } else {
        return to_host(x) > to_host(y) ? x : y;
    }
}

template <class U>
inline U min(uint32_t fpcr, U op1, U op2) {
    const soft::Value a = soft::unpack(op1, fpcr), b = soft::unpack(op2, fpcr);
    if (a.type >= soft::Type::QNaN || b.type >= soft::Type::QNaN) return nan2(fpcr, op1, op2);
    if (a.type == soft::Type::Zero && b.type == soft::Type::Zero) return zero<U>(a.sign || b.sign);
    const U x = a.type == soft::Type::Zero ? zero<U>(a.sign) : op1;
    const U y = b.type == soft::Type::Zero ? zero<U>(b.sign) : op2;
    if constexpr (Fmt<U>::N == 16) {
        return x;
    } else {
        return to_host(x) < to_host(y) ? x : y;
    }
}

// FMAXNM/FMINNM: a quiet NaN loses against a number.
template <class U>
inline U maxnum(uint32_t fpcr, U a, U b) {
    const bool qa = is_nan(a) && !is_snan(a), qb = is_nan(b) && !is_snan(b);
    if (qa && !qb) a = infinity<U>(true);
    else if (!qa && qb) b = infinity<U>(true);
    return max(fpcr, a, b);
}

template <class U>
inline U minnum(uint32_t fpcr, U a, U b) {
    const bool qa = is_nan(a) && !is_snan(a), qb = is_nan(b) && !is_snan(b);
    if (qa && !qb) a = infinity<U>(false);
    else if (!qa && qb) b = infinity<U>(false);
    return min(fpcr, a, b);
}

// ---- comparisons ----------------------------------------------------------------
// FPCompare(): NZCV in bits 3..0 (N Z C V).
template <class U>
inline uint32_t compare(uint32_t fpcr, U op1, U op2) {
    const soft::Value a = soft::unpack(op1, fpcr), b = soft::unpack(op2, fpcr);
    if (a.type >= soft::Type::QNaN || b.type >= soft::Type::QNaN) return 0b0011;
    const U x = a.type == soft::Type::Zero ? U{0} : op1;
    const U y = b.type == soft::Type::Zero ? U{0} : op2;
    const auto hx = to_host(x), hy = to_host(y);
    if (hx == hy) return 0b0110;
    if (hx < hy) return 0b1000;
    return 0b0010;
}

// FCMEQ/FCMGE/FCMGT (and FACGE/FACGT on absolute values): false if unordered.
template <class U>
inline bool cmp_eq(uint32_t fpcr, U a, U b) {
    return compare(fpcr, a, b) == 0b0110;
}
template <class U>
inline bool cmp_ge(uint32_t fpcr, U a, U b) {
    const uint32_t c = compare(fpcr, a, b);
    return c == 0b0110 || c == 0b0010;
}
template <class U>
inline bool cmp_gt(uint32_t fpcr, U a, U b) {
    return compare(fpcr, a, b) == 0b0010;
}

// =============================================================================
// Conversions
// =============================================================================

// FPConvert() between half, single and double (FCVT, FCVTL, FCVTN, FCVTXN).
template <class D, class S>
inline D convert(uint32_t fpcr, S op, Rounding mode) {
    constexpr int M = Fmt<D>::N, N = Fmt<S>::N;
    const bool alt_hp = M == 16 && (fpcr & kFpcrAhp);
    // FPUnpackCV: the source half format follows AHP, single/double flush with FZ.
    const soft::Value v = soft::unpack(op, fpcr);
    if (v.type >= soft::Type::QNaN) {
        if (alt_hp) return zero<D>(v.sign);
        if (fpcr & kFpcrDn) return default_nan<D>();
        // FPConvertNaN(): keep the sign and the top fraction bits, force quiet.
        uint64_t frac;  // 51 bits below the quiet bit
        if constexpr (N == 16)
            frac = static_cast<uint64_t>(op & 0x1FF) << 42;
        else if constexpr (N == 32)
            frac = static_cast<uint64_t>(op & 0x3FFFFF) << 29;
        else
            frac = static_cast<uint64_t>(op) & 0x7FFFFFFFFFFFFull;
        const D sign = zero<D>(v.sign);
        if constexpr (M == 16)
            return static_cast<D>(sign | 0x7E00 | (frac >> 42));
        else if constexpr (M == 32)
            return static_cast<D>(sign | 0x7FC00000u | (frac >> 29));
        else
            return static_cast<D>(sign | 0x7FF8000000000000ull | frac);
    }
    if (v.type == soft::Type::Inf) {
        if (alt_hp) return static_cast<D>(zero<D>(v.sign) | 0x7FFF);
        return infinity<D>(v.sign);
    }
    if (v.type == soft::Type::Zero) return zero<D>(v.sign);
    return soft::round<D>(soft::exact_of(v), fpcr, mode);
}

// FPRoundInt(): FRINTN/P/M/Z/A/I/X.
template <class U>
inline U round_int(uint32_t fpcr, U op, Rounding mode) {
    const soft::Value v = soft::unpack(op, fpcr);
    if (v.type >= soft::Type::QNaN) return process_nan(fpcr, op);
    if (v.type == soft::Type::Inf) return infinity<U>(v.sign);
    if (v.type == soft::Type::Zero) return zero<U>(v.sign);
    if (v.exp >= 0) return op;  // already integral
    // value = mant * 2^exp with exp < 0: split into integer part and fraction.
    const int sh = -v.exp;
    uint64_t ip = 0, frac_hi = 0;  // frac_hi: fraction with MSB = one half
    bool frac_lo = false;          // nonzero bits below frac_hi
    if (sh >= 64) {
        frac_lo = true;  // 0 < value < 2^53 * 2^-64: below one half
    } else {
        ip = v.mant >> sh;
        frac_hi = v.mant << (64 - sh);
    }
    const bool half = (frac_hi >> 63) != 0;
    const bool below = (frac_hi << 1) != 0 || frac_lo;
    const bool inexact = frac_hi != 0 || frac_lo;
    bool up = false;
    switch (mode) {
    case kTieEven: up = half && (below || (ip & 1)); break;
    case kPosInf: up = inexact && !v.sign; break;
    case kNegInf: up = inexact && v.sign; break;
    case kZero: break;
    case kTieAway: up = half; break;
    case kOdd: break;
    }
    if (up) ++ip;
    if (ip == 0) return zero<U>(v.sign);
    soft::Exact x;
    x.sign = v.sign;
    x.m = {0, ip};
    x.exp = 0;
    return soft::round<U>(x, fpcr, kZero);  // exact: an integer below 2^F+1 in magnitude
}

// FPToFixed(): float -> (un)signed integer with `fbits` fraction bits,
// saturating to `width` bits (32 or 64). Returns the integer zero-extended.
template <class U>
inline uint64_t to_fixed(uint32_t fpcr, U op, int fbits, bool is_unsigned, int width, Rounding mode) {
    const soft::Value v = soft::unpack(op, fpcr);
    if (v.type >= soft::Type::QNaN || v.type == soft::Type::Zero) return 0;
    const uint64_t umax = width == 64 ? ~0ull : ((1ull << width) - 1);
    const uint64_t smax = umax >> 1, smin_mag = smax + 1;
    auto saturate = [&](bool sign) -> uint64_t {
        if (is_unsigned) return sign ? 0 : umax;
        return sign ? ((~smax) & umax) : smax;
    };
    if (v.type == soft::Type::Inf) return saturate(v.sign);
    // |value| * 2^fbits = mant * 2^(exp + fbits)
    const int e = v.exp + fbits;
    uint64_t ip;
    bool half = false, below = false;
    if (e >= 0) {
        if (e >= 64 || std::countl_zero(v.mant) < e) return saturate(v.sign);
        ip = v.mant << e;
    } else if (e > -64) {
        const int sh = -e;
        ip = v.mant >> sh;
        const uint64_t frac = v.mant << (64 - sh);
        half = (frac >> 63) != 0;
        below = (frac << 1) != 0;
    } else {
        ip = 0;
        half = e == -64 && (v.mant >> 63) != 0;
        below = e == -64 ? (v.mant << 1) != 0 : v.mant != 0;
    }
    const bool inexact = half || below;
    bool up = false;
    switch (mode) {
    case kTieEven: up = half && (below || (ip & 1)); break;
    case kPosInf: up = inexact && !v.sign; break;
    case kNegInf: up = inexact && v.sign; break;
    case kZero: break;
    case kTieAway: up = half; break;
    case kOdd: break;
    }
    if (up) {
        if (ip == ~0ull) return saturate(v.sign);
        ++ip;
    }
    if (is_unsigned) {
        if (v.sign) return 0;  // negative values (that did not round to zero) saturate to 0
        return ip > umax ? umax : ip;
    }
    if (v.sign) {
        if (ip > smin_mag) return saturate(true);
        return (0 - ip) & umax;
    }
    return ip > smax ? smax : ip;
}

// FixedToFP(): (un)signed `width`-bit integer with `fbits` fraction bits -> float.
template <class U>
inline U from_fixed(uint32_t fpcr, uint64_t v, int fbits, bool is_unsigned, int width, Rounding mode) {
    if (width < 64) v &= (1ull << width) - 1;
    bool sign = false;
    if (!is_unsigned && width > 0 && ((v >> (width - 1)) & 1)) {
        sign = true;
        v = (0 - v) & (width == 64 ? ~0ull : ((1ull << width) - 1));
        if (v == 0) v = 1ull << (width - 1);  // INT_MIN
    }
    if (v == 0) return zero<U>(false);
    soft::Exact x;
    x.sign = sign;
    x.m = {0, v};
    x.exp = -fbits;
    return soft::round<U>(x, fpcr, mode);
}

// =============================================================================
// Estimates (FRECPE, FRSQRTE, FRECPX) and their integer forms (URECPE, URSQRTE)
// =============================================================================

// RecipEstimate(): 256 <= a < 512 -> 256 <= r < 512.
inline uint32_t recip_estimate_int(uint32_t a) {
    a = a * 2 + 1;
    const uint32_t b = (1u << 19) / a;
    return (b + 1) / 2;
}

// RecipSqrtEstimate(): 128 <= a < 512 -> 256 <= r < 512.
inline uint32_t rsqrt_estimate_int(uint32_t a) {
    if (a < 256) {
        a = a * 2 + 1;
    } else {
        a = (a >> 1) << 1;
        a = (a + 1) * 2;
    }
    // The pseudocode counts b up from 512 while a * (b + 1)^2 < 2^28, up to
    // ~500 steps per lane (FRSQRTE/URSQRTE were the hottest soft-float leaf on
    // the Apple TV). Same result in closed form: c is the smallest integer
    // with a * c^2 >= 2^28, found from the host square root and corrected to
    // be exact; b = max(512, c - 1). tests/lifter/test_fp_estimate.cpp
    // compares every input with the loop.
    constexpr uint64_t kLimit = uint64_t{1} << 28;
    const uint64_t wide = a;
    uint64_t c = static_cast<uint64_t>(std::sqrt(static_cast<double>(kLimit) / static_cast<double>(a))) + 1;
    while (c > 1 && wide * (c - 1) * (c - 1) >= kLimit)
        --c;
    while (wide * c * c < kLimit)
        ++c;
    const uint64_t b = c - 1 < 512 ? 512 : c - 1;
    return static_cast<uint32_t>((b + 1) / 2);
}

template <class U>
inline U recip_estimate(uint32_t fpcr, U op) {
    constexpr int N = Fmt<U>::N, F = Fmt<U>::F;
    const soft::Value v = soft::unpack(op, fpcr);
    if (v.type >= soft::Type::QNaN) return process_nan(fpcr, op);
    if (v.type == soft::Type::Inf) return zero<U>(v.sign);
    if (v.type == soft::Type::Zero) return infinity<U>(v.sign);
    const uint64_t e_bits = (op & exp_mask<U>()) >> F;
    uint64_t fraction = static_cast<uint64_t>(op & frac_mask<U>()) << (52 - F);  // 52-bit fraction
    // |value| < 2^-128 (single) / 2^-1024 (double): the result overflows.
    const bool tiny = (N == 32) ? (e_bits == 0 && (fraction >> 50) == 0) : (e_bits == 0 && (fraction >> 50) == 0);
    if (tiny) {
        bool to_inf = false;
        switch (fpcr_rounding(fpcr)) {
        case kTieEven: to_inf = true; break;
        case kPosInf: to_inf = !v.sign; break;
        case kNegInf: to_inf = v.sign; break;
        default: break;
        }
        return to_inf ? infinity<U>(v.sign) : max_normal<U>(v.sign);
    }
    if ((fpcr & kFpcrFz) && ((N == 32 && e_bits >= 253) || (N == 64 && e_bits >= 2045))) return zero<U>(v.sign);
    int exp = static_cast<int>(e_bits);
    if (exp == 0) {
        if (((fraction >> 51) & 1) == 0) {
            exp = -1;
            fraction = (fraction << 2) & ((1ull << 52) - 1);
        } else {
            fraction = (fraction << 1) & ((1ull << 52) - 1);
        }
    }
    const uint32_t scaled = static_cast<uint32_t>(0x100 | (fraction >> 44));
    int result_exp = (N == 32 ? 253 : 2045) - exp;
    const uint32_t estimate = recip_estimate_int(scaled);
    fraction = static_cast<uint64_t>(estimate & 0xFF) << 44;
    if (result_exp == 0) {
        fraction = (1ull << 51) | (fraction >> 1);
    } else if (result_exp == -1) {
        fraction = (1ull << 50) | (fraction >> 2);
        result_exp = 0;
    }
    return static_cast<U>(zero<U>(v.sign) | (static_cast<U>(result_exp) << F) | static_cast<U>(fraction >> (52 - F)));
}

template <class U>
inline U rsqrt_estimate(uint32_t fpcr, U op) {
    constexpr int N = Fmt<U>::N, F = Fmt<U>::F;
    const soft::Value v = soft::unpack(op, fpcr);
    if (v.type >= soft::Type::QNaN) return process_nan(fpcr, op);
    if (v.type == soft::Type::Zero) return infinity<U>(v.sign);
    if (v.sign) return default_nan<U>();
    if (v.type == soft::Type::Inf) return zero<U>(false);
    int exp = static_cast<int>((op & exp_mask<U>()) >> F);
    uint64_t fraction = static_cast<uint64_t>(op & frac_mask<U>()) << (52 - F);
    if (exp == 0) {
        while (((fraction >> 51) & 1) == 0) {
            fraction = (fraction << 1) & ((1ull << 52) - 1);
            --exp;
        }
        fraction = (fraction << 1) & ((1ull << 52) - 1);
    }
    uint32_t scaled;
    if ((exp & 1) == 0)
        scaled = static_cast<uint32_t>(0x100 | (fraction >> 44));
    else
        scaled = static_cast<uint32_t>(0x80 | (fraction >> 45));
    const int result_exp = ((N == 32 ? 380 : 3068) - exp) / 2;
    const uint32_t estimate = rsqrt_estimate_int(scaled);
    return static_cast<U>((static_cast<U>(result_exp) << F) | (static_cast<U>(estimate & 0xFF) << (F - 8)));
}

// FRECPX: exponent inverted, fraction cleared.
template <class U>
inline U recpx(uint32_t fpcr, U op) {
    constexpr int E = Fmt<U>::E, F = Fmt<U>::F;
    const soft::Value v = soft::unpack(op, fpcr);
    if (v.type >= soft::Type::QNaN) return process_nan(fpcr, op);
    const U max_exp = static_cast<U>((U{1} << E) - 2);
    const U e = static_cast<U>((op & exp_mask<U>()) >> F);
    const U r = e == 0 ? max_exp : static_cast<U>(~e & ((U{1} << E) - 1));
    return static_cast<U>(zero<U>(v.sign) | static_cast<U>(r << F));
}

// URECPE: unsigned 32-bit fixed-point reciprocal estimate.
inline uint32_t urecpe(uint32_t op) {
    if ((op >> 31) == 0) return 0xFFFFFFFFu;
    return recip_estimate_int(op >> 23) << 23;
}

// URSQRTE: unsigned 32-bit fixed-point reciprocal square root estimate.
inline uint32_t ursqrte(uint32_t op) {
    if ((op >> 30) == 0) return 0xFFFFFFFFu;
    return rsqrt_estimate_int(op >> 23) << 23;
}

}  // namespace lgp::lifted::fp
