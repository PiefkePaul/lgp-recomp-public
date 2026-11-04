# 0005 – Handling of `sdk`/`subsdk*` NSOs: recompile them too vs. HLE at SDK level

Status: **Accepted** (Paul, 2026-10-10) · Date: 2026-10-10 · Updated: 2026-10-10 (P1.6, measurement data from P1.1–P1.5) · Decides: Paul · Gate: data from P1.2 and P1.5

## Context

The ExeFS of a Switch game typically contains `rtld` (dynamic linker), `main` (game code), `sdk` (Nintendo SDK: libc, `nn::fs`, `nn::hid`, NVN, IPC clients …) and, depending on the game, `subsdk0..n` (middleware). LGP has `rtld`, `main` (`beluga.nss`), `subsdk0` (`glslc`), `subsdk1` (`multimedia`) and `sdk` (`nnSdk`), see the next section.

Important point: `main` and `sdk` are **dynamically linked**. The interface between them consists of named (mangled C++) symbols. So we know the names of all SDK functions the game calls, without disassembly.

Emulators execute the SDK unchanged and implement SVCs and IPC services beneath it. This is the boundary proven by Ryujinx/yuzu.

## Measurement data (P1.1–P1.5)

Sources: `01-inventar.md`, `01b-imports.md`, `02-services.md`, `03-grafik.md`, `04-code-statistik.md`.

| Module | Instructions | Content | Run time up to Route 1 | Interface to `main` |
|---|---:|---|---|---|
| `main` | 3.35 million | game + statically linked middleware (NintendoWare, Wwise, NEX/Pia, libcurl …), stripped | – | – |
| `sdk` | 1.24 million | libc, C++ runtime, `nn::*` clients, **NVN driver**, all 60 `SVC` instructions, 1,227 functions with LSDA; **fully named** (22,986 exports) | 4,513 functions ran (PTC) | 802 imports from `main` (551 `nn::`), 8 callbacks into `main` (`nnMain`, `malloc` …) |
| `subsdk0` (`glslc`) | 2.02 million | GLSL compiler | **module init only** (10 functions), no `glslc*` | 8 imports from `main`, only via a path that never runs |
| `subsdk1` (`multimedia`) | 0.84 million | Stagefright port with NvRm/NvOs, hardware video via `nvdrv`/`mm:u` | intro video only (17 threads) | 28 `movie::` imports |
| `rtld` | 1,575 | dynamic linker | – | replaced by static linking in the recompiler |

Further facts:
- All strong imports resolve within the ExeFS (0 unresolved) → static linking in the recompiler is sufficient.
- `main` contains **no** `SVC` instruction and only reads `tpidr_el0`; system calls, `tpidrro_el0`, `cntpct_el0`, FPCR/FPSR all lie in `sdk`.
- 21 services with 150 different IPC commands were actually used, some of them Ryujinx-internal (`02-services.md`, appendix A). Ryujinx implements all of them; 42 places are only stubs there.
- Most frequent import call sites in `main`: `nn::os::GetTlsValue` 4,488, `memset` 2,390, `memcpy` 2,200, `nn::os::LockMutex`/`UnlockMutex` 1,028/1,142.

### Critical review of the recommendation

1. **The draft (C: recompile everything, replace selectively) was too sweeping.** "Everything" would be 7.4 million instructions. The data shows that `subsdk0` and `subsdk1` together make up 2.86 million instructions (39 %), but only provide module init or a video for the game. Neither is recompiled.
2. **Recompile `sdk` too or replace it at API level?** In favor of complete replacement (B): no SVC/IPC re-creation, no exception code, 1.24 million fewer instructions. More speaks against it: besides easily replaceable parts (libc, `nn::crypto`, network stubs), the 802 imports also include parts with a lot of internal state and no documentation (`nn::fontll` 24, probably a font rasterizer, `nn::mem` 16 = allocator, `nn::hid` 81, `nn::audio` 22, `nn::fs` 19, `nn::account`, `nn::oe`, `nn::swkbd`). The IPC services beneath them, by contrast, are well documented via Switchbrew and Ryujinx (MIT) and manageable (21 services, 150 commands). Recompiling too yields a reference path that matches the original and can be tested differentially on the Mac.
3. **Performance does not necessarily favor API HLE.** The ≈ 30,000 sync operations per second (ADR-0002) also become direct runtime calls in the SVC path, without a trap. Replacements are made where it measurably pays off (`memcpy`/`memset`, `GetTlsValue`).
4. **NVN is the big exception** (ADR-0003): the slots are filled via HLE, the NVN driver in `sdk` never runs. It is still translated along (otherwise `sdk` would have to be cut apart) or excluded from translation via a function list from the NVN proc table; that is an optimization for P2.9, not an architecture question.
5. **Exceptions in `sdk`:** if `sdk` is recompiled too, 1,227 functions with LSDA are added. Without an unwinder (ADR-0004), landing pads are not translated and `__cxa_throw` aborts. Risk: an SDK path does throw during normal operation after all. That shows up immediately and loudly in the first real run (P3.3).

