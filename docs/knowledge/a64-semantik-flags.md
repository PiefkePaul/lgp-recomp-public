# A64 semantics: flags and edge cases for lifters

Game-independent. Everything here is confirmed by the differential tests in `tests/lifter/` against Unicorn (P2.3); the
implementation is in `recomp/lifter/include/lgp/lifted/helpers.hpp` and `recomp/lifter/src/lifter.cpp`.

## NZCV

- **Addition/subtraction** all go through `AddWithCarry(x, y, carry_in)`:
  - `ADDS a, b` = `AWC(a, b, 0)`, `SUBS a, b` = `AWC(a, ~b, 1)`, `ADCS` = `AWC(a, b, C)`, `SBCS` = `AWC(a, ~b, C)`.
  - `C` is the **unsigned carry** of `a + y + carry_in`, so for subtraction it is **NOT borrow** (C = 1 if there is no
    borrow, i.e. `a >= b` unsigned). The x86 habit (CF = borrow) is exactly the opposite.
  - `V` = signed overflow = `(~(a ^ y) & (a ^ r))` in the sign bit, with `y` **after** the inversion.
  - Edge case that breaks naive implementations: `carry_in = 1` and `y = ~0` (e.g. `SUBS x, 0`). Then
    `r == a` and C must be 1; the test `r < a` alone yields 0. Correct: `carry_in ? r <= a : r < a`.
  - 32-bit variants compute flags on 32 bits (C from bit 32 of the 64-bit sum, N from bit 31).
- **Logical operations with S** (`ANDS`, `BICS`, hence `TST`): N and Z from the result, **C and V become 0**
  (not "unchanged", and not the shifter carry as in AArch32 either).
- **CCMP/CCMN:** condition true → flags from the `SUBS`/`ADDS` of the comparison; false → flags = the 4-bit immediate
  `nzcv`. The immediate format (`#imm5`) sits in the Rm field.
- **MSR NZCV** writes bits 31..28 of the register, the other bits are ignored; **MRS NZCV** returns only
  these four bits, the rest 0.

## Conditions

`EQ Z` · `CS C` · `MI N` · `VS V` · `HI C && !Z` · `GE N == V` · `GT !Z && N == V` · `AL` — each negated when
bit 0 is set (`NE`, `CC`, `PL`, `VC`, `LS`, `LT`, `LE`). **`NV` (1111) also means "always" in A64**, not
"never" as formerly in ARM.

## Arithmetic without traps

- `UDIV/SDIV` by 0 yields 0, `SDIV INT_MIN, -1` yields `INT_MIN`. In C/C++ both cases are UB or trap on
  x86 – the lifter needs its own helpers.
- Variable shifts (`LSLV/LSRV/ASRV/RORV`) take the amount **modulo register width** (`& 63` or `& 31`). In C++
  a shift by ≥ width is UB, so always mask.
- `ASR` in C++: right shift of negative values is defined as arithmetic since C++20, but still cast explicitly via `int64_t`.
- 32-bit results are **always** written zero-extended into the whole X register (also for SP with
  `ADD WSP, …`).
- `SMULH/UMULH`: upper 64 bits of the 128-bit product. Portable without `__int128`: from the unsigned variant; for signed,
  subtract the other factor for each negative factor.
- `CLS`: number of leading bits equal to the sign bit, **without** the sign bit: `clz((x ^ (x << 1)) | 1)`.
  `CLS(0) = CLS(-1) = 63`.
- `CRC32*` without pre/post inversion, reflected polynomials `0xEDB88320` (CRC32) or `0x82F63B78` (CRC32C), the
  B/H/W/X variants process 1/2/4/8 bytes of Rm.

## Bitfields

`UBFM/SBFM/BFM` with masks per the pseudocode (`DecodeBitMasks(N, imms, immr, immediate = FALSE)` yields `wmask` and
`tmask`):

```
bot = (dst & ~wmask) | (ROR(src, R) & wmask)     // dst = 0 for SBFM/UBFM
top = SBFM ? Replicate(src<S>) : dst
res = (top & ~tmask) | (bot & tmask)
```

The aliases (`LSL/LSR/ASR #imm`, `UBFX/SBFX`, `BFI/BFXIL`, `UXTB/SXTH/SXTW`) need no special cases if the
masks are computed as constants at lift time; the C++ compiler folds the rest.

## Loads/stores

- Sign extension depends on the **destination width**: `LDRSB W` extends to 32 bits and then zero-extends into the X register;
  `LDRSB X` extends to 64 bits.
- SIMD&FP loads of B/H/S/D clear the rest of the 128-bit register.
- Address calculation first, then load, then writeback. For `LDP x1, x2, [x1]` (without writeback, legal) the address must
  be fixed before the first load.
- Register offset: `UXTW` takes the lower 32 bits **unsigned**, `SXTW` signed; a small negative index only makes sense with
  `SXTW`/`SXTX`/`LSL`.
- `DC ZVA` zeroes an aligned block of the size from `DCZID_EL0` (A57: 64 bytes); the address is aligned
  down, not checked.

## Exclusive accesses

Local monitor as compare-and-swap: `LDXR` remembers address, size and the value read; `STXR` succeeds only if
the monitor is valid, address and size match and memory still holds the remembered value (CAS). Afterwards the
monitor is always invalid; `CLREX` invalidates it as well. Status 0 = success, 1 = failure. An `STXR` without
a preceding `LDXR` fails and writes nothing. This is the same approximation as in QEMU (the ABA problem is
accepted); for byte/halfword a 32-bit CAS on the surrounding aligned word is enough, because exclusive accesses
must be aligned.

## FPCR/FPSR (registers only, FP arithmetic is P2.4)

On the A57 (without FP exception traps) the writable FPCR fields are: AHP, DN, FZ, RMode and the AArch32 fields
Stride/Len (no effect in AArch64, but readable: mask `0x07F70000`). The trap enable bits are RAZ/WI. FPSR: N Z C V
QC and the cumulative flags (`0xF800009F`).
