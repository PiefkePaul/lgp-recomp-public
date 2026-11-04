# Detecting jump tables on AArch64

Cross-game knowledge from P2.2 (as of 2026-10-11). Game-specific numbers (how many `BR`, which patterns) are
in `docs/analysis/04-code-statistik.md`. Implementation: `recomp/core/src/jump_table.cpp` (symbolic evaluation),
`jt_decode.cpp` (mini decoder for the instructions involved).

## The patterns

**clang (NintendoSDK, standard case for retail games):** table of s32 offsets relative to the table start.

```
cmp   w8, #N            ; check index
b.hi  default           ; (or b.ls onward)
adrp  x9, table@PAGE
add   x9, x9, table@PAGEOFF
ldrsw x10, [x9, w8, uxtw #2]
add   x10, x10, x9      ; target = table + entry   (base == table!)
br    x10
```

Variants: index as `x8` instead of `w8` (then `lsl #2` instead of `uxtw #2`), `ldrsw` + `adr` instead of `adrp/add`, and with
`-Oz` byte or halfword tables with `ldrb/ldrh` + `add x10, x9, x10, lsl #2` (entries counted in instructions).

**GCC (devkitA64, libnx homebrew):** table of s16 or s8 entries in instruction words; the base is a label
(`.Lrtx`), not the table:

```
adrp  x1, .L4
add   x1, x1, :lo12:.L4
ldrh  w0, [x1, w0, uxtw #1]
adr   x1, .Lrtx4
add   x0, x1, w0, sxth #2
br    x0
```

**Absolute tables** (8-byte pointers, in `.data.rel.ro`, with RELATIVE relocations): `ldr x0, [x1, x0, lsl #3]; br x0`.
Occur in `-fno-pic` code and in hand-written dispatch tables; after the link step the entries are
finished addresses.

**Not a jump table pattern, but looks like one:**

- PLT stub: `adrp x16, got@PAGE; ldr x17, [x16, #got@PAGEOFF]; add x16, …; br x17`. One target, via a GOT slot. Report it as a
  tail call with a known slot, not as a table.
- Tail call via a loaded function pointer (`ldr x8, [x0, #0x10]; br x8`): a real indirect jump, no index.
- `br` on a register that comes directly from an `adr`/literal pool: direct target.

## Detection: trace backwards or evaluate forwards?

Searching for the definitions backwards from `BR` sounds obvious, but breaks at block boundaries and with reordered
instructions (clang likes to place `adrp/add` far before the `cmp`). More robust: **evaluate symbolically forwards** from the
function start along the chain of blocks with exactly one predecessor (up to ~6 blocks), with a small
expression tree per register: `Const`, `Reg` (unknown input), `Add`, `Shl`, `Ext(sxtb/sxth/sxtw/uxt*)`,
`Load(size, signed, addr)`. All other instructions conservatively kill their destination register; a `BL` kills `x0–x18`
and `x30`. **"Kill" means: a new, opaque leaf, not a shared "Unknown" that makes every computation with it
unknown itself** (see below, experience with a real game). At the `BR` the expression must have one of these forms:

```
Add(Const base, Shl(Ext(Load(size, Add(Const table, Shl(Reg idx, log2 size)))), shift))   → relative
Load(8, Add(Const table, Shl(Reg idx, 3)))                                                 → absolute
Const                                                                                      → direct target
Load(8, Const slot)                                                                        → GOT slot (PLT)
```

This single match covers all the compiler patterns above, including the base variants (base == table for
clang, base == label for GCC) and the scalings (`uxtw #2`, `lsl #2`, `sxth #2`).

## Finding the upper bound

The number of entries is never at the table, but in the **`cmp idx, #N`** before it plus the condition of the following
`B.cond`. Along the predecessor chain, remember the last `cmp` on the index expression:

| Condition towards the table | Entries |
|---|---|
| `LS`, `LE` (idx ≤ N) | N + 1 |
| `CC`/`LO`, `LT` (idx < N) | N |
| inverted (`HI`/`GT`/`CS`/`GE` jumps **away**, fallthrough leads to the table) | as above, invert the condition |

**Range check with offset** (`case 3 … case 6`): the compiler subtracts the smallest case. Three forms occur;
the analysis recognizes all three:

| Code | Table index | Entries |
|---|---|---|
| `sub w8, w0, #3; cmp w8, #3; b.hi` and load with `x8` | `v - 3` | 4 (check directly on the index) |
| `sub w8, w0, #3; cmp w8, #3; b.hi` and load with `w0`, table address moved forward by 3 entries | `v` | 4 from `table + 3·size` |
| `cmp w0, #6; b.hi` (smaller cases handled before), then `sub w8, w0, #3` | `v - 3` | 7 − 3 = 4 |

The last two only apply to **unsigned** conditions (`HI`/`LS`/`LO`/`HS`): there `v - k < N` also implies
`v ≥ k`. With `LE`/`LT` the index must be exactly the compared expression.

Without a `cmp` (rare, e.g. when the index comes from an enum the compiler knows to be complete) only
scanning remains: read entries until a target is no longer in `.text`, not aligned or outside the function;
set an upper bound (here 4096) and mark the result as **low confidence**. Such tables belong in the
config file (`jumptable br=… count=…`) so that the output is reproducible.

## Pitfalls from practice

- The index register is usually `w` width: the `uxtw` is part of the address computation, not a separate step. Anyone who
  "normalizes" the expression (`Ext(Uxtw, Reg)` = `Reg`) will otherwise not recognize the clang form.
- `ldrsw` with halfword tables does not exist; `ldrh` + `sxth` only with GCC. Anyone who only checks `ldrsw` misses all
  GCC tables.
- Table targets can lie **outside** the previously known function end (blocks after the `br`); the targets
  must therefore be added as new blocks to the same function, not as new functions.
- A table target can point to a weak seed (data pointer, gap scan). Then the seed was wrong: retract it
  and re-evaluate the function, otherwise a "function" appears in the middle of the `switch`.
- Two `switch`es in one function often share `adrp` to the same page; the tables differ only in the
  `add …, :lo12:`. Do not stop computing at the page start.
- `-Oz` code uses byte tables with `shift 2`; entries are then **instruction distances**, not byte offsets.

## Experience with a real game `main` (NintendoSDK clang, 3.3 million instructions)

Counter-check with Ghidra's Switch analysis (numbers in `docs/analysis/06-recomp-gegen-main.md`). Two bugs cost almost
half of all tables. In synthetic tests both stayed invisible, because there the index always came cleanly from an
argument register:

- **An unknown index must not hide the table.** In real code the index often comes from a field of an
  object that was itself loaded via a computed address (`ldr x0, [x20, x21, lsl #3]; ldr w8, [x0, #0x68]`).
  If an unmodeled value is "Unknown" and `Add(Unknown, …)` again yields "Unknown", the constant
  table address disappears with it. Solution: treat unknown values as separate leaves (equal only to themselves) and
  keep computing with them. The pattern only requires that **table and base** are constant; the index may be anything, and
  matching against the `cmp` still works via object identity.
- **Query the end of the worklist cleanly.** Anyone who resolves collected `BR`s only "when the worklist is empty" must check that at the
  top of the loop, not only after building a new block. If the last work item is an already known
  block, the loop otherwise ends silently with unresolved `BR`s, without a diagnosis, recognizable only by the missing code.
