# SDK process start without rtld (boot program of a static recompilation)

Cross-game knowledge from P3.4 (as of 2026-10-11). Code: `runtime/boot/` (`lgp_boot`, `lgp_add_boot_executable`),
tests: `tests/boot/` (`ctest -L boot`). Sources: oss-rtld (Thog, ISC-like license, read only as interface documentation,
no code taken), switchbrew ("NSO", "NPDM", "rtld"), libnx. Decision that rtld is not recompiled:
[ADR-0005](../decisions/0005-sdk-nsos.md).

## What rtld does when an SDK title starts

A Nintendo SDK title consists of `rtld`, `main`, `subsdk0..9` and `sdk`. The kernel starts the main thread at
`rtld`'s entry with `x0 = 0`, `x1 = handle of the main thread` (`x0 ≠ 0` is the exception entry). rtld

1. relocates itself,
2. finds the other modules via `svcQueryMemory` (code blocks with permissions R-X, MOD0 offset at `+4`), zeroes their
   `.bss`, applies their relocations and binds imports (global symbol lookup in load order, `main` first),
3. in every module that defines them, stores pointers to its own structures:
   `nn::ro::detail::g_pAutoLoadList`, `g_pManualLoadList`, `g_pRoDebugFlag`, `g_pLookupGlobalAutoFunctionPointer`,
4. calls `nn::init::Start(thread_handle, argument_address, notify_exception_handler_ready, call_initializator)`
   (mangled `_ZN2nn4init5StartEjmPFvvES2_`, in `sdk`). `argument_address` is the end of rtld's own image
   (`__argdata__`, page-aligned), where the loader places the argument field (`ProgramArguments`) if needed.
   `call_initializator` calls **only `DT_INIT`** of each module, in **reverse load order** (`sdk` first, `main`
   last; rtld itself is not in the list). `nn::init::Start` does not return; it ends with `svcExitProcess`.

Older SDKs without `nn::init::Start` (before 3.x) call the individual steps themselves (`__nnDetailInitLibc0`, `nnosInitialize`,
…, `nnMain`, `nnosQuickExit`); that is not implemented here, the boot then aborts with a message.

## What the boot program does instead

| rtld step | Boot program |
|---|---|
| Relocations, imports | `lgp::recomp::link()` with **the same options as when lifting** (`lgp_lift`): same bases, same order, same resolved addresses. The images are copied into guest memory fully relocated (text RX, ro R as `Code`, data+bss RW as `CodeData`). |
| Does the layout match the code? | Next to the dispatch entries the codegen writes a module table (`<entries>_modules[]`: name, base, size, build ID). `boot()` compares it with the loaded layout and refuses to start on any mismatch, because lifted addresses are baked in. |
| `nn::init::Start` | Host function as the main thread's entry (dispatch key after the last module), calls the lifted function with the four arguments. |
| Callbacks | Two more host functions at reserved addresses; a `BLR` from lifted code lands directly in them via the dispatch table. `call_initializator` calls the lifted `DT_INIT` of each module (rtld skipped). |
| `nn::ro` globals | Empty module lists (`front = back = list`), debug flag `false`, lookup function as a host function via the link's symbol table. Code that does not use `nn::ro` notices nothing; code that wants to walk the list finds no modules instead of crashing. |
| Argument field | `argument_address` points to a **never-mapped** page (the key page after the modules). The sdk (`nnosInitialize`) queries the address via `svcQueryMemory` and evaluates `ProgramArguments` only with RW permissions; otherwise `argc`/`argv` stay at their built-in defaults (`argc` 1, `argv[0]` a valid string). A mapped empty header, in contrast, yields `argc` 0 and `argv[0] == NULL`, and games read `argv[0]` at startup (LGP: crash in `nnMain`, boot test 2026-10-11). |

Calling guest code from host code is cheap in this model: lifted functions are C++ functions with native
return (ADR-0004). The host sets `x0..x3` and `x30`, calls the function and restores `x30`. No objects with
destructors may live between host frames while doing so: a memory fault in the guest jumps back into the
scheduler via `longjmp` (`run_lifted`).

