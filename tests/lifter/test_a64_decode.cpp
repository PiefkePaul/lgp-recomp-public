// Decoder unit tests: fields, control-flow classification and branch targets
// of hand-picked encodings (from llvm-mc), including the extremes of every
// PC-relative immediate. Runs natively and under Node (WebAssembly preset).
#include <cstdio>
#include <cstdlib>

#include "lgp/recomp/a64.hpp"

namespace {

using namespace lgp::recomp::a64;

int g_failures = 0;

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            std::printf("FAIL %s:%d: %s  [%s]\n", __FILE__, __LINE__, #cond, describe(i).c_str()); \
            ++g_failures;                                                                \
        }                                                                                \
    } while (0)

constexpr uint64_t kPc = 0x10001000;

void branches() {
    Inst i = decode(0x17FFFFFF, kPc);  // b #-4
    CHECK(i.op == Op::B && i.flow == Flow::Branch && i.target == kPc - 4);
    i = decode(0x95FFFFFF, kPc);  // bl #0x7fffffc
    CHECK(i.op == Op::Bl && i.flow == Flow::Call && i.target == kPc + 0x7FFFFFC);
    i = decode(0x54800001, kPc);  // b.ne #-0x100000
    CHECK(i.op == Op::BCond && i.flow == Flow::CondBranch && i.cond == 1 && i.target == kPc - 0x100000);
    i = decode(0xB4800003, kPc);  // cbz x3, #-0x100000
    CHECK(i.op == Op::Cbz && i.sf && i.rt == 3 && i.target == kPc - 0x100000);
    i = decode(0x37FBFFE5, kPc);  // tbnz w5, #31, #0x7ffc
    CHECK(i.op == Op::Tbnz && i.bit == 31 && i.rt == 5 && i.target == kPc + 0x7FFC);
    i = decode(0xB6FC0007, kPc);  // tbz x7, #63, #-0x8000
    CHECK(i.op == Op::Tbz && i.bit == 63 && i.rt == 7 && i.target == kPc - 0x8000);
    i = decode(0xD61F0200, kPc);  // br x16
    CHECK(i.op == Op::Br && i.flow == Flow::IndirectBranch && i.rn == 16);
    i = decode(0xD63F0100, kPc);  // blr x8
    CHECK(i.op == Op::Blr && i.flow == Flow::IndirectCall && i.rn == 8);
    i = decode(0xD65F03C0, kPc);  // ret
    CHECK(i.op == Op::Ret && i.flow == Flow::Return && i.rn == 30);
    i = decode(0xD65F0020, kPc);  // ret x1
    CHECK(i.op == Op::Ret && i.rn == 1);
    i = decode(0xD4000FE1, kPc);  // svc #0x7f
    CHECK(i.op == Op::Svc && i.flow == Flow::Syscall && i.imm == 0x7F);
    i = decode(0xD42988E0, kPc);  // brk #0x4c47
    CHECK(i.op == Op::Brk && i.flow == Flow::Trap && i.imm == 0x4C47);
}

void pc_relative() {
    Inst i = decode(0xF03FFFE0, 0x12345);  // adrp x0, #0x7ffff000 (relative to page 0x12000)
    CHECK(i.op == Op::Adrp && i.rd == 0 && i.target == 0x12000 + 0x7FFFF000ull);
    i = decode(0x10800002, kPc);  // adr x2, #-0x100000
    CHECK(i.op == Op::Adr && i.rd == 2 && i.target == kPc - 0x100000);
    i = decode(0x58FFFFC0, kPc);  // ldr x0, #-8
    CHECK(i.op == Op::LdrLit && i.size_log2 == 3 && i.target == kPc - 8 && !i.sign_extend);
    i = decode(0x98000021, kPc);  // ldrsw x1, #4
    CHECK(i.op == Op::LdrLit && i.size_log2 == 2 && i.sign_extend && i.dest64 && i.target == kPc + 4);
    i = decode(0x9C000083, kPc);  // ldr q3, #16
    CHECK(i.op == Op::LdrLit && i.simd && i.size_log2 == 4 && i.rt == 3 && i.target == kPc + 16);
}

void data_processing() {
    Inst i = decode(0x910043FF, kPc);  // add sp, sp, #16
    CHECK(i.op == Op::AddImm && i.rd_sp && i.rn_sp && i.rd == 31 && i.imm == 16 && !i.setflags);
    i = decode(0xF100143F, kPc);  // cmp x1, #5 (subs xzr)
    CHECK(i.op == Op::SubImm && i.setflags && !i.rd_sp && i.rd == 31 && i.imm == 5);
    i = decode(0x9200F020, kPc);  // and x0, x1, #0x5555555555555555
    CHECK(i.op == Op::AndImm && i.imm == 0x5555555555555555ull && i.rd_sp);
    i = decode(0x72B7DDE3, kPc);  // movk w3, #0xbeef, lsl #16
    CHECK(i.op == Op::Movk && !i.sf && i.imm == 0xBEEF && i.amount == 16);
    i = decode(0xDA82A420, kPc);  // csneg x0, x1, x2, ge
    CHECK(i.op == Op::Csneg && i.cond == 10 && i.rn == 1 && i.rm == 2);
    i = decode(0x7A5FB82F, kPc);  // ccmp w1, #31, #0xf, lt
    CHECK(i.op == Op::Ccmp && i.ccmp_imm && i.imm == 31 && i.nzcv_imm == 0xF && i.cond == 11 && !i.sf);
    i = decode(0x93C2FC20, kPc);  // extr x0, x1, x2, #63
    CHECK(i.op == Op::Extr && i.imms == 63 && i.rn == 1 && i.rm == 2);
}

