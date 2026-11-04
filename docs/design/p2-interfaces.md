# P2 – Interfaces between loader/CFG (P2.1, P2.2) and lifter (P2.3 ff.)

As of: 2026-10-11 (lifter feedback from PR #16 incorporated) · Owner: thread "P2.1 loader and P2.2 CFG" · Status: **work in progress**, maintained by both threads.
Please make changes to this file as a small, separate commit, so that merges between the P2 branches stay trivial.

Purpose: two threads build in parallel. So that it fits together, this file defines **which data** the analysis part delivers
and **who owns which code**. Everything that depends on the open ADRs 0001–0005 is marked as replaceable.

## 1. Directories and CMake targets

| Path | Owner | Target | Content |
|---|---|---|---|
| `recomp/CMakeLists.txt` | P2.1/2.2 | – | only `add_subdirectory(core)` (+ later `add_subdirectory(lifter)`, one line, trivial conflict) |
| `recomp/core/` | P2.1/2.2 | `lgp_recomp_core` (static lib) | NSO/NRO loader, LZ4, MOD0/`.dynamic`, symbols, relocations, link step, `.eh_frame_hdr`, function discovery, CFG, jump tables, config file |
| `recomp/core/include/lgp/recomp/*.hpp` | P2.1/2.2 | – | public headers of the analysis (namespace `lgp::recomp`) |
| `recomp/cli/` | P2.1/2.2 | `lgp_recomp` (exe) | command line: `info`, `functions`, `check-functions` (comparison with ELF symbols) |
| `recomp/lifter/` | P2.3 | `lgp_recomp_lifter` | A64 decoder (`lgp::recomp::a64`), lifter → C++ output |
| `tests/recomp/` | P2.1/2.2 | ctest label `recomp` | loader/CFG tests with synthetic modules and (after P0.7) the fixtures |
| `tests/lifter/` | P2.3 | ctest label `lifter` | lifter tests, Unicorn differential tests (oracle interface: `tests/difftest/oracle.h` from P0.7) |

Both libraries are C++20 without third-party dependencies (no Capstone, no LLVM, no JSON parser), so that the
WASM build (`--preset wasm`) builds them along without extra work.

## 2. Decoder: one decoder, two users

Decision (agreed 2026-10-10): **there is exactly one bit-exact A64 decoder**; it belongs to the lifter thread:

```cpp
// recomp/lifter/include/lgp/recomp/a64.hpp, target lgp_recomp_lifter (PR #16)
namespace lgp::recomp::a64 {
struct Inst {
    Op op;                                     // opcode enum; invalid words: Op::Invalid
    enum class Invalid { Unallocated, Unpredictable, Privileged, NotImplemented } invalid;
    enum class Flow { Normal, Branch, CondBranch, Call, IndirectBranch, IndirectCall, Return, Syscall, Trap } flow;
    uint64_t target;                           // absolute target for Branch/CondBranch/Call
    // operands …
};
Inst decode(uint32_t raw, uint64_t pc);        // bit-exact, never throws
}
```

Mapping onto the CFG interface (below): `Inst::flow` corresponds 1:1 to `CfKind` (`Normal → None`, `Syscall → Svc`).
Of `Op::Invalid`, **only** `Unallocated`, `Unpredictable` and `Privileged` end a path (`CfKind::Unknown`);
`NotImplemented` (SIMD/FP, LD1/ST1 – comes with P2.4) is a normal fallthrough for the CFG (`CfKind::None`), because
the instruction does not change control flow. `SVC` falls through, `BRK` and `UDF` end the path (`Trap`).

From the decoder, the CFG only needs the **control flow classification** and a few address arithmetic instructions for the
jump table heuristic. So that the threads do not have to wait for each other, the following applies:

1. The CFG accesses the decoder **only through this narrow interface** (`recomp/core/include/lgp/recomp/a64_cf.hpp`):

   ```cpp
   namespace lgp::recomp {
   enum class CfKind : uint8_t {
       None,          // no control flow (falls through)
       Branch,        // B imm26
       CondBranch,    // B.cond, CBZ/CBNZ, TBZ/TBNZ  (target + fallthrough)
       Call,          // BL imm26
       IndirectBranch,// BR Xn           (jump table or tail call)
       IndirectCall,  // BLR Xn
       Return,        // RET, ERET, DRPS
       Trap,          // BRK, HLT, UDF(0x00000000): no fallthrough
       Svc,           // SVC/HVC/SMC: falls through (runtime call)
       Unknown        // not decodable → diagnostic, block ends
   };
   enum class CondKind : uint8_t { BCond, Cbz, Cbnz, Tbz, Tbnz };
   struct CfInfo {
       CfKind kind;
       uint64_t target;      // absolute target for Branch/CondBranch/Call
       uint8_t reg;          // Xn for IndirectBranch/IndirectCall, test register for CBZ/TBZ (31 = unused)
       uint8_t cond;         // B.cond: condition 0..15; TBZ/TBNZ: bit number
       CondKind cond_kind;
       uint32_t imm;         // SVC/BRK immediate
   };
   CfInfo classify(uint32_t insn, uint64_t pc);
   }
   ```

2. Until the lifter decoder is merged, `recomp/core/src/a64_cf.cpp` implements this function **itself** for the
   control flow class plus the subset `ADR/ADRP, ADD/SUB (imm, shifted, extended), CMP/SUBS, MOV/MOVZ/MOVK, AND imm,
   UBFM/SBFM, LDR (literal, reg-offset, imm), LDRB/H/SB/SH/SW` for the jump table slice (own internal mini decoder
   `jt_decode`, not public).
3. After the lifter decoder is merged (PR #16), `a64_cf.cpp` is switched over to `a64::decode()` (task of the
   CFG thread; `lgp_recomp_core` then links against `lgp_recomp_lifter`); the mini decoder remains only
   as a **cross-check test** (two independent implementations must classify identically on all fixtures) and
   disappears once the test has been green for a quarter. Nobody but the CFG depends on the mini decoder.

   **Status (lifter thread, after PR #14):** `classify()` runs on `a64::decode()`, `lgp_recomp_core` links
   `lgp_recomp_lifter`. The original classifier exists as `tests/recomp/classify_reference.hpp` and is
   checked against `classify()` in `recomp.classify_crosscheck` (≈ 1 million random words plus a dense branch/system space:
   where the decoder accepts a word, kind, target and operands must match; where it rejects it, the result is
   `Unknown`). Deviation from the table above: privileged encodings in the branch/exception space keep their
   class (`HLT`/`DCPS` → `Trap`, `ERET`/`DRPS` → `Return`, `HVC`/`SMC` → `Svc`), all other privileged words
   (e.g. `MSR` to EL1 registers) are `Unknown`. `jt_decode` (jump table slice) has not been switched over yet.

This avoids CFG and lifter decoding differently (argument of the lifter thread, accepted), without the
CFG having to wait for the decoder.

## 3. Data model of the analysis (output of `lgp_recomp_core`)

All addresses are **guest VAs after the link step** (`uint64_t`), i.e. module base + offset. The base is configurable per module
(`LinkOptions::first_base`, default `0x0800'0000`; modules are placed one after another, page by page without gaps, in rtld load order
`rtld, main, subsdk0..9, sdk`). Every structure allows computing back module and offset
(`LinkedImage::locate(va) → {module*, offset}`), so that ADR-0001 (address space/remapping) later only changes the
choice of base, not the analysis.

```cpp
namespace lgp::recomp {

enum class SegmentKind : uint8_t { Text, Ro, Data, Bss };
struct Segment { SegmentKind kind; uint64_t va; uint64_t size; bool r, w, x; };

struct Symbol {                 // from dynsym (exports/imports) or ELF .symtab (fixtures only)
    std::string name; uint64_t va; uint64_t size;
    uint8_t bind; uint8_t type; bool defined; uint32_t module_index;
};

struct Relocation { uint64_t va; uint32_t type; uint64_t addend; uint32_t sym; bool applied; };

struct Import {                 // for ADR-0005: replacement by symbol name (GOT/PLT slot is known)
    std::string name; uint64_t slot_va;            // GOT entry in the importing module
    uint64_t target_va;                            // 0 = unresolved (only allowed for weak)
    uint32_t from_module, to_module;               // index into LinkedImage::modules
};

struct Module {
    std::string name;            // "main", "sdk", fixture file name
    std::string internal_name;   // from the .ro path (NSO only), e.g. "beluga.nss"
    std::array<uint8_t,32> module_id;
    bool is_nro;
    uint64_t header_code_exclusion; // bytes after the .text start that are not code: NRO header 0x80, NSO with MOD0 8
                                    // (the entry `b start` at 0 stays code, the MOD0 pointer word at +4 does not)
    uint64_t base;               // guest VA of the image
    std::vector<Segment> segments;
    std::vector<uint8_t> image;  // decompressed, **after** relocation; length incl. BSS
    std::vector<Symbol> dynsym;
    std::vector<Relocation> relocations;      // RELA + JMPREL + RELR (RELR expanded to type RELATIVE)
    std::vector<uint64_t> fde_starts;         // function starts from .eh_frame_hdr (sorted)
    std::vector<std::pair<uint64_t,uint64_t>> fde_ranges; // [start,end) from the FDEs, if parsable
    std::vector<uint64_t> init_array, fini_array; uint64_t init = 0, fini = 0;
    uint64_t entry;              // entry point (= .text start)
};

struct LinkedImage {
    std::vector<Module> modules;
    std::vector<Import> imports;
    uint32_t read_u32(uint64_t va) const;     // throws std::out_of_range outside all segments
    uint64_t read_u64(uint64_t va) const;
    const Segment* segment_at(uint64_t va) const;
    std::optional<std::pair<const Module*, uint64_t>> locate(uint64_t va) const;
    const Symbol* symbol_at(uint64_t va) const; // defined symbols, exact
};
```

CFG:

```cpp
enum class FunctionSource : uint8_t { Fde, Symbol, Entry, InitArray, Config, CallTarget, TailTarget, DataPointer, CodePointer, GapScan };
enum class Terminator : uint8_t { Fallthrough, Branch, CondBranch, Return, Trap, IndirectJump, JumpTable, NoReturnCall, Unknown };

struct JumpTable {
    uint64_t br_pc;              // address of the BR instruction
    uint64_t table_va;           // table start (.ro or .text)
    uint8_t entry_size;          // 1, 2, 4 (relative) or 8 (absolute)
    bool entries_signed;
    uint64_t base_va;            // base for relative entries (target = base + entry << shift)
    uint8_t shift;               // 0 or 2
    uint32_t count;              // from `cmp idx, #N; b.hi` (bound) – 0 = unknown → heuristic scan, confidence Low
    std::vector<uint64_t> targets;
    enum class Confidence : uint8_t { High, Medium, Low } confidence;
};

struct Call {
    uint64_t pc, target;         // target 0 for indirect
    bool indirect;               // BLR / BR without a table
    bool tail;                   // B / B.cond to a foreign entry, PLT stub (BR x17) or falling into the neighboring function
    uint64_t via_slot;           // PLT stub: GOT slot the target was loaded from (0 = direct)
};

struct BasicBlock {
    uint64_t start, end;         // [start, end), end exclusive, multiple of 4
    Terminator term;
    std::vector<uint64_t> successors;   // fallthrough first, then targets; for JumpTable all table targets
    std::vector<Call> calls;            // BL/BLR in the block; calls do **not** end a block
    std::optional<uint32_t> jump_table; // index into Function::jump_tables
};

struct Function {
    uint64_t entry;
    uint64_t end;                        // end of the last reachable block (or FDE end)
    FunctionSource source;
    std::string name;                    // symbol name, otherwise "fn_%016x"
    std::vector<BasicBlock> blocks;      // sorted by start, non-overlapping
    std::vector<JumpTable> jump_tables;
    bool has_unresolved_indirect;        // at least one BR without a recognized table
    bool noreturn;                       // derived (see below)
    bool has_fde_range; uint64_t fde_end;
};

struct Diagnostic { enum class Kind { ... } kind; uint64_t pc; std::string text; };

struct AnalysisConfig {                  // ADR-0004: config file per game, only addresses/names, no code
    std::vector<uint64_t> extra_functions;
    std::vector<std::string> noreturn_symbols;     // default: abort, exit, _exit, __stack_chk_fail, __cxa_throw, …
    std::vector<uint64_t> noreturn_addresses;
    std::vector<JumpTableOverride> jump_table_overrides;   // br_pc + table_va + entry_size + count (+ base, shift, signed)
    bool scan_gaps = true, seed_data_pointers = true;
    uint32_t max_jump_table_scan = 4096;
};
AnalysisConfig parse_config(std::string_view text);   // text format, see docs/knowledge/recompiler-config.md

struct AnalysisResult {
    std::vector<Function> functions;     // sorted by entry, overlap-free
    std::vector<uint64_t> function_entries; // sorted set of all entries (for BL → direct call)
    std::vector<Diagnostic> diagnostics;
    AnalysisStats stats;
    const Function* function_at(uint64_t entry) const;
    const Function* function_containing(uint64_t va) const;
    bool is_entry(uint64_t va) const;
};

AnalysisResult analyze(const LinkedImage&, const AnalysisConfig&);
}
```

Guarantees of the analysis to the lifter:

- Blocks do not overlap; every block ends either with a terminator or because another block begins at `end`
  (`Terminator::Fallthrough`, exactly one successor).
- Every function has an entry block; all blocks are reachable from the entry (unreachable code is reported as a diagnostic,
  not delivered).
- `successors` point **only within** the same function. A direct jump (`B`) to a foreign function entry is
  a tail call: block terminator `Branch`, `successors` empty, the call is in `calls` with the `pc` of the B instruction and
  is lifted by the lifter as "call + return".
- `BL` to a known entry → direct call. `BL` to an address that is not an entry → diagnostic + the entry is
  added as a function afterwards (`FunctionSource::CallTarget`); this case therefore does not occur in the output.
- A `B` to a foreign entry (`Call::tail`) becomes `LGP_TAIL_CALL`; a PLT stub (`ldr x17,[x16,#got]; br x17`)
  is resolved and delivered as a tail call with `via_slot` (ADR-0005: replacement by import name via the slot).
- `BLR`/`BR` without a table: `Call::indirect` or `Terminator::IndirectJump`. The lifter emits
  `LGP_CALL_INDIRECT(va)` / `LGP_JUMP_INDIRECT(va)`; the runtime (P2.5, ADR-0004) decides what happens.
  According to the ADR recommendation, unknown targets are an error, not a silent fallback. The config file takes additions.
- Noreturn: a callee counts as `noreturn` if (a) its name is in `noreturn_symbols` or its address in
  `noreturn_addresses`, (b) a `BL` to it is the last instruction of an FDE range, (c) a `BL` to it
  immediately precedes a strong neighboring entry (FDE, symbol, call target) that the code would otherwise fall into
  (diagnostic `NoReturnInferred`), or (d) all paths of a function end in Trap/NoReturnCall/an endless loop
  (propagation). A `BL` to a noreturn callee is `Terminator::NoReturnCall` without a successor.
- Function boundary sources in this priority: FDE table (`.eh_frame_hdr`, 59,324 entries for `main`), dynsym
  `STT_FUNC`, entry, `.init_array`/`.fini_array`/`DT_INIT`/`DT_FINI`, `BL` targets, config, RELATIVE relocations into
  `.text` (vtables, function pointer tables; `DataPointer`), finally gap scan (`GapScan`, confidence Low).

## 4. Memory and runtime calls (names only, semantics belong to P2.6 / ADR-0001)

The lifter emits memory accesses and indirect jumps **exclusively** via these macros/functions, so that
ADR-0001 (memory64 vs. 32-bit remapping) and ADR-0004 only change one header file:

All macros have an **implicit `ctx`** (the `CpuState*` of the generated function); the header is included via
`LGP_RUNTIME_HEADER`, default `lgp/runtime/guest.hpp`.

| Group | Names |
|---|---|
| Memory | `lgp_mem_read8/16/32/64/128`, `lgp_mem_write8/16/32/64/128`, `lgp_mem_cas32/64/128` (exclusive/atomics) |
| Control flow | `LGP_CALL_INDIRECT(va)` (BLR), `LGP_JUMP_INDIRECT(va)` (BR without a table), `LGP_TAIL_CALL(fn)` (B to a foreign entry), `LGP_SVC(nr)`, `LGP_BRK(imm)`, `LGP_UNDEFINED(raw)` |
| System | `LGP_YIELD`, `LGP_BARRIER`, `LGP_READ_SYSREG(name)` |
| Runtime hooks | `LGP_TRACE_BLOCK(va)`, `LGP_BACKEDGE(va)`, `LGP_HALT_CHECK` |

The register set lives in `recomp/lifter/include/lgp/lifted/cpu_state.hpp` (`CpuState`: `x[31]`, `sp`, `pc`, NZCV as
individual bytes, `fpcr`, `fpsr`, `tpidr_el0`, `tpidrro_el0`, `vreg[32]`, exclusive monitor, `halt`/`halt_info`, `user`).

The macros are defined in `runtime/core/include/lgp/runtime/guest.hpp` (owner: P2.6 thread; until then a stub in the
lifter test code).

## 4a. Input of the lifter

Per function the lifter takes

```cpp
struct LiftFunction {
    uint64_t entry;
    std::vector<std::pair<uint64_t, uint64_t>> blocks;              // [start, end) from BasicBlock
    std::map<uint64_t, std::vector<uint64_t>> indirect_targets;     // br_pc → table targets (JumpTable::targets)
};
// plus: the list of all function entries (AnalysisResult::function_entries) for BL → direct call
```

The adapter `AnalysisResult → LiftFunction` is a small function without logic of its own:
`recomp/core/include/lgp/recomp/lift_input.hpp` (`to_lift_function`, `to_lift_functions`, `code_reader`,
`make_lifter`). The lifter derives calls, tail calls (`B`/`B.cond` to foreign entries, PLT stubs) and falling into the
neighboring function itself from the entry list; `Call::tail`/`via_slot` are not used yet
(PLT stubs run as `LGP_JUMP_INDIRECT` via the dispatch table; the optimization via the slot follows with P2.5).
Whole chain as a CLI: `lgp_recomp lift <module…> --out lifted.cpp`.

## 5. Fixtures and tests

- Until P0.7 is merged: only synthetic modules (C++ builder in `tests/recomp/synthetic_module.hpp`: builds NSO
  compressed/uncompressed and NRO with MOD0, `.dynamic`, dynsym, RELA, RELR, `.eh_frame_hdr`).
- After P0.7 (PR #9): fixture `basic` (NRO + NSO, built in CI, not committed). Acceptance test P2.1: relocations from
  `lgp_recomp info --json` match `tools/analyze/nso_inventory.py --json`. Acceptance test P2.2:
  `lgp_recomp check-functions fixture.nro --elf fixture.elf` reports 0 missing and 0 wrong function starts
  compared to the ELF `.symtab` symbols (`STT_FUNC`).
- The run against the real `main` only happens on the PC; the output (`--summary`) only contains counters and addresses.

## 6. Open points

- Decoder switchover (section 2, point 3) after the lifter decoder is merged: task of the CFG thread.
- `RET` as a native C++ `return` (ADR-0004 option A) vs. dispatcher: the CFG supports both; it does **not** mark
  functions whose blocks treat LR/SP unusually (that would be lifter analysis).
- TLSDESC relocations: not present in `main` (inventory), are counted and reported as "not supported".
- Several entries per function (shared tails, `B` into the middle of a foreign function): diagnostic `CrossFunctionJump`;
  the lifter only handles the case once it shows up in fixtures or in the real `main`.