## Graphics stop as a measurement point

In mode "stop" the boot program replaces the entry points of the graphics stack via the dispatch table (later registration
wins) with a host function that reports the name and ends the process via `svcExitProcess`: `nvnBootstrapLoader`
and `nv::SetGraphicsAllocator`, `nv::SetGraphicsDevtoolsAllocator`, `nv::InitializeGraphics`, `nv::FinalizeGraphics`
(prefixes of the mangled names). Because `main` imports these symbols via PLT/GOT from `sdk`, every call lands there
via the dispatch table. A direct `BL` within `sdk` would bypass it; for `main` → `sdk` there is none.
This is the acceptance point "boots up to the first graphics call". Games typically call
`nv::SetGraphicsAllocator` and `nv::InitializeGraphics` first and only then `nvnBootstrapLoader`.

## Hooking up the NVN HLE (mode "hle")

With the NVN HLE ([`nvn-hle.md`](nvn-hle.md)) the driver in `sdk` never runs. The boot program

- places the HLE's thunk block directly after its own pages (dispatch table stays small),
- replaces the `sdk` export `nvnBootstrapLoader` by address with the HLE's (later registration wins; also
  `&nvnBootstrapLoader` from `main`, which points to the same address via the GOT, lands there),
- turns `nv::SetGraphicsAllocator`, `nv::SetGraphicsDevtoolsAllocator`, `nv::InitializeGraphics` and
  `nv::FinalizeGraphics` into empty functions. They only set up the driver (memory, `nvdrv`); all return `void`.
  Assumption: the game does not check any driver state outside NVN afterwards; to be checked with the real game.
- wraps the thunk of `nvnQueuePresentTexture` with a frame counter: after `--frames N` frames or when the window
  was closed, the process ends via `svcExitProcess` at the next present (clean measurement point instead of an abort).

The SDL window needs its events on the main thread; the process then runs in a second host thread
(guest threads present from their own host threads anyway, ADR-0002).

## Proving it without game data

- **SDK-like module pair** (`tests/boot/guest/`): our own code, built with host clang as shared objects
  (`-shared --hash-style=both`, linker script with three page-aligned segments from 0, `-init=` for `DT_INIT`) and
  packed with `elf2nso.py` as an uncompressed NSO, plus an NPDM from `make_npdm.py`. `sdk` exports
  `nn::init::Start` and `nvnBootstrapLoader`, `main` exports `nnMain`; both have a `DT_INIT` and import
  functions (PLT) and data (GOT) from each other. Checked are handle, argument field, init order, `nn::ro` pointers,
  symbol lookup, data import and the graphics stop. Pitfalls when building: linker script symbols in a shared object
  are global and thus preemptible; set `HIDDEN()`, otherwise lld rejects the PC-relative MOD0 fields.
- **libnx fixtures** via the same boot program (homebrew mode: one module, image **not** relocated, because libnx's
  crt0 relocates itself and RELR is not idempotent). `basic`/`threads` produce the same debug output as
  under Unicorn; `services` uses libnx's own clients against the HLE services (sm, applet, set, time, csrng, acc,
  fsp-srv with RomFS and save data, swkbd).

What libnx required in addition to the game (games with the SDK do not necessarily need it):
`apm` at application start (`apmInitialize`), cloning a domain session (libnx's `fsInitialize` opens several
fsp-srv sessions this way), and `printf` cannot be used as long as the jump table detection does not track a
table base loaded before the loop ([`jump-table-patterns.md`](jump-table-patterns.md)).

## Large code: split it

A game's `main` has millions of instructions (LGP: approx. 3.5 million). As a single translation unit this practically does
not compile. `lgp_lift --split N` writes `out.cpp` (with dispatch and module table) and `out.1.cpp … out.<N-1>.cpp`,
balanced by text size; each file declares all functions. `lgp_add_boot_executable(<target> <files…>)` or
`-DLGP_GAME_LIFTED=<directory>` builds the boot program from them.
