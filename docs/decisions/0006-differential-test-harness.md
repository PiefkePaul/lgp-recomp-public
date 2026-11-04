# 0006 – Differential test harness: original on real ARM64 hardware vs. recompiled code

Status: **Accepted** (Paul, 2026-10-10) · Date: 2026-10-10 · Decides: Paul · Roadmap: P2.7 (dep. P0.6, P0.8, P2.3)

Not a core ADR (0001–0005), but it defines interfaces on which recompiler tests and later runtime tests build. This document contains no game code and only describes methods.

## Context

The lifter (P2.3/P2.4) needs an oracle that says what an AArch64 instruction sequence *really* does. Two are planned:

1. **Unicorn** (QEMU TCG, P0.8): runs everywhere, i.e. in CI and cloud threads. Weaknesses: Unicorn is itself an emulation. Known problem zones are FP/NEON edge cases (NaN propagation, FPCR bits, rounding for FMA/conversions), exclusive accesses (the monitor is simplified) and system/counter registers.
2. **Real hardware** on the Mac M1 via **Hypervisor.framework**: guest code runs natively on the Apple core, we see every exception (SVC, BRK, data abort) as a VM exit. This is the ground truth for everything ARMv8.0 defines architecturally.

Ryujinx uses exactly this approach on Apple Silicon (backend `Ryujinx.Cpu/AppleHv`, MIT). For us it is doubly useful: as a reference for the technique and as a full-game reference (P1.3/P0.6).

### Important differences M1 ↔ Switch (Cortex-A57, ARMv8.0-A)

The hardware is not identical. The harness must deal with these points deliberately; otherwise false errors appear or real errors stay invisible:

| Point | Consequence for the harness |
|---|---|
| M1 can do more (v8.4+: LSE atomics, RCpc, FP16, …) | Generators only produce ARMv8.0 instructions; the harness checks against an opcode whitelist. |
| ID/cache registers (`MIDR`, `CTR_EL0`, `DCZID_EL0`) differ | Reading such registers is trapped or not compared; values come from a fixed Switch table (the same as in the runtime). |
| Host page size 16 KiB (stage 2), NSO segments are 4 KiB aligned | Stage 2 mappings in 16 KiB blocks, permissions per 4 KiB via our own stage 1 page tables (4 KiB granule) in the guest. |
| Memory ordering (weak) | Cannot be tested deterministically. Multi-threaded cases only run in baton mode (ADR-0002); real ordering tests are explicitly **not** a goal. |
| Implementation-defined behavior (e.g. unaligned exclusive accesses, `DC ZVA` block size) | List in `docs/analysis/`; such cases are marked instead of compared. |

## What is compared

A **test case** is: code bytes (our own fixture, never game code in the repo), initial state, stop condition.

```
GuestState {
  x[31], sp, pc, nzcv,
  v[32] (128 bit), fpcr, fpsr,
  tpidr_el0, tpidrro_el0,
  memory: list of writable regions (base, size, hash; on mismatch a byte diff)
}
```

Stop: `BRK #0xLGP` as sentinel, `svcExitProcess`, instruction limit or timeout. In addition, the **sequence of SVC calls** (number, arguments, return value) is compared; on both sides the SVCs themselves are served by **the same** C++ handler from the runtime (P2.6/P3), so that only the CPU part runs against each other.

## Options

### A: Unicorn only (no Mac harness)

- Pros: one oracle, runs in CI, no Mac needed.
- Cons: bugs in QEMU/Unicorn become bugs in the lifter, and the tests even confirm them. FP/NEON in particular (P2.4, size XL) is prone to this.

### B: Own minimal harness on Hypervisor.framework (recommendation)

A small C++ program (`tests/difftest/hv/`, only built on `APPLE AND arm64`):

