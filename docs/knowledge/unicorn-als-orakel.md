# Unicorn as an ARM64 oracle: lessons learned

Game-independent. Unicorn 2.1.4 (QEMU-based), CPU model Cortex-A57, integrated in P0.8
(`tests/difftest/unicorn/`, observations there in `tests/difftest/README.md`). This page lists the points that came up
during the lifter test (P2.3).

## Differences from hardware that affect tests

| Point | Unicorn | Switch (A57, EL0) | Handling |
|---|---|---|---|
| Exception level | EL1 | EL0 | EL1-only instructions (`MSR TPIDRRO_EL0`, `DC IVAC`, `MRS MIDR_EL1`, …) execute in Unicorn; the decoder marks them `Invalid::Privileged`, tests do not generate them. |
| "Should be one" fields | ignored | CONSTRAINED UNPREDICTABLE | Decoder rejects (`Invalid::Unpredictable`), the fuzz test counts them separately. |
| FPCR Stride/Len (bits 21:20, 18:16) | readable and writable | readable and writable (AArch32 context) | Lifter masks with `0x07F70000`; initially wrongly `0x07C00000`, the test found it. |
| FPCR trap enables | not stored | RAZ/WI (A57 without traps) | matches. |
| Unaligned normal accesses | allowed | allowed (Normal Memory) | result matches. |
| `HLT` | UNDEFINED | UNDEFINED in EL0 | matches. |
| Counters (`CNTVCT_EL0`, `CNTFRQ_EL0`) | QEMU values | 19.2 MHz | not compared, goes through `LGP_READ_SYSREG`. |
| `CTR_EL0`, `DCZID_EL0` | `0x8444C004`, `4` | same | constants in the lifter. |

## Practical notes

- Build Unicorn itself with `-fwrapv`. QEMU relies on overflowing signed arithmetic (its own
  build sets `-fwrapv`), Unicorn's CMake does not. Without it, clang 18 at `-O2` folds the NEON helper
  `a > b ? a - b : b - a` in a way that makes `SABD`/`SABA` return the negated result for lanes with a difference ≥ 2^31. This
  only happened in the release tree; Debug and CI were green. `tests/difftest/unicorn/CMakeLists.txt` sets the flag.
  If an oracle deviates in only one build type, suspect UB in the oracle first.

- `uc_mem_write` to an already translated code page invalidates the translation cache: the fuzz test writes
  1 million different words to the same address and gets the behavior of the new word every time.
- One engine per case (`uc_open` + mapping) costs roughly 1–2 ms; for ~5,000 cases that is fine (≈ 8 s with
  three regions). For millions of single instructions, reuse one engine and only reset registers/code
  (≈ 10 µs per word).
- Exceptions arrive as `UC_HOOK_INTR` with QEMU numbers: 1 = UNDEFINED, 2 = SVC, 7 = BRK. For SVC, PC already points
  past the instruction; for BRK and UNDEFINED it points at it.
- Unicorn is itself an emulation: a green test against Unicorn only proves agreement with QEMU. Where
  hardware fidelity matters (FP/NEON edge cases, exclusive monitor, microarchitecture quirks; `FRECPE`/`FRSQRTE`, on the other hand, are architecturally defined in ARMv8.0, see
  [a64-fp-simd-semantik.md](a64-fp-simd-semantik.md)),
  the hypervisor harness decides (ADR-0006, P2.7).
