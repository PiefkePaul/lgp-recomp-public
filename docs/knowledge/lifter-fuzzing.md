# Lifter fuzzing: mixed instruction sequences against Unicorn

Game-independent. Applies to any static A64 recompiler that checks its translation against an emulator as an oracle.
Code: `tests/lifter_fuzz/`, ctest label `lifter_fuzz`. Builds on the single-instruction differential tests from P2.3/P2.4
(`tests/lifter/`, `tests/lifter_simd/`) and the Unicorn oracle ([`unicorn-als-orakel.md`](unicorn-als-orakel.md)).

## Why, when there are already differential tests

The generators from P2.3/P2.4 produce random words per encoding group and mostly **one** instruction per case. That checks
the semantics of each instruction, but hardly how instructions interact. Typical lifter bugs that only show up in sequences:

- Register aliasing: an instruction writes `Rd` before it reads `Rn` a second time (`Rd == Rn`).
- Flags: an `ADDS` before `CSEL`/`CCMP`/`B.cond`, an FP compare before `FCSEL`, `MSR NZCV` in the middle of the sequence.
- Upper bits of V registers that a scalar or 64-bit vector instruction must zero and that a later 128-bit instruction
  reads.
- FPCR changes in the middle of the function (rounding mode, FZ, DN, AHP), in case the lifter caches FPCR.
- Values across a back edge (loop) and mixed memory accesses (integer store, then a SIMD load of the same
  address).
- Undefined behavior in the generated C++ (shift ≥ width, signed overflow) that only shows up under optimization.
  That is why the fuzzer runs with `-O2`, like a game build, and optionally with UBSan.

The fuzzer does not replace the single-instruction tests. A deliberately planted bug in `CLS` (32-bit, wrong only for the
inputs 0 and −1) was only noticed after about 1,600 cases; the group generator finds it immediately.

## Generator (`fuzz_gen.cpp`)

Each case is a sequence of 1–16 instructions plus a sentinel `BRK`, derived deterministically from a 64-bit case seed:

| Share | Content |
|---|---|
| 30 % | integer ALU from the groups in `tests/lifter/int_groups.hpp` |
| 14 % | integer loads/stores, literal loads |
| 36 % | scalar FP and Advanced SIMD from the groups in `tests/lifter_simd/groups_*.inc` |
| 8 % | SIMD structure accesses (LD1–LD4, ST1–ST4, LDnR) |
| 7 % | forward branches (`B`, `B.cond`, `CBZ/CBNZ`, `TBZ/TBNZ`) |
| 5 % | `MSR FPCR` with a random value, `MSR/MRS NZCV`, `MRS FPCR`, `SVC` (since P6) |

In addition, 20 % of the cases get a counting loop (1–3 iterations, counter `x29`) around the whole sequence.

**Calls (since P6, call costs).** In 25 % of the cases a second function follows the sentinel (set flags,
1–16 random instructions with forward branches and `SVC`s, `RET`), which the caller calls via `BL` (at least once,
also inside the loop). Both are translated as separate lifted functions; the call is a real
C++ call with write-back and reload of the locals. So that the cases comply with AAPCS64 (flags undefined at function boundaries,
`LiftOptions::flags_across_calls = false` in the release shape), the generator sets the flags at the beginning of both
functions and directly after each `BL` (`MSR NZCV` or `CMP`). Since P6 part 3, `RET` in the release shape does not write back the
registers that are volatile under AAPCS64 (x9–x17, v4–v7, v16–v31, `LiftOptions::registers_across_calls = false`); the
generator therefore overwrites them after each `BL` (`MOVZ`, `MOVI`), so that the caller never reads the callee's values there.
The callee never writes `x30`. The minimizer keeps
`BL`, `RET`, the inner sentinel and these flag instructions. Calibration: a lifter that does not reload `x0` after calls
was caught in 8 of 800 cases.

Rules that turned out to be necessary:

