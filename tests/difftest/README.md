# Differential test harness

Concept and decision: [ADR-0006](../../docs/decisions/0006-differential-test-harness.md).

| Part | Path | Status |
|---|---|---|
| Common interface (`TestCase`, `GuestState`, `RunResult`, `IExecutor`, `diff()`) | `oracle.h`, `oracle.cpp` | P0.8 |
| Unicorn executor (CI oracle) | `unicorn/` | P0.8, ctest `difftest.unicorn_oracle` |
| Hypervisor.framework executor (Mac) | `hv/` | Spike `difftest.hv_spike` (PR #8); `HvExecutor`/`hv::Machine` (EL0, MMU on), ctest `difftest.hv_executor` (P2.7) |
| Function comparison native ↔ recompiled (stage 3) | `fn/` | `lgp_fn_select` + runner, self-test `difftest.fn_selftest` (P2.7) |

## Unicorn

- Version 2.1.4, AArch64 architecture only, downloaded at configure time (official release sdist from PyPI, pinned by SHA-256) and statically linked into the **test** programs. License GPL-2.0: Unicorn must never be linked into `recomp/`, `runtime/`, `gpu/` or `platform/`.
- Switch `LGP_ENABLE_UNICORN`: on by default, off under Emscripten and (until checked on the PC) under Windows.
- CPU model Cortex-A57 (`UC_CPU_ARM64_A57`), as on the Switch.
- Stop: `BRK #0x4C47` (sentinel), `svc #0x7` (`svcExitProcess`), reaching `TestCase::stop_pc` (no longer executed), instruction limit or fault. Every SVC is recorded with its number, x0–x7 before and after; the reply comes from the test case's `SvcHandler`.
- `final.pc` is the architectural return address: for the sentinel, the `BRK` itself; for `svcExitProcess`, the instruction after it.

### Known differences Unicorn ↔ Switch (for the exception list from ADR-0006)

Observed with small probes (not checked in as a test):

- Unicorn executes the code in **EL1** (`CurrentEL` = 1); on the Switch game code runs in EL0. Accesses to EL1 registers therefore go through in Unicorn, while on hardware they would trap.
- The MMU is off, yet an unaligned `LDUR` produces no fault (QEMU does not check this here). On the Switch (EL0, Normal Memory) it is allowed as well, so the result is correct, but for a different reason.
- `LDXR`/`STXR` succeed in a single thread as expected; the exclusive monitor is simplified (ADR-0006).

## Hypervisor (macOS on Apple Silicon only)

Technique and pitfalls: [`hypervisor-framework-el0-harness.md`](../../docs/knowledge/hypervisor-framework-el0-harness.md).

- `hv::Machine` executes guest code in **EL0 with MMU** (4 KiB page tables from the host, VA = IPA, guest VAs below
  60 GiB). Exceptions from EL0 reach the host via a vector table from `hvc #n`, `BRK` directly.
- `HvExecutor` is an `IExecutor` like the Unicorn executor: same stop rules (sentinel `BRK`,
  `svcExitProcess`, fault), same SVC recording. `max_instructions` is approximated via a time limit
  (≈ 1 ns per instruction, at least 100 ms) and yields `InstructionLimit`. `stop_pc` is not supported.
- `difftest.hv_executor` checks the same cases as the Unicorn test plus EL0-specific ones (unaligned accesses,
  exclusive accesses, `DC ZVA`, writing to read-only pages, executing data, counter reads trap,
  time limit) and compares every case that is defined the same on both sides with `diff()` against Unicorn.
  So far no difference between M1 and Unicorn in these cases.

## Function comparison (`fn/`, ADR-0006 stage 3)

```sh
# 1. Select functions and lift only these (output from game code goes to $LGP_PRIVATE_DIR)
build/default/tests/difftest/fn/lgp_fn_select <modul…> --out $LGP_PRIVATE_DIR/difftest/all
# 2. Build the runner against this selection (Release with checked guest memory is ≈ 20× faster than Debug)
cmake -S . -B build/difftest -G Ninja -DCMAKE_BUILD_TYPE=Release -DLGP_GUEST_MEM_CHECKED=ON \
      -DLGP_INSTALL_GIT_HOOKS=OFF -DLGP_FN_DIFFTEST_LIFTED=$LGP_PRIVATE_DIR/difftest/all
cmake --build build/difftest --target lgp_fn_difftest
# 3. Compare
build/difftest/tests/difftest/fn/lgp_fn_difftest <modul…> --candidates $LGP_PRIVATE_DIR/difftest/all/candidates.tsv \
      --report $LGP_PRIVATE_DIR/difftest/report.txt [--cases 16] [--seed S] [--fpcr-mix] [--only ADDR --case K --verbose]
```

- **Selection:** a function and everything it calls directly, without `SVC`, `BRK`, `BLR`, unresolved `BR`,
  system registers other than NZCV/FPCR/FPSR, WFE-like hints, non-returning calls; at most 64 functions or
  20,000 instructions in the call closure. Only these functions are lifted.
- **Inputs** (deterministic from seed, function and case number), four profiles in rotation: pointer arguments to
  random buffers, small integers, arbitrary 64-bit values, buffers full of pointers ("structures"). FP arguments are
  "normal" numbers with interspersed NaNs, infinity, ±0 and denormals. `--fpcr-mix` additionally varies FPCR
  (FZ, DN, rounding modes). x30 points to a sentinel `BRK`.
- **Compared:** exit, x0–x30, sp, NZCV, v0–v31, FPCR, FPSR without the IEEE exception flags (deliberately not modeled
  by the lifter), every writable byte (data/BSS of all modules, buffers, stack). If there is no way back (abort
  on both sides), only the fact that both abort counts; a differing fault address is counted separately.
- A child process does the computing; if the recompiled run crashes or hangs for longer than 10 s, the parent process reports
  the function as `crash` and continues with the next one. If the hypervisor runs into a time limit, the case is
  skipped (`hv-timeout`).
- `--known FILE` (lines `ADDRESS reason`): differences in these functions appear as `known` instead of `MISMATCH`
  and do not fail the run. Intended for explained differences **M1 ≠ A57**: writable FPCR bits
  (trap enables, FZ16; RAZ/WI on the A57), FPSR N/Z/C/V (RES0 on the M1), unaligned exclusive accesses within
  a 16-byte block (M1 without alignment faults), plus code that evaluates the FPSR IEEE flags that are not modeled
  (`fetestexcept`). The list for the game lives in `$LGP_PRIVATE_DIR`, background in
  `11-difftest-spielfunktionen.md`.
- The self-test `difftest.fn_selftest` builds its own functions (`fn/guest/functions.c`: hashes, CRC, sorting,
  division, 128-bit multiplication, bit tricks, jump table, scalar FP, conversions, NEON, atomics, recursion,
  stack arguments) with host clang + ld.lld and runs them with 64 cases and `--fpcr-mix`.
