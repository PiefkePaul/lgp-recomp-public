# A64 floating point and Advanced SIMD: semantics for lifter and WASM

Game-independent. Applies to every static recompiler that translates A64 code (ARMv8.0-A, e.g. the Switch's Cortex-A57) to
C++/WebAssembly. Implemented in P2.4: semantics in `recomp/lifter/include/lgp/lifted/fp.hpp` (FP core) and
`simd_*.hpp`, decoder/emitter in `recomp/lifter/src/simd/`. Checked against Unicorn (QEMU softfloat) in
`tests/lifter_simd/` and against the host FPU under `fesetround()` (`test_fp_soft.cpp`).

## Summary

1. **The host computes IEEE-754-exact, but not Arm-exact.** For normal numbers in rounding mode RN without
   flush-to-zero the results of `+ - * / sqrt fma` are bit-identical. Differences arise in **NaN results**,
   **FPCR.FZ**, **other rounding modes**, **float→int conversion** and the **estimate instructions**. That is exactly
   where the lifter needs its own code.
2. **Two paths per arithmetic operation:** fast path (FPCR.RMode = RN and FZ = 0) with host arithmetic and
   NaN correction afterwards. Otherwise an exact software path following the pseudocode of the Arm ARM
   (`FPUnpack`/`FPRound`). The test costs one well-predictable branch per operation.
3. **Pass values as bit patterns**, never as `float`/`double` across function boundaries. Then no host can silently
   alter a signaling NaN, and in WebAssembly NaN bits do not depend on the engine.
4. **Build without FMA contraction** (`-ffp-contract=off`). Clang on arm64 (macOS) and GCC in `gnu++` mode otherwise
   turn `a*b+c` into an FMA and change the rounding.
5. **FPSR exception flags (IOC, DZC, OFC, UFC, IXC, IDC) are not emulated**, QC (saturation) is. The A57
   has no FP traps; the flags are only visible via `MRS FPSR`. The differential tests mask them.

## Where host and Arm diverge

| Topic | Arm (AArch64) | x86-64 SSE | WebAssembly | Consequence for the lifter |
|---|---|---|---|---|
| Default NaN (invalid operation, `0/0`, `inf-inf`, `sqrt(-1)`) | `0x7FC00000` / `0x7FF8000000000000` (positive) | `0xFFC00000` ("real indefinite", negative) | any NaN (canonical or not, engine-dependent) | always build the NaN result ourselves |
| NaN propagation with 2 operands | first SNaN of op1, then SNaN op2, then QNaN op1, then QNaN op2 (`FPProcessNaNs`), each quieted | first NaN operand in the source register | unspecified | `nan2()` in Arm order |
| NaN propagation for FMA (`FMADD d = a + n*m`) | order **addend, op1, op2** (`FPProcessNaNs3`); special case: QNaN addend and `inf*0` yield default NaN | – | – | own special case, otherwise wrong NaN |
| FPCR.DN = 1 | every NaN result becomes default NaN | – | – | in `process_nan()` |
| Flush-to-zero | FZ: denormal **inputs** become 0, results become 0 if they are tiny **before** rounding | FTZ/DAZ: detection **after** rounding | no FTZ | soft path |
| Rounding modes | RN, RP, RM, RZ via FPCR; plus fixed in the instruction: TieAway (`FCVTA*`, `FRINTA`), round-to-odd (`FCVTXN`) | via MXCSR | **RN only** | soft path; WASM cannot do `fesetround` |
| Float→int (`FCVTZS` etc.) | saturates, NaN → 0 | `cvttss2si` yields `0x80000000` ("integer indefinite") | `i32.trunc_f32_s` **traps**; `trunc_sat` saturates and yields 0 for NaN (like Arm `FCVTZS`) | own exact integer routine; a C++ cast is UB outside the range |
| `max`/`min` | `FMAX` propagates NaN, `FMAXNM` prefers a number over QNaN; `max(+0,-0) = +0`, `min = -0` | `maxss` returns the second operand for NaN/±0 | `f32.max` propagates NaN, ±0 correct | own function |
| `FRECPE`/`FRSQRTE` | **architecturally defined** (8-bit table from `RecipEstimate()`/`RecipSqrtEstimate()`), not implementation-defined | `rcpps`/`rsqrtps` different values | – | reproduce the pseudocode exactly, never host estimate instructions |
| FMA | fused | `fma()` correct (software or FMA3) | **no** FMA in the core instruction set; `relaxed_madd` is not deterministic | `std::fma` (in WASM software, slow) |
| Half precision | ARMv8.0 only as a conversion format; FPCR.AHP switches to "alternative half" (no Inf/NaN, exponent 31 normal) | F16C without AHP | none | soft conversion |

## FZ and rounding in detail (pseudocode `FPRound`)

- Normalize the exact value to `1.f × 2^e`. If FZ is set (only single and double precision) and
  `e < minimum_exp` (−126 or −1022), the result is ±0. This holds even if rounding would have raised the value to the
  smallest normal number ("tininess before rounding").
- Otherwise: round denormal (`biased_exp = 0`) `minimum_exp − e` places further right. A rounded-up
  denormal value can become the smallest normal number (`int_mant == 2^F` → `biased_exp = 1`).
- Overflow: RN and TieAway → ±inf; RZ → ±max; RP → +inf or −max; RM the other way round; round-to-odd → ±max.
- Exact zero from an addition with different signs: +0, except in RM (−0). Two zeros with the same sign
  keep the sign.
- FZ also affects **comparisons** (`FCMP`, `FCMEQ`…), `FMAX`/`FMIN`, conversions, `FRINT*` and the estimate instructions,
  because all of them read via `FPUnpack`. **Not** affected are `FMOV`, `FABS`, `FNEG` and `FCSEL`: they only touch bits.
- In ARMv8.0 FZ does not affect half precision (FZ16 only comes with ARMv8.2).
- AArch64 Advanced SIMD fully obeys FPCR. The AArch32 "standard FPSCR" for NEON does not exist here.

## Individual instructions

- `FMSUB`/`FNMADD`/`FNMSUB`/`FMLS` negate operands **before** NaN processing (`FPNeg` also flips the
  sign of a NaN), `FNMUL` negates the **result** (even a default NaN then becomes `0xFFC00000`).
- `FMULX`: like `FMUL`, but `±inf × ±0 = ±2.0`.
- `FRECPS` = `2 − a·b` and `FRSQRTS` = `(3 − a·b) / 2`, both fused with **one** rounding (for `FRSQRTS` after the
  halving); `inf × 0` yields +2.0 or +1.5.
- `FRECPE`: inputs below 2^−128 (single) overflow to ±inf or ±max depending on the rounding mode; with FZ,
  results for inputs ≥ 2^126 become ±0.
- `FCVT` NaN conversion (`FPConvertNaN`): the sign stays, the quiet bit is set, the upper fraction bits
  are taken over or padded with zeros. With AHP a NaN becomes ±0, Inf becomes ±max (`0x7FFF`).
- `FRINT*`: a result of 0 keeps the sign of the input (`FRINTZ(−0.3) = −0.0`).
- `FCVTZS Wd, Sn`: saturates to `INT32_MIN/MAX`, NaN → 0; unsigned: negative values → 0.
- `FMOV Vd.D[1], Xn` writes only the upper half and **keeps** the lower; almost all other writes to a
  V register clear the bits above the written width.

## Decoding traps (ARMv8.0, Cortex-A57)

All confirmed with the Unicorn decoder fuzz (1 million random words, 0 deviations). The Unicorn decoder matches
the A57 feature set exactly: no RDM, no dot product, no FP16 arithmetic, `PMULL` with 64-bit elements present.

- **Not every reserved case is "size = 11 and Q = 0".** Integer across-lanes also reject size = 10 with Q = 0
  (only `4S`). Scalar shifts other than `SQSHL`/`UQSHL`/`SQSHLU` (immediate) exist only with 64-bit elements; the
  narrowing forms reject `immh = 1xxx`. Scalar `XTN`/`SHRN`/`RSHRN` does not exist.
- **By-element forms:** integer only with 16- and 32-bit elements; with 16 bits `M` becomes an index bit, `Rm` is then
  restricted to V0–V15. FP double with `L = 1` is reserved.
- **Size field as opcode:** for `NOT`/`RBIT`, `size` selects the operation (00 = `NOT`, 01 = `RBIT`), lanes are always
  bytes. In `FCVTN`/`FCVTXN`/`FCVTL`, `sz` selects the formats and `Q` the half; the usual rule "sz = 1 with Q = 0
  is reserved" does not apply there. Bit 23 = 1 there is `BFCVTN` (v8.6, reserved).
- **FP16 lives in its own classes** (e.g. bits 22:17 = 111100), which the classifier already rejects. Within
  shared classes separate checks are needed: `immh = 001x` (fixed point), element size 00 for by-element,
  `U = 0` for FP across-lanes and scalar FP pairwise.
- **Modified immediate:** the `cmode` groups determine form and shift amount; `o2 = 1` is FP16 `FMOV` and thus
  reserved. `immh = 0000` in the shift class is actually `MOVI`.
- **`UMOV`/`SMOV`:** the allowed sizes depend on `Q` (`UMOV Wd` up to 32 bits, `Xd` only 64 bits; `SMOV Wd` only
  8/16 bits). For `INS` (element) the low-order `imm4` bits below the element size are ignored.
- **Structure load/store:** `LDnR` requires `L = 1` and `S = 0`. For multiple structures, size = 11 with Q = 0 is only
  reserved for `selem > 1` (`LD1 {v.1d}` is valid).
- **Later extensions are UNDEFINED:** `SQRDMLAH`/`SQRDMLSH` (v8.1), `SDOT`/`UDOT`, `FMLAL`, `FCMLA`
  (v8.2/8.3), `FRINT32/64` (v8.5), `BFCVTN` (v8.6). A recompiler for the Switch must reject them, not
  implement them.

## Integer semantics that easily go wrong

- **Shift by register** (`SSHL`, `URSHL`, `SQRSHL` …): a single identity reproduces the pseudocode for all
  shift amounts −128…127 and all element sizes without wide types: `(x + 2^(s−1)) >> s == floor(x / 2^s) + bit(s−1)`,
  where a bit at or above `esize` is the sign bit (signed) or 0 (unsigned). `URSHL`/`URSHR` by exactly `−esize` yields
  the top bit, not 0. Rounding right shifts never saturate; saturating left shifts saturate from `esize` on for every
  element ≠ 0.
- **`SQDMLAL`/`SQDMLSL` saturate twice:** when doubling the product (only `MIN × MIN`) and when accumulating. QC
  is set if either of them saturates.
- **Without `__int128`:** 64-bit saturation (`SQADD`, `SUQADD`, `USQADD`) needs explicit overflow checks; everything
  else works in `int64`/`uint64` with unsigned wrap-around (UBSan-clean).
- **Crypto:** the SHA rotations correspond to word shifts in the 128-bit register; the AES S-box can be generated at
  compile time (no table needed in the source).

## FP vector details

- `FABD` = `FPAbs(FPSub)`: a propagated NaN loses its sign.
- `FRECPS`/`FRSQRTS` negate op1 **before** NaN processing, so a passed-through NaN has the flipped sign.
- Across-lanes order `op(op(e0,e1), op(e2,e3))` (Arm `Reduce()`, QEMU identical). It determines which NaN
  comes out.
- QEMU/Unicorn matched the Arm ARM in all checked cases (NaN propagation, FZ/DN/AHP, all
  rounding modes, round-to-odd in `FCVTXN`, FRECPE thresholds with FZ).

## Test methodology that has proven itself

- **Three independent checks:** (1) Unicorn differential test with random encodings, "interesting"
  register values (±0, denormals, limits, Inf, QNaN/SNaN with payload, numbers with short mantissa) and random
  FPCR (RMode, FZ, DN, AHP); (2) decoder fuzz against Unicorn: which words are UNDEFINED?; (3) soft path against the
  host FPU under `fesetround()` with millions of operands in all four modes (needs `-frounding-math`).
- **Do a mutation test.** First attempt: we deliberately replaced "ties to even" with "ties away", and the
  Unicorn test with random values stayed green. Real ties arise almost only with **short mantissas**, values
  near the denormal boundary only with **arbitrary exponents**. Only with these operand classes does the test fire.
- Unicorn is QEMU. For FP, QEMU's softfloat is very close to the Arm ARM; a green test nevertheless only proves equality with
  QEMU, not with the A57. Final hardware fidelity is checked by the hypervisor harness on the Mac (ADR-0006, P2.7).

## Performance (relevant for WASM)

- Fast path: one FPCR test plus one `r != r` test per operation. For vector instructions this costs per lane; the
  compiler can hoist the FPCR test out of the lane loop.
- In WebAssembly `std::fma` is a software routine (musl), so considerably slower than a hardware FMA. Code with
  many `FMADD`/`FMLA` (matrices, physics) is a candidate for profiling in WASM. `relaxed-simd` (`f32x4.relaxed_madd`)
  is no solution, because the result is fused or not fused depending on the engine.
- If a game runs permanently with FZ = 1, every FP operation takes the soft path. Then a dedicated FZ fast path is worthwhile:
  flush the inputs, compute singles in `double` (addition and multiplication are exact there, division is precise
  enough to decide "tiny before rounding", a square root never becomes tiny), flush the result.