- **Pick the group first, then roll within the group.** If you pick a new group on an invalid word, rejection
  sampling distributes the cases by the density of valid encodings: `dp1` (only 10 valid opcodes) occurred as `CLS` (32-bit)
  just 7 times in 3,000 cases.
- **Hot registers:** in 60 % of the ALU instructions `Rd`/`Rn` (and partly `Rm`) point to `x0..x5`/`v0..v5`. Otherwise almost
  every instruction reads fresh random values, and results never flow onward.
- **Protected registers:** `x18..x28` and `SP` are base and index registers of the memory accesses (they point into the
  data window), `x29` is the loop counter. Integer ALU targets and bits 4:0 of SIMD instructions (where the
  X target of `FMOV`, `UMOV`, `FCVTZS` … lives) are remapped to `x0..x17`. Exception: `FCMP`/`FCCMP`, whose bits 4:0 are opcode
  or flags. Without this, sequences almost always ended in a memory fault.
- **Forward branches only** (plus the one loop), so that every case terminates. Branches never target the loop's `CBNZ`,
  otherwise the decrement is skipped.
- Initial state: X registers with many boundary values (also W boundary values under random upper 32 bits), V registers each
  randomly as bit patterns, FP values (NaN, denormals, ±0, ±inf, conversion boundary values) or integer lanes, FPCR ≠ 0 in
  60 % of the cases, FPSR.QC sometimes already set.

## Comparison (`fuzz_run.cpp`)

As in P2.4: all X/V registers, SP, PC, NZCV, FPCR, FPSR without the unmodeled IEEE exception flags, writable
memory and exit. A memory fault is a valid result if both sides stop at the same instruction with the same
state (the lifter runs with `precise_fault_pc`). Anything other than the sentinel or a fault in the oracle counts as a
generator bug.

An `SVC` (never `svcExitProcess`) produces new values in x0–x7 in both executors (`x[i] = x[i] · 0x9E3779B97F4A7C15 +
imm + i`). The arguments at the call and the final state are compared; this way the fuzzer checks that registers held in
locals are written back before a call and reloaded afterwards
([`lifted-code-performance.md`](lifted-code-performance.md)).

**Code shape.** The default is `precise_fault_pc` (like the tests). `--release-shape` lifts like a game build: registers
live only in locals until the next call, `CpuState` is stale on a memory fault, and cases with calls
translate flags and registers at `RET` according to AAPCS64 (like `lgp_lift` without `--exact-calls`). If both sides end with a
fault, only the result counts there (ctest `lifter_fuzz.driver_release`, which also replays `regressions.txt` in this form).

`make_test_case()` defines how the seed becomes registers and memory. This function is **frozen**: the
regression lines name only the seed and overrides, and a change would silently alter every regression case.

## Two ways to run

1. **ctest** (`lifter_fuzz.difftest`): `lgp_lifter_fuzzgen` generates 3,000 cases with a fixed seed at build time plus all
   cases from `tests/lifter_fuzz/regressions.txt`, lifts them and compiles them with `-O2` into the test. Needs no compiler at
   run time. Options: `LGP_LIFTER_FUZZ_SEED`, `LGP_LIFTER_FUZZ_CASES`.
2. **Long runs** (`build/<preset>/tests/lifter_fuzz/lgp_lifter_fuzz`, POSIX with GCC/Clang only): generates batches,
   compiles them at run time in parallel with the host compiler into shared libraries, loads them via `dlopen` and compares.
   The program exports its symbols (`ENABLE_EXPORTS`), because the lifted code calls back into the test runtime
   (`g_guest`, `svc`, `call_indirect`).

   ```sh
   lgp_lifter_fuzz --seed 0x1001 --seconds 1800           # 30 min, stops after 5 mismatches
   lgp_lifter_fuzz --cases 20000 --cxx clang++            # with the compiler of the game build
   lgp_lifter_fuzz --cases 5000 --ubsan                   # UB in generated code, aborts at the first finding
   lgp_lifter_fuzz --case-seed 0x281e1eaa814123e4         # repeat a reported case
   lgp_lifter_fuzz --replay tests/lifter_fuzz/regressions.txt
   ```

   `LGP_LIFTER_FUZZ_LONG_SECONDS=<s>` additionally creates the ctest `lifter_fuzz.long` (label `lifter_fuzz_long`).
   `lifter_fuzz.driver*` check the run-time compilation path with 60 cases.

