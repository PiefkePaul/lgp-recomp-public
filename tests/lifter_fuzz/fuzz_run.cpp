#include "fuzz_run.hpp"

#include <cstdio>
#include <cstring>
#include <iterator>

#include "fuzz_gen.hpp"
#include "simd_case_defs.hpp"

namespace lgp_fuzz {

namespace {

using lgp::difftest::GuestState;
using lgp::difftest::Outcome;
using lgp::difftest::TestCase;
using lgp_test::SplitMix;

uint64_t random_reg(SplitMix& rng) {
    static const uint64_t kInteresting[] = {0, 1, 2, 0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFEull, 0x7FFFFFFF, 0x80000000,
                                            0xFFFFFFFF, 0x80000001, 0x100000000ull, 0x7FFFFFFFFFFFFFFFull,
                                            0x8000000000000000ull, 0x8000000000000001ull, 0x3F800000,
                                            0x3FF0000000000000ull, 0x7FC00000, 0xFFFFFFFF00000000ull, 31, 32, 63, 64};
    static const uint32_t kInteresting32[] = {0, 1, 0xFFFFFFFF, 0x7FFFFFFF, 0x80000000, 0x80000001, 31, 32};
    switch (rng.next() % 6) {
    case 0:
    case 1: return kInteresting[rng.next() % std::size(kInteresting)];
    case 2: {
        const uint64_t v = rng.next() % 512;
        return (rng.next() & 1) ? v : static_cast<uint64_t>(-static_cast<int64_t>(v));
    }
    case 3:  // W-register edge values below random upper bits
        return (rng.next() & 0xFFFFFFFF00000000ull) | kInteresting32[rng.next() % std::size(kInteresting32)];
    default: return rng.next();
    }
}

std::vector<uint8_t> random_bytes(uint64_t seed, size_t n) {
    SplitMix rng(seed);
    std::vector<uint8_t> out(n);
    for (size_t i = 0; i < n; i += 8) {
        const uint64_t v = rng.next();
        for (size_t k = 0; k < 8 && i + k < n; ++k)
            out[i + k] = static_cast<uint8_t>(v >> (8 * k));
    }
    return out;
}

}  // namespace

TestCase make_test_case(const lgp_test::CaseDef& c) {
    using namespace lgp_test;
    TestCase tc;
    tc.name = c.name;
    tc.max_instructions = 100000;
    // Generated SVCs (never svcExitProcess) return new values in x0..x7, so
    // registers the lifted code keeps in locals must be reloaded after them.
    tc.svc = [](uint32_t imm, auto& regs) {
        for (size_t i = 0; i < regs.size(); ++i)
            regs[i] = regs[i] * 0x9E3779B97F4A7C15ull + imm + i;
    };

    std::vector<uint8_t> code(kCodeSize, 0);
    std::memcpy(code.data(), c.code, c.code_words * sizeof(uint32_t));
    tc.memory.push_back({kRodataBase, random_bytes(c.seed ^ 0x526F64617461ull, kRodataSize), lgp::difftest::kRead});
    tc.memory.push_back({kCodeBase, std::move(code), lgp::difftest::kRead | lgp::difftest::kExec});
    tc.memory.push_back(
        {kDataBase, random_bytes(c.seed ^ 0x44617461ull, kDataSize), lgp::difftest::kRead | lgp::difftest::kWrite});

    SplitMix rng(c.seed);
    GuestState& s = tc.initial;
    for (auto& x : s.x)
        x = random_reg(rng);
    s.sp = kStackTop;
    s.pc = c.entry_pc;
    s.nzcv = static_cast<uint32_t>(rng.next() & 0xF) << 28;
    for (auto& v : s.v)
        v = {rng.next(), rng.next()};
    s.tpidr_el0 = rng.next();
    s.tpidrro_el0 = rng.next();
    for (uint32_t i = 0; i < c.override_count; ++i) {
        const RegOverride& o = c.overrides[i];
        if (o.reg < 31)
            s.x[o.reg] = o.value;
        else if (o.reg == 31)
            s.sp = o.value;
        else if (o.reg == 32)
            s.nzcv = static_cast<uint32_t>(o.value);
        else if (o.reg == kRegFpcr)
            s.fpcr = static_cast<uint32_t>(o.value);
        else if (o.reg == kRegFpsr)
            s.fpsr = static_cast<uint32_t>(o.value);
        else if (o.reg >= kRegVLo && o.reg < kRegVLo + 32)
            s.v[o.reg - kRegVLo].lo = o.value;
        else if (o.reg >= kRegVHi && o.reg < kRegVHi + 32)
            s.v[o.reg - kRegVHi].hi = o.value;
    }
    return tc;
}

Verdict check(const lgp_test::CaseDef& c, lgp::difftest::UnicornExecutor& unicorn, lgp_test::RecompiledExecutor& lifted,
              bool precise) {
    const TestCase tc = make_test_case(c);
    lifted.set_case(&c);
    Verdict v;
    v.expected = unicorn.run(tc);
    v.actual = lifted.run(tc);
    v.outcome = v.expected.outcome;
    // The IEEE exception flags of FPSR are not modelled (docs/knowledge/a64-fp-simd-semantik.md).
    v.expected.final.fpsr &= ~lgp_test::kFpsrUntracked;
    v.actual.final.fpsr &= ~lgp_test::kFpsrUntracked;
    v.diffs = lgp::difftest::diff(v.expected, v.actual);
    // Release shape: after a fault registers held in locals are stale in
    // CpuState (and so is pc). Memory, SVC records and the outcome still count.
    if (!precise && v.expected.outcome == Outcome::Fault && v.actual.outcome == Outcome::Fault)
        std::erase_if(v.diffs, [](const std::string& d) {
            return d.rfind("memory", 0) != 0 && d.rfind("svc", 0) != 0 && d.rfind("outcome", 0) != 0;
        });
    if (v.expected.outcome != Outcome::Sentinel && v.expected.outcome != Outcome::Fault)
        v.diffs.insert(v.diffs.begin(), std::string("unexpected oracle outcome: ") +
                                            lgp::difftest::to_string(v.expected.outcome) + " " + v.expected.detail);
    return v;
}

std::string report(const lgp_test::CaseDef& c, const Verdict& v) {
    const TestCase tc = make_test_case(c);
    const uint32_t f = tc.initial.fpcr;
    char head[160];
    std::snprintf(head, sizeof(head), "%s %s  fpcr=%08x (RMode=%u FZ=%u DN=%u AHP=%u)\n", v.diffs.empty() ? "ok  " : "FAIL",
                  c.name, f, (f >> 22) & 3, (f >> 24) & 1, (f >> 25) & 1, (f >> 26) & 1);
    std::string out = head;
    out += listing(std::vector<uint32_t>(c.code, c.code + c.code_words));
    if (!v.actual.detail.empty()) out += "      recompiled: " + v.actual.detail + "\n";
    if (!v.expected.detail.empty()) out += "      unicorn: " + v.expected.detail + "\n";
    for (const auto& d : v.diffs)
        out += "      unicorn vs recompiled: " + d + "\n";
    return out;
}

}  // namespace lgp_fuzz
