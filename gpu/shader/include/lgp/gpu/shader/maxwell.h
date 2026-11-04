// Maxwell (SM50/SM52, Tegra X1 = GM20B) shader instruction decoder.
//
// Covers the instruction subset needed for simple vertex and fragment shaders
// (P4 shader spike). Encodings were derived from envytools' gm107 tables and
// Mesa's NAK SM50 encoder (both MIT, see THIRD_PARTY.md) and are checked
// against envydis by tests/gpu/test_maxwell_decoder.cpp.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace lgp::gpu::maxwell {

// One 64-bit instruction word. Code is grouped in bundles of four words: a
// scheduling word (stall counts, barriers) followed by three instructions.
constexpr uint32_t kBundleBytes = 32;
constexpr uint32_t kRegZero = 255;  // RZ: reads 0, writes are discarded
constexpr uint32_t kPredTrue = 7;   // PT: reads true, writes are discarded

enum class Op : uint8_t {
    Invalid,
    // Float ALU
    FADD, FADD32I, FMUL, FMUL32I, FFMA, FMNMX, FSETP, FSET, MUFU, RRO,
    F2F, F2I, I2F,
    // Integer ALU
    IADD, IADD32I, IADD3, ISCADD, SHL, SHR, LOP, LOP32I, ISETP, ISET, IMNMX, SEL,
    MOV, MOV32I, PSETP, PSET, CSETP, R2P, I2I, XMAD, BFE,
    // Attribute, constant and texture access
    ALD, AST, IPA, LDC, TEXS, TEX, TLDS, TXQ, TMML, TLD4,
    // Compute: system registers, memory, atomics, barriers (P4.3)
    S2R, LDG, STG, LDS, STS, LDL, STL, ATOM, RED, ATOMS, BAR, MEMBAR,
    // Integer helpers for address arithmetic
    LEA, LEA_HI, SHF_L, SHF_R,
    // Control flow and misc
    EXIT, BRA, SSY, SYNC, PBK, BRK, KIL, NOP, DEPBAR,
};

// Encoding form of the "B" operand (ALU source at bits 20+).
enum class Form : uint8_t {
    None,
    Reg,    // B = register at bits 20..27
    CBuf,   // B = c[bank 34..38][offset 20..33 * 4]
    Imm20,  // B = 19-bit immediate at 20..38, sign/high bit at 56
    Imm32,  // B = 32-bit immediate at 20..51 (the *32I opcodes)
    Imm16,  // XMAD only: B = unsigned 16-bit immediate at 20..35
    RegC,   // FFMA/XMAD: B = register 39..46, C = c[] (opcodes 0x5180, 0x5100)
};

struct Instr {
    uint64_t raw = 0;
    Op op = Op::Invalid;
    Form form = Form::None;
};

constexpr uint32_t field(uint64_t w, unsigned lo, unsigned n) {
    return static_cast<uint32_t>((w >> lo) & ((uint64_t{1} << n) - 1));
}
constexpr bool bit(uint64_t w, unsigned b) { return ((w >> b) & 1) != 0; }

// Common operand fields.
constexpr uint32_t rd(uint64_t w) { return field(w, 0, 8); }
constexpr uint32_t ra(uint64_t w) { return field(w, 8, 8); }
constexpr uint32_t rb(uint64_t w) { return field(w, 20, 8); }
constexpr uint32_t rc(uint64_t w) { return field(w, 39, 8); }
constexpr uint32_t guard_pred(uint64_t w) { return field(w, 16, 3); }
constexpr bool guard_not(uint64_t w) { return bit(w, 19); }
constexpr uint32_t cbuf_bank(uint64_t w) { return field(w, 34, 5); }
constexpr uint32_t cbuf_offset(uint64_t w) { return field(w, 20, 14) * 4; }
constexpr uint32_t imm32(uint64_t w) { return field(w, 20, 32); }
// 20-bit immediate: low 19 bits at 20, bit 19 at 56. Integer ops sign-extend
// it; float ops use it as the top 20 bits of an f32 (low 12 bits zero).
constexpr uint32_t imm20_raw(uint64_t w) { return field(w, 20, 19) | (field(w, 56, 1) << 19); }
constexpr uint32_t imm20_int(uint64_t w) {
    const uint32_t v = imm20_raw(w);
    return (v & 0x80000u) ? (v | 0xfff00000u) : v;
}
constexpr uint32_t imm20_float(uint64_t w) { return imm20_raw(w) << 12; }
// Signed 24-bit branch offset relative to the next instruction (bytes).
constexpr int32_t branch_offset(uint64_t w) {
    const uint32_t v = field(w, 20, 24);
    return static_cast<int32_t>((v & 0x800000u) ? (v | 0xff000000u) : v);
}

// Decodes one instruction word; std::nullopt if the opcode is not in the
// supported subset.
std::optional<Instr> decode(uint64_t word);

// Disassembles in envydis syntax (gm107), so tests can compare against
// envydis output word by word. Returns "???" for unsupported words.
std::string disassemble(const Instr& in, uint32_t address);

// True if the byte offset (relative to the code start) holds a scheduling word.
constexpr bool is_sched_slot(uint32_t byte_offset) { return byte_offset % kBundleBytes == 0; }

}  // namespace lgp::gpu::maxwell
