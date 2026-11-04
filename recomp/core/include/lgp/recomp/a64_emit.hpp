// Small A64 encoder for the code the native rewriter generates (stubs and the
// gate entry, native_image.hpp). Only the forms the generator needs; every
// form is pinned by a decode round trip in tests/recomp/test_native_image.cpp
// and by llvm-mc in tests/recomp/test_emit_vs_llvm.py.
#pragma once

#include <cstdint>
#include <stdexcept>

namespace lgp::recomp::emit {

inline constexpr uint8_t kSp = 31, kZr = 31;
inline constexpr uint32_t kNop = 0xD503201Fu;
inline constexpr uint32_t kRet = 0xD65F03C0u;

namespace detail {
inline uint32_t field(int64_t v, int bits) { return static_cast<uint32_t>(v) & ((1u << bits) - 1u); }
inline void need(bool ok, const char* what) {
    if (!ok) throw std::out_of_range(what);
}
inline int64_t branch_delta(uint64_t from, uint64_t to, int bits, const char* what) {
    const int64_t d = static_cast<int64_t>(to - from);
    need((d & 3) == 0, what);
    const int64_t lim = int64_t{1} << (bits + 1);  // bits of word offset, times 4, signed
    need(d >= -lim && d < lim, what);
    return d >> 2;
}
}  // namespace detail

// Branches: `from` is the address of the branch, `to` its target.
inline uint32_t b(uint64_t from, uint64_t to) { return 0x14000000u | detail::field(detail::branch_delta(from, to, 26, "b: out of range"), 26); }
inline uint32_t bl(uint64_t from, uint64_t to) { return 0x94000000u | detail::field(detail::branch_delta(from, to, 26, "bl: out of range"), 26); }
inline bool b_reaches(uint64_t from, uint64_t to) {
    const int64_t d = static_cast<int64_t>(to - from);
    return (d & 3) == 0 && d >= -(int64_t{1} << 27) && d < (int64_t{1} << 27);
}
inline uint32_t cbz(bool sf, uint8_t rt, uint64_t from, uint64_t to, bool nonzero = false) {
    return (sf ? 0x80000000u : 0u) | 0x34000000u | (nonzero ? 1u << 24 : 0u) |
           (detail::field(detail::branch_delta(from, to, 19, "cbz: out of range"), 19) << 5) | rt;
}
inline uint32_t tbz(uint8_t rt, unsigned bit, uint64_t from, uint64_t to, bool nonzero = false) {
    return 0x36000000u | (nonzero ? 1u << 24 : 0u) | ((bit & 32u) ? 0x80000000u : 0u) | ((bit & 31u) << 19) |
           (detail::field(detail::branch_delta(from, to, 14, "tbz: out of range"), 14) << 5) | rt;
}
inline uint32_t br(uint8_t rn) { return 0xD61F0000u | (uint32_t{rn} << 5); }
inline uint32_t blr(uint8_t rn) { return 0xD63F0000u | (uint32_t{rn} << 5); }

// ADR rd, to (within 1 MiB of `from`); ADRP rd, page of `to`.
inline uint32_t adr(uint8_t rd, uint64_t from, uint64_t to) {
    const int64_t d = static_cast<int64_t>(to - from);
    detail::need(d >= -(1 << 20) && d < (1 << 20), "adr: out of range");
    return 0x10000000u | (detail::field(d, 2) << 29) | (detail::field(d >> 2, 19) << 5) | rd;
}
inline uint32_t adrp(uint8_t rd, uint64_t from, uint64_t to) {
    const int64_t pages = static_cast<int64_t>((to & ~uint64_t{0xFFF}) - (from & ~uint64_t{0xFFF})) >> 12;
    detail::need(pages >= -(1 << 20) && pages < (1 << 20), "adrp: out of range");
    return 0x90000000u | (detail::field(pages, 2) << 29) | (detail::field(pages >> 2, 19) << 5) | rd;
}

// ADD/SUB (immediate), 64-bit; rd/rn 31 = SP.
inline uint32_t add_imm(uint8_t rd, uint8_t rn, uint32_t imm12) {
    detail::need(imm12 < 4096, "add: immediate");
    return 0x91000000u | (imm12 << 10) | (uint32_t{rn} << 5) | rd;
}
inline uint32_t sub_imm(uint8_t rd, uint8_t rn, uint32_t imm12) { return add_imm(rd, rn, imm12) | 0x40000000u; }

// Single register load/store, unsigned scaled offset. size_log2 0..3;
// opc 0 store, 1 load, 2 load signed into X.
inline uint32_t ldst_uimm(unsigned size_log2, unsigned opc, uint8_t rt, uint8_t rn, uint64_t byte_off) {
    detail::need(byte_off % (1u << size_log2) == 0 && (byte_off >> size_log2) < 4096, "ldst: offset");
    return (size_log2 << 30) | 0x39000000u | (opc << 22) | (static_cast<uint32_t>(byte_off >> size_log2) << 10) |
           (uint32_t{rn} << 5) | rt;
}
inline uint32_t ldr_x(uint8_t rt, uint8_t rn, uint64_t off) { return ldst_uimm(3, 1, rt, rn, off); }
inline uint32_t str_x(uint8_t rt, uint8_t rn, uint64_t off) { return ldst_uimm(3, 0, rt, rn, off); }
inline uint32_t ldr_w(uint8_t rt, uint8_t rn, uint64_t off) { return ldst_uimm(2, 1, rt, rn, off); }

// LDUR/STUR X, signed 9-bit byte offset.
inline uint32_t ldur_x(uint8_t rt, uint8_t rn, int64_t off) {
    detail::need(off >= -256 && off < 256, "ldur: offset");
    return 0xF8400000u | (detail::field(off, 9) << 12) | (uint32_t{rn} << 5) | rt;
}
inline uint32_t stur_x(uint8_t rt, uint8_t rn, int64_t off) { return ldur_x(rt, rn, off) & ~0x00400000u; }

// LDP/STP X, signed offset (no writeback).
inline uint32_t stp_x(uint8_t rt, uint8_t rt2, uint8_t rn, int64_t off) {
    detail::need(off % 8 == 0 && off >= -512 && off < 512, "stp: offset");
    return 0xA9000000u | (detail::field(off / 8, 7) << 15) | (uint32_t{rt2} << 10) | (uint32_t{rn} << 5) | rt;
}
inline uint32_t ldp_x(uint8_t rt, uint8_t rt2, uint8_t rn, int64_t off) { return stp_x(rt, rt2, rn, off) | 0x00400000u; }

// LDP/STP Q, signed offset (no writeback).
inline uint32_t stp_q(uint8_t rt, uint8_t rt2, uint8_t rn, int64_t off) {
    detail::need(off % 16 == 0 && off >= -1024 && off < 1024, "stp q: offset");
    return 0xAD000000u | (detail::field(off / 16, 7) << 15) | (uint32_t{rt2} << 10) | (uint32_t{rn} << 5) | rt;
}
inline uint32_t ldp_q(uint8_t rt, uint8_t rt2, uint8_t rn, int64_t off) { return stp_q(rt, rt2, rn, off) | 0x00400000u; }

// LDR (literal): size_log2 2 (W) or 3 (X); `sign` with size 2 is LDRSW.
inline uint32_t ldr_lit(unsigned size_log2, bool sign, uint8_t rt, uint64_t from, uint64_t to) {
    detail::need(size_log2 == 3 ? !sign : size_log2 == 2, "ldr literal: form");
    const uint32_t opc = sign ? 0x98000000u : size_log2 == 3 ? 0x58000000u : 0x18000000u;
    return opc | (detail::field(detail::branch_delta(from, to, 19, "ldr literal: out of range"), 19) << 5) | rt;
}

// MRS/MSR with a packed system register (a64::sysreg()).
inline uint32_t mrs(uint8_t rt, uint32_t sysreg) { return 0xD5300000u | (sysreg << 5) | rt; }
inline uint32_t msr(uint32_t sysreg, uint8_t rt) { return 0xD5100000u | (sysreg << 5) | rt; }

}  // namespace lgp::recomp::emit
