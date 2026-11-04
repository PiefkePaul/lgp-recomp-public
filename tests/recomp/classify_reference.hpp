// The CFG's original control-flow classifier (P2.2), kept as an independent
// second implementation: test_classify_crosscheck compares it with classify(),
// which now runs on the lifter's decoder (docs/design/p2-interfaces.md, section 2).
#pragma once

#include <cstdint>

#include "lgp/recomp/a64_cf.hpp"

namespace lgp::recomp::ref {

namespace detail {
inline int64_t ref_sext(uint64_t v, unsigned bits) {
    uint64_t m = uint64_t(1) << (bits - 1);
    return static_cast<int64_t>((v ^ m) - m);
}
}  // namespace detail

inline CfInfo classify_reference(uint32_t insn, uint64_t pc) {
    CfInfo r;
    // UDF: 0000 0000 0000 0000 iiii iiii iiii iiii
    if ((insn & 0xFFFF0000) == 0) {
        r.kind = CfKind::Trap;
        r.imm = insn & 0xFFFF;
        return r;
    }
    uint32_t op0 = (insn >> 25) & 0xF;
    // Unallocated top-level classes (ARMv8.0): 0000 (except UDF, handled), 0001, 0011.
    if (op0 == 0x0 || op0 == 0x1 || op0 == 0x3) {
        r.kind = CfKind::Unknown;
        return r;
    }
    if ((op0 & 0xE) != 0xA) return r;  // not "branches, exception generating and system"

    // B / BL
    if ((insn & 0x7C000000) == 0x14000000) {
        r.kind = (insn >> 31) ? CfKind::Call : CfKind::Branch;
        r.target = pc + static_cast<uint64_t>(detail::ref_sext(insn & 0x03FFFFFF, 26) * 4);
        return r;
    }
    // B.cond (bit 4 = 0; bit 4 = 1 is BC.cond from v8.8, classified the same way)
    if ((insn & 0xFF000000) == 0x54000000) {
        r.kind = CfKind::CondBranch;
        r.cond_kind = CondKind::BCond;
        r.cond = insn & 0xF;
        r.target = pc + static_cast<uint64_t>(detail::ref_sext((insn >> 5) & 0x7FFFF, 19) * 4);
        return r;
    }
    // CBZ / CBNZ
    if ((insn & 0x7E000000) == 0x34000000) {
        r.kind = CfKind::CondBranch;
        r.cond_kind = (insn & (1u << 24)) ? CondKind::Cbnz : CondKind::Cbz;
        r.reg = insn & 0x1F;
        r.target = pc + static_cast<uint64_t>(detail::ref_sext((insn >> 5) & 0x7FFFF, 19) * 4);
        return r;
    }
    // TBZ / TBNZ
    if ((insn & 0x7E000000) == 0x36000000) {
        r.kind = CfKind::CondBranch;
        r.cond_kind = (insn & (1u << 24)) ? CondKind::Tbnz : CondKind::Tbz;
        r.reg = insn & 0x1F;
        r.target = pc + static_cast<uint64_t>(detail::ref_sext((insn >> 5) & 0x3FFF, 14) * 4);
        return r;
    }
    // Unconditional branch (register): 1101 011x xxxx xxxx xxxx xxxx xxxx xxxx
    if ((insn & 0xFE000000) == 0xD6000000) {
        uint32_t opc = (insn >> 21) & 0xF;
        uint32_t op2 = (insn >> 16) & 0x1F;
        r.reg = (insn >> 5) & 0x1F;
        if (op2 != 0x1F) { r.kind = CfKind::Unknown; return r; }
        switch (opc) {
            case 0: r.kind = CfKind::IndirectBranch; return r;   // BR (and BRAA/BRAB variants)
            case 1: r.kind = CfKind::IndirectCall; return r;     // BLR
            case 2: r.kind = CfKind::Return; return r;           // RET
            case 4: r.kind = CfKind::Return; r.reg = 31; return r;   // ERET
            case 5: r.kind = CfKind::Return; r.reg = 31; return r;   // DRPS
            case 8: case 9: r.kind = CfKind::IndirectBranch; return r;   // BRAA/BRAB with modifier
            default: r.kind = CfKind::Unknown; return r;
        }
    }
    // Exception generation: 1101 0100 opc(3) imm16 op2(3) LL(2)
    if ((insn & 0xFF000000) == 0xD4000000) {
        uint32_t opc = (insn >> 21) & 7, ll = insn & 3, op2 = (insn >> 2) & 7;
        r.imm = (insn >> 5) & 0xFFFF;
        if (op2 != 0) { r.kind = CfKind::Unknown; return r; }
        if (opc == 0 && ll != 0) { r.kind = CfKind::Svc; return r; }       // SVC / HVC / SMC
        if ((opc == 1 || opc == 2) && ll == 0) { r.kind = CfKind::Trap; return r; }  // BRK / HLT
        if (opc == 5 && ll != 0) { r.kind = CfKind::Trap; return r; }      // DCPS1-3 (debug)
        r.kind = CfKind::Unknown;
        return r;
    }
    return r;  // system instructions (NOP, MSR, DSB, ...): no control flow
}

}  // namespace lgp::recomp::ref
