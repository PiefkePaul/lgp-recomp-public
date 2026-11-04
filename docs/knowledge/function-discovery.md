# Function discovery and CFG for static recompilation (AArch64)

Cross-game knowledge from P2.2 (status 2026-10-11). Implementation: `recomp/core/src/cfg.cpp`; check tool
`lgp_recomp check-functions <module> --elf <module.elf>` (compares with the `STT_FUNC` symbols of a homebrew ELF).
Jump tables: [`jump-table-patterns.md`](jump-table-patterns.md); boundaries from FDEs:
[`eh-frame-function-bounds.md`](eh-frame-function-bounds.md).

## Seeds by reliability

| Source | Reliability | Note |
|---|---|---|
| FDE start (`.eh_frame_hdr`) | very high | also provides the end |
| dynsym `STT_FUNC` (defined) | high | in games only exports (few), in NROs almost nothing |
| Entry (`.text+0`), `DT_INIT`/`DT_FINI`, `.init_array`/`.fini_array` | high | read the arrays only **after** relocation |
| Config file (`function 0x…`) | high | manual additions |
| `BL` target | high | every `BL` to an address without a function creates one |
| `B` target outside its own boundaries | medium | tail call target; can also be a shared epilogue |
| Data pointer (RELATIVE/ABS64 from `.data`/`.ro` into `.text`) | medium | vtables, function pointer tables; ignore if the target lies strictly within an FDE range |
| Code pointer (`adrp xd` + directly following `add xd, xd, #lo12` into `.text`) | medium | callbacks (thread entry, `atexit`); only important without `.eh_frame_hdr`, ignored within an FDE range |
| `B` forward over pure padding (without FDE) | medium | stub that tail-calls into the next function (newlib `__call_exitprocs: b __libc_fini_array`) |
| Gap scan | low | only what remains after padding |

Weak seeds (data pointer, code pointer, gap scan) are **withdrawn** if a conditional branch or a table target
from another function points to them or code falls into them: then they were a block start, not a
function start. Strong seeds are never withdrawn; instead there is a diagnostic (`CrossFunctionJump`).

## Boundaries of a function

End = FDE end, otherwise the next seed after the entry, otherwise the end of the segment. Blocks are collected by recursive descent from
the entry and split at every branch target; `BL` does **not** end a block (otherwise the block count explodes; in
game code a call comes every 6 instructions). Terminators: fallthrough (next block begins), internal `B`,
`B.cond`/`CBZ`/`TBZ`, `RET`, trap (`BRK`, `UDF`, `HLT`), indirect branch (resolved to a jump table or open),
call of a noreturn callee, undecodable word. `SVC` falls through (runtime call, returns).

What stood out in the fixture comparison (libnx, GCC 15):

- **Entry stub** `b start` at `.text+0`; it is followed by header words (NRO: up to 0x80; NSO with MOD0: 4 bytes) that
  must be excluded from the code, otherwise the gap scan detects `NRO0` as a function. If the stub jumps directly past the
  header to a word without its own seed (libnx/clang-crt0 `_start: b 1f`), stub and code are **one** function with
  the header as a gap; otherwise `_start` (symbol size spanning the header) counts the code at 0x80 as a wrong
  function start. If the stub points to an FDE/symbol seed, it stays a tail call.
- `crt0` ends with `br x30`-like constructs and data words without a symbol; undecodable words after a `BR`
  are normal and not analysis errors.
- **devkitA64 links without `.eh_frame_hdr`** (no `--eh-frame-hdr` in the bare-metal spec), although `.eh_frame` is present and
  MOD0 points to the header: the loader then sees **zero** FDEs. All boundaries come from BL targets and heuristics.
  With `-Wl,--eh-frame-hdr` (fixture `threads`) the module behaves like an SDK module. Without the header, functions
  that are only addressed via `adrp/add` were missing (libnx `_EntryWrap`, newlib `__libc_fini_array`), hence the
  code pointer seeds (P2.8).
- GCC creates **no** function without an FDE in `-O2` C code; all missing starts were assembler without
  `.cfi_startproc` or labels within a `.cfi` range.
