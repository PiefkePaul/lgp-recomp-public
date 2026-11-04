// Running fuzz cases: initial state, Unicorn vs. lifted code, comparison.
// Shared by the ctest driver (test_lifter_fuzz.cpp) and the long-run driver.
#pragma once

#include <string>
#include <vector>

#include "case_defs.hpp"
#include "oracle.h"
#include "recompiled_executor.hpp"
#include "unicorn/unicorn_executor.h"

namespace lgp_fuzz {

// Initial state and memory of a case. Frozen: regression lines name a seed,
// so changing how the seed expands silently changes every regression case.
lgp::difftest::TestCase make_test_case(const lgp_test::CaseDef& c);

struct Verdict {
    std::vector<std::string> diffs;  // empty: Unicorn and lifted code agree
    lgp::difftest::Outcome outcome = lgp::difftest::Outcome::Fault;  // of the oracle
    lgp::difftest::RunResult expected, actual;
};

// Runs the case in both executors. A fault is a valid outcome (both sides must
// fault at the same pc with the same state); anything but a sentinel stop or a
// fault in the oracle is reported as a generator error.
// precise = false: cases lifted without precise_fault_pc; when both sides fault,
// only the outcome is compared (registers and pc are stale by design).
Verdict check(const lgp_test::CaseDef& c, lgp::difftest::UnicornExecutor& unicorn, lgp_test::RecompiledExecutor& lifted,
              bool precise = true);

// Multi-line report: listing, initial FPCR and the differences.
std::string report(const lgp_test::CaseDef& c, const Verdict& v);

}  // namespace lgp_fuzz
