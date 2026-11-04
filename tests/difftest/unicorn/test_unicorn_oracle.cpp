// P0.8: Unicorn as ARM64 oracle. Runs short own instruction sequences and
// compares the resulting state against values computed by hand. Encodings
// were produced with llvm-mc (-triple=aarch64) and are listed next to each word.
#include "unicorn_executor.h"

#include <bit>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace lgp::difftest;

namespace {

constexpr uint64_t kCodeBase = 0x10000;
constexpr uint64_t kDataBase = 0x20000;
constexpr uint64_t kStackTop = 0x40000;
constexpr uint32_t kNzcvZ = 1u << 30;
constexpr uint32_t kNzcvC = 1u << 29;

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

std::vector<uint8_t> code_bytes(std::initializer_list<uint32_t> words) {
    std::vector<uint8_t> out(words.size() * 4);
    size_t i = 0;
    for (uint32_t w : words) {
        for (int b = 0; b < 4; ++b)
            out[i++] = static_cast<uint8_t>(w >> (8 * b));
    }
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

RunResult run(const TestCase& tc) {
    std::printf("[%s]\n", tc.name.c_str());
    UnicornExecutor ex;
    RunResult r = ex.run(tc);
    if (!r.detail.empty())
        std::printf("  detail: %s\n", r.detail.c_str());
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
        0xd42988e0,  // brk  #0x4c47
    });
    RunResult r = run(tc);
    CHECK(r.outcome == Outcome::Sentinel);
    CHECK_EQ(r.final.x[0], 0x12345678ull);
    CHECK_EQ(r.final.x[1], 0x12345688ull);
    CHECK_EQ(r.final.x[4], 0x12345678ull * 17);
    CHECK_EQ(r.final.x[3], 0ull);
    CHECK_EQ(r.final.nzcv, kNzcvZ | kNzcvC);  // 0x10 - 0x10: zero, no borrow
    CHECK_EQ(r.final.pc, kCodeBase + 6 * 4);   // on the BRK
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
        0xd42988e0,  // brk  #0x4c47
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
    CHECK_EQ(r.final.x[0], kDataBase + 64);  // post-index walked over all 8 entries
    CHECK_EQ(r.final.x[1], 0ull);
    CHECK(r.memory.size() == 2);  // data + stack; code is not writable
    if (!r.memory.empty())
        CHECK_EQ(load64(r.memory[0].bytes, 64), expected);
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
        CHECK_EQ(r.svcs[0].args[0], 0x11ull);
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
        0xd42988e0,  // brk    #0x4c47
    });
    RunResult r = run(tc);
    CHECK(r.outcome == Outcome::Sentinel);
    CHECK_EQ(r.final.v[2].lo, std::bit_cast<uint64_t>(3.375));
    CHECK_EQ(r.final.v[3].lo, std::bit_cast<uint64_t>(4.875));
    CHECK_EQ(r.final.v[5].lo, std::bit_cast<uint64_t>(1.5));
    CHECK_EQ(r.final.v[2].hi, 0ull);  // scalar writes clear the upper half
    CHECK_EQ(r.final.x[4], 4ull);
    CHECK_EQ(r.final.fpsr & 0x10u, 0x10u);  // IXC: 4.875 -> 4 is inexact
    CHECK_EQ(r.final.fpcr, 0u);
}

void test_thread_registers() {
    TestCase tc = make_case("thread_registers", {
        0xd53bd040,  // mrs x0, tpidr_el0
        0xd53bd061,  // mrs x1, tpidrro_el0
        0xd42988e0,  // brk #0x4c47
    });
    tc.initial.tpidr_el0 = 0x1122334455667788ull;
    tc.initial.tpidrro_el0 = 0x00000000DEAD0000ull;
    RunResult r = run(tc);
    CHECK(r.outcome == Outcome::Sentinel);
    CHECK_EQ(r.final.x[0], 0x1122334455667788ull);
    CHECK_EQ(r.final.x[1], 0x00000000DEAD0000ull);
}

void test_fault_and_limit() {
    TestCase unmapped = make_case("fault_unmapped_load", {
        0xf9400020,  // ldr x0, [x1]
        0xd42988e0,  // brk #0x4c47
    });
    unmapped.initial.x[1] = 0x7000000;
    RunResult r = run(unmapped);
    CHECK(r.outcome == Outcome::Fault);

    TestCase other_brk = make_case("fault_other_brk", {
        0xd4200020,  // brk #0x1
    });
    r = run(other_brk);
    CHECK(r.outcome == Outcome::Fault);

    TestCase spin = make_case("instruction_limit", {
        0x14000000,  // b .
    });
    spin.max_instructions = 1000;
    r = run(spin);
    CHECK(r.outcome == Outcome::InstructionLimit);
    CHECK_EQ(r.final.pc, kCodeBase);

    TestCase stop_at = make_case("stop_address", {
        0xd2800202,  // mov x2, #0x10
        0xd2800020,  // mov x0, #1     <- stop here, not executed
        0xd42988e0,  // brk #0x4c47
    });
    stop_at.stop_pc = kCodeBase + 4;
    r = run(stop_at);
    CHECK(r.outcome == Outcome::StopAddress);
    CHECK_EQ(r.final.pc, kCodeBase + 4);
    CHECK_EQ(r.final.x[2], 0x10ull);
    CHECK_EQ(r.final.x[0], 0ull);
}

void test_diff_reports_differences() {
    TestCase tc = make_case("diff_self_check", {
        0xd2800202,  // mov x2, #0x10
        0xd42988e0,  // brk #0x4c47
    });
    RunResult a = run(tc);
    RunResult b = UnicornExecutor().run(tc);
    CHECK(diff(a, b).empty());  // deterministic: two runs agree

    b.final.x[2] ^= 1;
    b.memory[0].bytes[0x10] = 0xFF;
    b.svcs.push_back({});
    const std::vector<std::string> d = diff(a, b);
    for (const std::string& line : d)
        std::printf("  expected diff: %s\n", line.c_str());
    CHECK(d.size() == 3);
}

}  // namespace

int main() {
    test_integer_and_flags();
    test_loop_and_memory();
    test_svc_trace_and_exit();
    test_floating_point();
    test_thread_registers();
    test_fault_and_limit();
    test_diff_reports_differences();
    if (g_failures != 0) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
