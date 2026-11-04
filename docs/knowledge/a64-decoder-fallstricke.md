# A64 decoder: pitfalls

Game-independent knowledge from building the decoder `recomp/lifter/src/a64_decode.cpp` (P2.3). Applies to any
AArch64 recompiler, emulator or disassembler that must decode ARMv8.0-A bit-exactly. The source of truth is
the Arm ARM (DDI 0487), chapters C4 (encodings) and C6 (pseudocode of the instructions).

## Register 31 is sometimes SP, sometimes XZR

Field `11111` means something different depending on the instruction **and operand position**. The most common source of errors.

| Instruction | Rd | Rn | Rm |
|---|---|---|---|
| ADD/SUB (immediate), without S | SP | SP | – |
| ADDS/SUBS (immediate), thus also CMP/CMN | **XZR** | SP | – |
| ADD/SUB (extended register), without S | SP | SP | XZR |
| ADDS/SUBS (extended register) | XZR | SP | XZR |
| ADD/SUB (shifted register), all | XZR | XZR | XZR |
| AND/ORR/EOR (immediate), without S | SP | XZR | – |
| ANDS (immediate), thus TST | XZR | XZR | – |
| Logical (shifted register) | XZR | XZR | XZR |
| Loads/stores: base register | – | SP | XZR (index) |
| Loads/stores: data register Rt/Rt2 | XZR | – | – |

That is why the decoder sets `rd_sp`/`rn_sp` explicitly instead of leaving it to the emitter. `MOV SP, X0` is in
truth `ADD SP, X0, #0`; `MOV X0, XZR` is `ORR X0, XZR, XZR`.

## Logical immediates (DecodeBitMasks)

- `N:imms` determines the element size: highest set bit of `N:NOT(imms)` (7 bits). Result < 1 → reserved.
- `imms` with all ones within the element size → reserved (an element consisting only of ones cannot be encoded;
  `0` and `~0` are therefore not logical immediates).
- 32-bit form (`sf = 0`) with `N = 1` → unallocated.
- For **bitfield** instructions (`SBFM`/`BFM`/`UBFM`) the same function is used with `immediate = FALSE`: no
  reserved check, and there is a second mask `tmask` from `S - R`. The element size there is always the
  register width, because `N == sf` is required.

## What is unallocated (UNDEFINED on the A57)

- `MOVZ/MOVN/MOVK` with `opc = 01`; 32-bit with `hw >= 2`.
- `SBFM/BFM/UBFM` with `N != sf` or 32-bit with `immr`/`imms >= 32`. `EXTR` likewise, plus `o0 = 1`.
- ADD/SUB (shifted register) with shift `ROR` (`11`); 32-bit with shift amount ≥ 32. ADD/SUB (extended) with
  `imm3 > 4` or `opt != 00`.
- `LDR (literal)` with `V = 1, opc = 11`; `LDP/STP` with `opc = 11`; `LDPSW` does not exist as a non-temporal form
  (`LDNP` with `opc = 01`), and `opc = 01` with `L = 0` is `STGP` (MTE, v8.5).
- `PRFM` only exists with unsigned offset, unscaled (`PRFUM`), register offset and literal, **not** with
  pre/post-index. The unprivileged forms (`LDTR` …) do not exist for SIMD&FP and not as `PRFM`.
- Register-offset loads with `option<1> = 0` (i.e. UXTB/UXTH/SXTB/SXTH) are unallocated.
- Everything from later versions: LSE atomics (`LDADD`, `CAS`, `CASP`, `SWP`), PAC branches (`BRAA`, `RETAA`) and
  PAC loads (`LDRAA`), `RMIF`/`SETF` (v8.4), MTE, `LDLAR`/`STLLR` (v8.1 LOR), `SB`.
- **Exception hint space:** PAC hints such as `PACIASP`/`AUTIASP` and `BTI` lie in the hint space (`HINT #imm`) and run
  as **NOP** on ARMv8.0. A decoder must accept the entire hint space as NOP.

## UNDEFINED only in EL0

Game code runs in EL0. The following instructions exist but trap there: `HLT` (as long as halting debug is off),
`HVC`, `SMC`, `ERET`, `DRPS`, `DCPSx`, `MSR` (immediate) for almost all PSTATE fields, `DC IVAC` (only DC ZVA, CVAC,
CVAU, CIVAC and IC IVAU are allowed from EL0) and all EL1 system registers (`MIDR_EL1` etc.). Readable from EL0 are
among others `NZCV`, `FPCR`, `FPSR`, `TPIDR_EL0`, `TPIDRRO_EL0` (read only), `CTR_EL0`, `DCZID_EL0`, `CNTFRQ_EL0`,
`CNTVCT_EL0`/`CNTPCT_EL0` (if the kernel allows it; on Horizon it does). The decoder marks such cases as
`Invalid::Privileged`, so that an exception list against EL1 oracles (Unicorn) stays clean.

## CONSTRAINED UNPREDICTABLE

These combinations do not occur in compiler output; the decoder rejects them (`Invalid::Unpredictable`) instead of
committing to arbitrary behavior:

- Writeback with `Rn == Rt` (or `Rt2`) for GPR loads/stores, except `Rn = 31`.
- `LDP` (also `LDXP`) with `Rt == Rt2`.
- `STXR`/`STXP` with `Rs == Rt`/`Rt2` or `Rs == Rn` (except `Rn = 31`).
- "Should be one" fields violated: `Rt2 != 31` for the single exclusive accesses, `Rs != 31` for load-exclusive and
  `LDAR`/`STLR`, `Ra != 31` for `SMULH`/`UMULH`. QEMU ignores these fields, the decoder does not.

## Miscellaneous

- `RET` without an operand is `RET X30`; `RET Xn` with another register is legal.
- `TBZ/TBNZ`: the bit number is `b5:b40`, `b5` is at the same time `sf` (register width only relevant for display).
- `ADRP`: the base is `PC` with the lower 12 bits cleared, the immediate is 21 bits **times 4096**, signed.
- `CRC32X`/`CRC32CX` require `sf = 1`, the B/H/W forms `sf = 0`; CRC32 is optional in v8.0, the A57 has it.
- `REV32` only exists in 64-bit; the 32-bit encoding with `opc = 10` is `REV` (32 bits).
- Unprivileged loads/stores (`LDTR`, `STTR`) behave in EL0 exactly like the normal forms.
- `SMULH`/`UMULH`: `Ra` (bits 14:10) is "should be one". QEMU ignores the field; the decoder rejects `Ra != 31` as
  `Invalid::Unpredictable`, like the other "should be one" cases (note from the code review of P2.3).

## How to find gaps

A fuzz test against an oracle finds missing and wrongly accepted encodings faster than any table:
`tests/lifter/test_decode_validity.cpp` executes random 32-bit words one at a time in Unicorn and compares
"UNDEFINED" with `Op::Invalid`. If the decoder accepts a word that the CPU rejects, that is a bug; if it rejects
one that the CPU executes, the reason (`Invalid::*`) must be explainable. Status P2.3: 1 million words, 0
unexplained deviations.
