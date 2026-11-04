# Function boundaries from `.eh_frame_hdr` / `.eh_frame`

Cross-game knowledge from P2.2 (status 2026-10-11). Complements the Ghidra section in
[`ghidra-nso-headless.md`](ghidra-nso-headless.md) with what a custom parser needs to know.
Implementation: `recomp/core/src/eh_frame.cpp`.

## Why this is the best source

clang (NintendoSDK) and GCC (devkitA64) on AArch64 generate an FDE for every function with a prologue, also in C and also
without exceptions (`-funwind-tables` is the default). The FDE provides **start and length**, not just the start. In a
large SDK game 97 % of all functions lay exactly on an FDE; without an FDE remained PLT thunks and hand-written
assembler stubs without `.cfi_startproc`. Everything else (symbols, BL targets, data pointers, gap scan) is a supplement.

## `.eh_frame_hdr` (version 1)

```
u8 version = 1
u8 eh_frame_ptr_enc      // typically 0x1B = pcrel | sdata4
u8 fde_count_enc         // typically 0x03 = udata4
u8 table_enc             // typically 0x3B = datarel | sdata4
<eh_frame_ptr>  <fde_count>
Table: fde_count × (initial_location, fde_address), sorted by initial_location
```

`datarel` refers to the **start of `.eh_frame_hdr`**, `pcrel` to the address of the field itself. With the table
one has all function starts without walking `.eh_frame`. The length needs the FDE.

## Reading FDE/CIE

- Each entry: `u32 length` (0xFFFFFFFF → u64 length, does not occur in practice), `u32 cie_pointer`
  (0 = CIE, otherwise distance **backwards** from the field to the CIE).
- CIE: version (1 or 3), augmentation string; with `z` the augmentation length follows. For the boundaries only
  `R` (pointer encoding of the FDE addresses, typically `0x1B`) matters; `L` (LSDA encoding) and `P` (personality) are just skipped.
  Order of the augmentation data = order of the letters in the string.
- FDE: `pc_begin` (with CIE encoding `R`, `pcrel` relative to the field), `pc_range` (same format, but **always absolute**,
  do not apply pcrel), then augmentation length and LSDA.
- End of the function = `pc_begin + pc_range`. This is also the best source for **padding**: between the FDE end and
  the next start there are NOPs (`0xD503201F`) or zeros.
- An FDE with LSDA ≠ 0 means: the function has C++ landing pads. A quick test of whether a module really uses exceptions.

## What can be derived from FDE boundaries

1. **noreturn callees without names.** If a `BL` is the last instruction of an FDE range, the callee never returns
   (otherwise a `RET` would have to follow). This finds `abort`, `exit`, `__assert_func` and SDK abort variants even without
   symbol names. The same argument holds for a `BL` immediately before another strong function start (symbol,
   FDE, BL target): the compiler never lets code run into the neighboring function.
2. **Tail call vs. internal branch.** A `B` to an address outside its own FDE range is a tail call;
   inside it is a branch. Without FDEs, `b <next instruction of another function>` cannot be distinguished from an
   internal branch if the target function has no seed of its own.
3. **Data pointers into the inside of a function** (RELATIVE relocation from `.data`/`.ro` to an address strictly inside
   an FDE range) are not function starts but e.g. landing pads or jump table targets. Ignore them as seeds,
   otherwise real functions get split.
4. **Unreachable code** between the last reachable block and the FDE end is a hint of an undetected
   jump table or a `BR` whose targets are missing.

## Limits of the method

- Hand-written assembler without `.cfi_startproc` has no FDE (libnx: `crt0`, parts of the SVC wrappers). These
  functions only come via symbols, `BL` targets or the gap scan.
- Several labels in **one** `.cfi_startproc` range (a symbol in the middle of the FDE) are one function from the FDE's point of view.
  In the comparison with ELF symbols such a case counts as "missing", although the code is covered correctly.
- `pc_range` can slightly overshoot the function (alignment padding at the end belongs to the FDE). Padding at the end of a function is
  therefore normal and not an error.
- An `.eh_frame` without `.eh_frame_hdr` (MOD0 fields equal) must be walked linearly; then resolve the CIE mapping via
  the backwards `cie_pointer` and sort the starts yourself.
