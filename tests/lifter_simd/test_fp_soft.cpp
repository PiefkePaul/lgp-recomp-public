// Soft-float path of lgp/lifted/fp.hpp against the host FPU (P2.4).
//
// The soft path (any FPCR rounding mode other than RN, or FZ) is rarely hit by
// the Unicorn differential test with random operands. Here it is checked
// against the host's IEEE-754 arithmetic under fesetround() for all four
// rounding modes with millions of operands, including ties, denormals and
// overflow. NaN results are covered by the Unicorn test (the host's NaN rules
// differ from Arm's); FZ has no host equivalent and is covered there too.
//
// Usage: lgp_test_fp_soft [iterations]
#include <cfenv>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "case_defs.hpp"
#include "lgp/lifted/fp.hpp"

#pragma STDC FENV_ACCESS ON

namespace {

namespace fp = lgp::lifted::fp;

lgp_test::SplitMix g_rng(0x534F4654ull);
unsigned g_failures = 0;

uint64_t rnd() { return g_rng.next(); }

template <class U>
U operand() {
    constexpr int F = fp::Fmt<U>::F, E = fp::Fmt<U>::E;
    const U sign = (rnd() & 1) ? fp::sign_bit<U>() : U{0};
    const int bias = (1 << (E - 1)) - 1;
    switch (rnd() % 10) {
    case 0: return static_cast<U>(rnd());  // anything (NaNs are skipped by the caller)
    case 8:
    case 9: {  // any exponent, short or long mantissa: products and quotients near the denormal boundary
        const unsigned keep = (rnd() & 1) ? static_cast<unsigned>(F) : 1 + static_cast<unsigned>(rnd() % 8);
        const U frac = static_cast<U>((rnd() >> (64 - keep)) << (F - keep));
        const U exp = static_cast<U>(rnd() % ((1u << E) - 1));
        return static_cast<U>(sign | (exp << F) | frac);
    }
    case 1: {                              // denormal or tiny normal
        const U frac = static_cast<U>(rnd() >> (64 - F));
        const U exp = static_cast<U>(rnd() % 3);
        return static_cast<U>(sign | (exp << F) | frac);
    }
    case 2: {  // huge
        const U exp = static_cast<U>((1 << E) - 2 - rnd() % 3);
        return static_cast<U>(sign | (exp << F) | static_cast<U>(rnd() >> (64 - F)));
    }
    case 3:
    case 4: {  // short mantissa around 1: many exact ties in add/mul
        const unsigned keep = 1 + static_cast<unsigned>(rnd() % 8);
        const U frac = static_cast<U>((rnd() >> (64 - keep)) << (F - keep));
        const U exp = static_cast<U>(bias - 30 + static_cast<int>(rnd() % 61));
        return static_cast<U>(sign | (exp << F) | frac);
    }
    default: {  // moderate range
        const U exp = static_cast<U>(bias - 20 + static_cast<int>(rnd() % 41));
        return static_cast<U>(sign | (exp << F) | static_cast<U>(rnd() >> (64 - F)));
    }
    }
}

const int kHostModes[] = {FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO};  // FPCR.RMode 0..3

template <class U>
void report(const char* op, unsigned mode, U a, U b, U c, U got, U want) {
    if (++g_failures > 30) return;
    std::printf("FAIL %s%d rmode=%u a=%0*" PRIx64 " b=%0*" PRIx64 " c=%0*" PRIx64 ": got %0*" PRIx64 " want %0*" PRIx64 "\n", op,
                fp::Fmt<U>::N, mode, fp::Fmt<U>::N / 4, static_cast<uint64_t>(a), fp::Fmt<U>::N / 4,
                static_cast<uint64_t>(b), fp::Fmt<U>::N / 4, static_cast<uint64_t>(c), fp::Fmt<U>::N / 4,
                static_cast<uint64_t>(got), fp::Fmt<U>::N / 4, static_cast<uint64_t>(want));
}

template <class U>
void check_arith(uint64_t iterations) {
    using H = typename fp::Fmt<U>::Host;
    for (uint64_t it = 0; it < iterations; ++it) {
        const unsigned mode = static_cast<unsigned>(rnd() % 4);
        // RMode in FPCR; the soft path is forced by also comparing mode 0 through soft::.
        const uint32_t fpcr = mode << fp::kFpcrRModeShift;
        const U a = operand<U>(), b = operand<U>(), c = operand<U>();
        if (fp::is_nan(a) || fp::is_nan(b) || fp::is_nan(c)) continue;
        volatile H ha = fp::to_host(a), hb = fp::to_host(b), hc = fp::to_host(c);
        std::fesetround(kHostModes[mode]);
        const H r_add = ha + hb, r_sub = ha - hb, r_mul = ha * hb, r_div = ha / hb;
        const H r_sqrt = std::sqrt(ha), r_fma = std::fma(hb, hc, ha);
        std::fesetround(FE_TONEAREST);
        auto cmp = [&](const char* op, U got, H want_h) {
            const U want = fp::from_host<U>(want_h);
            if (want_h != want_h) return;  // NaN: Arm rules differ from the host
            if (got != want) report<U>(op, mode, a, b, c, got, want);
        };
        cmp("add", fp::soft::add<U>(fpcr, a, b, false), r_add);
        cmp("sub", fp::soft::add<U>(fpcr, a, b, true), r_sub);
        cmp("mul", fp::soft::mul<U>(fpcr, a, b, false), r_mul);
        cmp("div", fp::soft::div<U>(fpcr, a, b), r_div);
        cmp("sqrt", fp::soft::sqrt<U>(fpcr, a), r_sqrt);
        cmp("fma", fp::soft::muladd<U>(fpcr, a, b, c), r_fma);
        // The public entry points must agree too (fast path for mode 0).
        cmp("add_pub", fp::add<U>(fpcr, a, b), r_add);
        cmp("fma_pub", fp::muladd<U>(fpcr, a, b, c), r_fma);
    }
}

void check_conversions(uint64_t iterations) {
    for (uint64_t it = 0; it < iterations; ++it) {
        const unsigned mode = static_cast<unsigned>(rnd() % 4);
        const uint32_t fpcr = mode << fp::kFpcrRModeShift;
        const fp::Rounding rm = static_cast<fp::Rounding>(mode);
        const uint64_t d = operand<uint64_t>();
        const uint32_t s = operand<uint32_t>();
        const int64_t i64 = static_cast<int64_t>(rnd()) >> (rnd() % 64);
        if (!fp::is_nan(d)) {
            volatile double hd = fp::to_host(d);
            std::fesetround(kHostModes[mode]);
            const float f = static_cast<float>(hd);
            const double ri = std::nearbyint(hd);
            std::fesetround(FE_TONEAREST);
            const uint32_t got = fp::convert<uint32_t, uint64_t>(fpcr, d, rm);
            if (got != fp::from_host<uint32_t>(f)) report<uint64_t>("fcvt_ds", mode, d, 0, 0, got, fp::from_host<uint32_t>(f));
            const uint64_t gri = fp::round_int<uint64_t>(fpcr, d, rm);
            if (gri != fp::from_host<uint64_t>(ri)) report<uint64_t>("frint", mode, d, 0, 0, gri, fp::from_host<uint64_t>(ri));
            const uint64_t gra = fp::round_int<uint64_t>(fpcr, d, fp::kTieAway);
            if (gra != fp::from_host<uint64_t>(std::round(hd)))
                report<uint64_t>("frinta", mode, d, 0, 0, gra, fp::from_host<uint64_t>(std::round(hd)));
            // FCVTZS (to int32/int64), in range only (saturation is checked by the Unicorn test).
            if (std::fabs(hd) < 2147483647.0) {
                const uint64_t got32 = fp::to_fixed<uint64_t>(fpcr, d, 0, false, 32, fp::kZero);
                const uint64_t want32 = static_cast<uint32_t>(static_cast<int32_t>(hd));
                if (got32 != want32) report<uint64_t>("fcvtzs32", mode, d, 0, 0, got32, want32);
            }
            // FCVTNS = round to nearest even, then convert.
            if (std::fabs(hd) < 9.0e18) {
                const uint64_t gotn = fp::to_fixed<uint64_t>(fpcr, d, 0, false, 64, fp::kTieEven);
                std::fesetround(FE_TONEAREST);
                const uint64_t wantn = static_cast<uint64_t>(static_cast<int64_t>(std::nearbyint(hd)));
                if (gotn != wantn) report<uint64_t>("fcvtns64", mode, d, 0, 0, gotn, wantn);
            }
        }
        if (!fp::is_nan(s)) {
            volatile float hs = fp::to_host(s);
            const double widened = static_cast<double>(hs);
            const uint64_t got = fp::convert<uint64_t, uint32_t>(fpcr, s, rm);
            if (got != fp::from_host<uint64_t>(widened))
                report<uint32_t>("fcvt_sd", mode, s, 0, 0, static_cast<uint32_t>(got), 0);
        }
        {
            volatile int64_t vi = i64;
            std::fesetround(kHostModes[mode]);
            const double hd = static_cast<double>(vi);
            const float hf = static_cast<float>(vi);
            std::fesetround(FE_TONEAREST);
            const uint64_t gd = fp::from_fixed<uint64_t>(fpcr, static_cast<uint64_t>(i64), 0, false, 64, rm);
            const uint32_t gf = fp::from_fixed<uint32_t>(fpcr, static_cast<uint64_t>(i64), 0, false, 64, rm);
            if (gd != fp::from_host<uint64_t>(hd)) report<uint64_t>("scvtf_d", mode, static_cast<uint64_t>(i64), 0, 0, gd, fp::from_host<uint64_t>(hd));
            if (gf != fp::from_host<uint32_t>(hf))
                report<uint32_t>("scvtf_s", mode, static_cast<uint32_t>(i64), 0, 0, gf, fp::from_host<uint32_t>(hf));
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    const uint64_t iterations = argc > 1 ? std::strtoull(argv[1], nullptr, 0) : 300000;
    check_arith<uint32_t>(iterations);
    check_arith<uint64_t>(iterations);
    check_conversions(iterations);
    std::printf("fp soft path vs host: %" PRIu64 " iterations per format, %u failures\n", iterations, g_failures);
    return g_failures == 0 ? 0 : 1;
}
