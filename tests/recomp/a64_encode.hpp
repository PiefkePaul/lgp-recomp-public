// Minimal A64 instruction encoder for synthetic test code (our own code, never game code).
// Encodings were cross-checked against llvm-mc once; see test_cfg.cpp "encoder" test for the
// fixed vectors that pin them down.
#pragma once

#include <cstdint>
#include <vector>

namespace a64 {

constexpr uint32_t NOP = 0xD503201F;
constexpr uint32_t RET = 0xD65F03C0;
constexpr uint32_t UDF0 = 0x00000000;
constexpr uint8_t SP = 31, ZR = 31;

inline uint32_t u(int64_t v, int bits) { return static_cast<uint32_t>(v) & ((1u << bits) - 1); }

inline uint32_t b(int64_t off) { return 0x14000000 | u(off >> 2, 26); }
inline uint32_t bl(int64_t off) { return 0x94000000 | u(off >> 2, 26); }
inline uint32_t bcond(uint8_t cond, int64_t off) { return 0x54000000 | (u(off >> 2, 19) << 5) | (cond & 15); }
inline uint32_t cbz(bool sf, uint8_t rt, int64_t off) { return (sf ? 0x80000000u : 0) | 0x34000000 | (u(off >> 2, 19) << 5) | rt; }
inline uint32_t cbnz(bool sf, uint8_t rt, int64_t off) { return cbz(sf, rt, off) | (1u << 24); }
inline uint32_t tbz(uint8_t rt, uint8_t bit, int64_t off) {
    return 0x36000000 | ((bit & 0x20) ? 0x80000000u : 0) | (u(bit & 31, 5) << 19) | (u(off >> 2, 14) << 5) | rt;
}
inline uint32_t tbnz(uint8_t rt, uint8_t bit, int64_t off) { return tbz(rt, bit, off) | (1u << 24); }
inline uint32_t br(uint8_t rn) { return 0xD61F0000 | (rn << 5); }
inline uint32_t blr(uint8_t rn) { return 0xD63F0000 | (rn << 5); }
inline uint32_t svc(uint16_t imm) { return 0xD4000001 | (static_cast<uint32_t>(imm) << 5); }
inline uint32_t brk(uint16_t imm) { return 0xD4200000 | (static_cast<uint32_t>(imm) << 5); }

// ADR/ADRP: imm is the byte offset (ADR) or the page delta in pages (ADRP).
inline uint32_t adr(uint8_t rd, int64_t imm) {
    return 0x10000000 | (u(imm, 2) << 29) | (u(imm >> 2, 19) << 5) | rd;
}
inline uint32_t adrp(uint8_t rd, int64_t pages) {
    return 0x90000000 | (u(pages, 2) << 29) | (u(pages >> 2, 19) << 5) | rd;
}
inline uint32_t add_imm(bool sf, uint8_t rd, uint8_t rn, uint16_t imm12, bool sh12 = false) {
    return (sf ? 0x80000000u : 0) | 0x11000000 | (sh12 ? 1u << 22 : 0) | (u(imm12, 12) << 10) | (rn << 5) | rd;
}
inline uint32_t sub_imm(bool sf, uint8_t rd, uint8_t rn, uint16_t imm12) { return add_imm(sf, rd, rn, imm12) | 0x40000000; }
inline uint32_t subs_imm(bool sf, uint8_t rd, uint8_t rn, uint16_t imm12) { return sub_imm(sf, rd, rn, imm12) | 0x20000000; }
inline uint32_t cmp_imm(bool sf, uint8_t rn, uint16_t imm12) { return subs_imm(sf, ZR, rn, imm12); }
// ADD rd, rn, rm, LSL #sh
inline uint32_t add_reg(bool sf, uint8_t rd, uint8_t rn, uint8_t rm, uint8_t sh = 0) {
    return (sf ? 0x80000000u : 0) | 0x0B000000 | (rm << 16) | (u(sh, 6) << 10) | (rn << 5) | rd;
}
inline uint32_t sub_reg(bool sf, uint8_t rd, uint8_t rn, uint8_t rm, uint8_t sh = 0) { return add_reg(sf, rd, rn, rm, sh) | 0x40000000; }
inline uint32_t cmp_reg(bool sf, uint8_t rn, uint8_t rm) { return sub_reg(sf, ZR, rn, rm) | 0x20000000; }
// ADD Xd, Xn, Wm, <ext> #sh   option: 0 uxtb 1 uxth 2 uxtw 3 uxtx 4 sxtb 5 sxth 6 sxtw 7 sxtx
inline uint32_t add_ext(uint8_t rd, uint8_t rn, uint8_t rm, uint8_t option, uint8_t sh) {
    return 0x8B200000 | (rm << 16) | (u(option, 3) << 13) | (u(sh, 3) << 10) | (rn << 5) | rd;
}
inline uint32_t mov_reg(bool sf, uint8_t rd, uint8_t rm) { return (sf ? 0x80000000u : 0) | 0x2A0003E0 | (rm << 16) | rd; }
inline uint32_t movz(bool sf, uint8_t rd, uint16_t imm, uint8_t hw = 0) {
    return (sf ? 0x80000000u : 0) | 0x52800000 | (u(hw, 2) << 21) | (static_cast<uint32_t>(imm) << 5) | rd;
}
inline uint32_t movk(bool sf, uint8_t rd, uint16_t imm, uint8_t hw) { return movz(sf, rd, imm, hw) | 0x20000000; }
// UBFM/SBFM
inline uint32_t ubfm(bool sf, uint8_t rd, uint8_t rn, uint8_t immr, uint8_t imms) {
    return (sf ? 0x80000000u | (1u << 22) : 0) | 0x53000000 | (u(immr, 6) << 16) | (u(imms, 6) << 10) | (rn << 5) | rd;
}
inline uint32_t sbfm(bool sf, uint8_t rd, uint8_t rn, uint8_t immr, uint8_t imms) { return ubfm(sf, rd, rn, immr, imms) & ~0x40000000u; }
inline uint32_t lsl_imm(bool sf, uint8_t rd, uint8_t rn, uint8_t sh) {
    uint8_t rs = sf ? 64 : 32;
    return ubfm(sf, rd, rn, static_cast<uint8_t>((rs - sh) % rs), static_cast<uint8_t>(rs - 1 - sh));
}
inline uint32_t sxtw(uint8_t xd, uint8_t wn) { return sbfm(true, xd, wn, 0, 31); }
inline uint32_t uxtw(uint8_t xd, uint8_t wn) { return ubfm(true, xd, wn, 0, 31); }
// AND Xd, Xn, #0xffffffff
inline uint32_t and_uxtw(uint8_t rd, uint8_t rn) { return 0x92407C00 | (rn << 5) | rd; }

// Loads. size: 0=B 1=H 2=W 3=X. opc: 1 = unsigned, 2 = signed->64, 3 = signed->32.
// Register offset: option 2 uxtw, 3 lsl, 6 sxtw; s = scaled.
inline uint32_t ldr_reg(uint8_t size, uint8_t opc, uint8_t rt, uint8_t rn, uint8_t rm, uint8_t option, bool s) {
    return (static_cast<uint32_t>(size) << 30) | 0x38200800 | (static_cast<uint32_t>(opc) << 22) | (rm << 16) |
           (u(option, 3) << 13) | (s ? 1u << 12 : 0) | (rn << 5) | rt;
}
inline uint32_t ldr_imm(uint8_t size, uint8_t opc, uint8_t rt, uint8_t rn, uint16_t imm_scaled) {
    return (static_cast<uint32_t>(size) << 30) | 0x39000000 | (static_cast<uint32_t>(opc) << 22) | (u(imm_scaled, 12) << 10) | (rn << 5) | rt;
}
inline uint32_t ldr_lit(uint8_t opc, uint8_t rt, int64_t off) { return (static_cast<uint32_t>(opc) << 30) | 0x18000000 | (u(off >> 2, 19) << 5) | rt; }
// Convenience
inline uint32_t ldrb_uxtw(uint8_t wt, uint8_t xn, uint8_t wm) { return ldr_reg(0, 1, wt, xn, wm, 2, false); }
inline uint32_t ldrb_x(uint8_t wt, uint8_t xn, uint8_t xm) { return ldr_reg(0, 1, wt, xn, xm, 3, false); }
inline uint32_t ldrh_uxtw_s(uint8_t wt, uint8_t xn, uint8_t wm) { return ldr_reg(1, 1, wt, xn, wm, 2, true); }
inline uint32_t ldrsw_uxtw_s(uint8_t xt, uint8_t xn, uint8_t wm) { return ldr_reg(2, 2, xt, xn, wm, 2, true); }
inline uint32_t ldr_x_lsl3(uint8_t xt, uint8_t xn, uint8_t xm) { return ldr_reg(3, 1, xt, xn, xm, 3, true); }
inline uint32_t ldr_x_imm(uint8_t xt, uint8_t xn, uint16_t off) { return ldr_imm(3, 1, xt, xn, static_cast<uint16_t>(off / 8)); }
inline uint32_t ldrsb_w_uxtw(uint8_t wt, uint8_t xn, uint8_t wm) { return ldr_reg(0, 3, wt, xn, wm, 2, false); }
// Stores / other instructions as opaque words for "no control flow" filler.
inline uint32_t stp_x29_x30_pre(int imm7_scaled) { return 0xA9800000 | (u(imm7_scaled, 7) << 15) | (30u << 10) | (SP << 5) | 29; }  // stp x29, x30, [sp, #imm]!
inline uint32_t mul_w(uint8_t rd, uint8_t rn, uint8_t rm) { return 0x1B007C00 | (rm << 16) | (rn << 5) | rd; }

inline void emit(std::vector<uint32_t>& code, uint32_t insn) { code.push_back(insn); }

}  // namespace a64
