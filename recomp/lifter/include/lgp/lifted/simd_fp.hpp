// Advanced SIMD floating-point operations (per lane, built on fp.hpp) (P2.4).
// Header-only helpers called by lifted code; see simd_common.hpp for lane access.
//
// Every operation honours FPCR (RMode, FZ, DN, AHP) exactly like the scalar
// FP instructions: the per-lane work is done by lgp::lifted::fp on raw bits.
// Vector helpers take their operands by value and build the result in a
// temporary, so the destination may alias any source register.
#pragma once

#include <cstdint>
#include <cstring>
#include <type_traits>

#include "lgp/lifted/fp.hpp"
#include "lgp/lifted/simd_common.hpp"

namespace lgp::lifted::simd {

// Two-operand lane operations (three same, by element, pairwise, across lanes).
enum class FpBin : uint8_t {
    Add,
    Sub,
    Mul,
    Mulx,
    Div,
    Max,
    Min,
    MaxNum,
    MinNum,
    Abd,     // FPAbs(FPSub(n, m))
    RecpS,   // FRECPS
    RsqrtS,  // FRSQRTS
    CmEq,    // all-ones / all-zeros lane results
    CmGe,
    CmGt,
    AcGe,  // compares of FPAbs values
    AcGt,
};

template <class U>
inline constexpr U lane_mask(bool b) {
    return b ? static_cast<U>(~U{0}) : U{0};
}

template <FpBin Op, class U>
inline U fp2(uint32_t fpcr, U a, U b) {
    if constexpr (Op == FpBin::Add) return fp::add<U>(fpcr, a, b);
    else if constexpr (Op == FpBin::Sub) return fp::sub<U>(fpcr, a, b);
    else if constexpr (Op == FpBin::Mul) return fp::mul<U>(fpcr, a, b);
    else if constexpr (Op == FpBin::Mulx) return fp::mulx<U>(fpcr, a, b);
    else if constexpr (Op == FpBin::Div) return fp::div<U>(fpcr, a, b);
    else if constexpr (Op == FpBin::Max) return fp::max<U>(fpcr, a, b);
    else if constexpr (Op == FpBin::Min) return fp::min<U>(fpcr, a, b);
    else if constexpr (Op == FpBin::MaxNum) return fp::maxnum<U>(fpcr, a, b);
    else if constexpr (Op == FpBin::MinNum) return fp::minnum<U>(fpcr, a, b);
    else if constexpr (Op == FpBin::Abd) return fp::abs<U>(fp::sub<U>(fpcr, a, b));
    else if constexpr (Op == FpBin::RecpS) return fp::recip_step<U>(fpcr, a, b);
    else if constexpr (Op == FpBin::RsqrtS) return fp::rsqrt_step<U>(fpcr, a, b);
    else if constexpr (Op == FpBin::CmEq) return lane_mask<U>(fp::cmp_eq<U>(fpcr, a, b));
    else if constexpr (Op == FpBin::CmGe) return lane_mask<U>(fp::cmp_ge<U>(fpcr, a, b));
    else if constexpr (Op == FpBin::CmGt) return lane_mask<U>(fp::cmp_gt<U>(fpcr, a, b));
    else if constexpr (Op == FpBin::AcGe) return lane_mask<U>(fp::cmp_ge<U>(fpcr, fp::abs<U>(a), fp::abs<U>(b)));
    else return lane_mask<U>(fp::cmp_gt<U>(fpcr, fp::abs<U>(a), fp::abs<U>(b)));
}

// Whole-vector fast path for + - * / (FPCR.RMode = RN, FZ = 0): every lane in
// host arithmetic without branches, so the compiler can use host SIMD (SSE,
// NEON, wasm simd128). Host and Arm agree bit for bit except for NaN results;
// if any used lane is NaN the caller redoes the vector lane by lane (not with
// LGP_FP_HOST_NAN, fp.hpp). Lanes above a 64-bit vector are computed on
// whatever is there and discarded.
template <FpBin Op, class U>
inline constexpr bool kHostVectorOp = (Op == FpBin::Add || Op == FpBin::Sub || Op == FpBin::Mul || Op == FpBin::Div) &&
                                      (std::is_same_v<U, uint32_t> || std::is_same_v<U, uint64_t>);

template <FpBin Op, class U>
inline bool fp2v_host(const V128& a, const V128& b, bool q, V128& out) {
    using H = typename fp::Fmt<U>::Host;
    constexpr unsigned kLanes = 16 / sizeof(U);
    H x[kLanes], y[kLanes], r[kLanes];
    std::memcpy(x, &a, 16);
    std::memcpy(y, &b, 16);
    for (unsigned i = 0; i < kLanes; ++i) {
        if constexpr (Op == FpBin::Add) r[i] = x[i] + y[i];
        else if constexpr (Op == FpBin::Sub) r[i] = x[i] - y[i];
        else if constexpr (Op == FpBin::Mul) r[i] = x[i] * y[i];
        else r[i] = x[i] / y[i];
    }
    if constexpr (!fp::host_nan(fp::kHostNanVector)) {
        const unsigned used = lanes_of<U>(q);
        bool nan = false;
        for (unsigned i = 0; i < kLanes; ++i)
            nan |= (i < used) & (r[i] != r[i]);
        if (nan) return false;
    }
    std::memcpy(&out, r, 16);
    if (!q) out.hi = 0;
    return true;
}

// Vector, lane by lane.
template <FpBin Op, class U>
inline V128 fp2v(uint32_t fpcr, V128 a, V128 b, bool q) {
    if constexpr (kHostVectorOp<Op, U>) {
        V128 r;
        if ((fpcr & fp::kFpcrSoftMask) == 0 && fp2v_host<Op, U>(a, b, q, r)) {
            fp::count_fast_lanes(lanes_of<U>(q));
            return r;
        }
    }
    return map2<U>(a, b, q, [fpcr](U x, U y) { return fp2<Op, U>(fpcr, x, y); });
}

// Vector by element: the second operand is one lane of Vm for all lanes.
template <FpBin Op, class U>
inline V128 fp2v_elem(uint32_t fpcr, V128 a, U m, bool q) {
    if constexpr (kHostVectorOp<Op, U>) {
        V128 b, r;
        for (unsigned i = 0; i < 16 / sizeof(U); ++i)
            set_lane<U>(b, i, m);
        if ((fpcr & fp::kFpcrSoftMask) == 0 && fp2v_host<Op, U>(a, b, q, r)) {
            fp::count_fast_lanes(lanes_of<U>(q));
            return r;
        }
    }
    return map1<U>(a, q, [fpcr, m](U x) { return fp2<Op, U>(fpcr, x, m); });
}

// Pairwise over the concatenation b:a (a supplies the lower result lanes).
template <FpBin Op, class U>
inline V128 fp2v_pair(uint32_t fpcr, V128 a, V128 b, bool q) {
    const unsigned n = lanes_of<U>(q), half = n / 2;
    V128 r = zero();
    for (unsigned i = 0; i < n; ++i) {
        const V128& src = i < half ? a : b;
        const unsigned j = 2 * (i % half);
        set_lane<U>(r, i, fp2<Op, U>(fpcr, lane<U>(src, j), lane<U>(src, j + 1)));
    }
    return r;
}

// Scalar pairwise: lanes 0 and 1 of a.
template <FpBin Op, class U>
inline U fp2_pair(uint32_t fpcr, V128 a) {
    return fp2<Op, U>(fpcr, lane<U>(a, 0), lane<U>(a, 1));
}

// Across lanes (4S only in ARMv8.0), in the order of the Arm Reduce():
// op(Reduce(lower half), Reduce(upper half)).
template <FpBin Op>
inline uint32_t fp2_reduce4(uint32_t fpcr, V128 a) {
    const uint32_t lo = fp2<Op, uint32_t>(fpcr, lane<uint32_t>(a, 0), lane<uint32_t>(a, 1));
    const uint32_t hi = fp2<Op, uint32_t>(fpcr, lane<uint32_t>(a, 2), lane<uint32_t>(a, 3));
    return fp2<Op, uint32_t>(fpcr, lo, hi);
}

// FMLA/FMLS: FPMulAdd(d, n, m) and FPMulAdd(d, FPNeg(n), m).
template <bool Sub, class U>
inline U fma(uint32_t fpcr, U d, U n, U m) {
    return fp::muladd<U>(fpcr, d, Sub ? fp::neg<U>(n) : n, m);
}

template <bool Sub, class U>
inline V128 fmav(uint32_t fpcr, V128 d, V128 n, V128 m, bool q) {
    return map3<U>(d, n, m, q, [fpcr](U x, U y, U z) { return fma<Sub, U>(fpcr, x, y, z); });
}

template <bool Sub, class U>
inline V128 fmav_elem(uint32_t fpcr, V128 d, V128 n, U m, bool q) {
    return map2<U>(d, n, q, [fpcr, m](U x, U y) { return fma<Sub, U>(fpcr, x, y, m); });
}

// One-operand lane operations (two-register misc).
enum class FpUn : uint8_t {
    Abs,
    Neg,
    Sqrt,
    RecpE,    // FRECPE
    RsqrtE,   // FRSQRTE
    RecpX,    // FRECPX
    URecpE,   // URECPE (32-bit unsigned fixed point)
    URsqrtE,  // URSQRTE
    CmGtZ,    // compares against +0.0
    CmGeZ,
    CmEqZ,
    CmLeZ,
    CmLtZ,
};

template <FpUn Op, class U>
inline U fp1(uint32_t fpcr, U a) {
    if constexpr (Op == FpUn::Abs) return fp::abs<U>(a);
    else if constexpr (Op == FpUn::Neg) return fp::neg<U>(a);
    else if constexpr (Op == FpUn::Sqrt) return fp::sqrt<U>(fpcr, a);
    else if constexpr (Op == FpUn::RecpE) return fp::recip_estimate<U>(fpcr, a);
    else if constexpr (Op == FpUn::RsqrtE) return fp::rsqrt_estimate<U>(fpcr, a);
    else if constexpr (Op == FpUn::RecpX) return fp::recpx<U>(fpcr, a);
    else if constexpr (Op == FpUn::URecpE) return fp::urecpe(a);
    else if constexpr (Op == FpUn::URsqrtE) return fp::ursqrte(a);
    else if constexpr (Op == FpUn::CmGtZ) return lane_mask<U>(fp::cmp_gt<U>(fpcr, a, U{0}));
    else if constexpr (Op == FpUn::CmGeZ) return lane_mask<U>(fp::cmp_ge<U>(fpcr, a, U{0}));
    else if constexpr (Op == FpUn::CmEqZ) return lane_mask<U>(fp::cmp_eq<U>(fpcr, a, U{0}));
    else if constexpr (Op == FpUn::CmLeZ) return lane_mask<U>(fp::cmp_ge<U>(fpcr, U{0}, a));
    else return lane_mask<U>(fp::cmp_gt<U>(fpcr, U{0}, a));
}

template <FpUn Op, class U>
inline V128 fp1v(uint32_t fpcr, V128 a, bool q) {
    return map1<U>(a, q, [fpcr](U x) { return fp1<Op, U>(fpcr, x); });
}

// FRINT* (vector).
template <class U>
inline V128 frintv(uint32_t fpcr, V128 a, bool q, fp::Rounding mode) {
    return map1<U>(a, q, [fpcr, mode](U x) { return fp::round_int<U>(fpcr, x, mode); });
}

// FCVT[NPMZA][SU] and FCVTZ[SU] (fixed point): lane-sized integers.
template <class U>
inline U to_fixed(uint32_t fpcr, U a, int fbits, bool is_unsigned, fp::Rounding mode) {
    return static_cast<U>(fp::to_fixed<U>(fpcr, a, fbits, is_unsigned, fp::Fmt<U>::N, mode));
}

template <class U>
inline V128 to_fixed_v(uint32_t fpcr, V128 a, bool q, int fbits, bool is_unsigned, fp::Rounding mode) {
    return map1<U>(a, q, [=](U x) { return to_fixed<U>(fpcr, x, fbits, is_unsigned, mode); });
}

// SCVTF/UCVTF (with and without fbits): rounding from FPCR.
template <class U>
inline U from_fixed(uint32_t fpcr, U a, int fbits, bool is_unsigned) {
    return fp::from_fixed<U>(fpcr, a, fbits, is_unsigned, fp::Fmt<U>::N, fp::fpcr_rounding(fpcr));
}

template <class U>
inline V128 from_fixed_v(uint32_t fpcr, V128 a, bool q, int fbits, bool is_unsigned) {
    return map1<U>(a, q, [=](U x) { return from_fixed<U>(fpcr, x, fbits, is_unsigned); });
}

// FCVTN/FCVTN2/FCVTXN/FCVTXN2: the 128 bits of n narrow to 64 bits of D lanes,
// written to the lower half (upper half cleared) or, with `upper`, to the upper
// half (lower half of d kept).
template <class D, class S>
inline V128 fcvtn(uint32_t fpcr, V128 d, V128 n, bool upper, fp::Rounding mode) {
    constexpr unsigned kLanes = 16 / sizeof(S);
    V128 r = upper ? V128{d.lo, 0} : zero();
    for (unsigned i = 0; i < kLanes; ++i)
        set_lane<D>(r, (upper ? kLanes : 0) + i, fp::convert<D, S>(fpcr, lane<S>(n, i), mode));
    return r;
}

// FCVTL/FCVTL2: the lower (or, with `upper`, the upper) 64 bits of n widen to 128 bits.
template <class D, class S>
inline V128 fcvtl(uint32_t fpcr, V128 n, bool upper) {
    constexpr unsigned kLanes = 16 / sizeof(D);
    V128 r = zero();
    for (unsigned i = 0; i < kLanes; ++i)
        set_lane<D>(r, i, fp::convert<D, S>(fpcr, lane<S>(n, (upper ? kLanes : 0) + i), fp::fpcr_rounding(fpcr)));
    return r;
}

}  // namespace lgp::lifted::simd
