// AArch64 (A64) instruction decoder for the static recompiler.
//
// Bit-exact decoder for the ARMv8.0-A subset the Switch's Cortex-A57 runs in
// EL0: integer data processing, loads/stores (including SIMD&FP register
// loads/stores and exclusives), branches, exceptions and the EL0-visible
// system instructions. SIMD&FP data processing is decoded as Op::Invalid for
// now (roadmap P2.4).
//
// One decoder serves both the lifter and the control-flow analysis, so the two
// can never disagree about what an instruction word means.
#pragma once

#include <cstdint>
#include <string>

namespace lgp::recomp::a64 {

enum class Op : uint16_t {
    Invalid,  // unallocated, unpredictable or not supported (yet)

    // Data processing, immediate
    Adr,
    Adrp,
    AddImm,  // ADD/ADDS (setflags)
    SubImm,  // SUB/SUBS
    AndImm,
    OrrImm,
    EorImm,
    AndsImm,
    Movn,
    Movz,
    Movk,
    Sbfm,
    Bfm,
    Ubfm,
    Extr,

    // Data processing, register
    AndReg,
    BicReg,
    OrrReg,
    OrnReg,
    EorReg,
    EonReg,
    AndsReg,
    BicsReg,
    AddReg,  // shifted register, ADD/ADDS
    SubReg,
    AddExt,  // extended register, ADD/ADDS
    SubExt,
    Adc,  // ADC/ADCS
    Sbc,  // SBC/SBCS
    Ccmn,
    Ccmp,
    Csel,
    Csinc,
    Csinv,
    Csneg,
    Rbit,
    Rev16,
    Rev32,
    Rev,
    Clz,
    Cls,
    Udiv,
    Sdiv,
    Lslv,
    Lsrv,
    Asrv,
    Rorv,
    Crc32,   // CRC32B/H/W/X, size_log2 = element size
    Crc32c,  // CRC32CB/H/W/X
    Madd,
    Msub,
    Smaddl,
    Smsubl,
    Umaddl,
    Umsubl,
    Smulh,
    Umulh,

    // Branches
    B,
    Bl,
    BCond,
    Cbz,
    Cbnz,
    Tbz,
    Tbnz,
    Br,
    Blr,
    Ret,

    // Exception generation
    Svc,
    Brk,

    // System
    Hint,  // NOP and every other hint (YIELD/WFE/WFI/SEV/SEVL and unallocated hints run as NOP)
    Clrex,
    Dsb,
    Dmb,
    Isb,
    Mrs,
    Msr,  // register form
    DcZva,
    CacheMaint,  // DC CVAC/CVAU/CIVAC, IC IVAU: no architectural effect for user code

    // Loads and stores
    LdrLit,      // LDR (literal), including LDRSW and SIMD&FP
    PrfmLit,     // PRFM (literal)
    LdSt,        // single register: unsigned offset, unscaled, pre/post-index, register offset, unprivileged
    Prfm,        // PRFM/PRFUM (immediate, unscaled, register)
    LdStPair,    // LDP/STP/LDPSW/LDNP/STNP, including SIMD&FP
    LdStExcl,    // LDXR/LDAXR/STXR/STLXR and the pair forms LDXP/LDAXP/STXP/STLXP
    LdStAcqRel,  // LDAR/STLR

    // SIMD&FP data processing and Advanced SIMD load/store structures (P2.4):
    // decoded and lifted by a64_simd.hpp, always Flow::Normal.
    Simd,