## Options

### A: Recompile everything, HLE only at SVC/IPC level (emulator boundary)

- Pros: behavior identical to the original, services are well documented on Switchbrew and in Ryujinx, little game-specific knowledge needed.
- Cons: more generated code (the SDK is large), IPC overhead remains, NVN runs as a Maxwell driver (see ADR-0003).

### B: Replace SDK functions with our own HLE implementations

- Pros: less code, faster (e.g. `nn::fs` directly on `IStorage`), no IPC layer.
- Cons: re-create the semantics of hundreds of SDK functions without documentation; SDK-internal state (allocators, SDK threads) becomes inconsistent if only parts are replaced; hard to test.

### C: Hybrid

A as the basis. Individual functions are replaced **by symbol name** with host implementations where it clearly pays off: `memcpy`/`memset`/math (performance), `nvnBootstrapLoader` (if ADR-0003 = NVN HLE), possibly logging/assert functions.

## Recommendation

**C, but only `main` + `sdk`** (adjusted):

| Module | Treatment |
|---|---|
| `main` | recompile |
| `sdk` | recompile; **SVC/IPC is the main HLE boundary** |
| `subsdk1` (`multimedia`) | **do not recompile**; replace the 28 `movie::` imports via HLE (browser: WebCodecs, desktop: FFmpeg or similar), `mm:u` only as a stub |
| `subsdk0` (`glslc`) | **do not recompile**; the 8 `glslc*` imports as stubs that abort loudly; the 4 `.init_array` constructors are not needed, because nothing in the module is called (confirm in P3.3) |
| `rtld` | do not recompile; the recompiler links statically (imports/exports, `RELATIVE`/`ABS64`/`GLOB_DAT`/`JUMP_SLOT`, RELR for homebrew) and calls the `.init_array` entries itself |

Replacements by symbol name (recompiler config, each one can be switched off individually):
- **NVN:** `nvnBootstrapLoader` (+ `nv::InitializeGraphics`/`SetGraphicsAllocator`/`FinalizeGraphics`) → NVN HLE (ADR-0003).
- **Performance:** `memcpy`, `memset`, `memmove`, `nn::os::GetTlsValue`/`SetTlsValue`; others only after measurement.
- **Control flow:** `setjmp`/`longjmp` → host variant, `__cxa_throw`/`_Unwind_RaiseException` → loud abort (ADR-0004).
- **Video:** `movie::*` (28).

Network, Bluetooth, `ldn`, `bcat`, `nifm` etc. are answered as "not available" **at IPC level**, not at API level: then the SDK code handles the errors the way Nintendo intended.

Code size: ≈ 4.6 million instead of 7.4 million instructions (−38 %). Whether further `sdk` parts are later worth replacing at API level (e.g. `nn::fs` directly on `IStorage`) is only decided after P3 with measurements; the symbol list makes this possible at any time.

## Open questions / data needed

- ~~NSO list, sizes, middleware in `subsdk*` (P1.1)~~ → answered.
- ~~Imported SDK symbols (P1.2)~~ → `01b-imports.md`.
- Size of the generated code and Wasm compile time for `main` + `sdk` (P2.9, P6.1). Target indicator: does loading a Wasm file of this size work in all target browsers?
- **New:** does an SDK path throw a C++ exception during normal operation? (P3.3, a loud abort will show it)
- **New:** does any path after the intro video need `subsdk1`/`subsdk0` after all? Only the beginning of the game was measured.

## Consequences

- The recompiler must process several NSOs together and resolve imports/exports + relocations between them (P2.1).
- `rtld` is not recompiled but replaced by our own linker step (data: all imports resolvable within the ExeFS).
- P3.3–P3.5 implement the 21 services used at IPC level (Ryujinx as MIT reference), order according to `02-services.md` §7.
- P2.9 recompiles `main` + `sdk`, not `subsdk0`/`subsdk1`.

## Sources

- Switchbrew NSO/ExeFS: <https://switchbrew.org/wiki/NSO>
- Ryujinx as reference for the SVC/IPC boundary: <https://emulation.gametechwiki.com/index.php/Ryujinx>
