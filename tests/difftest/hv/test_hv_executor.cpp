// P2.7: Hypervisor.framework executor (EL0, MMU on) as ARM64 oracle.
// Runs short own instruction sequences natively on Apple Silicon, checks them
// against hand-computed values and, when Unicorn is built, against the Unicorn
// executor with diff(). Encodings come from llvm-mc (-triple=aarch64).
#include "hv_machine.h"

#if LGP_HV_TEST_UNICORN
#include "unicorn/unicorn_executor.h"
#endif

#include <bit>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace lgp::difftest;

namespace {

constexpr uint64_t kCodeBase = 0x10000;
constexpr uint64_t kDataBase = 0x20000;
constexpr uint64_t kRoBase = 0x30000;
constexpr uint64_t kStackTop = 0x50000;
constexpr uint32_t kNzcvZ = 1u << 30;
constexpr uint32_t kNzcvC = 1u << 29;
constexpr uint32_t kSentinel = 0xd42988e0;  // brk #0x4c47

int g_failures = 0;

#define CHECK_EQ(actual, expected)                                                                   \
    do {                                                                                             \
        const auto a_ = (actual);                                                                    \
        const auto e_ = (expected);                                                                  \
        if (!(a_ == e_)) {                                                                           \
            std::printf("  FAIL %s:%d: %s == 0x%llx, expected 0x%llx\n", __FILE__, __LINE__, #actual, \
                        static_cast<unsigned long long>(a_), static_cast<unsigned long long>(e_));   \
            ++g_failures;                                                                            \
        }                                                                                            \
    } while (0)

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

hv::HvExecutor& executor() {
    static hv::HvExecutor ex;  // one VM per process
    return ex;
}

std::vector<uint8_t> code_bytes(std::initializer_list<uint32_t> words) {
    std::vector<uint8_t> out(words.size() * 4);
    size_t i = 0;
    for (uint32_t w : words)
        for (int b = 0; b < 4; ++b)
            out[i++] = static_cast<uint8_t>(w >> (8 * b));
    return out;
}

TestCase make_case(const char* name, std::initializer_list<uint32_t> words) {
    TestCase tc;
    tc.name = name;
    tc.memory.push_back({kCodeBase, code_bytes(words), kRead | kExec});
    tc.memory.push_back({kDataBase, std::vector<uint8_t>(0x1000), kRead | kWrite});
    tc.memory.push_back({kStackTop - 0x1000, std::vector<uint8_t>(0x1000), kRead | kWrite});
    tc.initial.pc = kCodeBase;
    tc.initial.sp = kStackTop;
    return tc;
}

uint64_t load64(const std::vector<uint8_t>& bytes, size_t off) {
    uint64_t v = 0;
    std::memcpy(&v, bytes.data() + off, sizeof(v));
    return v;
}

void store64(std::vector<uint8_t>& bytes, size_t off, uint64_t v) { std::memcpy(bytes.data() + off, &v, sizeof(v)); }

// Runs on the hypervisor; with Unicorn also there, and both must agree
// (unless the case is marked hardware-only).
RunResult run(const TestCase& tc, bool compare_with_unicorn = true) {
    std::printf("[%s]\n", tc.name.c_str());
    RunResult r = executor().run(tc);
    std::printf("  outcome %s%s%s\n", to_string(r.outcome), r.detail.empty() ? "" : ": ", r.detail.c_str());
#if LGP_HV_TEST_UNICORN
    if (compare_with_unicorn) {
        UnicornExecutor uc;
        const RunResult u = uc.run(tc);
        const std::vector<std::string> d = diff(r, u);
        for (const std::string& line : d)
            std::printf("  FAIL hv != unicorn: %s\n", line.c_str());
        g_failures += static_cast<int>(d.size());
    }
#else
    (void)compare_with_unicorn;
#endif
    return r;
}

void test_integer_and_flags() {
    TestCase tc = make_case("integer_and_flags", {
        0xd2a24680,  // mov  x0, #0x12340000
        0xf28acf00,  // movk x0, #0x5678
        0x91004001,  // add  x1, x0, #0x10
        0xab001004,  // adds x4, x0, x0, lsl #4
        0xd2800202,  // mov  x2, #0x10
        0xf1004043,  // subs x3, x2, #0x10
        kSentinel,
    });
    RunResult r = run(tc);
    CHECK(r.outcome == Outcome::Sentinel);
    CHECK_EQ(r.final.x[0], 0x12345678ull);
    CHECK_EQ(r.final.x[1], 0x12345688ull);
    CHECK_EQ(r.final.x[4], 0x12345678ull * 17);
    CHECK_EQ(r.final.x[3], 0ull);
    CHECK_EQ(r.final.nzcv, kNzcvZ | kNzcvC);
    CHECK_EQ(r.final.pc, kCodeBase + 6 * 4);  // on the BRK
    CHECK_EQ(r.final.sp, kStackTop);
}

void test_loop_and_memory() {
    TestCase tc = make_case("loop_and_memory", {
        0xd2800002,  // mov  x2, #0
        0xf8408403,  // ldr  x3, [x0], #8      <- loop
        0x8b030042,  // add  x2, x2, x3
        0xf1000421,  // subs x1, x1, #1
        0x54ffffa1,  // b.ne loop
        0xf9000002,  // str  x2, [x0]
        kSentinel,
    });
    std::vector<uint8_t>& data = tc.memory[1].bytes;
    uint64_t expected = 0;
    for (uint64_t i = 0; i < 8; ++i) {
        const uint64_t v = 0x1111111111111111ull * (i + 1) + i;
        store64(data, i * 8, v);
        expected += v;
    }
    tc.initial.x[0] = kDataBase;
    tc.initial.x[1] = 8;
    RunResult r = run(tc);
    CHECK(r.outcome == Outcome::Sentinel);
    CHECK_EQ(r.final.x[2], expected);
    CHECK_EQ(r.final.x[0], kDataBase + 64);
    CHECK(r.memory.size() == 2);
    if (!r.memory.empty())
        CHECK_EQ(load64(r.memory[0].bytes, 64), expected);
}

void test_stack_and_calls() {
    TestCase tc = make_case("stack_and_calls", {
        0xa9bf7bfd,  // stp x29, x30, [sp, #-16]!
        0x910003fd,  // mov x29, sp
        0x94000004,  // bl  +16 (callee)
        0xa8c17bfd,  // ldp x29, x30, [sp], #16
        0x91000400,  // add x0, x0, #1
        kSentinel,
        0x8b000000,  // add x0, x0, x0           <- callee
        0xd65f03c0,  // ret
    });
    tc.initial.x[0] = 20;
    tc.initial.x[30] = 0x1234;
    RunResult r = run(tc);
    CHECK(r.outcome == Outcome::Sentinel);
    CHECK_EQ(r.final.x[0], 41ull);
    CHECK_EQ(r.final.x[30], 0x1234ull);
    CHECK_EQ(r.final.sp, kStackTop);
    CHECK_EQ(load64(r.memory[1].bytes, 0x1000 - 8), 0x1234ull);  // saved x30
}

void test_svc_trace_and_exit() {
    TestCase tc = make_case("svc_trace_and_exit", {
        0xd2800220,  // mov x0, #0x11
        0xd2800061,  // mov x1, #3
        0xd40004e1,  // svc #0x27
        0xaa0003e5,  // mov x5, x0
        0xd2800000,  // mov x0, #0
        0xd40000e1,  // svc #0x7   (svcExitProcess)
    });
    tc.svc = [](uint32_t imm, SvcRegs& regs) {
        if (imm == 0x27)
            regs[0] = 0xCAFE + regs[1];
    };
    RunResult r = run(tc);
    CHECK(r.outcome == Outcome::ExitProcess);
    CHECK_EQ(r.final.x[5], 0xCAFEull + 3);
    CHECK_EQ(r.final.pc, kCodeBase + 6 * 4);  // after the exit SVC
    CHECK(r.svcs.size() == 2);
    if (r.svcs.size() == 2) {
        CHECK_EQ(r.svcs[0].imm, 0x27u);
        CHECK_EQ(r.svcs[0].args[1], 3ull);
        CHECK_EQ(r.svcs[0].results[0], 0xCAFEull + 3);
        CHECK_EQ(r.svcs[1].imm, kSvcExitProcess);
    }
}

void test_floating_point() {
    TestCase tc = make_case("floating_point", {
        0x1e6f1000,  // fmov   d0, #1.5
        0x1e605001,  // fmov   d1, #2.25
        0x1e610802,  // fmul   d2, d0, d1
        0x1e602843,  // fadd   d3, d2, d0
        0x9e780064,  // fcvtzs x4, d3
        0x1e61c025,  // fsqrt  d5, d1
        0x4ea18426,  // add    v6.4s, v1.4s, v1.4s
        kSentinel,
    });
    tc.initial.v[1] = {0x0000000100000002ull, 0x7fffffff80000000ull};
    RunResult r = run(tc);
    CHECK(r.outcome == Outcome::Sentinel);
    CHECK_EQ(r.final.v[2].lo, std::bit_cast<uint64_t>(3.375));
    CHECK_EQ(r.final.v[3].lo, std::bit_cast<uint64_t>(4.875));
    CHECK_EQ(r.final.v[5].lo, std::bit_cast<uint64_t>(1.5));
    CHECK_EQ(r.final.v[2].hi, 0ull);
    CHECK_EQ(r.final.x[4], 4ull);
    CHECK_EQ(r.final.fpsr & 0x10u, 0x10u);  // IXC
    CHECK_EQ(r.final.fpcr, 0u);
}

void test_thread_registers() {
    TestCase tc = make_case("thread_registers", {
        0xd53bd040,  // mrs x0, tpidr_el0
        0xd53bd061,  // mrs x1, tpidrro_el0
        kSentinel,
    });
    tc.initial.tpidr_el0 = 0x1122334455667788ull;
    tc.initial.tpidrro_el0 = 0x00000000DEAD0000ull;
    RunResult r = run(tc);
    CHECK(r.outcome == Outcome::Sentinel);
    CHECK_EQ(r.final.x[0], 0x1122334455667788ull);
    CHECK_EQ(r.final.x[1], 0x00000000DEAD0000ull);
}

// Things that need EL0 with the MMU on (normal memory): unaligned accesses,
// exclusives, DC ZVA.
void test_normal_memory() {
    TestCase tc = make_case("normal_memory", {
        0xf8401001,  // ldur  x1, [x0, #1]      unaligned
        0xc85f7c02,  // ldxr  x2, [x0]
        0x91000442,  // add   x2, x2, #1
        0xc8037c02,  // stxr  w3, x2, [x0]
        0x91010004,  // add   x4, x0, #0x40
        0xd50b7424,  // dc    zva, x4
        kSentinel,
    });
    std::vector<uint8_t>& data = tc.memory[1].bytes;
    for (size_t i = 0; i < 0x100; ++i)
        data[i] = static_cast<uint8_t>(i + 1);
    tc.initial.x[0] = kDataBase;
    RunResult r = run(tc, /*compare_with_unicorn=*/false);  // DC ZVA block size differs in Unicorn
    CHECK(r.outcome == Outcome::Sentinel);
    CHECK_EQ(r.final.x[1], 0x0908070605040302ull);
    CHECK_EQ(r.final.x[3], 0ull);  // store-exclusive succeeded
    CHECK_EQ(load64(r.memory[0].bytes, 0), 0x0807060504030202ull);
    bool zeroed = true;  // 64-byte DC ZVA block, as on the A57
    for (size_t i = 0x40; i < 0x80; ++i)
        zeroed = zeroed && r.memory[0].bytes[i] == 0;
    CHECK(zeroed);
    CHECK_EQ(r.memory[0].bytes[0x80], 0x81u);
}

// Where the M1 differs from the A57 (informational, not asserted; see
// docs/analysis/11-difftest-spielfunktionen.md): unaligned exclusive loads
// (ARMv8.0: always an alignment fault), FPCR trap-enable and FZ16 bits
// (RAZ/WI on the A57), FPSR N/Z/C/V (writable on the A57, which has AArch32).
void test_m1_vs_a57_info() {
    TestCase excl = make_case("info_unaligned_ldaxr", {
        0x885ffc20,  // ldaxr w0, [x1]
        kSentinel,
    });
    excl.initial.x[1] = kDataBase + 3;
    RunResult r = run(excl, false);
    std::printf("  info: unaligned LDAXR inside a 16-byte block: %s\n",
                r.outcome == Outcome::Sentinel ? "no fault (A57: alignment fault)" : "alignment fault (as A57)");
    excl.initial.x[1] = kDataBase + 14;  // crosses a 16-byte boundary
    r = run(excl, false);
    std::printf("  info: unaligned LDAXR across 16 bytes: %s\n",
                r.outcome == Outcome::Sentinel ? "no fault" : "fault");

    TestCase fp = make_case("info_fpcr_fpsr_bits", {
        0xd51b4400,  // msr fpcr, x0
        0xd53b4402,  // mrs x2, fpcr
        0xd51b4421,  // msr fpsr, x1
        0xd53b4423,  // mrs x3, fpsr
        kSentinel,
    });
    fp.initial.x[0] = 0xFFFFFFFFull;
    fp.initial.x[1] = 0xFFFFFFFFull;
    r = run(fp, false);
    std::printf("  info: FPCR writable bits 0x%08llx (A57 per lifter: 0x07F70000), FPSR 0x%08llx (A57: 0xF800009F)\n",
                static_cast<unsigned long long>(r.final.x[2]), static_cast<unsigned long long>(r.final.x[3]));
}

void test_faults() {
    TestCase unmapped = make_case("fault_unmapped_load", {
        0xf9400020,  // ldr x0, [x1]
        kSentinel,
    });
    unmapped.initial.x[1] = 0x7000000;
    RunResult r = run(unmapped, false);
    CHECK(r.outcome == Outcome::Fault);
    CHECK_EQ(r.final.pc, kCodeBase);  // on the faulting load

    TestCase ro = make_case("fault_store_read_only", {
        0xf9000020,  // str x0, [x1]
        kSentinel,
    });
    ro.memory.push_back({kRoBase, std::vector<uint8_t>(0x1000, 0xAB), kRead});
    ro.initial.x[1] = kRoBase;
    r = run(ro, false);
    CHECK(r.outcome == Outcome::Fault);

    TestCase nx = make_case("fault_execute_data", {
        0xd61f0020,  // br x1
    });
    nx.initial.x[1] = kDataBase;
    r = run(nx, false);
    CHECK(r.outcome == Outcome::Fault);

    TestCase counter = make_case("fault_counter_read", {
        0xd53be040,  // mrs x0, cntvct_el0 (trapped: not deterministic)
        kSentinel,
    });
    r = run(counter, false);
    CHECK(r.outcome == Outcome::Fault);

    TestCase other_brk = make_case("fault_other_brk", {
        0xd4200020,  // brk #0x1
    });
    r = run(other_brk);
    CHECK(r.outcome == Outcome::Fault);

    TestCase spin = make_case("timeout", {
        0x14000000,  // b .
    });
    spin.max_instructions = 1000;  // -> minimum wall-clock budget
    r = run(spin, false);
    CHECK(r.outcome == Outcome::InstructionLimit);
    CHECK_EQ(r.final.pc, kCodeBase);

    // The machine is usable again after a timeout and after faults.
    TestCase after = make_case("after_timeout", {
        0xd2800540,  // mov x0, #42
        kSentinel,
    });
    r = run(after);
    CHECK(r.outcome == Outcome::Sentinel);
    CHECK_EQ(r.final.x[0], 42ull);
}

void test_many_runs() {
    // Re-mapping per run must stay cheap and correct.
    TestCase tc = make_case("many_runs", {
        0x8b010000,  // add x0, x0, x1
        kSentinel,
    });
    bool ok = true;
    for (uint64_t i = 0; i < 200; ++i) {
        tc.initial.x[0] = i;
        tc.initial.x[1] = 3 * i;
        const RunResult r = executor().run(tc);
        ok = ok && r.outcome == Outcome::Sentinel && r.final.x[0] == 4 * i;
    }
    std::printf("[many_runs]\n");
    CHECK(ok);
}

}  // namespace

int main() {
    test_integer_and_flags();
    test_loop_and_memory();
    test_stack_and_calls();
    test_svc_trace_and_exit();
    test_floating_point();
    test_thread_registers();
    test_normal_memory();
    test_m1_vs_a57_info();
    test_faults();
    test_many_runs();
    if (g_failures != 0) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("hv_executor: all checks passed\n");
    return 0;
}
