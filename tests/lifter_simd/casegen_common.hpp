// Common part of the SIMD&FP test-case generator (P2.4): types, random
// helpers and interesting operand values. The family files groups_*.inc
// build on it; casegen_simd.cpp includes them all.
#pragma once

#include <cstdint>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include "case_defs.hpp"
#include "lgp/recomp/a64.hpp"
#include "lgp/recomp/a64_simd.hpp"
#include "simd_case_defs.hpp"

namespace simdgen {

namespace a64 = lgp::recomp::a64;
using lgp_test::kCodeBase;

constexpr uint32_t kSentinel = 0xD4200000u | (0x4C47u << 5);  // BRK #0x4C47

struct Rng {
    lgp_test::SplitMix sm;
    explicit Rng(uint64_t seed) : sm(seed) {}
    uint64_t u64() { return sm.next(); }
    uint32_t u32() { return static_cast<uint32_t>(sm.next()); }
    uint64_t below(uint64_t n) { return n ? u64() % n : 0; }
    bool chance(unsigned percent) { return below(100) < percent; }
};

struct GenFunction {
    uint64_t entry;
    uint64_t end;
};

struct GenCase {
    std::string group;
    std::string name;
    uint64_t seed = 0;
    std::vector<uint32_t> code;
    std::vector<lgp_test::RegOverride> overrides;  // see simd_case_defs.hpp for the extended register numbers
};

// How the V registers are filled before a case runs.
enum class VInit : uint8_t {
    Random,  // random bits (the test driver's default)
    Fp,      // interesting FP values: each register gets lanes of one random format (H, S or D)
    Int,     // interesting integer lanes (0, 1, -1, min/max of every lane width, random)
};

// How FPCR is set.
enum class FpcrInit : uint8_t {
    Zero,    // FPCR = 0 (round to nearest, no flush, NaN propagation)
    Random,  // 40 % zero, otherwise random RMode/FZ/DN/AHP
};

struct Group;
// Optional per-word fix-up (e.g. point a base register at the data window).
// Returns false if the word is not usable; may add register overrides.
using PatchFn = bool (*)(Rng& rng, uint32_t& word, GenCase& c);

enum class Kind : uint8_t { Alu, Mem };

struct Group {
    const char* name;
    uint32_t mask;   // fixed bits
    uint32_t value;  // their values
    Kind kind;
    unsigned max_len;  // instructions per case (1..max_len)
    VInit vinit;
    FpcrInit fpcr;
    PatchFn patch = nullptr;
};

inline uint32_t set_field(uint32_t w, int lo, int width, uint32_t v) {
    const uint32_t m = ((1u << width) - 1) << lo;
    return (w & ~m) | ((v << lo) & m);
}

inline bool valid(uint32_t w) { return a64::decode(w, kCodeBase).op != a64::Op::Invalid; }

// ---- interesting values -------------------------------------------------------
inline uint16_t fp16_value(Rng& rng) {
    static const uint16_t k[] = {0x0000, 0x8000, 0x0001, 0x8001, 0x03FF, 0x83FF, 0x0400, 0x8400, 0x3C00, 0xBC00,
                                 0x4000, 0x3800, 0x3E00, 0x7BFF, 0xFBFF, 0x7C00, 0xFC00, 0x7E00, 0xFE00, 0x7C01,
                                 0x7D55, 0xFD00, 0x4900, 0x5640, 0x3555};
    switch (rng.below(3)) {
    case 0: return k[rng.below(std::size(k))];
    case 1: return static_cast<uint16_t>(rng.u32());
    default: return static_cast<uint16_t>(0x3000 + rng.below(0x2000)) | static_cast<uint16_t>(rng.chance(50) ? 0x8000 : 0);
    }
}

inline uint32_t fp32_value(Rng& rng) {
    static const uint32_t k[] = {
        0x00000000, 0x80000000, 0x00000001, 0x80000001, 0x007FFFFF, 0x807FFFFF, 0x00800000, 0x80800000, 0x00800001,
        0x3F800000, 0xBF800000, 0x40000000, 0x3F000000, 0x3FC00000, 0xBFC00000, 0x40200000, 0x40400000, 0x3EAAAAAB,
        0x7F7FFFFF, 0xFF7FFFFF, 0x7F800000, 0xFF800000, 0x7FC00000, 0xFFC00000, 0x7F800001, 0xFF800001, 0x7FA00000,
        0x7FC12345, 0xFF812345, 0x4F000000, 0xCF000000, 0x4EFFFFFF, 0x5F000000, 0xDF000000, 0x5EFFFFFF, 0x4F800000,
        0x3F7FFFFF, 0x3F800001, 0x4B000000, 0x4B000001, 0x4AFFFFFF, 0x3F400000, 0x3EFFFFFF, 0x00400000, 0x7E800000,
        0x01000000, 0x0C000000, 0x72000000, 0x5A800000, 0x1A800000, 0x40490FDB, 0x358637BD};
    switch (rng.below(6)) {
    case 0:
    case 1: return k[rng.below(std::size(k))];
    case 2: return rng.u32();
    case 5: {  // any exponent, short mantissa (ties, results near the denormal boundary)
        const unsigned keep = 1 + static_cast<unsigned>(rng.below(8));
        return (rng.chance(50) ? 0x80000000u : 0) | (static_cast<uint32_t>(rng.below(255)) << 23) |
               ((rng.u32() >> (32 - keep)) << (23 - keep));
    }
    case 3:  // moderate magnitudes, frequent in game code
        return static_cast<uint32_t>(0x3A000000u + rng.below(0x0C000000u)) | (rng.chance(50) ? 0x80000000u : 0);
    default:  // small integers and halves (rounding cases)
    {
        const int64_t n = static_cast<int64_t>(rng.below(64)) - 32;
        float f = static_cast<float>(n) + (rng.chance(50) ? 0.5f : (rng.chance(50) ? 0.25f : 0.0f));
        uint32_t b;
        std::memcpy(&b, &f, 4);
        return b;
    }
    }
}

inline uint64_t fp64_value(Rng& rng) {
    static const uint64_t k[] = {0x0000000000000000ull, 0x8000000000000000ull, 0x0000000000000001ull,
                                 0x8000000000000001ull, 0x000FFFFFFFFFFFFFull, 0x800FFFFFFFFFFFFFull,
                                 0x0010000000000000ull, 0x8010000000000000ull, 0x3FF0000000000000ull,
                                 0xBFF0000000000000ull, 0x4000000000000000ull, 0x3FE0000000000000ull,
                                 0x3FF8000000000000ull, 0xBFF8000000000000ull, 0x7FEFFFFFFFFFFFFFull,
                                 0xFFEFFFFFFFFFFFFFull, 0x7FF0000000000000ull, 0xFFF0000000000000ull,
                                 0x7FF8000000000000ull, 0xFFF8000000000000ull, 0x7FF0000000000001ull,
                                 0xFFF0000000000001ull, 0x7FF4000000000000ull, 0x7FF8123456789ABCull,
                                 0x41E0000000000000ull, 0xC1E0000000000000ull, 0x41DFFFFFFFC00000ull,
                                 0x43E0000000000000ull, 0xC3E0000000000000ull, 0x43DFFFFFFFFFFFFFull,
                                 0x41F0000000000000ull, 0x43F0000000000000ull, 0x4330000000000000ull,
                                 0x4330000000000001ull, 0x3FEFFFFFFFFFFFFFull, 0x47EFFFFFE0000000ull,
                                 0x47EFFFFFF0000000ull, 0x36A0000000000000ull, 0x3690000000000000ull,
                                 0x380FFFFFFFFFFFFFull, 0x3810000000000000ull, 0x400921FB54442D18ull,
                                 0x3E7AD7F29ABCAF48ull};
    switch (rng.below(6)) {
    case 0:
    case 1: return k[rng.below(std::size(k))];
    case 2: return rng.u64();
    case 5: {
        const unsigned keep = 1 + static_cast<unsigned>(rng.below(8));
        return (rng.chance(50) ? 0x8000000000000000ull : 0) | (rng.below(2047) << 52) |
               ((rng.u64() >> (64 - keep)) << (52 - keep));
    }
    case 3:
        return (0x3F00000000000000ull + rng.below(0x0180000000000000ull)) | (rng.chance(50) ? 0x8000000000000000ull : 0);
    default: {
        const int64_t n = static_cast<int64_t>(rng.below(64)) - 32;
        double d = static_cast<double>(n) + (rng.chance(50) ? 0.5 : (rng.chance(50) ? 0.25 : 0.0));
        uint64_t b;
        std::memcpy(&b, &d, 8);
        return b;
    }
    }
}

inline uint64_t int_lane(Rng& rng, unsigned bits) {
    const uint64_t m = bits == 64 ? ~0ull : ((1ull << bits) - 1);
    switch (rng.below(6)) {
    case 0: return 0;
    case 1: return m;                              // -1
    case 2: return (1ull << (bits - 1)) & m;       // min
    case 3: return ((1ull << (bits - 1)) - 1) & m;  // max
    case 4: return rng.below(5) & m;
    default: return rng.u64() & m;
    }
}

// 128 bits of lanes for one V register.
inline void vector_value(Rng& rng, VInit mode, uint64_t& lo, uint64_t& hi) {
    uint64_t half[2] = {0, 0};
    if (mode == VInit::Random) {
        lo = rng.u64();
        hi = rng.u64();
        return;
    }
    if (mode == VInit::Fp) {
        const unsigned fmt = static_cast<unsigned>(rng.below(4));  // 0: H, 1/2: S (most common), 3: D
        for (auto& h : half) {
            if (fmt == 0) {
                for (int k = 0; k < 4; ++k)
                    h |= uint64_t{fp16_value(rng)} << (16 * k);
            } else if (fmt == 3) {
                h = fp64_value(rng);
            } else {
                h = uint64_t{fp32_value(rng)} | (uint64_t{fp32_value(rng)} << 32);
            }
        }
    } else {
        const unsigned bits = 8u << rng.below(4);
        for (auto& h : half)
            for (unsigned k = 0; k < 64; k += bits)
                h |= int_lane(rng, bits) << k;
    }
    lo = half[0];
    hi = half[1];
}

inline uint32_t random_fpcr(Rng& rng) {
    if (rng.chance(40)) return 0;
    uint32_t f = static_cast<uint32_t>(rng.below(4)) << 22;  // RMode
    if (rng.chance(30)) f |= 1u << 24;                       // FZ
    if (rng.chance(30)) f |= 1u << 25;                       // DN
    if (rng.chance(30)) f |= 1u << 26;                       // AHP
    return f;
}

// Base registers for memory cases point into the data window (as in tests/lifter).
constexpr unsigned kBaseRegs[] = {20, 21, 22, 23, 24, 25, 26, 27, 28, 31};
constexpr unsigned kIndexPos = 19;  // small non-negative index register
constexpr unsigned kIndexNeg = 18;  // small negative index register

inline void memory_overrides(Rng& rng, GenCase& c, unsigned align_log2) {
    for (unsigned r : kBaseRegs) {
        uint64_t v = lgp_test::kDataWindow + rng.below(0x1000);
        if (align_log2) v &= ~((uint64_t{1} << align_log2) - 1);
        if (r == 31) v = lgp_test::kDataWindow + 16 * rng.below(0x100);
        c.overrides.push_back({static_cast<uint8_t>(r), v});
    }
    c.overrides.push_back({kIndexPos, rng.below(128)});
    c.overrides.push_back({kIndexNeg, static_cast<uint64_t>(-static_cast<int64_t>(1 + rng.below(128)))});
}

inline GenCase make_case(const char* group, unsigned index, Rng& rng) {
    GenCase c;
    c.group = group;
    c.name = std::string(group) + "/" + std::to_string(index);
    c.seed = rng.u64();
    return c;
}

}  // namespace simdgen