- **Table base before the loop header (solved):** the base is in a register (`x12` in a leaf function,
  `x23`/`x26` callee-saved), loaded via `adrp/add` before a loop. The loop header has two predecessors; the
  one-predecessor chain never sees the `adrp`. Solution without a full data flow analysis: if the expression does not match, its
  register leaves are replaced by **function constants**. A register is one if the whole range of the function
  (linear, not only the blocks found so far, because code behind the table itself is still unknown) writes it exactly
  once via `adrp` (+ directly following `add r, r, #imm`) before the `br`. Counted as writes are calls
  (x0–x18, x30), SVCs (x0–x7) and every unmodeled instruction with the register as Rt/Rd; not, however, stores
  and SP-based loads into x19–x29 (epilogue restores). For LGP: `main` 1,569 tables (previously 1,452 of Ghidra's 1,541),
  plus `strzcmp16` in `sdk`.
- **FP code between check and jump (solved, P6, Route 1 in LGP):** `sub w8, w8, #3; cmp w8, #3; b.hi`, then
  `adrp/add x9`, then FP instructions and `smaddl`, only after that `ldrsw x8, [x9, x8, lsl #2]; add x8, x8, x9; br x8`. The
  evaluation treated every unmodeled instruction as a write of its Rd/Rt field. With `fmul d8, …`,
  `ldr s9, […]` or `str d8, […]`, however, the field is a **V register**; a store writes no register at all. So
  `x8`/`x9` were lost, the `br` stayed unresolved, and at runtime the game crashed on a never-lifted `case`.
  Rules since then (`invalidate_other` in `jump_table.cpp`): SIMD&FP data processing writes an X register only for
  FP→integer conversion (`fcvtzs`, `fmov w/x, s/d`) and `umov`/`smov`; SIMD&FP loads write V registers;
  stores write nothing except a written-back base; `fcmp`/`fccmp`, `adds`/`cmn`, `ands`/`tst`, on the other hand, end
  a remembered range check (previously it stayed valid across them). `smaddl` and other
  unmodeled integer instructions still cost only their destination register.
  Counter-check on the PC (`main`+`sdk`): the Route 1 table and 8 more bounds new, but an `sdk` table with its base
  in `x25` before the loop was lost. Cause: the first attempt also killed, across the board for exclusive/literal loads, the
  fields "Rs" (bits 20:16) and "Rt2" (bits 14:10). In `ldr w8, <literal>` these are bits of imm19; if they happened to be
  25, `x25` was "overwritten". Now only the fields the encoding really uses as registers (status register
  of `stxr`, `cas`, `ldxp`), test `jump_table_base_survives_literal_load_in_the_loop`. Lesson: a conservative
  "kill all possible register fields" is not conservative for fields that are immediate bits in other encodings,
  but random. Until P6, `jt::decode` never recognized `LDR (literal)` (the condition checked bit 28
  contradictorily); literal loads ran as unmodeled; fixed since then, `ldr x16, <literal>; br x16` is a
  direct tail call (test `br_through_literal_pool_is_a_direct_tail_call`). In the game (LGP `main`+`sdk`) this changes
  nothing: 0 tables and 0 unresolved `br` more or less, smoke test bit-identical. The compiler apparently does not generate
  `ldr xN, <literal>; br xN` there; the fix is pure correctness for other code.
- **State machine with a constant initial state (solved, P6, `sdk`):** the actual reason for the lost
  `sdk` table (found with `LGP_JT_TRACE`): `mov w24, #13` before a loop, inside it `switch (w24)`, the cases set
  a new state and jump back. As long as the table is not resolved, the analysis does not know the cases;
  the loop header seemingly has only the entry path as predecessor, the chain runs up to the `mov`, and the index
  becomes a constant. With that, `ldrsw` folds to a fixed entry, and the pattern no longer matches. That it worked before
  was chance: an FP instruction with Rd = 24 had wrongly cleared `x24`. Solution: if the pattern does not match, the
  chain is evaluated a second time, with `movz`/`movn` yielding opaque values (table addresses come from
  `adrp`/`adr` and stay constant). Test `jump_table_state_machine_with_constant_initial_state`. Open, same
  cause: with an absolute table (`ldr x8, [x9, x24, lsl #3]; br x8`) the constant index would become a
  "literal pool" direct target; only a table of this kind in the game will check that.
- **Tables of function pointers** (`ldr x2, [x9, x8, lsl #3]; br x2`, entries are starts of other functions) are
  not `switch` tables, but indirect tail calls with a known target set. Translating them as a `switch` of tail calls with
  dispatch fallback is correct.
- **Trap fill words:** NintendoSDK clang places the word `0xe7ffdefe` (in A64
  "unallocated") after calls that do not return. Anyone who does not know the call as noreturn runs into it and reports "undecodable instruction". That is
  not a missing opcode, but a good hint to mark the previous call as noreturn.
- **Table base loaded before a loop (resolved since P3.4, see above):** GCC hoists the table's `adrp/add` out of loops. In newlib's
  `_svfprintf_r` (format `switch` in the parser loop) the base is in `x26`, set before the loop; the `br` is in the
  loop header with several predecessors. The forward evaluation along the one-predecessor chain therefore does not find the base,
  the `br` stays a jump via the dispatch table and at runtime lands on an unknown address
  in the middle of the function (`kHaltBadJump`). Seen with the services fixture (P3.4, `snprintf`); the fixture therefore uses
  no `printf`. Workaround today: a `jumptable` line in the recompiler config ([`recompiler-config.md`](recompiler-config.md))
  or `function` entries for the targets (an indirect jump is a tail call; lifting the target as its own function
  is semantically correct, only slower). Proper solution: a reaching-definitions analysis over all
  predecessors that takes over the base when all paths deliver the same value. Whether clang code (retail games) also has the pattern
  will only be shown by the list of unresolved targets from the boot test.