    kCount
};

enum class Shift : uint8_t { Lsl, Lsr, Asr, Ror };
enum class Extend : uint8_t { Uxtb, Uxth, Uxtw, Uxtx, Sxtb, Sxth, Sxtw, Sxtx };
enum class AddrMode : uint8_t { Offset, PreIndex, PostIndex, RegOffset, Literal };

// Control-flow class, for the CFG (P2.2) and the lifter.
enum class Flow : uint8_t {
    Normal,          // falls through to pc + 4
    Branch,          // B: unconditional, static target
    CondBranch,      // B.cond/CBZ/CBNZ/TBZ/TBNZ: static target or pc + 4
    Call,            // BL: static target, returns to pc + 4
    IndirectBranch,  // BR
    IndirectCall,    // BLR
    Return,          // RET
    Syscall,         // SVC: continues at pc + 4 after the runtime call
    Trap,            // BRK and Op::Invalid: does not continue
};

// Why an encoding decodes as Op::Invalid. Every Invalid word raises an
// UNDEFINED exception on the Switch, except possibly Unpredictable ones.
enum class Invalid : uint8_t {
    None,
    Unallocated,     // unallocated in ARMv8.0-A (includes later extensions: LSE, PAC, MTE, ...)
    Unpredictable,   // CONSTRAINED UNPREDICTABLE operand combination or "should be" field violated
    Privileged,      // exists, but UNDEFINED at EL0 (HLT, HVC, ERET, EL1 system registers, ...)
    NotImplemented,  // valid ARMv8.0 user instruction the decoder does not cover yet (SIMD&FP: P2.4)
};

// System register encoding op0:op1:CRn:CRm:op2 (2:3:4:4:3 bits) as used by MRS/MSR.
constexpr uint32_t sysreg(uint32_t op0, uint32_t op1, uint32_t crn, uint32_t crm, uint32_t op2) {
    return (op0 << 14) | (op1 << 11) | (crn << 7) | (crm << 3) | op2;
}
namespace sr {
inline constexpr uint32_t kNzcv = sysreg(3, 3, 4, 2, 0);
inline constexpr uint32_t kFpcr = sysreg(3, 3, 4, 4, 0);
inline constexpr uint32_t kFpsr = sysreg(3, 3, 4, 4, 1);
inline constexpr uint32_t kTpidrEl0 = sysreg(3, 3, 13, 0, 2);
inline constexpr uint32_t kTpidrroEl0 = sysreg(3, 3, 13, 0, 3);
inline constexpr uint32_t kCtrEl0 = sysreg(3, 3, 0, 0, 1);
inline constexpr uint32_t kDczidEl0 = sysreg(3, 3, 0, 0, 7);
inline constexpr uint32_t kCntfrqEl0 = sysreg(3, 3, 14, 0, 0);
inline constexpr uint32_t kCntpctEl0 = sysreg(3, 3, 14, 0, 1);
inline constexpr uint32_t kCntvctEl0 = sysreg(3, 3, 14, 0, 2);
}  // namespace sr

struct Inst {
    uint32_t raw = 0;
    uint64_t pc = 0;
    Op op = Op::Invalid;
    Flow flow = Flow::Trap;
    Invalid invalid = Invalid::None;

    bool sf = false;        // 64-bit operation (data processing) / 64-bit register (loads, CBZ, ...)
    bool setflags = false;  // the S variants
    // Register fields; 31 means SP where the matching *_sp flag is set, otherwise XZR/WZR.
    uint8_t rd = 31, rn = 31, rm = 31, ra = 31, rt = 31, rt2 = 31, rs = 31;
    bool rd_sp = false;
    bool rn_sp = false;

    Shift shift = Shift::Lsl;  // shifted-register operand
    uint8_t amount = 0;        // shift amount, extend left shift (0..4), MOVx hw*16, ADD/SUB imm shift
    Extend extend = Extend::Uxtx;
    uint8_t cond = 0;      // B.cond, CSEL, CCMP
    uint8_t nzcv_imm = 0;  // CCMP/CCMN flags when the condition fails
    bool ccmp_imm = false;  // CCMP/CCMN with an immediate second operand (in imm)
    uint8_t immr = 0, imms = 0;  // bitfield, EXTR (imms = lsb)
    uint8_t bit = 0;             // TBZ/TBNZ bit number
    uint64_t imm = 0;  // decoded immediate (logical bitmask, ADD/SUB imm12 unshifted, MOVx imm16, SVC/BRK imm16, CCMP imm5, ...)

    // Loads and stores
    uint8_t size_log2 = 0;   // element size: 0=B 1=H 2=W/S 3=X/D 4=Q
    bool load = false;
    bool sign_extend = false;  // LDRSB/LDRSH/LDRSW/LDPSW
    bool dest64 = false;       // sign-extending load into an X register
    bool simd = false;         // SIMD&FP register (B/H/S/D/Q)
    bool acquire = false;
    bool release = false;
    bool pair = false;  // exclusive pair forms
    AddrMode mode = AddrMode::Offset;
    int64_t offset = 0;  // byte offset for immediate modes (already scaled)

    uint64_t target = 0;  // branch target, literal address, ADR/ADRP result
    uint32_t sysreg = 0;  // MRS/MSR, packed with a64::sysreg()
    uint8_t barrier = 0;  // DMB/DSB option (CRm)
};

// Decodes one instruction word located at `pc`. Unallocated, CONSTRAINED
// UNPREDICTABLE and not-yet-supported encodings come back as Op::Invalid.
Inst decode(uint32_t raw, uint64_t pc);

// Lower-case mnemonic of the decoded operation (the base form, not the
// preferred alias), e.g. "add", "ldst", "ubfm". For statistics and messages.
const char* op_name(Op op);

// Short human-readable form for diagnostics (not a full disassembler).
std::string describe(const Inst& inst);

// DecodeBitMasks() for logical immediates (immediate = true). Returns false
// for reserved encodings.
bool decode_bit_masks(bool n, uint32_t imms, uint32_t immr, bool sf, uint64_t& wmask);

}  // namespace lgp::recomp::a64
