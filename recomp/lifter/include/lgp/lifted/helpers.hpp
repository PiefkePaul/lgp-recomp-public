// Pure helper functions used by lifted code: flag computation, bit
// manipulation, wide multiplies, CRC32. No runtime dependencies, header-only,
// portable C++20 (GCC, Clang, MSVC, Emscripten).
#pragma once

#include <bit>
#include <cstdint>

#include "lgp/lifted/cpu_state.hpp"

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif

namespace lgp::lifted {

// Values the Switch's Cortex-A57 reports to EL0.
inline constexpr uint64_t kCtrEl0 = 0x8444C004;  // 64-byte cache lines, VIPT I-cache
inline constexpr uint64_t kDczidEl0 = 0x4;       // DC ZVA block = 2^4 words = 64 bytes
inline constexpr uint64_t kDcZvaBytes = 64;
// FPCR/FPSR bits that exist in AArch64 on ARMv8.0 without FP exception trapping
// (the A57 does not implement trapped FP exceptions, the enable bits are RAZ/WI).
// Stride/Len have no effect in AArch64 but are kept for the AArch32 FPSCR view.
inline constexpr uint32_t kFpcrMask = 0x07F70000;  // AHP, DN, FZ, RMode, Stride, Len
inline constexpr uint32_t kFpsrMask = 0xF800009F;  // N Z C V QC (AArch32 view) + cumulative flags

// NZCV as four 0/1 bytes. Lifted functions keep their flags in a local of
// this type (or use CpuState itself, which has the same members); the flag
// helpers below accept either.
struct Nzcv {
    uint8_t n, z, c, v;
};

// AddWithCarry() from the Arm ARM, setting NZCV.
template <class F>
inline uint64_t adds64(F* s, uint64_t a, uint64_t b, uint64_t carry) {
    const uint64_t r = a + b + carry;
    s->n = static_cast<uint8_t>(r >> 63);
    s->z = static_cast<uint8_t>(r == 0);
    // Unsigned carry out: a + b + carry >= 2^64.
    s->c = static_cast<uint8_t>(carry ? (r <= a) : (r < a));
    s->v = static_cast<uint8_t>(((~(a ^ b)) & (a ^ r)) >> 63);
    return r;
}

template <class F>
inline uint32_t adds32(F* s, uint32_t a, uint32_t b, uint32_t carry) {
    const uint64_t wide = uint64_t{a} + uint64_t{b} + uint64_t{carry};
    const uint32_t r = static_cast<uint32_t>(wide);
    s->n = static_cast<uint8_t>(r >> 31);
    s->z = static_cast<uint8_t>(r == 0);
    s->c = static_cast<uint8_t>(wide >> 32);
    s->v = static_cast<uint8_t>(((~(a ^ b)) & (a ^ r)) >> 31);
    return r;
}

// Logical operations with S: N and Z from the result, C and V cleared.
template <class F>
inline uint64_t logic_flags64(F* s, uint64_t r) {
    s->n = static_cast<uint8_t>(r >> 63);
    s->z = static_cast<uint8_t>(r == 0);
    s->c = 0;
    s->v = 0;
    return r;
}

template <class F>
inline uint32_t logic_flags32(F* s, uint32_t r) {
    s->n = static_cast<uint8_t>(r >> 31);
    s->z = static_cast<uint8_t>(r == 0);
    s->c = 0;
    s->v = 0;
    return r;
}

template <class F>
inline void set_nzcv_imm(F* s, uint32_t nzcv4) {
    s->n = static_cast<uint8_t>((nzcv4 >> 3) & 1);
    s->z = static_cast<uint8_t>((nzcv4 >> 2) & 1);
    s->c = static_cast<uint8_t>((nzcv4 >> 1) & 1);
    s->v = static_cast<uint8_t>(nzcv4 & 1);
}

// MRS/MSR NZCV: the flags in bits 31:28.
template <class F>
inline uint32_t pack_flags(const F& s) {
    return (uint32_t{s.n} << 31) | (uint32_t{s.z} << 30) | (uint32_t{s.c} << 29) | (uint32_t{s.v} << 28);
}

template <class F>
inline void unpack_flags(F* s, uint64_t nzcv) {
    s->n = static_cast<uint8_t>((nzcv >> 31) & 1);
    s->z = static_cast<uint8_t>((nzcv >> 30) & 1);
    s->c = static_cast<uint8_t>((nzcv >> 29) & 1);
    s->v = static_cast<uint8_t>((nzcv >> 28) & 1);
}

inline uint64_t ror64(uint64_t v, unsigned r) { return std::rotr(v, static_cast<int>(r & 63)); }
inline uint32_t ror32(uint32_t v, unsigned r) { return std::rotr(v, static_cast<int>(r & 31)); }
inline uint64_t asr64(uint64_t v, unsigned r) { return static_cast<uint64_t>(static_cast<int64_t>(v) >> (r & 63)); }
inline uint32_t asr32(uint32_t v, unsigned r) { return static_cast<uint32_t>(static_cast<int32_t>(v) >> (r & 31)); }

inline uint64_t clz64(uint64_t v) { return static_cast<uint64_t>(std::countl_zero(v)); }
inline uint32_t clz32(uint32_t v) { return static_cast<uint32_t>(std::countl_zero(v)); }
// CountLeadingSignBits(): leading bits equal to the sign bit, not counting the sign bit itself.
inline uint64_t cls64(uint64_t v) { return clz64((v ^ (v << 1)) | 1); }
inline uint32_t cls32(uint32_t v) { return clz32((v ^ (v << 1)) | 1); }

inline uint64_t bswap64(uint64_t v) {
    v = ((v & 0x00FF00FF00FF00FFull) << 8) | ((v >> 8) & 0x00FF00FF00FF00FFull);
    v = ((v & 0x0000FFFF0000FFFFull) << 16) | ((v >> 16) & 0x0000FFFF0000FFFFull);
    return (v << 32) | (v >> 32);
}
inline uint32_t bswap32(uint32_t v) {
    v = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
    return (v << 16) | (v >> 16);
}
// REV16: swap bytes within each halfword. REV32 (64-bit): within each word.
inline uint64_t rev16_64(uint64_t v) { return ((v & 0x00FF00FF00FF00FFull) << 8) | ((v >> 8) & 0x00FF00FF00FF00FFull); }
inline uint32_t rev16_32(uint32_t v) { return ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu); }
inline uint64_t rev32_64(uint64_t v) {
    return (uint64_t{bswap32(static_cast<uint32_t>(v >> 32))} << 32) | bswap32(static_cast<uint32_t>(v));
}

