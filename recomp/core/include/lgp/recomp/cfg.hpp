// Function discovery and control-flow graph (P2.2).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "lgp/recomp/link.hpp"

namespace lgp::recomp {

enum class FunctionSource : uint8_t {
    Fde, Symbol, Entry, InitArray, Config, CallTarget, TailTarget, DataPointer, CodePointer, GapScan
};
const char* function_source_name(FunctionSource s);

enum class Terminator : uint8_t {
    Fallthrough,    // another block starts at `end`
    Branch,         // B inside the function, or tail call (then successors is empty and calls has the target)
    CondBranch,
    Return,
    Trap,
    IndirectJump,   // BR without a recognised table
    JumpTable,
    NoReturnCall,   // BL to a noreturn callee
    Unknown         // undecodable instruction / ran into the function bound
};
const char* terminator_name(Terminator t);

enum class Confidence : uint8_t { High, Medium, Low };
const char* confidence_name(Confidence c);

struct JumpTable {
    uint64_t br_pc = 0;
    uint64_t table_va = 0;
    uint8_t entry_size = 0;     // 1, 2, 4 relative; 8 absolute
    bool entries_signed = false;
    uint64_t base_va = 0;       // relative tables: target = base + (entry << shift)
    uint8_t shift = 0;
    uint32_t count = 0;
    bool bound_known = false;   // count came from cmp/b.cond, not from scanning
    std::vector<uint64_t> targets;   // in table order (duplicates kept)
    Confidence confidence = Confidence::Low;
};

struct Call {
    uint64_t pc;
    uint64_t target;      // 0 for indirect
    bool indirect = false;
    bool tail = false;    // B / BR to another function (or fallthrough into the next one)
    uint64_t via_slot = 0; // GOT slot the target was read from (PLT stub), 0 otherwise
};

struct BasicBlock {
    uint64_t start = 0, end = 0;   // [start, end)
    Terminator term = Terminator::Unknown;
    std::vector<uint64_t> successors;   // fallthrough first, then targets
    std::vector<Call> calls;
    std::optional<uint32_t> jump_table;
};

struct Function {
    uint64_t entry = 0;
    uint64_t end = 0;
    FunctionSource source = FunctionSource::GapScan;
    std::string name;
    std::vector<BasicBlock> blocks;      // sorted by start
    std::vector<JumpTable> jump_tables;
    bool has_unresolved_indirect = false;
    bool noreturn = false;
    bool has_fde_range = false;
    uint64_t fde_end = 0;
    uint32_t instruction_count() const;
};

struct Diagnostic {
    enum class Kind : uint8_t {
        UnresolvedIndirectJump, JumpTableUnbounded, CrossFunctionJump, CallIntoFunctionBody,
        FallsIntoNextFunction, UnreachableCode, UndecodableInstruction, SeedDemoted,
        JumpTableOverrideUnused, DataPointerIgnored, NoReturnInferred,
        JumpTableOverrideRejected,         // a target is misaligned or outside .text: not applied
        JumpTableOverrideOutsideFunction   // applied, but a target leaves the function
    } kind;
    uint64_t pc;
    std::string text;
};
const char* diagnostic_kind_name(Diagnostic::Kind k);
// Diagnostics about the config file itself (jumptable overrides): the CLIs print them always.
bool is_config_diagnostic(Diagnostic::Kind k);

struct JumpTableOverride {
    uint64_t br_pc, table_va;
    uint8_t entry_size;
    uint32_t count;
    uint64_t base_va = 0;     // 0 = table_va
    bool entries_signed = true;
    // target = base + (entry << shift). parse_config sets the default by entry size, as the
    // compilers emit it: 2 for 1/2-byte entries (counted in instructions), 0 for 4-byte entries
    // (byte offsets, ldrsw + add); ignored for 8-byte (absolute) entries.
    uint8_t shift = 0;
};

struct AnalysisConfig {
    std::vector<uint64_t> extra_functions;
    std::vector<std::string> noreturn_symbols = default_noreturn_symbols();
    std::vector<uint64_t> noreturn_addresses;
    std::vector<JumpTableOverride> jump_table_overrides;
    bool scan_gaps = true;
    bool seed_data_pointers = true;
    uint32_t max_jump_table_scan = 4096;   // entries read when the bound is unknown

    static std::vector<std::string> default_noreturn_symbols();
};

// Text format: one directive per line, '#' comments. See docs/knowledge/recompiler-config.md.
AnalysisConfig parse_config(const std::string& text);
AnalysisConfig load_config_file(const std::string& path);
// `cfg` for an image linked at `to` instead of `from` (its addresses moved
// by to - from; the native start links at another base than the lift).
AnalysisConfig rebase_config(AnalysisConfig cfg, uint64_t from, uint64_t to);

struct AnalysisStats {
    uint64_t functions = 0;
    uint64_t blocks = 0;
    uint64_t instructions = 0;
    uint64_t by_source[9] = {};
    uint64_t jump_tables = 0, jump_tables_bounded = 0;
    uint64_t indirect_jumps_unresolved = 0, indirect_calls = 0, direct_calls = 0, tail_calls = 0;
    uint64_t noreturn_functions = 0;
    uint64_t text_bytes = 0, covered_bytes = 0, padding_bytes = 0;
};

struct AnalysisResult {
    std::vector<Function> functions;         // sorted by entry, non-overlapping
    std::vector<uint64_t> function_entries;  // sorted
    std::vector<Diagnostic> diagnostics;
    AnalysisStats stats;

    const Function* function_at(uint64_t entry) const;     // exact entry
    const Function* function_containing(uint64_t va) const;
    bool is_entry(uint64_t va) const;
};

AnalysisResult analyze(const LinkedImage& image, const AnalysisConfig& cfg = {});

}  // namespace lgp::recomp