1. `hv_vm_create`, guest memory via `mmap` + `hv_vm_map` (16 KiB aligned).
2. A tiny EL1 stub (our own code, ~1 page): page tables for the fixture layout, a vector table that forwards every exception from EL0 to the host via `HVC`. The test code runs in **EL0** with the MMU on, as on the Switch. (MMU off would treat everything as device memory: unaligned accesses and exclusive accesses would then behave differently.)
3. Counter/ID register accesses from EL0 are redirected to the host via `CNTKCTL_EL1` or trap configuration and answered there with deterministic values.
4. Host loop: `hv_vcpu_run` → evaluate the exit (`ESR`/`ELR`/`FAR` from the system registers) → SVC to the runtime handler, BRK sentinel = end, any other exception = test result "Fault" (also compared).
5. For instruction tests, bundle many cases per VM exit (the stub loads state, executes, saves state, `HVC`), because an exit is expensive (order of magnitude microseconds).

Important: `hv_vcpu_*` must run on the thread that created the vCPU; the binary needs the entitlement `com.apple.security.hypervisor` (ad-hoc signature via `codesign -s - --entitlements …` as a post-build step; to be confirmed on macOS 26 in the spike).

- Pros: real hardware as the oracle, full control, small (estimated 1–2 kLOC), no third-party code needed, Ryujinx's AppleHv backend serves only as a reading reference.
- Cons: runs only on the Mac (not in GitHub CI without macOS arm64 runners, and even there the hypervisor is not available in VMs). Our own EL1 stub is error-prone systems programming.

### C: Instrument Ryujinx (fork) as an oracle

Patch Ryujinx with the hypervisor backend so that it outputs states at function boundaries.

- Pros: the real game with real memory state is available immediately.
- Cons: large (.NET, an entire emulator) and hard to make deterministic (threads, timing, GPU). Oversized for instruction and fixture tests. Remains a tool for **later** game-level tests (see stage 3).

## Recommendation

**B as the Mac oracle, Unicorn remains the CI oracle.** Both sit behind a common interface, so that every test case can run against all three sides:

```cpp
// tests/difftest/oracle.h (sketch)
struct RunResult { GuestState final; std::vector<SvcRecord> svcs; Outcome outcome; };
class IExecutor {
 public:
  virtual ~IExecutor() = default;
  virtual RunResult run(const TestCase& tc) = 0;
};
// UnicornExecutor (everywhere), HvExecutor (macOS arm64 only), RecompiledExecutor (generated code)
```

Comparison: `Recompiled == HV` is the acceptance criterion on the Mac. `Unicorn != HV` is a finding of its own (Unicorn bug or implementation-defined) and goes into an exception list, so that CI does not permanently test against a wrong oracle.

### Stages

| Stage | Content | When | Data |
|---|---|---|---|
| 0 Spike | `hv_vm_create` + one `ADD` + `BRK` runs, signature/entitlement clarified | right after P0.6 | none |
| 1 Instructions | generator for random states × opcode whitelist (integer, then FP/NEON), bundled per exit; the same cases run in CI against Unicorn | in parallel with P2.3/P2.4 | none |
| 2 Fixtures | whole homebrew NSOs/NROs up to `svcExitProcess`, SVC trace comparison, baton mode for threads | P2.7/P2.8 | only our own fixtures |
| 3 Game functions | individual functions from the real `main` with recorded inputs (registers + touched memory pages) in the HV vs. recompiled | from P2.9, only **locally on the Mac** | game data, results only in `$LGP_PRIVATE_DIR` |

For stage 2/3 on a mismatch: **block trace mode**. The recompiler can emit an optional hook `LGP_TRACE_BLOCK(pc, state)` per basic block; in the HV the same trace is produced with single-step (`MDSCR_EL1.SS`) or breakpoints at block starts. The first differing block pair is the location of the error.

Stage 3 is the Mac's real added value for the real game. Inputs for it can be recorded from stage-2-like runs of our own runtime as soon as P3 boots; Ryujinx (option C) is only plan B for this.

## Open questions / data needed

