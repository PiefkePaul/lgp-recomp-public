// Mixed-sequence fuzzer for the lifter: case generation, C++ emission and the
// regression file format. No Unicorn here, so the build-time generator
// (fuzzgen.cpp) and the long-run driver (lifter_fuzz.cpp) share it.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "case_defs.hpp"
#include "lgp/recomp/lifter.hpp"

namespace lgp_fuzz {

// One fuzz case: own instruction words plus the initial state. The state is
// the deterministic random state of `seed` (see make_test_case() in
// fuzz_run.hpp) with `overrides` applied on top.
struct FuzzCase {
    std::string name;
    uint64_t seed = 0;
    std::vector<uint32_t> code;  // without the final sentinel BRK
    std::vector<lgp_test::RegOverride> overrides;
};

struct GenOptions {
    unsigned max_len = 16;  // instructions per case (1..max_len), not counting the sentinel
};

// Per-case seed of case `index` of a run with `run_seed`; generate() depends
// on nothing else, so any single case can be regenerated from its case seed.
uint64_t case_seed(uint64_t run_seed, uint64_t index);

// A random sequence that mixes integer ALU, loads/stores, literal loads,
// scalar FP, Advanced SIMD (including structure loads/stores), forward
// branches, NZCV/FPCR writes, SVCs and calls of a generated callee, with
// interesting FP/integer register contents and a random FPCR. Code around
// calls follows AAPCS64 for the flags: they are set after every BL and at the
// callee's entry before anything reads them.
FuzzCase generate(uint64_t case_seed, const GenOptions& options);

// C++ source of a table of lifted cases:
//   extern "C" const lgp_test::CaseDef <table>[]; extern "C" const size_t <table>_count;
// `group` names the CaseDef group of every case. Lifted with precise fault pcs
// unless `precise` is false (the release code shape: registers are written back
// to CpuState only at calls and exits, so a fault leaves them stale).
std::string emit_cases(const std::vector<FuzzCase>& cases, const std::string& table, const std::string& group,
                       lgp::recomp::LiftStats& stats, bool precise = true);

// Regression file: one case per line,
//   <name> seed=<hex> code=<hex>,<hex>,... [ov=<reg>:<hex>,...]
// with <reg> one of x0..x30, sp, nzcv, fpcr, fpsr, v<n>.lo, v<n>.hi. Lines
// starting with '#' and empty lines are ignored.
bool parse_regressions(const std::string& text, std::vector<FuzzCase>& out, std::string& error);
std::string format_regression(const FuzzCase& c);

// Assembly-like listing of the code (one line per word), for reports.
std::string listing(const std::vector<uint32_t>& code);

inline constexpr uint32_t kSentinel = 0xD4200000u | (0x4C47u << 5);  // BRK #0x4C47
inline constexpr uint32_t kNop = 0xD503201Fu;
inline constexpr uint32_t kRet = 0xD65F03C0u;

// A case with a callee: its code is caller, kSentinel, callee (ending in RET).
// Index of that inner sentinel, or SIZE_MAX for a case without callee.
size_t callee_split(const std::vector<uint32_t>& code);

}  // namespace lgp_fuzz