- **A `BL` to an undefined weak symbol is turned into a `NOP` by the linker.** libnx' `__libnx_exception_returnentry`
  therefore starts with a NOP (`bl __libnx_exception_handler`, handler not defined), has no FDE and is only
  referenced via `adrp/add`. A gap scan that blindly skips leading NOPs as padding starts the function
  one instruction too late. Rule: padding always ends on an address aligned to at least 8 bytes; NOPs
  that lead to a word aligned only to 4 are code.

## noreturn

Without noreturn knowledge, functions ending with `bl abort` fall into the next function and pull its blocks to themselves.
Sources in this order:

1. Name list (`abort`, `exit`, `_exit`, `__stack_chk_fail`, `__cxa_throw`, `__assert_func`, `svcExitProcess`,
   `nn::diag::detail::AbortImpl…`, …), resolved via dynsym and imports. In games this works for `sdk` exports,
   in NROs practically not at all (names are only in `.symtab`, which the loader does not see).
2. `BL` as the last instruction of an FDE range.
3. `BL` immediately before a strong neighboring seed (code would otherwise fall into it).
4. Propagation: all paths of a function end in a trap, a noreturn call or an infinite loop (`b .`) → the function is
   itself noreturn. A few iterations, then it is stable.

Conditional calls (`cbz; bl abort; ...`) are not a problem: only the block with the `BL` ends.

**Sources 2–4 are guesses and must be refutable.** In a real NintendoSDK `sdk`, cleanup paths
(EH landing pads) end with a `bl` to an ordinary thunk (`nn::os::UnlockMutex`), because the compiler left out the continuation as
unreachable. Rule 2 therefore considered the thunk noreturn, and in thousands of callers the code
after the call was missing: at run time the lifted function then jumps via the dispatch table to the return address and
halts (`unresolved indirect branch target` = a return address). The analysis therefore withdraws guessed noreturn
as soon as the function's own code can return (`RET`, unresolved `BR` or tail call into such a
function, as a fixed point); only config names/addresses stay fixed. For LGP this reduced the number of noreturn functions
from 2,704 to 155. Errors in this direction are cheap (some unreachable code is lifted), in the other direction fatal.

## Diagnostics that indicate rework

| Diagnostic | Meaning | Reaction |
|---|---|---|
| `unresolved_indirect_jump` | `BR` without a table, no PLT, no constant target | tail call via function pointer (common) or table in an unknown pattern → config `jumptable …` |
| `jump_table_unbounded` | table without `cmp` | check the count, `count=` in config |
| `jump_table_override_rejected` | `jumptable` line yields targets outside `.text` or unaligned, not applied | check `shift=`/`entry=`/`base=` ([`recompiler-config.md`](recompiler-config.md)) |
| `jump_table_override_outside_function` | `jumptable` line applied, one target lies outside the function | usually `count=` too large |
| `falls_into_next_function` | last block without terminator before another function | missing noreturn callee → `noreturn <name|0xaddr>` |
| `cross_function_jump` | `B` into the middle of a foreign function | shared epilogue or wrong boundary; the lifter must support multiple entry or the config splits |
| `call_into_function_body` | `BL` to an address inside a function | for FDE functions: code bug or thunk; without FDE: the function is split |
| `undecodable_instruction` | data word in code | normal after `BR`/`RET` (literal pool), otherwise the boundary is wrong |
| `seed_demoted` | weak seed was a block start | informational only |

## Acceptance test against ELF symbols

`check-functions` takes all `STT_FUNC` symbols with a size in the ELF's `.text`, rebases them onto the module base and
checks: **missing** (symbol without function start), **wrong** (function start inside the body of a symbol), **extra**
(start without a symbol outside all bodies; permissible, e.g. entry stub, `crt0` labels). Only "missing" and "wrong"
make the test fail; "extra" is printed. For retail `main` there is no ELF; there the
comparison with the Ghidra numbers (`docs/analysis/04-code-statistik.md`) replaces the acceptance test.
