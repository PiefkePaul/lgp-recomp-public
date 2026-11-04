// Differential test: every generated case runs in Unicorn and as lifted C++;
// final registers, flags, memory, SVC trace and outcome must agree.
//
// Options: --group <name> (only that group), --verbose (print every case),
//          --max-failures <n> (detail lines printed, default 20).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

#include "case_defs.hpp"
#include "lgp/recomp/a64.hpp"
#include "oracle.h"
#include "recompiled_executor.hpp"
#include "unicorn/unicorn_executor.h"

namespace lgp_test {
extern const CaseShard kAllShards[];
extern const size_t kShardCount;
extern const char* const kLifterStats;
}  // namespace lgp_test

namespace {

using namespace lgp_test;
using lgp::difftest::GuestState;
using lgp::difftest::MemoryRegion;
using lgp::difftest::TestCase;

uint64_t random_reg(SplitMix& rng) {
    static const uint64_t kInteresting[] = {0,
                                            1,
                                            2,
                                            0xFFFFFFFFFFFFFFFFull,
                                            0xFFFFFFFFFFFFFFFEull,
                                            0x7FFFFFFF,
                                            0x80000000,
                                            0xFFFFFFFF,
                                            0x100000000ull,
                                            0x7FFFFFFFFFFFFFFFull,
                                            0x8000000000000000ull,
                                            0x8000000000000001ull,
                                            0xFFFFFFFF00000000ull,
                                            0xFFFFFFFE,
                                            31,
                                            32,
                                            63,
                                            64};
    const uint64_t pick = rng.next() % 4;
    if (pick == 0) return kInteresting[rng.next() % std::size(kInteresting)];
    if (pick == 1) {
        const uint64_t v = rng.next() % 512;
        return (rng.next() & 1) ? v : static_cast<uint64_t>(-static_cast<int64_t>(v));
    }
    return rng.next();
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

TestCase build(const CaseDef& c) {
    TestCase tc;
    tc.name = c.name;
    tc.max_instructions = 100000;

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
    s.fpcr = 0;
    s.fpsr = 0;
    for (uint32_t i = 0; i < c.override_count; ++i) {
        const RegOverride& o = c.overrides[i];
        if (o.reg < 31)
            s.x[o.reg] = o.value;
        else if (o.reg == 31)
            s.sp = o.value;
        else
            s.nzcv = static_cast<uint32_t>(o.value);
    }
    return tc;
}

struct GroupStats {
    unsigned run = 0;
    unsigned failed = 0;
};

}  // namespace

int main(int argc, char** argv) {
    const char* only_group = nullptr;
    bool verbose = false;
    unsigned max_failures = 20;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--group") && i + 1 < argc)
            only_group = argv[++i];
        else if (!std::strcmp(argv[i], "--verbose"))
            verbose = true;
        else if (!std::strcmp(argv[i], "--max-failures") && i + 1 < argc)
            max_failures = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10));
    }

    lgp::difftest::UnicornExecutor unicorn;
    RecompiledExecutor recompiled;
    std::map<std::string, GroupStats> groups;
    unsigned total = 0, failed = 0, printed = 0;

    for (size_t s = 0; s < kShardCount; ++s) {
        for (size_t k = 0; k < kAllShards[s].count; ++k) {
            const CaseDef& c = kAllShards[s].cases[k];
            if (!c.name) continue;
            if (only_group && std::strcmp(only_group, c.group) != 0) continue;
            const TestCase tc = build(c);
            recompiled.set_case(&c);
            const auto expected = unicorn.run(tc);
            const auto actual = recompiled.run(tc);
            auto diffs = lgp::difftest::diff(expected, actual);
            // Only the "undefined" and "mem_fault" groups are meant to fault; anywhere
            // else a fault in the oracle means the generator built a broken case.
            const bool expect_fault = std::strcmp(c.group, "undefined") == 0 || std::strcmp(c.group, "mem_fault") == 0;
            if (expect_fault ? expected.outcome != lgp::difftest::Outcome::Fault
                             : (expected.outcome != lgp::difftest::Outcome::Sentinel &&
                                expected.outcome != lgp::difftest::Outcome::ExitProcess))
                diffs.insert(diffs.begin(), std::string("unexpected oracle outcome: ") +
                                                lgp::difftest::to_string(expected.outcome) + " " + expected.detail);

            GroupStats& g = groups[c.group];
            ++g.run;
            ++total;
            if (!diffs.empty()) {
                ++g.failed;
                ++failed;
            }
            if ((!diffs.empty() && printed < max_failures) || verbose) {
                if (!diffs.empty()) ++printed;
                std::printf("%s %s\n", diffs.empty() ? "ok  " : "FAIL", c.name);
                for (uint32_t w = 0; w < c.code_words; ++w)
                    std::printf("      %s\n",
                                lgp::recomp::a64::describe(lgp::recomp::a64::decode(c.code[w], kCodeBase + 4 * w)).c_str());
                if (!actual.detail.empty()) std::printf("      recompiled: %s\n", actual.detail.c_str());
                if (!expected.detail.empty()) std::printf("      unicorn: %s\n", expected.detail.c_str());
                for (const auto& d : diffs)
                    std::printf("      unicorn vs recompiled: %s\n", d.c_str());
            }
        }
    }

    std::printf("\n%-20s %6s %6s\n", "group", "cases", "failed");
    for (const auto& [name, g] : groups)
        std::printf("%-20s %6u %6u\n", name.c_str(), g.run, g.failed);
    std::printf("\nlifter: %s\n", kLifterStats);
    std::printf("difftest: %u cases, %u failed\n", total, failed);
    return (failed == 0 && total > 0) ? 0 : 1;
}
