# Analyzing Switch NSOs in Ghidra (headless)

Knowledge across games from P1.5 (status 2026-10-11, Ghidra 12.1.4, Java 21+). Applies to every ExeFS module
(`main`, `sdk`, `subsdk*`, `rtld`) and to NROs. Game-specific numbers are in `docs/analysis/`, not here.

## No NSO loader needed: NSO → ELF

Ghidra ships no NSO/NRO loader; the community extension (Ghidra-Switch-Loader) often lags behind new Ghidra versions.
Simpler and independent of the version: pack the decompressed module image into a **section-less ELF64**.

- The NSO image already contains a complete `.dynamic` (found via the `MOD0` header at the start of `.text`),
  including `DT_SYMTAB`/`DT_STRTAB`/`DT_HASH`/`DT_GNU_HASH`, `DT_RELA` and `DT_JMPREL`.
- Program headers are enough: `PT_LOAD` per segment (`.text` R-X, `.ro` R--, `.data` RW- with `.bss` in `p_memsz`),
  `PT_DYNAMIC` on the `.dynamic` from MOD0, optionally `PT_GNU_EH_FRAME` on `.eh_frame_hdr`. `ET_DYN`, `EM_AARCH64` (183),
  base 0, `p_vaddr` = module offset.
- Ghidra's ElfLoader reads the dynamic symbols from it, applies all relocations (RELATIVE, ABS64, GLOB_DAT,
  JUMP_SLOT) and creates PLT thunks. With `ET_DYN` Ghidra places the image at **0x100000** by default, so always compute
  addresses relative to the image base (`currentProgram.getImageBase()`).
- Imports from other modules stay open as `<EXTERNAL>` functions ("no external libraries configured"); that is
  fine for single-module statistics.
- Implementation: `tools/analyze/nso2elf.py` (uses the LZ4 decoder from `nso_inventory.py`, no dependencies).

## Function boundaries from `.eh_frame`

SDK-built modules (clang, NintendoSDK) have an FDE for practically every function. `.eh_frame_hdr` (via MOD0) provides
a sorted table (encoding `0x3B` = datarel|sdata4, relative to the start of the header), the FDEs themselves start and
length (pointer encoding from the CIE augmentation `R`, typically `0x1B` = pcrel|sdata4). Experience from a large
game `main`: all FDE starts are real functions, and apart from PLT/forwarding thunks there are hardly any functions
without an FDE.

- FDEs with LSDA (CIE augmentation `L`, pointer ≠ 0) = functions with C++ landing pads. A good quick test of whether
  exceptions play a role at all.
- Procedure in Ghidra: a pre-script creates a function at every FDE start **before** the auto-analysis runs
  (`tools/analyze/ghidra/LgpSeedFunctions.java`). This makes function detection deterministic.

## Headless invocation and run time

```
analyzeHeadless <project-folder> <project-name> -import mod.elf -overwrite -loader ElfLoader \
    -scriptPath tools/analyze/ghidra -preScript LgpSeedFunctions.java mod.meta.json \
    -postScript LgpCodeStats.java mod.meta.json mod.stats.json -max-cpu 14
```

- Heap via `GHIDRA_HEADLESS_MAXMEM` (default only 2 GB). 16 GB are plenty for a 13 MiB `.text`.
- Run time of the auto-analysis incl. "Decompiler Switch Analysis" for 3.3 million instructions: ~5 minutes on 14 cores
  (biggest items: Basic Constant Reference Analyzer, Stack, Decompiler Switch Analysis). Smaller modules 1–3 minutes.
- One project per module, then several `analyzeHeadless` processes can run in parallel (a project is locked).
- Java GhidraScripts are compiled on the fly; `com.google.gson` is on the classpath. For JSON output set
  `disableHtmlEscaping()`, otherwise `<=` becomes `<=`.
- Harmless log messages: `VarnodeContext: out of address spaces` (constant propagation gives up on individual
  functions), `Unable to disassemble EXTERNAL block location` (decompiler on import thunks).
- Windows: Python output with `≠` and the like fails on the cp1252 console; write Markdown directly as UTF-8.

## Code patterns of NintendoSDK clang (AArch64, as of SDK 5.x)

- **Jump tables:** uniformly `ldrsw xI, [xT, xIdx, lsl #2]` → `add xT, xT, xI` → `br xT` (32-bit entries relative
  to the table start). Ghidra's switch analysis recognizes most, but not all; an own recognizer for exactly this
  pattern is simple.
- **PLT stubs:** `adrp x16` / `ldr x17, [x16, #..]` / `add x16, ..` / `br x17`, one stub per `JUMP_SLOT` relocation.
- **Virtual calls:** `ldr xV, [xObj]` → `ldr xF, [xV, #slot]` → `blr xF`; at the end of a function as `br xF` (tail call).
- **Local statics:** `ldarb` on the guard byte, otherwise `__cxa_guard_acquire`/`__cxa_guard_release`.
- **ISA:** pure ARMv8.0 (no LSE atomics, no PAC). Atomics as `ldaxr`/`stlxr` loops plus `ldar`/`stlr`.
- **System registers:** game code reads at most `tpidr_el0`; `tpidrro_el0` (thread pointer of the Horizon TLS),
  `cntpct_el0`, FPCR/FPSR and all `svc` are in the `sdk` module. Whoever replaces `sdk` at the API level hardly has to
  handle system registers in game code.
- `setjmp`/`longjmp` occur as imports from `sdk` (libc), typically from embedded libraries
  (e.g. image formats, script VMs).

## Statistics tools in the repo

| File | Purpose |
|---|---|
| `tools/analyze/nso2elf.py` | NSO/NRO → ELF + FDE table (JSON) |
| `tools/analyze/ghidra/LgpSeedFunctions.java` | Pre-script: functions at FDE starts |
| `tools/analyze/ghidra/LgpCodeStats.java` | Post-script: functions, instruction mix, BR/BLR/RET classes, jump tables, address-taken functions, import calls → JSON |
| `tools/analyze/ghidra/run_code_stats.py` | Driver for all modules (writes only to `--work`, refuses paths in the repo) |
| `tools/analyze/ghidra/stats_report.py` | JSON → Markdown tables |
