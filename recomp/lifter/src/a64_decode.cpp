// A64 decoder, following the encoding tables of the Arm ARM (DDI 0487),
// chapter C4 "A64 Instruction Set Encoding", restricted to ARMv8.0-A.
#include "lgp/recomp/a64.hpp"
#include "lgp/recomp/a64_simd.hpp"

#include <cstdio>

namespace lgp::recomp::a64 {
namespace {

constexpr uint32_t bits(uint32_t v, int hi, int lo) { return (v >> lo) & ((1u << (hi - lo + 1)) - 1u); }
constexpr uint32_t bit(uint32_t v, int b) { return (v >> b) & 1u; }

constexpr int64_t sext(uint64_t v, int width) {
    const uint64_t m = 1ull << (width - 1);
    v &= (width == 64) ? ~0ull : ((1ull << width) - 1);
    return static_cast<int64_t>((v ^ m) - m);
}

constexpr uint64_t ones(int n) { return n >= 64 ? ~0ull : ((1ull << n) - 1); }

uint64_t ror(uint64_t v, int r, int esize) {
    r %= esize;
    if (r == 0) return v;
    return ((v >> r) | (v << (esize - r))) & ones(esize);
}

int highest_set_bit(uint32_t v) {
    for (int i = 31; i >= 0; --i)
        if (v & (1u << i)) return i;
    return -1;
}

Inst invalid(Inst i, Invalid why = Invalid::Unallocated) {
    i.op = Op::Invalid;
    i.flow = Flow::Trap;
    i.invalid = why;
    return i;
}
Inst unpredictable(Inst i) { return invalid(i, Invalid::Unpredictable); }
Inst privileged(Inst i) { return invalid(i, Invalid::Privileged); }
[[maybe_unused]] Inst not_implemented(Inst i) { return invalid(i, Invalid::NotImplemented); }

Inst normal(Inst i, Op op) {
    i.op = op;
    i.flow = Flow::Normal;
    return i;
}

// SIMD&FP data processing and load/store structures (P2.4, a64_simd.hpp).
Inst decode_simd(Inst i) {
    const simd::Lifted l = simd::lift(i.raw, i.pc, false);
    if (l.invalid != Invalid::None) return invalid(i, l.invalid);
    return normal(i, Op::Simd);
}

// ---------------------------------------------------------------------------
// Data processing -- immediate
// ---------------------------------------------------------------------------
Inst decode_dp_imm(Inst i) {
    const uint32_t r = i.raw;
    const uint32_t op0 = bits(r, 25, 23);
    i.sf = bit(r, 31);
    i.rd = bits(r, 4, 0);
    i.rn = bits(r, 9, 5);

    switch (op0) {
    case 0b000:
    case 0b001: {  // PC-rel. addressing
        const uint64_t immlo = bits(r, 30, 29);
        const uint64_t immhi = bits(r, 23, 5);
        const int64_t imm = sext((immhi << 2) | immlo, 21);
        i.sf = true;
        if (bit(r, 31)) {
            i.target = (i.pc & ~0xFFFull) + static_cast<uint64_t>(imm << 12);
            return normal(i, Op::Adrp);
        }
        i.target = i.pc + static_cast<uint64_t>(imm);
        return normal(i, Op::Adr);
    }
    case 0b010: {  // Add/subtract (immediate)
        i.setflags = bit(r, 29);
        i.amount = bit(r, 22) ? 12 : 0;
        i.imm = bits(r, 21, 10);
        i.rn_sp = true;
        i.rd_sp = !i.setflags;
        return normal(i, bit(r, 30) ? Op::SubImm : Op::AddImm);
    }
    case 0b011:  // Add/subtract (immediate, with tags): MTE, not in v8.0
        return invalid(i);
    case 0b100: {  // Logical (immediate)
        const uint32_t opc = bits(r, 30, 29);
        const bool n = bit(r, 22);
        if (!i.sf && n) return invalid(i);
        uint64_t mask = 0;
        if (!decode_bit_masks(n, bits(r, 15, 10), bits(r, 21, 16), i.sf, mask)) return invalid(i);
        i.imm = mask;
        static constexpr Op ops[4] = {Op::AndImm, Op::OrrImm, Op::EorImm, Op::AndsImm};
        i.setflags = opc == 3;
        i.rd_sp = opc != 3;
        return normal(i, ops[opc]);
    }
    case 0b101: {  // Move wide (immediate)
        const uint32_t opc = bits(r, 30, 29);
        const uint32_t hw = bits(r, 22, 21);
        if (opc == 1) return invalid(i);
        if (!i.sf && hw >= 2) return invalid(i);
        i.imm = bits(r, 20, 5);
        i.amount = static_cast<uint8_t>(hw * 16);
        static constexpr Op ops[4] = {Op::Movn, Op::Invalid, Op::Movz, Op::Movk};
        return normal(i, ops[opc]);
    }
    case 0b110: {  // Bitfield
        const uint32_t opc = bits(r, 30, 29);
        const bool n = bit(r, 22);
        i.immr = bits(r, 21, 16);
        i.imms = bits(r, 15, 10);
        if (opc == 3) return invalid(i);
        if (i.sf != n) return invalid(i);
        if (!i.sf && (i.immr >= 32 || i.imms >= 32)) return invalid(i);
        static constexpr Op ops[3] = {Op::Sbfm, Op::Bfm, Op::Ubfm};
        return normal(i, ops[opc]);
    }
    case 0b111: {  // Extract
        const uint32_t op21 = bits(r, 30, 29);
        const bool n = bit(r, 22);
        const bool o0 = bit(r, 21);
        i.rm = bits(r, 20, 16);
        i.imms = bits(r, 15, 10);
        if (op21 != 0 || o0 || i.sf != n) return invalid(i);
        if (!i.sf && i.imms >= 32) return invalid(i);
        return normal(i, Op::Extr);
    }
    }
    return invalid(i);
}

// ---------------------------------------------------------------------------
// Branches, exception generating and system instructions
// ---------------------------------------------------------------------------
Inst decode_system(Inst i) {
    const uint32_t r = i.raw;
    const bool l = bit(r, 21);
    const uint32_t op0 = bits(r, 20, 19);
    const uint32_t op1 = bits(r, 18, 16);
    const uint32_t crn = bits(r, 15, 12);
    const uint32_t crm = bits(r, 11, 8);
    const uint32_t op2 = bits(r, 7, 5);
    i.rt = bits(r, 4, 0);

    if (op0 == 0) {
        if (l) return invalid(i);
        if (op1 == 3 && crn == 2 && i.rt == 31)  // hints: unallocated hints behave as NOP
            return normal(i, Op::Hint);
        if (op1 == 3 && crn == 3 && i.rt == 31) {
            i.barrier = static_cast<uint8_t>(crm);
            switch (op2) {
            case 0b010: return normal(i, Op::Clrex);
            case 0b100: return normal(i, Op::Dsb);  // CRm 0b0000 (SSBB) / 0b0100 (PSSBB) are v8.0 encodings too
            case 0b101: return normal(i, Op::Dmb);
            case 0b110: return normal(i, Op::Isb);
            default: return invalid(i);
            }
        }
        return privileged(i);  // MSR (immediate): PSTATE fields are not writable from EL0 (except DAIF with UMA)
    }
    if (op0 == 1) {  // SYS/SYSL: only the EL0-accessible cache maintenance operations
        if (l || op1 != 3 || crn != 7 || op2 != 1 ) return privileged(i);
        switch (crm) {
        case 4: return normal(i, Op::DcZva);
        case 5:   // IC IVAU
        case 10:  // DC CVAC
        case 11:  // DC CVAU
        case 14:  // DC CIVAC
            return normal(i, Op::CacheMaint);
        default: return privileged(i);
        }
    }
    // MRS/MSR (register): only the registers user code can access on Horizon.
    i.sysreg = sysreg(op0, op1, crn, crm, op2);
    switch (i.sysreg) {
    case sr::kNzcv:
    case sr::kFpcr:
    case sr::kFpsr:
    case sr::kTpidrEl0:
        return normal(i, l ? Op::Mrs : Op::Msr);
    case sr::kTpidrroEl0:
    case sr::kCtrEl0:
    case sr::kDczidEl0:
    case sr::kCntfrqEl0:
    case sr::kCntpctEl0:
    case sr::kCntvctEl0:
        if (!l) return privileged(i);  // read-only from EL0
        return normal(i, Op::Mrs);
    default: return privileged(i);
    }
}

Inst decode_branch_sys(Inst i) {
    const uint32_t r = i.raw;

    if ((r & 0xFF000010u) == 0x54000000u) {  // B.cond
        i.cond = bits(r, 3, 0);
        i.target = i.pc + static_cast<uint64_t>(sext(bits(r, 23, 5), 19) * 4);
        i.op = Op::BCond;
        i.flow = Flow::CondBranch;
        return i;
    }
    if ((r & 0xFF000000u) == 0xD4000000u) {  // exception generation
        const uint32_t opc = bits(r, 23, 21);
        const uint32_t op2 = bits(r, 4, 2);
        const uint32_t ll = bits(r, 1, 0);
        i.imm = bits(r, 20, 5);
        if (op2 != 0) return invalid(i);
        if (opc == 0 && ll == 1) {
            i.op = Op::Svc;
            i.flow = Flow::Syscall;
            return i;
        }
        if (opc == 1 && ll == 0) {
            i.op = Op::Brk;
            i.flow = Flow::Trap;
            return i;
        }
        if (opc == 2 && ll == 0) return privileged(i);  // HLT: UNDEFINED at EL0 while halting debug is off
        return privileged(i);  // HVC/SMC/DCPSx: undefined at EL0
    }
    if ((r & 0xFFC00000u) == 0xD5000000u) return decode_system(i);

    if ((r & 0x7C000000u) == 0x14000000u) {  // B, BL
        i.target = i.pc + static_cast<uint64_t>(sext(bits(r, 25, 0), 26) * 4);
        if (bit(r, 31)) {
            i.op = Op::Bl;
            i.flow = Flow::Call;
        } else {
            i.op = Op::B;
            i.flow = Flow::Branch;
        }
        return i;
    }
    if ((r & 0x7E000000u) == 0x34000000u) {  // CBZ, CBNZ
        i.sf = bit(r, 31);
        i.rt = bits(r, 4, 0);
        i.target = i.pc + static_cast<uint64_t>(sext(bits(r, 23, 5), 19) * 4);
        i.op = bit(r, 24) ? Op::Cbnz : Op::Cbz;
        i.flow = Flow::CondBranch;
        return i;
    }
    if ((r & 0x7E000000u) == 0x36000000u) {  // TBZ, TBNZ
        i.bit = static_cast<uint8_t>((bit(r, 31) << 5) | bits(r, 23, 19));
        i.sf = bit(r, 31);
        i.rt = bits(r, 4, 0);
        i.target = i.pc + static_cast<uint64_t>(sext(bits(r, 18, 5), 14) * 4);
        i.op = bit(r, 24) ? Op::Tbnz : Op::Tbz;
        i.flow = Flow::CondBranch;
        return i;
    }
    if ((r & 0xFE000000u) == 0xD6000000u) {  // unconditional branch (register)
        const uint32_t opc = bits(r, 24, 21);
        const uint32_t op2 = bits(r, 20, 16);
        const uint32_t op3 = bits(r, 15, 10);
        const uint32_t op4 = bits(r, 4, 0);
        i.rn = bits(r, 9, 5);
        if (op2 != 0x1F || op3 != 0 || op4 != 0) return invalid(i);  // PAC forms are v8.3
        switch (opc) {
        case 0:
            i.op = Op::Br;
            i.flow = Flow::IndirectBranch;
            return i;
        case 1:
            i.op = Op::Blr;
            i.flow = Flow::IndirectCall;
            return i;
        case 2:
            i.op = Op::Ret;
            i.flow = Flow::Return;
            return i;
        default: return privileged(i);  // ERET/DRPS: undefined at EL0
        }
    }
    return invalid(i);
}

// ---------------------------------------------------------------------------
// Loads and stores
// ---------------------------------------------------------------------------
Inst decode_ldst_exclusive(Inst i) {
    const uint32_t r = i.raw;
    const uint32_t size = bits(r, 31, 30);
    const bool o2 = bit(r, 23);
    const bool l = bit(r, 22);
    const bool o1 = bit(r, 21);
    const bool o0 = bit(r, 15);
    i.rs = bits(r, 20, 16);
    i.rt2 = bits(r, 14, 10);
    i.rn = bits(r, 9, 5);
    i.rt = bits(r, 4, 0);
    i.rn_sp = true;
    i.load = l;
    i.size_log2 = static_cast<uint8_t>(size);
    i.sf = size == 3;

    if (!o2) {
        if (o1) {  // pair exclusives (size 0x with o1 = 1 is CASP, v8.1)
            if (size < 2) return invalid(i);
            i.pair = true;
        } else if (i.rt2 != 31) {
            return unpredictable(i);  // Rt2 is "should be one" for the single forms
        }
        if (l) {
            if (i.rs != 31) return unpredictable(i);
            if (i.pair && i.rt == i.rt2) return unpredictable(i);  // CONSTRAINED UNPREDICTABLE
            i.acquire = o0;
        } else {
            i.release = o0;
            // CONSTRAINED UNPREDICTABLE register overlaps
            if (i.rs == i.rt || (i.pair && i.rs == i.rt2)) return unpredictable(i);
            if (i.rs == i.rn && i.rn != 31) return unpredictable(i);
        }
        return normal(i, Op::LdStExcl);
    }
    if (o1 || !o0) return invalid(i);  // CAS (v8.1) / LDLAR, STLLR (v8.1 LOR)
    if (i.rs != 31 || i.rt2 != 31) return unpredictable(i);
    if (l)
        i.acquire = true;
    else
        i.release = true;
    return normal(i, Op::LdStAcqRel);
}

Inst decode_ldr_literal(Inst i) {
    const uint32_t r = i.raw;
    const uint32_t opc = bits(r, 31, 30);
    const bool v = bit(r, 26);
    i.rt = bits(r, 4, 0);
    i.target = i.pc + static_cast<uint64_t>(sext(bits(r, 23, 5), 19) * 4);
    i.mode = AddrMode::Literal;
    i.load = true;
    if (v) {
        if (opc == 3) return invalid(i);
        i.simd = true;
        i.size_log2 = static_cast<uint8_t>(2 + opc);
        return normal(i, Op::LdrLit);
    }
    switch (opc) {
    case 0: i.size_log2 = 2; break;
    case 1:
        i.size_log2 = 3;
        i.sf = true;
        break;
    case 2:
        i.size_log2 = 2;
        i.sign_extend = true;
        i.dest64 = true;
        i.sf = true;
        break;
    case 3: return normal(i, Op::PrfmLit);
    }
    return normal(i, Op::LdrLit);
}

Inst decode_ldst_pair(Inst i) {
    const uint32_t r = i.raw;
    const uint32_t opc = bits(r, 31, 30);
    const bool v = bit(r, 26);
    const uint32_t mode = bits(r, 24, 23);
    const bool l = bit(r, 22);
    i.rt2 = bits(r, 14, 10);
    i.rn = bits(r, 9, 5);
    i.rt = bits(r, 4, 0);
    i.rn_sp = true;
    i.load = l;

    if (v) {
        if (opc == 3) return invalid(i);
        i.simd = true;
        i.size_log2 = static_cast<uint8_t>(2 + opc);
    } else {
        switch (opc) {
        case 0: i.size_log2 = 2; break;
        case 1:
            if (!l || mode == 0) return invalid(i);  // STGP (v8.5) / no LDNPSW
            i.size_log2 = 2;
            i.sign_extend = true;
            i.dest64 = true;
            break;
        case 2:
            i.size_log2 = 3;
            i.sf = true;
            break;
        default: return invalid(i);
        }
    }
    i.offset = sext(bits(r, 21, 15), 7) * (int64_t{1} << i.size_log2);
    static constexpr AddrMode modes[4] = {AddrMode::Offset, AddrMode::PostIndex, AddrMode::Offset, AddrMode::PreIndex};
    i.mode = modes[mode];

    if (l && i.rt == i.rt2) return unpredictable(i);  // CONSTRAINED UNPREDICTABLE
    const bool wb = i.mode != AddrMode::Offset;
    if (wb && !v && i.rn != 31 && (i.rn == i.rt || i.rn == i.rt2)) return unpredictable(i);
    return normal(i, Op::LdStPair);
}

// size/V/opc of the single-register forms. Returns false for unallocated combos.
bool ldst_single_kind(Inst& i, uint32_t size, bool v, uint32_t opc, bool& prfm) {
    prfm = false;
    if (v) {
        i.simd = true;
        if (opc == 0 || opc == 1) {
            i.size_log2 = static_cast<uint8_t>(size);
            i.load = opc == 1;
            return true;
        }
        if (size == 0) {  // 128-bit
            i.size_log2 = 4;
            i.load = opc == 3;
            return true;
        }
        return false;
    }
    i.size_log2 = static_cast<uint8_t>(size);
    switch (opc) {
    case 0: i.load = false; break;
    case 1: i.load = true; break;
    case 2:  // LDRSB/LDRSH/LDRSW into X, PRFM for size 3
        if (size == 3) {
            prfm = true;
            return true;
        }
        i.load = true;
        i.sign_extend = true;
        i.dest64 = true;
        break;
    case 3:  // LDRSB/LDRSH into W
        if (size >= 2) return false;
        i.load = true;
        i.sign_extend = true;
        i.dest64 = false;
        break;
    }
    i.sf = i.dest64 || size == 3;
    return true;
}

Inst decode_ldst_single(Inst i) {
    const uint32_t r = i.raw;
    const uint32_t size = bits(r, 31, 30);
    const bool v = bit(r, 26);
    const uint32_t opc = bits(r, 23, 22);
    i.rn = bits(r, 9, 5);
    i.rt = bits(r, 4, 0);
    i.rn_sp = true;

    bool prfm = false;
    if (!ldst_single_kind(i, size, v, opc, prfm)) return invalid(i);
    const int scale = i.size_log2;

    if (bit(r, 24)) {  // unsigned immediate offset
        i.mode = AddrMode::Offset;
        i.offset = static_cast<int64_t>(bits(r, 21, 10)) << scale;
        return normal(i, prfm ? Op::Prfm : Op::LdSt);
    }

    if (bit(r, 21)) {
        if (bits(r, 11, 10) != 0b10) return invalid(i);  // atomics (v8.1), LDRAA/LDRAB (v8.3)
        const uint32_t option = bits(r, 15, 13);
        if ((option & 0b010) == 0) return invalid(i);
        i.mode = AddrMode::RegOffset;
        i.rm = bits(r, 20, 16);
        i.extend = static_cast<Extend>(option);
        i.amount = bit(r, 12) ? static_cast<uint8_t>(scale) : 0;
        return normal(i, prfm ? Op::Prfm : Op::LdSt);
    }

    i.offset = sext(bits(r, 20, 12), 9);
    switch (bits(r, 11, 10)) {
    case 0b00:  // unscaled (LDUR/STUR/PRFUM)
        i.mode = AddrMode::Offset;
        return normal(i, prfm ? Op::Prfm : Op::LdSt);
    case 0b10:  // unprivileged (LDTR/STTR): same as the normal form when executed at EL0
        if (v || prfm) return invalid(i);
        i.mode = AddrMode::Offset;
        return normal(i, Op::LdSt);
    case 0b01: i.mode = AddrMode::PostIndex; break;
    case 0b11: i.mode = AddrMode::PreIndex; break;
    }
    if (prfm) return invalid(i);
    if (!v && i.rn == i.rt && i.rn != 31) return unpredictable(i);  // CONSTRAINED UNPREDICTABLE writeback overlap
    return normal(i, Op::LdSt);
}

Inst decode_ldst(Inst i) {
    const uint32_t r = i.raw;
    if ((r & 0x3F000000u) == 0x08000000u) return decode_ldst_exclusive(i);
    if ((r & 0x3B000000u) == 0x18000000u) return decode_ldr_literal(i);
    if ((r & 0x3A000000u) == 0x28000000u) return decode_ldst_pair(i);
    if ((r & 0x3A000000u) == 0x38000000u) return decode_ldst_single(i);
    return decode_simd(i);  // SIMD load/store structures (LD1..LD4, ST1..ST4, LDnR)
}

// ---------------------------------------------------------------------------
// Data processing -- register
// ---------------------------------------------------------------------------
Inst decode_dp_reg(Inst i) {
    const uint32_t r = i.raw;
    i.sf = bit(r, 31);
    i.rd = bits(r, 4, 0);
    i.rn = bits(r, 9, 5);
    i.rm = bits(r, 20, 16);

    if ((r & 0x1F000000u) == 0x0A000000u) {  // logical (shifted register)
        const uint32_t opc = bits(r, 30, 29);
        const bool n = bit(r, 21);
        i.shift = static_cast<Shift>(bits(r, 23, 22));
        i.amount = bits(r, 15, 10);
        if (!i.sf && i.amount >= 32) return invalid(i);
        static constexpr Op ops[8] = {Op::AndReg, Op::BicReg, Op::OrrReg, Op::OrnReg,
                                      Op::EorReg, Op::EonReg, Op::AndsReg, Op::BicsReg};
        i.setflags = opc == 3;
        return normal(i, ops[(opc << 1) | n]);
    }
    if ((r & 0x1F200000u) == 0x0B000000u) {  // add/subtract (shifted register)
        i.setflags = bit(r, 29);
        i.shift = static_cast<Shift>(bits(r, 23, 22));
        i.amount = bits(r, 15, 10);
        if (i.shift == Shift::Ror) return invalid(i);
        if (!i.sf && i.amount >= 32) return invalid(i);
        return normal(i, bit(r, 30) ? Op::SubReg : Op::AddReg);
    }
    if ((r & 0x1F200000u) == 0x0B200000u) {  // add/subtract (extended register)
        i.setflags = bit(r, 29);
        if (bits(r, 23, 22) != 0) return invalid(i);
        i.extend = static_cast<Extend>(bits(r, 15, 13));
        i.amount = bits(r, 12, 10);
        if (i.amount > 4) return invalid(i);
        i.rn_sp = true;
        i.rd_sp = !i.setflags;
        return normal(i, bit(r, 30) ? Op::SubExt : Op::AddExt);
    }
    if ((r & 0x1FE00000u) == 0x1A000000u) {  // add/subtract (with carry)
        if (bits(r, 15, 10) != 0) return invalid(i);  // RMIF/SETF (v8.4)
        i.setflags = bit(r, 29);
        return normal(i, bit(r, 30) ? Op::Sbc : Op::Adc);
    }
    if ((r & 0x1FE00000u) == 0x1A400000u) {  // conditional compare
        if (!bit(r, 29) || bit(r, 10) || bit(r, 4)) return invalid(i);
        i.cond = bits(r, 15, 12);
        i.nzcv_imm = bits(r, 3, 0);
        i.ccmp_imm = bit(r, 11);
        if (i.ccmp_imm) i.imm = i.rm;  // imm5 sits in the Rm field
        i.setflags = true;
        return normal(i, bit(r, 30) ? Op::Ccmp : Op::Ccmn);
    }
    if ((r & 0x1FE00000u) == 0x1A800000u) {  // conditional select
        if (bit(r, 29) || bit(r, 11)) return invalid(i);
        i.cond = bits(r, 15, 12);
        static constexpr Op ops[4] = {Op::Csel, Op::Csinc, Op::Csinv, Op::Csneg};
        return normal(i, ops[(bit(r, 30) << 1) | bit(r, 10)]);
    }
    if ((r & 0x1F000000u) == 0x1B000000u) {  // data processing (3 source)
        if (bits(r, 30, 29) != 0) return invalid(i);
        const uint32_t op31 = bits(r, 23, 21);
        const bool o0 = bit(r, 15);
        i.ra = bits(r, 14, 10);
        switch (op31) {
        case 0b000: return normal(i, o0 ? Op::Msub : Op::Madd);
        case 0b001:
            if (!i.sf) return invalid(i);
            return normal(i, o0 ? Op::Smsubl : Op::Smaddl);
        case 0b010:
            if (!i.sf || o0) return invalid(i);
            if (i.ra != 31) return unpredictable(i);  // Ra is "should be one"
            return normal(i, Op::Smulh);
        case 0b101:
            if (!i.sf) return invalid(i);
            return normal(i, o0 ? Op::Umsubl : Op::Umaddl);
        case 0b110:
            if (!i.sf || o0) return invalid(i);
            if (i.ra != 31) return unpredictable(i);
            return normal(i, Op::Umulh);
        default: return invalid(i);
        }
    }
    if ((r & 0x5FE00000u) == 0x1AC00000u) {  // data processing (2 source)
        if (bit(r, 29)) return invalid(i);
        const uint32_t opcode = bits(r, 15, 10);
        switch (opcode) {
        case 0b000010: return normal(i, Op::Udiv);
        case 0b000011: return normal(i, Op::Sdiv);
        case 0b001000: return normal(i, Op::Lslv);
        case 0b001001: return normal(i, Op::Lsrv);
        case 0b001010: return normal(i, Op::Asrv);
        case 0b001011: return normal(i, Op::Rorv);
        default: break;
        }
        if ((opcode & 0b111000) == 0b010000) {  // CRC32 (optional in v8.0, present on Cortex-A57)
            const uint32_t sz = opcode & 3;
            if ((sz == 3) != i.sf) return invalid(i);
            i.size_log2 = static_cast<uint8_t>(sz);
            return normal(i, (opcode & 0b100) ? Op::Crc32c : Op::Crc32);
        }
        return invalid(i);
    }
    if ((r & 0x5FE00000u) == 0x5AC00000u) {  // data processing (1 source)
        if (bit(r, 29) || bits(r, 20, 16) != 0) return invalid(i);
        switch (bits(r, 15, 10)) {
        case 0b000000: return normal(i, Op::Rbit);
        case 0b000001: return normal(i, Op::Rev16);
        case 0b000010: return normal(i, i.sf ? Op::Rev32 : Op::Rev);
        case 0b000011:
            if (!i.sf) return invalid(i);
            return normal(i, Op::Rev);
        case 0b000100: return normal(i, Op::Clz);
        case 0b000101: return normal(i, Op::Cls);
        default: return invalid(i);  // PAC (v8.3)
        }
    }
    return invalid(i);
}

}  // namespace

bool decode_bit_masks(bool n, uint32_t imms, uint32_t immr, bool sf, uint64_t& wmask) {
    const int len = highest_set_bit((n ? 0x40u : 0u) | (~imms & 0x3Fu));
    if (len < 1) return false;
    const uint32_t levels = static_cast<uint32_t>(ones(len));
    if ((imms & levels) == levels) return false;  // reserved (all-ones element)
    const int s = static_cast<int>(imms & levels);
    const int rr = static_cast<int>(immr & levels);
    const int esize = 1 << len;
    if (!sf && esize > 32) return false;
    uint64_t elem = ror(ones(s + 1), rr, esize);
    uint64_t result = 0;
    for (int pos = 0; pos < 64; pos += esize)
        result |= elem << pos;
    wmask = sf ? result : (result & 0xFFFFFFFFull);
    return true;
}

Inst decode(uint32_t raw, uint64_t pc) {
    Inst i;
    i.raw = raw;
    i.pc = pc;
    const uint32_t op0 = bits(raw, 28, 25);
    if ((op0 & 0b1110) == 0b1000) return decode_dp_imm(i);
    if ((op0 & 0b1110) == 0b1010) return decode_branch_sys(i);
    if ((op0 & 0b0101) == 0b0100) return decode_ldst(i);
    if ((op0 & 0b0111) == 0b0101) return decode_dp_reg(i);
    if ((op0 & 0b0111) == 0b0111) return decode_simd(i);  // SIMD&FP data processing
    return invalid(i);  // reserved/UDF, SVE
}

const char* op_name(Op op) {
    switch (op) {
    case Op::Invalid: return "invalid";
    case Op::Adr: return "adr";
    case Op::Adrp: return "adrp";
    case Op::AddImm: return "add_imm";
    case Op::SubImm: return "sub_imm";
    case Op::AndImm: return "and_imm";
    case Op::OrrImm: return "orr_imm";
    case Op::EorImm: return "eor_imm";
    case Op::AndsImm: return "ands_imm";
    case Op::Movn: return "movn";
    case Op::Movz: return "movz";
    case Op::Movk: return "movk";
    case Op::Sbfm: return "sbfm";
    case Op::Bfm: return "bfm";
    case Op::Ubfm: return "ubfm";
    case Op::Extr: return "extr";
    case Op::AndReg: return "and_reg";
    case Op::BicReg: return "bic_reg";
    case Op::OrrReg: return "orr_reg";
    case Op::OrnReg: return "orn_reg";
    case Op::EorReg: return "eor_reg";
    case Op::EonReg: return "eon_reg";
    case Op::AndsReg: return "ands_reg";
    case Op::BicsReg: return "bics_reg";
    case Op::AddReg: return "add_reg";
    case Op::SubReg: return "sub_reg";
    case Op::AddExt: return "add_ext";
    case Op::SubExt: return "sub_ext";
    case Op::Adc: return "adc";
    case Op::Sbc: return "sbc";
    case Op::Ccmn: return "ccmn";
    case Op::Ccmp: return "ccmp";
    case Op::Csel: return "csel";
    case Op::Csinc: return "csinc";
    case Op::Csinv: return "csinv";
    case Op::Csneg: return "csneg";
    case Op::Rbit: return "rbit";
    case Op::Rev16: return "rev16";
    case Op::Rev32: return "rev32";
    case Op::Rev: return "rev";
    case Op::Clz: return "clz";
    case Op::Cls: return "cls";
    case Op::Udiv: return "udiv";
    case Op::Sdiv: return "sdiv";
    case Op::Lslv: return "lslv";
    case Op::Lsrv: return "lsrv";
    case Op::Asrv: return "asrv";
    case Op::Rorv: return "rorv";
    case Op::Crc32: return "crc32";
    case Op::Crc32c: return "crc32c";
    case Op::Madd: return "madd";
    case Op::Msub: return "msub";
    case Op::Smaddl: return "smaddl";
    case Op::Smsubl: return "smsubl";
    case Op::Umaddl: return "umaddl";
    case Op::Umsubl: return "umsubl";
    case Op::Smulh: return "smulh";
    case Op::Umulh: return "umulh";
    case Op::B: return "b";
    case Op::Bl: return "bl";
    case Op::BCond: return "b_cond";
    case Op::Cbz: return "cbz";
    case Op::Cbnz: return "cbnz";
    case Op::Tbz: return "tbz";
    case Op::Tbnz: return "tbnz";
    case Op::Br: return "br";
    case Op::Blr: return "blr";
    case Op::Ret: return "ret";
    case Op::Svc: return "svc";
    case Op::Brk: return "brk";
    case Op::Hint: return "hint";
    case Op::Clrex: return "clrex";
    case Op::Dsb: return "dsb";
    case Op::Dmb: return "dmb";
    case Op::Isb: return "isb";
    case Op::Mrs: return "mrs";
    case Op::Msr: return "msr";
    case Op::DcZva: return "dc_zva";
    case Op::CacheMaint: return "cache_maint";
    case Op::LdrLit: return "ldr_lit";
    case Op::PrfmLit: return "prfm_lit";
    case Op::LdSt: return "ldst";
    case Op::Prfm: return "prfm";
    case Op::LdStPair: return "ldst_pair";
    case Op::LdStExcl: return "ldst_excl";
    case Op::LdStAcqRel: return "ldst_acqrel";
    case Op::Simd: return "simd";
    case Op::kCount: break;
    }
    return "?";
}

std::string describe(const Inst& i) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%08x @%llx %s rd=%u rn=%u rm=%u rt=%u rt2=%u sf=%d S=%d imm=%llx off=%lld",
                  i.raw, static_cast<unsigned long long>(i.pc), op_name(i.op), i.rd, i.rn, i.rm, i.rt, i.rt2,
                  i.sf ? 1 : 0, i.setflags ? 1 : 0, static_cast<unsigned long long>(i.imm),
                  static_cast<long long>(i.offset));
    return buf;
}

}  // namespace lgp::recomp::a64