inline uint64_t rbit64(uint64_t v) {
    v = ((v >> 1) & 0x5555555555555555ull) | ((v & 0x5555555555555555ull) << 1);
    v = ((v >> 2) & 0x3333333333333333ull) | ((v & 0x3333333333333333ull) << 2);
    v = ((v >> 4) & 0x0F0F0F0F0F0F0F0Full) | ((v & 0x0F0F0F0F0F0F0F0Full) << 4);
    return bswap64(v);
}
inline uint32_t rbit32(uint32_t v) { return static_cast<uint32_t>(rbit64(v) >> 32); }

// Division never traps on AArch64: x/0 = 0, INT_MIN/-1 = INT_MIN.
inline uint64_t udiv64(uint64_t a, uint64_t b) { return b ? a / b : 0; }
inline uint32_t udiv32(uint32_t a, uint32_t b) { return b ? a / b : 0; }
inline uint64_t sdiv64(uint64_t a, uint64_t b) {
    if (b == 0) return 0;
    if (a == 0x8000000000000000ull && b == ~0ull) return a;
    return static_cast<uint64_t>(static_cast<int64_t>(a) / static_cast<int64_t>(b));
}
inline uint32_t sdiv32(uint32_t a, uint32_t b) {
    if (b == 0) return 0;
    if (a == 0x80000000u && b == ~0u) return a;
    return static_cast<uint32_t>(static_cast<int32_t>(a) / static_cast<int32_t>(b));
}

// High 64 bits of the 128-bit product.
inline uint64_t umulh(uint64_t a, uint64_t b) {
#if defined(__SIZEOF_INT128__)
    return static_cast<uint64_t>((static_cast<unsigned __int128>(a) * b) >> 64);
#elif defined(_MSC_VER) && defined(_M_X64)
    return __umulh(a, b);
#else
    const uint64_t a_lo = a & 0xFFFFFFFFu, a_hi = a >> 32, b_lo = b & 0xFFFFFFFFu, b_hi = b >> 32;
    const uint64_t p0 = a_lo * b_lo, p1 = a_lo * b_hi, p2 = a_hi * b_lo, p3 = a_hi * b_hi;
    const uint64_t mid = (p0 >> 32) + (p1 & 0xFFFFFFFFu) + (p2 & 0xFFFFFFFFu);
    return p3 + (p1 >> 32) + (p2 >> 32) + (mid >> 32);
#endif
}
inline uint64_t smulh(uint64_t a, uint64_t b) {
    uint64_t hi = umulh(a, b);
    if (static_cast<int64_t>(a) < 0) hi -= b;
    if (static_cast<int64_t>(b) < 0) hi -= a;
    return hi;
}

// CRC32 (polynomial 0x04C11DB7, reflected 0xEDB88320) and CRC32C (0x1EDC6F41,
// reflected 0x82F63B78) over the low `bytes` bytes of `data`, as the A64 CRC32
// instructions define them (no pre/post inversion).
inline uint32_t crc32_update(uint32_t crc, uint64_t data, unsigned bytes, uint32_t poly_reflected) {
    for (unsigned i = 0; i < bytes * 8; ++i) {
        const uint32_t b = static_cast<uint32_t>((crc ^ (data >> i)) & 1);
        crc = (crc >> 1) ^ (b ? poly_reflected : 0u);
    }
    return crc;
}
inline uint32_t crc32(uint32_t crc, uint64_t data, unsigned bytes) { return crc32_update(crc, data, bytes, 0xEDB88320u); }
inline uint32_t crc32c(uint32_t crc, uint64_t data, unsigned bytes) { return crc32_update(crc, data, bytes, 0x82F63B78u); }

}  // namespace lgp::lifted
