// Control-flow classification on top of the lifter's bit-exact decoder
// (lgp::recomp::a64::decode), docs/design/p2-interfaces.md section 2: the CFG
// and the lifter see exactly the same instruction boundaries and targets.
#include "lgp/recomp/a64_cf.hpp"

#include "lgp/recomp/a64.hpp"

namespace lgp::recomp {

namespace {

// Encodings that exist in ARMv8.0 but are UNDEFINED at EL0 (the decoder reports
// them as Invalid::Privileged). The ones in the branch/exception space keep their
// architectural control-flow class so the CFG terminates the block the same way
// as before; every other privileged word ends the path as Unknown.
bool privileged_flow(uint32_t insn, CfInfo& r) {
    if ((insn & 0xFF000000u) == 0xD4000000u && ((insn >> 2) & 7) == 0) {  // exception generation
        const uint32_t opc = (insn >> 21) & 7, ll = insn & 3;
        r.imm = (insn >> 5) & 0xFFFF;
        if (opc == 0 && (ll == 2 || ll == 3)) { r.kind = CfKind::Svc; return true; }   // HVC / SMC
        if (opc == 2 && ll == 0) { r.kind = CfKind::Trap; return true; }               // HLT
        if (opc == 5 && ll != 0) { r.kind = CfKind::Trap; return true; }               // DCPS1-3
        return false;
    }
    if (insn == 0xD69F03E0u || insn == 0xD6BF03E0u) {  // ERET, DRPS
        r.kind = CfKind::Return;
        return true;
    }
    return false;
}

}  // namespace

CfInfo classify(uint32_t insn, uint64_t pc) {
    CfInfo r;
    // UDF #imm16: permanently undefined, used as a trap (and 0 as padding).
    if ((insn & 0xFFFF0000u) == 0) {
        r.kind = CfKind::Trap;
        r.imm = insn & 0xFFFF;
        return r;
    }
    const a64::Inst i = a64::decode(insn, pc);
    if (i.op == a64::Op::Invalid) {
        switch (i.invalid) {
            case a64::Invalid::None:
            case a64::Invalid::NotImplemented:
                return r;  // valid instruction, no control flow
            case a64::Invalid::Privileged:
                if (privileged_flow(insn, r)) return r;
                [[fallthrough]];
            case a64::Invalid::Unallocated:
            case a64::Invalid::Unpredictable:
                r.kind = CfKind::Unknown;
                return r;
        }
        r.kind = CfKind::Unknown;
        return r;
    }
    switch (i.flow) {
        case a64::Flow::Normal:
            return r;
        case a64::Flow::Branch:
            r.kind = CfKind::Branch;
            r.target = i.target;
            return r;
        case a64::Flow::Call:
            r.kind = CfKind::Call;
            r.target = i.target;
            return r;
        case a64::Flow::CondBranch:
            r.kind = CfKind::CondBranch;
            r.target = i.target;
            switch (i.op) {
                case a64::Op::Cbz: r.cond_kind = CondKind::Cbz; r.reg = i.rt; break;
                case a64::Op::Cbnz: r.cond_kind = CondKind::Cbnz; r.reg = i.rt; break;
                case a64::Op::Tbz: r.cond_kind = CondKind::Tbz; r.reg = i.rt; r.cond = i.bit; break;
                case a64::Op::Tbnz: r.cond_kind = CondKind::Tbnz; r.reg = i.rt; r.cond = i.bit; break;
                default: r.cond_kind = CondKind::BCond; r.cond = i.cond; break;
            }
            return r;
        case a64::Flow::IndirectBranch:
            r.kind = CfKind::IndirectBranch;
            r.reg = i.rn;
            return r;
        case a64::Flow::IndirectCall:
            r.kind = CfKind::IndirectCall;
            r.reg = i.rn;
            return r;
        case a64::Flow::Return:
            r.kind = CfKind::Return;
            r.reg = i.rn;
            return r;
        case a64::Flow::Syscall:
            r.kind = CfKind::Svc;
            r.imm = static_cast<uint32_t>(i.imm);
            return r;
        case a64::Flow::Trap:
            r.kind = CfKind::Trap;
            r.imm = static_cast<uint32_t>(i.imm);
            return r;
    }
    return r;
}

const char* cond_name(uint8_t cond) {
    static const char* names[16] = {"eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc",
                                    "hi", "ls", "ge", "lt", "gt", "le", "al", "nv"};
    return names[cond & 15];
}

}  // namespace lgp::recomp
