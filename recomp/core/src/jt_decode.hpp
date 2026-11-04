// Internal mini decoder for the jump-table slice: address arithmetic and loads only.
// Not part of the public interface; replaced by the lifter's decoder after its merge.
#pragma once

#include <cstdint>

namespace lgp::recomp::jt {

enum class Op : uint8_t {
    Other,
    Adr,        // rd = pc + imm
    Adrp,       // rd = (pc & ~0xfff) + imm
    AddImm,     // rd = rn + imm  (imm already shifted; negative for SUB)
    SubsImm,    // flags = rn - imm; rd (31 = CMP)
    AddShifted, // rd = rn + (rm << shift)  (sub: is_sub)
    SubsShifted,// flags = rn - (rm << shift); rd (31 = CMP)
    AddExt,     // rd = rn + ext(rm) << shift
    MovReg,     // rd = rm   (ORR rd, xzr, rm)
    Movz, Movk, Movn,
    AndImm,     // rd = rn & imm
    Ubfm, Sbfm,
    LdrLit,     // rt = load(pc + imm)
    LdrRegOff,  // rt = load(rn + ext(rm) << shift)
    LdrImm,     // rt = load(rn + imm)
};

enum class Ext : uint8_t { Uxtb, Uxth, Uxtw, Uxtx, Sxtb, Sxth, Sxtw, Sxtx, Lsl };

struct Insn {
    Op op = Op::Other;
    bool sf = true;          // 64-bit operation / destination
    bool is_sub = false;
    uint8_t rd = 31, rn = 31, rm = 31;
    int64_t imm = 0;
    uint8_t shift = 0;       // shift amount (lsl) for AddShifted/AddExt/LdrRegOff; hw*16 for MOVZ/K
    Ext ext = Ext::Lsl;
    uint8_t immr = 0, imms = 0;   // bitfield
    uint8_t size = 0;        // load size in bytes (1,2,4,8)
    bool load_signed = false;
    bool load_to_64 = true;  // destination width for signed loads
};

Insn decode(uint32_t insn, uint64_t pc);

// Decodes the logical-immediate bitmask (DecodeBitMasks); returns false if invalid.
bool decode_bitmask(bool sf, uint32_t n, uint32_t immr, uint32_t imms, uint64_t& out);

}  // namespace lgp::recomp::jt
