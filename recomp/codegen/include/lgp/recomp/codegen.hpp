// Code generator (roadmap P2.5): analysis result (P2.1/P2.2) -> lifter input
// -> C++ translation unit(s) per set of linked modules, plus the dispatch
// entries the runtime registers (ADR-0004: every function, direct-indexed).
//
// The output is derived from the input code. For the game it stays in build/
// or $LGP_PRIVATE_DIR and is never committed.
#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "lgp/recomp/cfg.hpp"
#include "lgp/recomp/lifter.hpp"

namespace lgp::recomp {

struct CodegenOptions {
    LiftOptions lift;
    // Replaced by the host variant of ADR-0004, found by symbol name: a
    // definition in a module, or the PLT stub of an import with that name.
    std::vector<std::string> setjmp_symbols = {"setjmp", "_setjmp"};
    std::vector<std::string> longjmp_symbols = {"longjmp", "_longjmp"};
    // Name of the generated `extern const lgp::LiftedEntry NAME[]` and of its
    // element count `extern const size_t NAME_count`.
    std::string entries_name = "lgp_lifted_entries";
    // Number of translation units (ModuleCode::sources). A game module has
    // millions of instructions; one file would not compile in reasonable
    // time and memory.
    size_t parts = 1;
    // Function entries that get a hook door (lgp/runtime/hooks.hpp): the
    // function is lifted as `<name>_orig`, `<name>` runs LGP_HOOKED. Each must
    // be an analysed function entry, not setjmp/longjmp.
    std::set<uint64_t> hook_entries;
};

struct CodegenStats {
    uint64_t functions = 0;       // lifted functions
    uint64_t replaced = 0;        // setjmp/longjmp bodies replaced
    uint64_t hooked = 0;          // functions with a hook door
    uint64_t jump_tables = 0;     // BRs that became a switch
    uint64_t indirect_jumps = 0;  // BRs left to the dispatch table
    LiftStats lift;
};

struct ModuleCode {
    std::string source;                // == sources.front()
    std::vector<std::string> sources;  // CodegenOptions::parts files; the first holds the tables
    CodegenStats stats;
    std::set<uint64_t> setjmp_entries;
    std::set<uint64_t> longjmp_entries;
};

// Adapter: one analysed function as the lifter takes it.
LiftFunction to_lift_function(const Function& f);

// Entries of the functions that are, or tail-call through a PLT stub into,
// one of the named symbols.
std::set<uint64_t> entries_for_symbols(const LinkedImage& image, const AnalysisResult& analysis,
                                       const std::vector<std::string>& names);

// Throws std::invalid_argument if a hook entry is no lifted function.
ModuleCode generate_module(const LinkedImage& image, const AnalysisResult& analysis, const CodegenOptions& options = {});

}  // namespace lgp::recomp
