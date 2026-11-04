# Recompiler config file (addresses and names only, no code)

Cross-game knowledge from P2.2 (as of 2026-10-11). ADR-0004 requires one config file per game for everything that
the analysis does not find by itself. It contains **only addresses, numbers and symbol names** and may therefore go into the repo
(game-specific files later under `docs/analysis/` or `recomp/configs/`). Parser: `recomp/core/src/config.cpp`,
usage: `lgp_recomp functions <module…> --config <file>`.

## Format

Line-based, `#` starts a comment, numbers decimal or `0x…`. Addresses are **guest VAs after the link step**
(module base + offset; base selectable with `--base`, default `0x08000000` for the first module in load order).

```
# additional function start (assembler stub without FDE, thunk, misattribution)
function 0x0800f2a0

# callee never returns: symbol name (dynsym/import) or address
noreturn _ZN2nn4diag6detail17AbortImplEPKcS3_S3_i
noreturn 0x08123450

# jump table that the heuristic does not find or whose size it does not know
#   br=    address of the BR instruction
#   table= table start
#   entry= entry size 1, 2, 4 (relative) or 8 (absolute)
#   count= entries
#   base=  base for relative entries (default: table)
#   shift= target = base + (entry << shift); default by entry: 2 for entry=1/2, 0 for entry=4
#   signed | unsigned (default: signed)
jumptable br=0x080a1234 table=0x08c00010 entry=4 count=17
jumptable br=0x080b0000 table=0x08c01000 entry=1 count=9 base=0x080b0010 shift=2 unsigned

# toggle heuristics
scan_gaps on        # gap scan after padding (default on)
data_pointers on    # RELATIVE/ABS64 pointers from .data/.ro into .text as seeds (default on)
```

**`shift` default:** the way compilers lay out the tables. Byte and halfword tables (`ldrb/ldrh`, `-Oz`, GCC)
count in instructions (`add x, base, w, lsl #2` or `sxth #2`), hence `shift=2`. Word tables (`ldrsw x8, […];
add x8, x8, x9`, clang default) contain byte offsets, hence `shift=0`. Until P6 the default for all sizes was 2: a
line `entry=4` without `shift=` multiplied the offsets by 4, and the targets ended up before the module (seen on the PC, LGP). When
in doubt, always write out `shift=`.

**Target check:** if a target of an override is not in `.text` or is not 4-byte aligned, the
line is **not applied** (`jump_table_override_rejected`, the `br` then goes to the heuristic). If a target is in `.text`
but outside the function, the line is applied and `jump_table_override_outside_function` is reported (usually wrong
`count`, rarely a real tail jump). `lgp_recomp` and `lgp_lift` always print both, and `jump_table_override_unused`,
as a warning on stderr, even without `--diagnostics`.

Unknown directives and incomplete `jumptable` lines are errors, not silently ignored. A `jumptable`
override whose `br` is never reached produces the diagnostic `jump_table_override_unused` (usually: address shifted
because the base was chosen differently).

## Workflow

1. Run `lgp_recomp functions … --summary --diagnostics`.
2. Go through the diagnostics (table in [`function-discovery.md`](function-discovery.md)): `falls_into_next_function` →
   `noreturn`, `jump_table_unbounded`/`unresolved_indirect_jump` with a table pattern → `jumptable`, missing stubs →
   `function`.
3. Run again until the counters are stable. For a retail game the config grows to a few dozen lines,
   no more: with clang code that has FDEs, the analysis finds almost everything by itself.

## What deliberately does **not** belong in the config

- Instruction bytes, disassembly, function names from own reverse engineering with game content → would be derived
  game material. Own technical names (`fn_…`, `jt_…`) are fine.
- Module bases: the link step sets them, not the config (ADR-0001 may change them; the config then only has to be
  regenerated – which is why addresses are always documented together with the base used).