Mismatching cases are minimized: each round replaces every instruction individually with `NOP` (all candidates of a round in
one compilation), keeps the first one that still mismatches, removes the `NOP`s at the end (only without PC-relative instructions) and
then drops overrides that change nothing (without recompiling; overrides only affect the initial state). A
reduction only counts if Unicorn still ends at the sentinel or with a fault; otherwise the minimizer would
for example replace the load of the loop counter with `NOP`, leaving a generator bug (instruction limit). The output is
a ready-made line for `regressions.txt`.

With `--ubsan`, UBSan reports the line in the generated batch (`<work>/batch_*.cpp`, left in place on abort). With clang on Linux the batches link clang's shared UBSan runtime (`-shared-libsan`, found through `-print-file-name`, else `-print-runtime-dir`, and an rpath), because the fuzzer itself does not carry it; the clang package must include the compiler-rt sanitizer libraries. The
comment `/* <pc>: <wort> */` at the start of the line names the instruction.

## Throughput

Compile time dominates, not Unicorn: 500 cases (≈ 5,000 instructions) take 3.8 s with GCC 13 at `-O0`, 10.6 s at `-O1`,
13.9 s at `-O2`, and 22 s with clang 18 `-O2`. Execution and comparison manage about 270 cases/s. On 4 cores this gives
about 70 cases/s, i.e. roughly 250,000 cases per hour.

## Procedure for a finding

1. Check against the Arm ARM first, do not blindly trust Unicorn: Unicorn is QEMU and runs at EL1. A Unicorn bug
   does not go into the lifter but into the docs as an exception with a justification.
2. Add the minimized line with a comment (bug, cause, fix) to `tests/lifter_fuzz/regressions.txt`.
3. Fix the lifter, add the semantics knowledge to [`a64-semantik-flags.md`](a64-semantik-flags.md) or
   [`a64-fp-simd-semantik.md`](a64-fp-simd-semantik.md).
4. `ctest --preset default -L lifter` (single-instruction tests, fuzz regressions, fixed fuzz run).

## Results

As of 2026-10-11, lifter from P2.3/P2.4 (main `4b07654`):

| Run | Cases | Mismatches |
|---|---|---|
| GCC 13, `-O2`, 25 min | 105,600 | 0 |
| clang 18, `-O2`, 15 min | 60,800 | 0 |
| GCC 13, `-O2` + UBSan, 10 min | 32,000 | 0 (no UB report) |
| GCC 13, `-O2`, up to 48 instructions, 5 min | 13,600 | 0 |
| ctest, fixed seed | 3,000 | 0 |

About 215,000 mixed sequences without a mismatch. This does not mean the lifter is bug-free, but that the space
this generator covers is clean against Unicorn. Not covered: exclusive accesses and barriers in sequences,
nested and indirect calls (`BLR`), indirect branches, `DC ZVA`, memory faults in the middle of a sequence (all runs ended at the sentinel),
multithreading. These are checked by the templates in `tests/lifter/casegen.cpp`, `tests/indirect/` and `tests/homebrew/`.

Calibration with deliberately planted bugs: the fuzzer found `CLS` (32-bit) without `| 1` after ~1,600 cases and
minimized it to one instruction; UBSan reported a shift mask of 63 instead of 31 in `LSLV` (32-bit) in the first batch. Before the
rule "group first, then word", the `CLS` bug went undetected in 4,000 cases.
