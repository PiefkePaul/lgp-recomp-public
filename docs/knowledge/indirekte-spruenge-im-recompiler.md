# Indirect branches in the static recompiler: dispatch table, tail calls, setjmp/longjmp

Cross-game knowledge from P2.5 (as of 2026-10-11, ADR-0004). Code: `runtime/core/include/lgp/runtime/dispatch.hpp`,
`host_jmp.hpp`, `guest.hpp`, `recomp/codegen/`, test `tests/indirect/`.

## Dispatch table

- **All** function starts go into a directly indexed table: slot = (va − base) / 4, entry = host function.
  A lookup is a subtraction, a range and alignment check and a load. In Switch games a large share
  of the functions are address-taken (vtables) and most `BLR`s cannot be resolved statically, so a "known targets only" approach
  is not enough.
- The code generator writes one array `{va, fn}` (`lgp::LiftedEntry`) per module set, and the runtime registers it before
  start (`register_functions`). The range grows as needed; several modules may be registered separately.
- Size: 4 bytes per 4 bytes of code in Wasm (function index), 8 bytes on 64-bit desktop. For 3 MiB of `.text` that is 3 or
  6 MiB respectively, not critical.
- Runtime code that enters guest code by address (thread entry, HLE callbacks) uses `lookup_function(va)`: first the
  table, then an optionally installed resolver (bring-up, hand-written functions in tests).

## Error path instead of a silent fallback

A target without a function (jump into the middle of a function, a function without FDE that the analysis did not find) halts the guest
with `kHaltBadJump` (`halt_info` = target), logs the address once together with the branch source, and counts it.
`unresolved_config()` returns ready-made lines `function 0x… # …` in the format of the recompiler config
([`recompiler-config.md`](recompiler-config.md)). Cycle: run → extend config → recompile. The test
`indirect.*` checks exactly this cycle with a second entry point of an assembler function.

## Tail calls and the host stack

- `BR` from a loaded pointer in tail position (`return fp(x);`) and `B` to another function's start are
  tail calls. Translated as call + `return`, the host stack grows where the guest stack stays constant. State machines
  that call each other via tail calls then overflow.
- Measurement (P2.5): a chain over 1,000,000 states runs in constant stack with `[[clang::musttail]]`; without the
  attribute the same test crashes with a stack overflow (SIGSEGV) with clang 18 in a Debug build.
- Compilers: clang has supported `musttail` since 13, GCC only from 15 (`[[gnu::musttail]]`); GCC 13 and MSVC optimize tail calls
  only with optimization enabled and without a guarantee. `LGP_HAS_GUARANTEED_TAIL_CALLS` indicates which applies. For builds of the game
  (desktop and Emscripten, both clang) this is not a problem. Wasm needs the tail-call feature (`-mtail-call`), otherwise
  `musttail` has no effect.
- Because all lifted functions share the same signature `void(CpuState*)`, `musttail` always fits, even through a
  function pointer from the dispatch table.

## setjmp/longjmp via the host variant

With "function = C++ function, `RET` = `return`", every guest frame corresponds to exactly one host frame. Guest `longjmp` can
therefore be mapped onto host `longjmp`, without a block dispatcher:

- `BL setjmp` becomes host `setjmp` **inline** in the calling frame, because the host jump buffer is only valid as long as
  the frame that `setjmp` returns into is alive. A helper function call would not work. Saved are the AAPCS64
  callee-saved registers x19–x30, sp, d8–d15, keyed by the address of the guest `jmp_buf` (thread-local table).
- `longjmp` restores these registers, sets x0 (0 becomes 1) and jumps via host `longjmp`. `ldp d8, d9` in the guest clears the
  upper half of the vector registers; the host variant does the same.
- `setjmp`/`longjmp` are found by symbol name (definition in the module or PLT stub of an import). `setjmp` via
  a function pointer or tail call does not work this way and halts.
- Limits: the guest `jmp_buf` in guest memory is not written (copying a `jmp_buf` does not work), and a
  `longjmp` across a nested `run_lifted` skips its cleanup.
- Check: lifted code after the `setjmp` reads values from callee-saved registers that the recursion overwrote before the `longjmp`.
  Without register restore the test turns red (mutation probe).

## Jump table forms from clang (AArch64)

For dense `switch` statements clang 18 generates two forms, both of which the jump table detector from P2.2 resolves:

| Form | Pattern | When |
|---|---|---|
| compressed (default) | `adrp/add` table, `adr` base, `ldrb`/`ldrh` entry, `add x, base, w, lsl #2`, `br` | function small enough for 1-/2-byte offsets |
| 32-bit | `adrp/add` table, `adr` base, `ldrsw x, [tab, idx, lsl #2]`, `add`, `br` | `-mllvm -aarch64-enable-compress-jump-tables=false`, older clang versions |

Older SDK toolchains (the pattern of the analyzed retail binaries) generate only the 32-bit form. Below about 10 cases
clang usually chooses a comparison chain instead of a table.

## Custom AArch64 test programs without devkitPro

For tests that need the control flow of a real compiler, the host clang with `--target=aarch64-none-elf` is enough:

- Compile freestanding (`-ffreestanding -fno-builtin -nostdlib -fno-exceptions -fno-rtti -fPIC -funwind-tables`),
  define `__cxa_pure_virtual` yourself, no static constructors.
- Custom `crt0`: `b start` at +0, MOD0 offset at +4, leave 0x80 bytes free for the NRO header; MOD0 with `_DYNAMIC`,
  BSS and `.eh_frame_hdr` bounds from the linker script.
- Link with ld.lld as PIE without a dynamic linker (`-pie --no-dynamic-linker --eh-frame-hdr -z norelro
  -z max-page-size=0x1000 --pack-dyn-relocs=none`), linker script with three page-aligned segments (RX, R, RW+BSS).
  The relocations (`RELATIVE`) are applied by the recompiler's loader; the program needs no self-relocation.
- `tests/indirect/elf2nro.py` packs it into an NRO. Put symbols that the recompiler should find by name into dynsym with
  `--export-dynamic-symbol=`.

This way the same image runs under Unicorn and recompiled, without Docker or devkitA64 (the libnx fixtures remain for
everything that needs libnx initialization, threads or services).
