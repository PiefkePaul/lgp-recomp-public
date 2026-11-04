// Small shader IR between the Maxwell decoder and the WGSL backend.
//
// Deliberately simple (spike): a linear list of operations on the hardware
// register file (R0..R254 as 32-bit values, P0..P6 as booleans), with
// encoding details removed (r/c/imm forms, modifier bit positions, envydis
// quirks). Control flow stays as branches to byte addresses; the backend
// turns it into a dispatcher loop. A real SSA IR with structurization is
// follow-up work for P4.2, see docs/knowledge/maxwell-shader-to-wgsl.md.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "lgp/gpu/shader/sph.h"

namespace lgp::gpu::ir {

enum class Kind : uint8_t {
    None,
    Reg,   // index = GPR (255 = zero)
    Pred,  // index = predicate (7 = true)
    Imm,   // imm = raw 32 bits
    CBuf,  // index = bank, imm = byte offset, base = optional dynamic GPR (255 = none)
};

struct Operand {
    Kind kind = Kind::None;
    uint32_t index = 0;
    uint32_t imm = 0;
    uint32_t base = 255;
    bool neg = false;  // float or integer negation, depending on the op
    bool abs = false;  // float only
    bool inv = false;  // bitwise / boolean not

    static Operand reg(uint32_t r) { return {Kind::Reg, r}; }
    static Operand pred(uint32_t p, bool inv = false) {
        Operand o{Kind::Pred, p};
        o.inv = inv;
        return o;
    }
    static Operand imm32(uint32_t v) {
        Operand o{Kind::Imm};
        o.imm = v;
        return o;
    }
};

enum class Opcode : uint8_t {
    // dst[0] = f(src...) on 32-bit values
    Mov,
    FMov,        // float move applying neg/abs (and sat)
    FAdd, FMul, FFma, FMin, FMax,
    FRcp, FRsq, FSqrt, FSin, FCos, FEx2, FLg2,
    FRound, FFloor, FCeil, FTrunc,  // float -> float (F2F .round/.floor/...)
    F2I, F2U,                       // float -> int, aux = rounding (0 nearest, 1 floor, 2 ceil, 3 trunc),
                                    // aux2 = result size (0 8, 1 16, 2 32 bits; saturated)
    I2F, U2F,
    IAdd, IScAdd /* (s0 << aux) + s1 */, Shl /* aux = wrap */, ShrU, ShrS, And, Or, Xor,
    IAdd3,       // dst = s0 + s1 + s2 (operand neg = two's complement negation)
    XMad,        // 16x16+32 multiply-add, aux = XMad* flags, aux2 = XMadMode, src[3] = raw B for mrg/cbcc
    I2I,         // integer resize: aux = source type, aux2 = dest type (IntType), aux3 = byte select
    Bfe,         // bit field extract: s1 = (len << 8) | pos, aux = 1 if signed
    R2P,         // P[i] = bit (aux + i) of s0, for each i set in mask s1 (i < 7)
    ISetBool,    // dst = (icmp bop p) ? 1.0f : 0   (aux = ICmp, aux2 = BoolOp, aux3 = 1 if signed)
    ISetMask,    // dst = (icmp bop p) ? 0xffffffff : 0
    PSetBool,    // dst = ((p0 aux p1) aux2 p2) ? 1.0f : 0
    PSetMask,    // dst = ((p0 aux p1) aux2 p2) ? 0xffffffff : 0
    CSetP,       // like PSetP, comparison = condition code test aux (CcTest) on the flags
    IMin, IMax, UMin, UMax,
    Select,      // dst = src[2] (pred) ? src[0] : src[1]
    FSetBool,    // dst = cmp ? 1.0f : 0.0f        (aux = FCmp, aux2 = bool op, src[2] = pred)
    FSetMask,    // dst = cmp ? 0xffffffff : 0
    // predicate results: pdst[0] = cmp bop p, pdst[1] = !cmp bop p
    FSetP,       // aux = FCmp, aux2 = BoolOp
    ISetP,       // aux = ICmp, aux2 = BoolOp, aux3 = 1 if signed
    PSetP,       // pdst[0] = (s0 aux s1) aux2 s2, pdst[1] = !(s0 aux s1) aux2 s2
    // I/O
    LoadAttr,    // dst[0..n) = a[addr + 4k], aux = addr, aux2 = n, aux3 = 1 for output read
    StoreAttr,   // a[addr + 4k] = src[k]
    Interp,      // dst = interpolated a[aux]; aux2 = mode (0 pass, 1 mul-w, 2 constant, 3 sc); src[0] = multiplier
    LoadConst,   // dst[0..n) = c[bank][base + offset + 4k], aux2 = n; src[0] = CBuf operand
    TexSample,   // dst[] = sample(tex handle aux, coords src[]), aux2 = lod mode, mask in aux3; Inst::tex_offset
    TexFetch,    // dst[] = texelFetch(handle aux, src = {x, y, lod}), mask aux3 (TLDS); Inst::tex_offset
    TexQuery,    // dst[] = (width, height, depth, levels) of handle aux at level src[0], mask aux3 (TXQ)
    TexLod,      // dst[] = (relative, absolute) LOD as 8.8 fixed point for coords src[0..1], mask aux3 (TMML)
    TexGather,   // dst[] = gather component aux2 of the 2x2 footprint at src[] (TLD4), mask aux3; tex_offset(2)
    // Compute stage and memory (P4.3). Memory sizes: aux = MemSize; aux2 = signed byte offset.
    SysReg,      // dst = system register aux (S2R)
    LoadGlobal,  // dst[] = g[(src[1]:src[0]) + aux2], src[1] = RZ for 32-bit addresses
    StoreGlobal, // g[(src[1]:src[0]) + aux2] = src[2...]
    LoadShared, StoreShared,  // s[src[0] + aux2]; stores take the data in src[1...]
    LoadLocal, StoreLocal,    // l[src[0] + aux2]
    AtomGlobal,  // dst (may be empty for RED) = atomic aux3 (AtomOp) on g[(src[1]:src[0]) + aux2] with src[2]
    AtomShared,  // dst = atomic aux3 on s[src[0] + aux2] with src[1]
    Barrier,     // BAR.SYNC 0 (all threads of the workgroup)
    Lea,         // dst = (src[0] << aux) + src[1]                       (carry: Inst::cc / carry_in)
    LeaHi,       // dst = hi32((src[2]:src[0]) << aux) + src[1]
    Shf,         // funnel shift of src[2]:src[0] by src[1]; aux = ShfFlags
    // Control flow (targets are byte addresses relative to code start)
    Branch,      // goto target (guarded)
    PushSync,    // push target on the SSY stack
    PopSync,     // pop the SSY stack and jump there
    PushBreak,   // push target on the PBK stack
    PopBreak,
    Kill,
    Exit,
};

enum class FCmp : uint8_t { F, Lt, Eq, Le, Gt, Ne, Ge, Num, Nan, Ltu, Equ, Leu, Gtu, Neu, Geu, T };
enum class ICmp : uint8_t { F, Lt, Eq, Le, Gt, Ne, Ge, T };
enum class BoolOp : uint8_t { And, Or, Xor };
enum class LodMode : uint8_t { Auto, Zero, Lod, Bias };
// Memory access sizes (LDG/STG/LDS/STS/LDL/STL encoding).
enum class MemSize : uint8_t { U8, S8, U16, S16, B32, B64, B128 };
// Atomic operations (ATOM/RED/ATOMS encoding); aux2 of AtomGlobal/AtomShared holds the
// offset, the operand type is Inst::atom_signed.
enum class AtomOp : uint8_t { Add, Min, Max, Inc, Dec, And, Or, Xor, Exch };
// SHF: aux bits.
enum ShfFlags : uint32_t { kShfRight = 1, kShfHigh = 2, kShfWrap = 4, kShf64 = 8, kShfSigned = 16 };
// S2R system registers the translator knows.
enum SysRegIndex : uint32_t {
    kSrLaneId = 0x00, kSrTid = 0x20, kSrTidX = 0x21, kSrTidY = 0x22, kSrTidZ = 0x23,
    kSrCtaIdX = 0x25, kSrCtaIdY = 0x26, kSrCtaIdZ = 0x27,
};
// XMAD: flags in Inst::aux, mode in Inst::aux2.
enum XMadFlags : uint32_t { kXMadSignA = 1, kXMadSignB = 2, kXMadHiA = 4, kXMadHiB = 8, kXMadPsl = 16, kXMadMrg = 32 };
enum class XMadMode : uint8_t { None, CLo, CHi, CBcc };
// I2I operand types (encoding: bits 0..1 size u8/u16/u32, bit 2 signed).
enum class IntType : uint8_t { U8 = 0, U16 = 1, U32 = 2, S8 = 4, S16 = 5, S32 = 6 };

struct Inst {
    Opcode op = Opcode::Mov;
    uint32_t address = 0;            // byte address of the source instruction
    uint32_t guard = 7;              // predicate guard (7 = always)
    bool guard_inv = false;
    std::vector<uint32_t> dst;       // GPRs written
    std::vector<uint32_t> pdst;      // predicates written
    std::vector<Operand> src;
    uint32_t aux = 0, aux2 = 0, aux3 = 0;
    bool sat = false;                // clamp float result to [0, 1]
    bool cc = false;                 // also write the condition codes (zero/sign of the 32-bit result)
    uint32_t pmode = 0;              // logic ops with predicate output: 1 = true, 2 = result == 0, 3 = != 0
    bool depth_compare = false;      // TexSample: s[last] is the depth reference (shadow sampler)
    uint32_t tex_dim = 2;            // TexSample/TexFetch: 2 = 2D, 3 = 3D (src then holds x, y, z first)
    bool tex_array = false;          // TexSample/TexGather: 2D array texture, src[2] = layer (integer register)
    uint32_t scale = 0;              // FMUL result scale: 1 = /2, 2 = /4, 3 = /8, 4 = *8, 5 = *4, 6 = *2
    uint32_t target = 0;             // branch target (byte address)
    bool carry_in = false;           // IADD.X / LEA.X: add the carry flag
    bool atom_signed = false;        // AtomGlobal/AtomShared: s32 instead of u32
    // TexSample/TexFetch with .AOFFI: register holding the texel offsets
    // (4-bit signed per component, x in bits 0..3, y in 4..7); Kind::None = no offset.
    // TexGather: 8 bits per component (x in bits 0..7, y in 8..15); with .PTP
    // (tex_ptp) one offset pair per gathered texel, texels 0 and 1 in tex_offset
    // (bits 0..15, 16..31), texels 2 and 3 in tex_offset2.
    Operand tex_offset;
    Operand tex_offset2;
    bool tex_ptp = false;
};

struct Program {
    shader::Sph sph;  // compute: no SPH in the binary; sph.stage = Compute, the rest zero
    std::vector<Inst> code;
    uint32_t code_bytes = 0;
};

struct LiftResult {
    bool ok = false;
    std::string error;  // first unsupported instruction / modifier, with address
    Program program;
};

// Decodes and lifts a complete shader (SPH followed by code). Stops at the
// end of the input; unsupported instructions or modifiers are an error, never
// silently dropped.
LiftResult lift(const uint8_t* data, size_t size);
// Compute shaders (NVN) have no SPH: the code starts with the first bundle.
LiftResult lift_compute(const uint8_t* data, size_t size);

}  // namespace lgp::gpu::ir
