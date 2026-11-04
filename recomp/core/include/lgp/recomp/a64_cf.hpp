// Narrow control-flow classification of A64 instructions, the only decoder
// interface the CFG uses. A thin adapter over the lifter's bit-exact decoder
// (lgp::recomp::a64::decode, docs/design/p2-interfaces.md section 2), so CFG
// and lifter never disagree on an instruction. Unallocated and CONSTRAINED
// UNPREDICTABLE words, and EL1-only instructions outside the branch/exception
// space, are Unknown; valid SIMD&FP instructions are None.
#pragma once

#include <cstdint>

namespace lgp::recomp {

enum class CfKind : uint8_t {
    None,            // no control flow, falls through
    Branch,          // B imm26
    CondBranch,      // B.cond, CBZ/CBNZ, TBZ/TBNZ
    Call,            // BL imm26
    IndirectBranch,  // BR Xn
    IndirectCall,    // BLR Xn
    Return,          // RET, ERET, DRPS
    Trap,            // BRK, HLT, UDF: no fallthrough
    Svc,             // SVC/HVC/SMC: falls through
    Unknown          // undecodable / unallocated encoding
};

// Condition codes for B.cond (ARM encoding); 16 = "not a B.cond" (CBZ/TBZ).
enum class CondKind : uint8_t { BCond = 0, Cbz, Cbnz, Tbz, Tbnz };

struct CfInfo {
    CfKind kind = CfKind::None;
    uint64_t target = 0;   // absolute target for Branch / CondBranch / Call
    uint8_t reg = 31;      // Xn for IndirectBranch / IndirectCall
    uint8_t cond = 0;      // B.cond condition (0..15) when cond_kind == BCond
    CondKind cond_kind = CondKind::BCond;
    uint32_t imm = 0;      // SVC / BRK / HLT immediate
};

CfInfo classify(uint32_t insn, uint64_t pc);

constexpr uint32_t kNop = 0xD503201F;
inline bool is_padding(uint32_t insn) { return insn == kNop || insn == 0; }

const char* cond_name(uint8_t cond);

}  // namespace lgp::recomp