- Does an ad-hoc signature with `com.apple.security.hypervisor` work on macOS 26 without a developer account? (spike, stage 0)
- Measure the exit cost on the M1 and derive the bundle size for stage 1 from it.
- Which Unicorn differences actually exist? (results from stage 1)
- Whether GitHub runners with macOS arm64 offer Hypervisor.framework: to current knowledge no (nested virtualization); then stages 1–2 on the Mac remain a local run, e.g. as part of the "done" check for lifter PRs.

## Result stage 0 (spike, 2026-10-11, Mac M1, macOS 26.7.1)

`tests/difftest/hv/hv_spike.cpp`, ctest `difftest.hv_spike`:
- An ad-hoc signature with `com.apple.security.hypervisor` via `codesign --sign -` as a post-build step is sufficient; no developer account needed.
- `ADD`/`SUBS` run natively in the guest (EL1, MMU off), `BRK` comes back as an exit with EC 0x3C with `hv_vcpu_set_trap_debug_exceptions`, PC is on the `BRK`. Registers and NZCV are as expected.
- Exit round trip (`HVC` → host → guest): approx. **0.74 µs** (20,000 exits). For stage 1, bundling a few hundred cases per exit is therefore already enough for the exits not to dominate.
- Next step (stage 1): EL1 stub with page tables, test code in EL0, loading/saving state in the stub.

## Result P2.7 (2026-10-11, Mac M1, macOS 26.7.1)

- **EL0 with MMU works** (`tests/difftest/hv/hv_machine.*`, `HvExecutor`): the host builds the page tables, a
  vector table of `hvc #n` brings every EL0 exception to the host, `BRK` comes directly. An EL1 guest stub with
  state loading/saving was not needed, because the host sets all registers (incl. `SP_EL0`, `ELR`/`SPSR`) directly.
  Technique: [`hypervisor-framework-el0-harness.md`](../knowledge/hypervisor-framework-el0-harness.md).
- `difftest.hv_executor`: all oracle cases of the Unicorn test yield the same state on the M1 as on Unicorn;
  EL0 specifics (unaligned, exclusive accesses, `DC ZVA` with 64 bytes, write protection, NX, counter trap) behave
  as expected on the Switch.
- The P2.5 program (`-L indirect`: function pointers, virtual calls, jump tables, tail call chain over one
  million states, `setjmp`/`longjmp`) runs in the hypervisor and recompiled with an identical final state.
- **Stage 3 brought forward, without recorded inputs:** instead of recording inputs from a runtime run,
  `lgp_fn_select` picks functions whose entire call closure only touches registers and memory, and the runner generates
  random states (pointer arguments to buffers, small/large numbers, pointer structures, FP special values, FPCR variants).
  Results: `11-difftest-spielfunktionen.md`.
- Not implemented: `stop_pc` in the `HvExecutor`, block trace mode, instruction generator (stage 1, belongs to
  lifter fuzzing) and the libnx fixtures in the hypervisor (need devkitPro on the Mac and an HV executor for
  guest threads on `lgp::rt::Kernel`).

## Consequences

- New subtree `tests/difftest/` (interface + Unicorn executor in P0.8, HV executor in P2.7). CMake option `LGP_ENABLE_HV` (default `ON` only on macOS arm64).
- The recompiler gets two mandatory features for testability: entry "from address X with state S until the sentinel" and the optional block trace hook.
- A shared table of Switch ID/system register values between runtime and harness.
- No game data, no generated code and no stage 3 results in the repo; stage 3 tools only read from `$LGP_PRIVATE_DIR`.

## Sources

- Apple, Hypervisor.framework (`hv_vm_create`, `hv_vm_map`, `hv_vcpu_run`, entitlement `com.apple.security.hypervisor`): <https://developer.apple.com/documentation/hypervisor>
- Ryujinx AppleHv backend (MIT, reading reference for EL0 execution with an EL1 stub): Ryubing fork, `src/Ryujinx.Cpu/AppleHv/`
- Unicorn Engine: <https://www.unicorn-engine.org/>
- Arm ARM (DDI 0487), chapters on debug single-step (`MDSCR_EL1.SS`) and the Generic Timer (`CNTKCTL_EL1`)
