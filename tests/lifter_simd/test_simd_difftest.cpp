// Differential test of the SIMD&FP lifter (P2.4): every generated case runs in
// Unicorn and as lifted C++; registers (X, V, NZCV, FPCR, FPSR except the
// untracked IEEE exception flags), memory and outcome must agree.
//
// Options: --group <name> (only that group), --prefix <p> (groups starting
//          with p), --verbose (print every case), --max-failures <n>.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

#include "case_defs.hpp"
#include "lgp/recomp/a64.hpp"
#include "lgp/recomp/a64_simd.hpp"
#include "oracle.h"
#include "recompiled_executor.hpp"
#include "simd_case_defs.hpp"
#include "unicorn/unicorn_executor.h"

namespace lgp_test {
extern const CaseShard kSimdShards[];
extern const size_t kSimdShardCount;
extern const char* const kSimdLifterStats;
}  // namespace lgp_test

namespace {

using namespace lgp_test;
using lgp::difftest::GuestState;
using lgp::difftest::TestCase;

uint64_t random_reg(SplitMix& rng) {
    static const uint64_t kInteresting[] = {0, 1, 2, 0xFFFFFFFFFFFFFFFFull, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF,
                                            0x100000000ull, 0x7FFFFFFFFFFFFFFFull, 0x8000000000000000ull,
                                            0x3F800000, 0x3FF0000000000000ull, 0x7FC00000, 0xFFFFFFFF00000000ull};
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

std::string fmt_fpcr(uint32_t f) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "fpcr=%08x (RMode=%u FZ=%u DN=%u AHP=%u)", f, (f >> 22) & 3, (f >> 24) & 1,
                  (f >> 25) & 1, (f >> 26) & 1);
    return buf;
}

struct GroupStats {
    unsigned run = 0;
    unsigned failed = 0;
};

}  // namespace

int main(int argc, char** argv) {
    const char* only_group = nullptr;
    const char* prefix = nullptr;
    bool verbose = false;
    unsigned max_failures = 20;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--group") && i + 1 < argc)
            only_group = argv[++i];
        else if (!std::strcmp(argv[i], "--prefix") && i + 1 < argc)
            prefix = argv[++i];
        else if (!std::strcmp(argv[i], "--verbose"))
            verbose = true;
        else if (!std::strcmp(argv[i], "--max-failures") && i + 1 < argc)
            max_failures = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10));
    }

    lgp::difftest::UnicornExecutor unicorn;
    RecompiledExecutor recompiled;
    std::map<std::string, GroupStats> groups;
    unsigned total = 0, failed = 0, printed = 0;

    for (size_t s = 0; s < kSimdShardCount; ++s) {
        for (size_t k = 0; k < kSimdShards[s].count; ++k) {
            const CaseDef& c = kSimdShards[s].cases[k];
            if (!c.name) continue;
            if (only_group && std::strcmp(only_group, c.group) != 0) continue;
            if (prefix && std::strncmp(prefix, c.group, std::strlen(prefix)) != 0) continue;
            const TestCase tc = build(c);
            recompiled.set_case(&c);
            auto expected = unicorn.run(tc);
            auto actual = recompiled.run(tc);
            expected.final.fpsr &= ~kFpsrUntracked;
            actual.final.fpsr &= ~kFpsrUntracked;
            auto diffs = lgp::difftest::diff(expected, actual);
            if (expected.outcome != lgp::difftest::Outcome::Sentinel)
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
                std::printf("%s %s  %s\n", diffs.empty() ? "ok  " : "FAIL", c.name, fmt_fpcr(tc.initial.fpcr).c_str());
                for (uint32_t w = 0; w + 1 < c.code_words; ++w) {
                    const uint32_t raw = c.code[w];
                    const auto l = lgp::recomp::a64::simd::lift(raw, kCodeBase + 4 * w, true);
                    std::printf("      %08x %s\n", raw, l.name);
                    if (!diffs.empty() && w == 0 && c.code_words <= 3) std::printf("        %s\n", l.code.c_str());
                }
                if (!actual.detail.empty()) std::printf("      recompiled: %s\n", actual.detail.c_str());
                if (!expected.detail.empty()) std::printf("      unicorn: %s\n", expected.detail.c_str());
                for (const auto& d : diffs)
                    std::printf("      unicorn vs recompiled: %s\n", d.c_str());
                if (!diffs.empty() && c.code_words == 2) {
                    // Operand registers of a single-instruction case (Rd, Rn, Rm, Ra fields), for reproduction.
                    const uint32_t raw = c.code[0];
                    const unsigned regs[] = {raw & 31, (raw >> 5) & 31, (raw >> 16) & 31, (raw >> 10) & 31};
                    for (unsigned r : regs)
                        std::printf("      before v%-2u = %016llx:%016llx  x%-2u = %016llx\n", r,
                                    static_cast<unsigned long long>(tc.initial.v[r].hi),
                                    static_cast<unsigned long long>(tc.initial.v[r].lo), r,
                                    static_cast<unsigned long long>(r < 31 ? tc.initial.x[r] : tc.initial.sp));
                }
            }
        }
    }

    std::printf("\n%-28s %6s %6s\n", "group", "cases", "failed");
    for (const auto& [name, g] : groups)
        std::printf("%-28s %6u %6u\n", name.c_str(), g.run, g.failed);
    std::printf("\nsimd lifter: %s\n", kSimdLifterStats);
    std::printf("simd difftest: %u cases, %u failed\n", total, failed);
    // No groups yet is not a failure while the families are being filled in.
    return failed == 0 ? 0 : 1;
}