void loads_stores() {
    Inst i = decode(0xF862D820, kPc);  // ldr x0, [x1, w2, sxtw #3]
    CHECK(i.op == Op::LdSt && i.load && i.mode == AddrMode::RegOffset && i.extend == Extend::Sxtw && i.amount == 3);
    i = decode(0xADFF07E0, kPc);  // ldp q0, q1, [sp, #-32]!
    CHECK(i.op == Op::LdStPair && i.simd && i.size_log2 == 4 && i.mode == AddrMode::PreIndex && i.offset == -32 &&
          i.rn == 31 && i.rt2 == 1);
    i = decode(0xC8007C41, kPc);  // stxr w0, x1, [x2]
    CHECK(i.op == Op::LdStExcl && !i.load && i.rs == 0 && i.rt == 1 && i.rn == 2 && i.size_log2 == 3);
    i = decode(0xC87F8440, kPc);  // ldaxp x0, x1, [x2]
    CHECK(i.op == Op::LdStExcl && i.load && i.pair && i.acquire && i.rt2 == 1);
    i = decode(0xD50B7423, kPc);  // dc zva, x3
    CHECK(i.op == Op::DcZva && i.rt == 3);
    i = decode(0xD50B7E23, kPc);  // dc civac, x3
    CHECK(i.op == Op::CacheMaint);
}

void system_registers() {
    Inst i = decode(0xD53BD065, kPc);  // mrs x5, tpidrro_el0
    CHECK(i.op == Op::Mrs && i.sysreg == sr::kTpidrroEl0 && i.rt == 5);
    i = decode(0xD503233F, kPc);  // paciasp: hint space, NOP on ARMv8.0
    CHECK(i.op == Op::Hint && i.flow == Flow::Normal);
}

// SIMD&FP words are handed to a64_simd.hpp (P2.4) and come back as Op::Simd.
void simd_hook() {
    Inst i = decode(0x1E622820, kPc);  // fadd d0, d1, d2
    CHECK(i.op == Op::Simd && i.flow == Flow::Normal);
    i = decode(0x4E208400, kPc);  // add v0.16b, v0.16b, v0.16b
    CHECK(i.op == Op::Simd && i.flow == Flow::Normal);
    i = decode(0x4C407000, kPc);  // ld1 {v0.16b}, [x0]
    CHECK(i.op == Op::Simd && i.flow == Flow::Normal);
}

void invalid_reasons() {
    struct {
        uint32_t raw;
        Invalid why;
        const char* what;
    } const cases[] = {
        {0x00000000, Invalid::Unallocated, "udf #0"},
        {0x1EA22820, Invalid::Unallocated, "fadd with type = 10"},
        {0xD4000002, Invalid::Privileged, "hvc #0"},
        {0xD69F03E0, Invalid::Privileged, "eret"},
        {0xD50342DF, Invalid::Privileged, "msr daifset, #2"},
        {0xD5380000, Invalid::Privileged, "mrs x0, midr_el1"},
        {0xD51BD065, Invalid::Privileged, "msr tpidrro_el0, x5"},
        {0xD5087623, Invalid::Privileged, "dc ivac, x3"},
        {0xD4400000, Invalid::Privileged, "hlt #0"},
        {0xF8E00041, Invalid::Unallocated, "ldaddal x0, x1, [x2] (LSE)"},
        {0x48207C82, Invalid::Unallocated, "casp x0, x1, x2, x3, [x4] (LSE)"},
        {0xA9400020, Invalid::Unpredictable, "ldp x0, x0, [x1]"},
        {0x9B420020, Invalid::Unpredictable, "smulh x0, x1, x2 with Ra = 0"},
        {0x9BC20020, Invalid::Unpredictable, "umulh x0, x1, x2 with Ra = 0"},
    };
    for (const auto& c : cases) {
        const Inst i = decode(c.raw, kPc);
        if (i.op != Op::Invalid || i.invalid != c.why) {
            std::printf("FAIL %s: expected Invalid reason %d, got op %s reason %d\n", c.what, static_cast<int>(c.why),
                        op_name(i.op), static_cast<int>(i.invalid));
            ++g_failures;
        }
    }
}

void bit_masks() {
    uint64_t m = 0;
    Inst i;  // for CHECK's describe()
    CHECK(decode_bit_masks(true, 0b000000, 0, true, m) && m == 0x0000000000000001ull);   // N=1: one bit of 64
    CHECK(decode_bit_masks(false, 0b111100, 0, true, m) && m == 0x5555555555555555ull);  // 2-bit elements
    CHECK(decode_bit_masks(false, 0b011110, 1, false, m) && m == 0xBFFFFFFFull);         // 31 ones rotated by 1
    CHECK(!decode_bit_masks(true, 0b111111, 0, true, m));   // all-ones element is reserved
    CHECK(!decode_bit_masks(false, 0b111111, 0, true, m));  // no element size
}

}  // namespace

int main() {
    branches();
    pc_relative();
    data_processing();
    loads_stores();
    system_registers();
    simd_hook();
    invalid_reasons();
    bit_masks();
    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("a64 decoder: all checks passed\n");
    return 0;
}
