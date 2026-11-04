// Deterministic fuzz run in ctest: the regression cases and a fixed-seed batch
// of random mixed sequences (generated and lifted at build time by
// lgp_lifter_fuzzgen) run in Unicorn and as lifted C++ and must agree.
//
// Options: --regressions-only, --verbose, --max-failures <n> (reports printed, default 10).
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "fuzz_run.hpp"

namespace lgp_test {
extern const CaseShard kFuzzTables[];
extern const size_t kFuzzTableCount;
extern const char* const kFuzzStats;
}  // namespace lgp_test

int main(int argc, char** argv) {
    bool regressions_only = false, verbose = false;
    unsigned max_failures = 10;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--regressions-only"))
            regressions_only = true;
        else if (!std::strcmp(argv[i], "--verbose"))
            verbose = true;
        else if (!std::strcmp(argv[i], "--max-failures") && i + 1 < argc)
            max_failures = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10));
    }

    lgp::difftest::UnicornExecutor unicorn;
    lgp_test::RecompiledExecutor lifted;
    unsigned total = 0, failed = 0, faults = 0, printed = 0, reg_total = 0, reg_failed = 0;
    for (size_t t = 0; t < lgp_test::kFuzzTableCount; ++t) {
        if (regressions_only && t > 0) break;
        for (size_t k = 0; k < lgp_test::kFuzzTables[t].count; ++k) {
            const lgp_test::CaseDef& c = lgp_test::kFuzzTables[t].cases[k];
            if (!c.name) continue;
            const lgp_fuzz::Verdict v = lgp_fuzz::check(c, unicorn, lifted);
            ++total;
            if (t == 0) ++reg_total;
            if (v.outcome == lgp::difftest::Outcome::Fault) ++faults;
            if (!v.diffs.empty()) {
                ++failed;
                if (t == 0) ++reg_failed;
            }
            if ((!v.diffs.empty() && printed++ < max_failures) || verbose)
                std::fputs(lgp_fuzz::report(c, v).c_str(), stdout);
        }
    }
    std::printf("\nlifter fuzz: %s\n", lgp_test::kFuzzStats);
    std::printf("lifter fuzz: %u cases (%u regressions, %u ending in a fault), %u failed (%u regressions)\n", total,
                reg_total, faults, failed, reg_failed);
    return (failed == 0 && total > 0) ? 0 : 1;
}
