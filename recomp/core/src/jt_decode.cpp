#include "jt_decode.hpp"

namespace lgp::recomp::jt {

namespace {
inline int64_t sext(uint64_t v, unsigned bits) {
    uint64_t m = uint64_t(1) << (bits - 1);
    return static_cast<int64_t>((v ^ m) - m);
}
inline uint64_t ror(uint64_t v, unsigned n, unsigned bits) {
    if (n == 0) return v;
    uint64_t mask = bits == 64 ? ~uint64_t(0) : ((uint64_t(1) << bits) - 1);
    v &= mask;
    return ((v >> n) | (v << (bits - n))) & mask;
}
}  // namespace

bool decode_bitmask(bool sf, uint32_t n, uint32_t immr, uint32_t imms, uint64_t& out) {
    uint32_t len = 0;
    uint32_t v = (n << 6) | (~imms & 0x3F);
    if (v == 0) return false;
    while ((v >> (len + 1)) != 0) ++len;  // highest set bit
    if (len < 1) return false;
    uint32_t esize = 1u << len;
    if (!sf && esize > 32) return false;
    uint32_t levels = esize - 1;
    uint32_t s = imms & levels, r = immr & levels;
    if (s == levels) return false;
    uint64_t welem = (esize == 64) ? ~uint64_t(0) : ((uint64_t(1) << (s + 1)) - 1);
    welem = ror(welem, r, esize);
    uint64_t result = 0;
    for (uint32_t i = 0; i < (sf ? 64u : 32u); i += esize) result |= welem << i;
    out = result;
    return true;
}

Insn decode(uint32_t insn, uint64_t pc) {
    Insn d;
    d.sf = (insn >> 31) & 1;
    uint32_t op0 = (insn >> 25) & 0xF;

    // ---- Data processing (immediate): op0 = 100x
    if ((op0 & 0xE) == 0x8) {
        uint32_t op1 = (insn >> 23) & 0x7;  // bits 25:23 incl. bit 25 -> use 28:23
        uint32_t cls = (insn >> 23) & 0x3F; // bits 28:23
        (void)op1;
        if ((cls & 0x3E) == 0x20) {  // 10000x PC-rel addressing
            int64_t immlo = (insn >> 29) & 3, immhi = sext((insn >> 5) & 0x7FFFF, 19);
            int64_t imm = (immhi << 2) | immlo;
            d.rd = insn & 0x1F;
            if (insn & (1u << 31)) { d.op = Op::Adrp; d.imm = static_cast<int64_t>((pc & ~uint64_t(0xFFF))) + (imm << 12); }
            else { d.op = Op::Adr; d.imm = static_cast<int64_t>(pc) + imm; }
            d.sf = true;
            return d;
        }
        if ((cls & 0x3E) == 0x22) {  // 10001x add/sub immediate
            bool sh = (insn >> 22) & 1;
            uint64_t imm12 = (insn >> 10) & 0xFFF;
            int64_t imm = static_cast<int64_t>(sh ? imm12 << 12 : imm12);
            bool sub = (insn >> 30) & 1, setflags = (insn >> 29) & 1;
            d.rd = insn & 0x1F;
            d.rn = (insn >> 5) & 0x1F;
            if (setflags) {
                if (!sub) return d;  // ADDS imm: not needed
                d.op = Op::SubsImm;
                d.imm = imm;
            } else {
                d.op = Op::AddImm;
                d.imm = sub ? -imm : imm;
                d.is_sub = sub;
            }
            return d;
        }
        if (cls == 0x24) {  // 100100 logical immediate
            uint32_t opc = (insn >> 29) & 3;
            uint32_t n = (insn >> 22) & 1, immr = (insn >> 16) & 0x3F, imms = (insn >> 10) & 0x3F;
            uint64_t mask;
            if (opc == 0 && decode_bitmask(d.sf, n, immr, imms, mask)) {
                d.op = Op::AndImm;
                d.rd = insn & 0x1F;
                d.rn = (insn >> 5) & 0x1F;
                d.imm = static_cast<int64_t>(mask);
            }
            return d;
        }
        if (cls == 0x25) {  // 100101 move wide
            uint32_t opc = (insn >> 29) & 3, hw = (insn >> 21) & 3;
            d.rd = insn & 0x1F;
            d.imm = static_cast<int64_t>((insn >> 5) & 0xFFFF);
            d.shift = static_cast<uint8_t>(hw * 16);
            if (opc == 0) d.op = Op::Movn;
            else if (opc == 2) d.op = Op::Movz;
            else if (opc == 3) d.op = Op::Movk;
            return d;
        }
        if (cls == 0x26) {  // 100110 bitfield
            uint32_t opc = (insn >> 29) & 3;
            d.rd = insn & 0x1F;
            d.rn = (insn >> 5) & 0x1F;
            d.immr = (insn >> 16) & 0x3F;
            d.imms = (insn >> 10) & 0x3F;
            if (opc == 0) d.op = Op::Sbfm;
            else if (opc == 2) d.op = Op::Ubfm;
            return d;
        }
        return d;
    }

    // ---- Data processing (register): op0 = x101
    if ((op0 & 0x7) == 0x5) {
        uint32_t cls = (insn >> 24) & 0x1F;  // bits 28:24
        if (cls == 0x0A) {  // 01010 logical shifted register
            uint32_t opc = (insn >> 29) & 3, shift = (insn >> 22) & 3, n = (insn >> 21) & 1;
            uint32_t imm6 = (insn >> 10) & 0x3F, rn = (insn >> 5) & 0x1F;
            if (opc == 1 && n == 0 && shift == 0 && imm6 == 0 && rn == 31) {
                d.op = Op::MovReg;
                d.rd = insn & 0x1F;
                d.rm = (insn >> 16) & 0x1F;
            }
            return d;
        }
        if (cls == 0x0B) {  // 01011 add/sub shifted or extended register
            bool sub = (insn >> 30) & 1, setflags = (insn >> 29) & 1;
            bool ext = (insn >> 21) & 1;
            d.rd = insn & 0x1F;
            d.rn = (insn >> 5) & 0x1F;
            d.rm = (insn >> 16) & 0x1F;
            if (!ext) {
                uint32_t shift = (insn >> 22) & 3, imm6 = (insn >> 10) & 0x3F;
                if (shift != 0) return d;  // only LSL is useful for us
                d.shift = static_cast<uint8_t>(imm6);
                if (setflags) { if (sub) d.op = Op::SubsShifted; }
                else { d.op = Op::AddShifted; d.is_sub = sub; }
            } else {
                if (((insn >> 22) & 3) != 0) return d;
                uint32_t option = (insn >> 13) & 7, imm3 = (insn >> 10) & 7;
                if (setflags || sub || imm3 > 4) return d;
                d.op = Op::AddExt;
                d.shift = static_cast<uint8_t>(imm3);
                static const Ext exts[8] = {Ext::Uxtb, Ext::Uxth, Ext::Uxtw, Ext::Uxtx,
                                            Ext::Sxtb, Ext::Sxth, Ext::Sxtw, Ext::Sxtx};
                d.ext = exts[option];
                if (d.ext == Ext::Uxtx || d.ext == Ext::Sxtx) d.ext = Ext::Lsl;
            }
            return d;
        }
        return d;
    }

    // ---- Loads and stores: op0 = x1x0
    if ((op0 & 0x5) == 0x4) {
        uint32_t size = (insn >> 30) & 3;
        bool v = (insn >> 26) & 1;
        uint32_t op24 = (insn >> 24) & 3;
        if (v) return d;  // SIMD/FP loads
        // Load register (literal): xx 011 0 00 imm19 Rt
        if (((insn >> 27) & 7) == 3 && op24 == 0) {  // bits 29:27 = 011, V = 0 (checked above)
            uint32_t opc = (insn >> 30) & 3;
            d.rd = insn & 0x1F;
            d.imm = static_cast<int64_t>(pc) + sext((insn >> 5) & 0x7FFFF, 19) * 4;
            if (opc == 0) { d.op = Op::LdrLit; d.size = 4; d.sf = false; }
            else if (opc == 1) { d.op = Op::LdrLit; d.size = 8; }
            else if (opc == 2) { d.op = Op::LdrLit; d.size = 4; d.load_signed = true; }
            return d;
        }
        if (((insn >> 27) & 7) != 7) return d;  // not "load/store register"
        uint32_t opc = (insn >> 22) & 3;
        d.rd = insn & 0x1F;
        d.rn = (insn >> 5) & 0x1F;
        d.size = static_cast<uint8_t>(1u << size);
        // opc: 00 store, 01 load (zero-extend), 10 load signed -> 64, 11 load signed -> 32 (size<3)
        if (opc == 0) return d;
        if (opc == 1) { d.load_signed = false; d.sf = size == 3; }
        else if (opc == 2) { if (size == 3) return d; d.load_signed = true; d.load_to_64 = true; d.sf = true; }
        else { if (size >= 2) return d; d.load_signed = true; d.load_to_64 = false; d.sf = false; }
        if (op24 == 1) {  // unsigned immediate offset
            d.op = Op::LdrImm;
            d.imm = static_cast<int64_t>(((insn >> 10) & 0xFFF) << size);
            return d;
        }
        if (op24 == 0) {
            bool reg = (insn >> 21) & 1;
            uint32_t bits1110 = (insn >> 10) & 3;
            if (reg && bits1110 == 2) {  // register offset
                uint32_t option = (insn >> 13) & 7;
                bool s = (insn >> 12) & 1;
                d.rm = (insn >> 16) & 0x1F;
                d.shift = s ? static_cast<uint8_t>(size) : 0;
                switch (option) {
                    case 2: d.ext = Ext::Uxtw; break;
                    case 3: d.ext = Ext::Lsl; break;
                    case 6: d.ext = Ext::Sxtw; break;
                    case 7: d.ext = Ext::Lsl; break;  // SXTX == plain 64-bit
                    default: return d;
                }
                d.op = Op::LdrRegOff;
                return d;
            }
            if (!reg && bits1110 == 0) {  // unscaled immediate (LDUR*)
                d.op = Op::LdrImm;
                d.imm = sext((insn >> 12) & 0x1FF, 9);
                return d;
            }
        }
        return d;
    }
    return d;
}

}  // namespace lgp::recomp::jt
