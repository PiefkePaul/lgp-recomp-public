# Roadmap LGP-Recomp

As of: 2026-10-10, P6/P7 updated 2026-10-13 · Status: merged (PR #2) · Maintenance: checked off after every completed thread.

Goal: Pokémon Let's Go Pikachu via static recompilation (ARM64 → C++ → WebAssembly/WebGPU) **playable in the browser** (main milestone = end of P6). P7 (further platforms) and P8 (gameplay mod) start only after P6 and only with Paul's approval.
Fixed follow-up goal after P6 (Paul, 2026-10-11): iOS/iPadOS as a **native arm64 app** from the same lift output, not as WASM (P7.2, [ADR-0007](decisions/0007-ios-nativ-statt-wasm.md)).
**End goal (Paul, 2026-10-13):** play on all of one's own devices: Windows, macOS, iOS/iPadOS, Android and Apple TV. The browser was meant as a practical route. P6 is formally closed (open points: `23-p6-abschluss-stand.md`), then P7 as native apps ([ADR-0008](decisions/0008-native-apps-auf-allen-geraeten.md), accepted 2026-10-13). Target devices: Windows PC, MacBook Air M1, iPad Pro M1, iPhone 17 Pro Max, OnePlus 7 Pro, Apple TV 4K (3rd gen).

## Honest assessment up front

- This is a **multi-year project**. To this day there is no publicly known Switch game that runs fully *without* an emulator underneath via static recompilation. The only known Switch AOT experiment (suyu's "Recompiler mode" in its last version 0.0.4, repo archived since September 2026) still uses the emulator's complete HLE layer for services, graphics and audio; solid performance data is missing. N64Recomp and XenonRecomp/UnleashedRecomp show that the approach works, but for much smaller or better understood platforms.
- The biggest effort is **not** in the CPU lifter, but in runtime/HLE (P3) and graphics (P4). The size estimates below are rough and rather optimistic.
- The biggest technical risks: memory needs vs. the 4 GB limit in WASM (ADR-0001), thread model in the browser (ADR-0002), NVN semantics without public documentation (ADR-0003), performance of the whole thing in the browser (P6).

## Legend

- **Location**: `Cloud` = cloud thread without game data · `PC` = local thread on the Windows PC (game data, GPU) · `Mac` = local thread on the M1 (ARM64 native, Hypervisor.framework, Safari/Metal)
- **Size** (rough, working threads, not calendar time): `S` = one short thread · `M` = one full thread · `L` = several threads · `XL` = many threads/weeks to months
- **Deps**: substeps that must be finished first
- **ADR**: related decision in [`docs/decisions/`](decisions/)
- Only set the check `[x]` once the acceptance criterion is met and the PR is merged.

## Critical path (short version)

```
P0.1–P0.4 ─► P1.1 inventory ─► P1.3/P1.4 analysis ─► ADR-0001…0005 decided
                         │
P0 ─► P2.1–P2.6 lifter + differential tests (homebrew) ─► P2.9 real main compiled
                                                              │
                         P3 runtime/HLE desktop ◄─────────────┘
                                │
                         P4 graphics ─► P5 audio/input/saves ─► P6 WASM/browser ★
                                                                     │
                                                    P7 platforms · P8 mod (only after approval)
```

P2 (lifter on homebrew) can run in parallel with P1. P3 needs the real `main` compiled (P2.9) and the service list from P1.

---

## P0 Infrastructure

- [x] **P0.1 Repo skeleton** (PR #1): directory structure according to the target picture, `CLAUDE.md` with build/test, `.gitignore`, `THIRD_PARTY.md`
  Location: Cloud · Deps: – · Size: S
  Acceptance: structure is on `main`, `CLAUDE.md` names the build and test commands.
- [x] **P0.2 Pre-commit guard against game data**: extensions, size limit, magic bytes `NCA3`/`NSO0`/`PFS0`, with test
  Location: Cloud · Deps: P0.1 · Size: S
  Acceptance: `ctest` contains a test that proves a file with `NCA3` magic is rejected; the guard is documented. (PR #1, `tests/guard/test_guard.py`)
- [x] **P0.3 CMake skeleton + `ctest` suite**, presets `default`/`release`/`ci` (PR #1)
  Location: Cloud · Deps: P0.1 · Size: S
  Acceptance: `cmake --preset … && cmake --build … && ctest` runs green locally and in the cloud.
- [x] **P0.3b Emscripten preset** (split out of P0.3, PR #1 does not contain it) — presets `wasm`/`ci-wasm`, CI job "Emscripten/Node"
  Location: Cloud · Deps: P0.3 · Size: S
  Acceptance: `cmake --preset wasm` builds the smoke test with Emscripten, `ctest` runs it in Node.
- [x] **P0.4 CI (GitHub Actions)**: build + `ctest` + guard, homebrew fixtures only
  Location: Cloud · Deps: P0.2, P0.3 · Size: S
  Acceptance: CI green on a PR; CI demonstrably has no access to game data. (PR #1; Linux only for now). Since PR #50 also Windows (Ninja; MSVC in PRs, additionally clang on main; all registered tests, none skipped; Unicorn stays off there).
- [ ] **P0.5 Toolchain PC**: git, gh, CMake, Ninja, clang/LLVM, Python, Emscripten, Ghidra, nstool/hactoolnet; `LGP_PRIVATE_DIR` checked
  Location: PC · Deps: – · Size: S
  Acceptance: toolchain list with versions in `docs/analysis/00-toolchain.md` (without paths to game data contents).
- [x] **P0.6 Toolchain Mac** (PR #5): Xcode CLT, CMake, Ninja, Emscripten, Ryujinx fork with Hypervisor backend
  Location: Mac · Deps: – · Size: M
  Acceptance: the Mac builds the repo and `ctest` is green; Ryujinx fork starts LGP (yes/no documented). Met: `ctest` 5/5 green, LGP boots in Ryubing 1.3.1 with Hypervisor (`docs/analysis/00-toolchain-mac.md`).
- [x] **P0.7 devkitA64/libnx + homebrew fixture build**: first own test NRO/NSO from C/ASM, reproducible via CMake/Docker
  Location: Cloud · Deps: P0.3 · Size: M
  Acceptance: the fixture is built in CI, is small, contains no Nintendo code except what libnx (ISC) brings along.
  Met (PR #9): fixture `basic` (C + ASM) in [`tests/fixtures/`](../tests/fixtures/README.md), CI job "Homebrew fixtures" in the digest-pinned image `devkitpro/devkita64:20260219`; NRO approx. 96 KiB, NSO approx. 47 KiB, two builds byte-identical. Binaries are not committed.
- [x] **P0.8 Integrate the Unicorn engine as ARM64 oracle**
  Location: Cloud · Deps: P0.3 · Size: S
  Acceptance: a ctest runs an instruction sequence in Unicorn and compares the register state.
  Met (PR #9): interface from ADR-0006 (`tests/difftest/oracle.h`) + `UnicornExecutor` (Unicorn 2.1.4, Cortex-A57); ctest `difftest.unicorn_oracle` checks integer/flags, loop + memory, SVC trace + `svcExitProcess`, FP, TLS registers, error and limit cases. Known Unicorn deviations (EL1 instead of EL0) in [`tests/difftest/README.md`](../tests/difftest/README.md).
- [ ] **P0.9 Remote Control PC + Mac stable** (manually by Paul, per `docs/project-setup/`)
  Location: PC + Mac · Deps: – · Size: S
  Acceptance: both machines accept local threads.

## P1 Extraction and analysis (local, with game data)

All results go to `docs/analysis/` **without game code**. Raw data, Ghidra project and logs stay in `$LGP_PRIVATE_DIR`.

- [x] **P1.1 Extraction + ExeFS inventory**: NSP → NCA → ExeFS/RomFS to `$LGP_PRIVATE_DIR/extracted/`; NSOs, sizes, segments, build IDs, SDK version, import/export relationships
  Location: PC · Deps: P0.5 · Size: M
  Acceptance: `docs/analysis/01-inventar.md`; extraction reproducible via script in `tools/extract/`.
  Status: `01-inventar.md` is on `main` (PR #3); the extraction tooling stays private (decryption is left out for legal reasons).
- [x] **P1.2 NSO parser + symbol/import inventory as its own tool** (`tools/analyze/`), tested on homebrew fixtures
  Location: Cloud (tool) + PC (run) · Deps: P0.7 · Size: M
  Acceptance: the tool runs in CI on fixtures; the PC run delivers the list of SDK symbols imported by `main` (names, count) in `docs/analysis/`.
  Status: tool done (`nso_inventory.py`: NSO + NRO, `--imports` list, robust against broken headers), runs in CI on **synthetic** NSOs/NROs. PC run with `--imports` done (2026-10-10): name list in `01b-imports.md` (main → sdk 802, of which 551 `nn::`). Completed with PR #9: ctest `tools.fixture_inventory` runs in CI on NRO and NSO of the fixture `basic` and checks build ID and relocations against `readelf`. Found and fixed along the way: devkitA64 r29 uses `DT_RELR`, the tool now counts RELR too (`reloc_counts["RELR"]`).
- [x] **P1.3 Ryujinx reference run with SVC and service call logging** into the game (title → game start → first route)
  Location: PC (Mac optional) · Deps: P0.5 · Size: M
  Acceptance: `docs/analysis/02-services.md`: SVCs, IPC services and commands used, with frequency; thread count and priorities; **peak memory need** (for ADR-0001).
  Status (2026-10-11): `02-services.md` with a run up to Route 1 (Paul plays: catch, battle, menus, saving); tools `tools/analyze/ryujinx_log_stats.py` and `ryujinx_heap_scan.ps1`. Peak memory: 1.29 GiB written heap + 1.26 GiB nvmap (range see document).
- [x] **P1.4 Graphics inventory**: NVN functions used (via `nvnBootstrapLoader`/GetProcAddress names), shader count and location in RomFS, whether shaders are generated at runtime
  Location: PC · Deps: P1.1, P1.3 · Size: M
  Acceptance: `docs/analysis/03-grafik.md`; input for ADR-0003.
  Status (2026-10-11): `03-grafik.md`. Tools `tools/analyze/nvn_inventory.py`, `shader_inventory.py`, `ryujinx_cache.py`; cross-game [`nvn-bootstrap-und-treiber.md`](knowledge/nvn-bootstrap-und-treiber.md), [`ryujinx-caches-als-laufzeitdaten.md`](knowledge/ryujinx-caches-als-laufzeitdaten.md), [`nx-grafikformate.md`](knowledge/nx-grafikformate.md). Key figures: NVN via bootstrap, all 498 functions loaded by name, 230 used (204 ran up to Route 1) → gate "< 300" met; glslc never called; ≈ 8,400 BNSH with ≈ 2,450 shader stages, 500 of 527 runtime shaders byte-identical in the game data (origin of 27 open); textures 94.5% BCn, 1.3% ASTC.
- [x] **P1.5 Ghidra project (headless) + function/jump table statistics**: number of functions, indirect branches, jump table patterns, C++ exceptions/setjmp yes/no, dynamically loaded NROs (`ro`) yes/no
  Location: PC · Deps: P1.1 · Size: M
  Acceptance: `docs/analysis/04-code-statistik.md`; input for ADR-0004/0005.
  Status (2026-10-11): `04-code-statistik.md` for all five modules (Ghidra 12.1.4 headless). Tools `tools/analyze/nso2elf.py` and `tools/analyze/ghidra/`; cross-game approach in [`docs/knowledge/ghidra-nso-headless.md`](knowledge/ghidra-nso-headless.md). Key figures `main`: 60,919 functions (FDEs cover all but 3 non-thunks), 109,287 BLR (94% vtable-like), 1,541 jump tables in a single pattern, 61% of functions address-taken, 0 RET ≠ x30, exceptions practically unused, setjmp 3×, no `ro`/NRO.
- [x] **P1.6 Make the core decisions**: update ADR-0001…0005 with P1 data and present them to Paul
  Location: Cloud · Deps: P1.3, P1.4, P1.5 · Size: S
  Acceptance: all five ADRs have status "Accepted" (Paul's approval).
  Met: measurement data and review in PR #12, all five ADRs accepted by Paul on 2026-10-10.

## P2 Recompiler

Test basis up to P2.8: exclusively our own homebrew fixtures, oracle Unicorn (Cloud) or Hypervisor.framework (Mac).

- [x] **P2.1 NSO/NRO loader + segment model** in the recompiler (text/RO/data/BSS, MOD0, dynamic symbols, relocations)
  Location: Cloud · Deps: P0.3, P0.7 · Size: M
  Acceptance: fixtures are loaded; relocations match the reference (e.g. nstool/own parser).
  Met (PR #14): `recomp/core` loads NSO (LZ4, SHA-256) and NRO incl. MOD0, `.dynamic`, dynsym, RELA/JMPREL/RELR,
  `.eh_frame_hdr`; link step in rtld order with import table. CI test `recomp.fixture_vs_reference_parser`
  compares segments, symbols and relocation counts of the fixture `basic` (NRO and NSO) with `nso_inventory.py`.
  Cross-game: [`nso-nro-format.md`](knowledge/nso-nro-format.md).
- [x] **P2.2 Disassembler integration + function discovery + CFG**
  Location: Cloud · Deps: P2.1 · Size: L
  Acceptance: function boundaries on fixtures 100% correct (checked against the fixtures' symbols).
  Met (PR #14): function discovery (FDE, symbols, BL/tail targets, data pointers, gap scan), basic blocks,
  noreturn inference, jump tables (clang `ldrsw/add/br`, gcc `sxth`, absolute, PLT stubs), config file (ADR-0004).
  CI test `recomp.fixture_check_functions_{nro,nso}`: 0 missing, 0 wrong function starts against the fixture's
  `.symtab`. No disassembler of its own: the CFG uses `classify()`; the bit-exact decoder belongs to the lifter (P2.3),
  switchover after its merge (`docs/design/p2-interfaces.md`). The run against the real `main` is in the PR (PC thread).
  Cross-game: [`eh-frame-function-bounds.md`](knowledge/eh-frame-function-bounds.md),
  [`jump-table-patterns.md`](knowledge/jump-table-patterns.md), [`function-discovery.md`](knowledge/function-discovery.md),
  [`recompiler-config.md`](knowledge/recompiler-config.md).
- [x] **P2.3 Lifter integer + load/store + branches** (incl. flags/NZCV, exclusive accesses → atomics)
  Location: Cloud · Deps: P2.2 · Size: L
  Acceptance: instruction differential tests against Unicorn green for all implemented opcodes.
  Met: PR #16. Decoder + lifter in `recomp/lifter/`, ~5,150 differential cases against Unicorn green, every implemented op covered ≥ 5× (enforced by the generator), decoder fuzz without deviation. Built independently of P2.2 (input `LiftFunction`). Connection to the CFG (adapter, `classify()` on the lifter decoder, `lgp_recomp lift`) in PR #22; there the supplied fixture is lifted completely and compiled against the real runtime header. Execution against the oracle follows with P2.8.
- [x] **P2.4 Lifter FP + NEON/ASIMD** (rounding modes, NaN behavior, FPCR)
  Location: Cloud · Deps: P2.3 · Size: XL
  Acceptance: differential tests (incl. random inputs) green; list of deliberately unsupported opcodes documented.
  Met: PR #18. Entire ARMv8.0 scope (scalar FP, integer/FP NEON, permute, structure load/store, AES/SHA1/SHA256; 249 mnemonics), Unicorn difftest 13,710 cases and decoder fuzz of 1 million words without deviation, soft-float path against the host FPU (`fesetround`). Deliberately unsupported: everything after ARMv8.0 (FP16 arithmetic, RDM, dot product, FCMLA, FRINT32/64, BFloat16), UNDEFINED on the A57 (`docs/knowledge/a64-fp-simd-semantik.md`). Limitation: FPSR IEEE flags (IOC…IDC) not modeled, QC is. Open: measure LGP's FPCR value (`docs/analysis/05-fp-neon.md`).
- [x] **P2.5 Indirect branches**: dispatch table, jump table detection, error path for unknown targets (ADR-0004)
  Location: Cloud · Deps: P2.3 · Size: L
  Acceptance: fixtures with function pointers, virtual calls and `switch` run identically.
  Status (2026-10-11): directly indexed dispatch table over all functions in `runtime/core` (`dispatch.hpp`, `lookup_function()` for runtime code), unknown targets halt with `kHaltBadJump` and yield config lines (`unresolved_config()`); `LGP_JUMP_INDIRECT` as guaranteed tail call (`musttail`); `setjmp`/`longjmp` via a host variant (`host_jmp.hpp`); code generator `recomp/codegen` with CLI `lgp_lift` (analysis from P2.2 → lifter → C++ with dispatch entries). Jump table detection comes from P2.2. Acceptance met by `indirect.*` (`ctest -L indirect`): our own AArch64 program built with host clang (function pointers, virtual calls, `switch` in 8- and 32-bit table form, indirect tail-call chain, `setjmp`/`longjmp`) runs recompiled with the same end state as under Unicorn, including the error path and config addendum. Limitation: no libnx fixture (needs P3.2/P3.3). PC run against `main` (2026-10-11, `06-recomp-gegen-main.md`): `lgp_recomp` plausibly resolves 54 of the 55 tables Ghidra does not resolve; two CFG bugs found and fixed along the way (722 → 1,452 of 1,541 Ghidra tables); 84 tables with a base loaded before the loop remain open. Knowledge doc: [`indirekte-spruenge-im-recompiler.md`](knowledge/indirekte-spruenge-im-recompiler.md).
- [x] **P2.6 SVC → runtime call + memory access abstraction** (macros/inline functions, switchable per ADR-0001)
  Location: Cloud · Deps: P2.3 · Size: M
  Acceptance: a fixture with SVC calls runs against a minimal runtime; memory mapping switchable via build flag.
  Status (2026-10-11): `runtime/core/include/lgp/runtime/guest.hpp` implements the macro contract from `lifted.hpp` (memory via `guest_mem.hpp`, SVC as a direct call into `lgp::rt::Kernel`, CAS for exclusive pairs, system registers, memory faults never return, `run_lifted()` turns them into `kHaltMemFault` via longjmp). Build flags `LGP_GUEST_MEM_CHECKED` (debug default), `LGP_GUEST_MEM_WRITE_WATCH`, `LGP_GUEST_MEM64`. Tests `runtime.guest_header(_checked)` run. Acceptance met by `runtime.kernel_lifted(_checked)`: three test programs with memory, transfer memory and unimplemented SVCs (`tests/runtime/kernel_program_code.hpp`) are translated to C++ by the lifter at build time and run against `lgp::rt::Kernel`, with the same expectations as under Unicorn (`runtime.kernel_unicorn`). Limitation: the programs are hand-coded, not a libnx build; a libnx fixture needs threads/TLS and `sm` (P3.2/P3.3) and belongs to the acceptance of P3.1.
- [ ] **P2.7 Differential test harness on the Mac** (original natively via Hypervisor.framework vs. recompiled)
  Location: Mac · Deps: P0.6, P2.3 · Size: L
  Acceptance: the same fixtures run on the Mac in the hypervisor and recompiled with an identical end state.
  Status (2026-10-11, PR #46): hypervisor executor with EL0 + MMU (`tests/difftest/hv/`, `difftest.hv_executor`, same results as Unicorn); the P2.5 program (`-L indirect`) and our own test functions (`difftest.fn_selftest`, with FPCR variants and FP special values) run identically in the hypervisor and recompiled. Pulled forward from ADR-0006 stage 3: function comparison on the real game (`tests/difftest/fn/`), 31,405 functions from `main`+`sdk` executable in isolation, with 16 random states each, without lifter errors; all deviations are M1 ≠ A57 differences (`11-difftest-spielfunktionen.md`, technique [`hypervisor-framework-el0-harness.md`](knowledge/hypervisor-framework-el0-harness.md)). Open: the libnx fixtures in the hypervisor (devkitPro is missing on the Mac, plus an HV executor for guest threads on `lgp::rt::Kernel`).
- [x] **P2.8 Milestone "all homebrew fixtures run identically"** (desktop native, and as WASM smoke test)
  Location: Cloud · Deps: P2.4, P2.5, P2.6, P0.3b · Size: M
  Acceptance: CI suite green, incl. Emscripten build of the fixtures in Node.
  Met (PR #26): all libnx fixtures (`basic`, new `threads` with threads, mutex/condvar, `malloc`, TLS, priorities; each as NSO and NRO) run as a guest process on `lgp::rt::Kernel` twice, with Unicorn as the executor of the guest threads and recompiled via `lgp_lift`; process exit, debug output and the contents of every mapped block are identical (`fixtures.run.*`, `ctest -L fixtures`, CI job "Homebrew fixtures"). The CI job `fixtures-wasm` builds the same generated code with Emscripten and compares the run in Node with the native Unicorn end state. Fixed along the way: RET from the outermost thread function jumps to x30 (libnx crt0), function discovery finds `adrp/add` code pointers and tail stubs across padding (without `.eh_frame_hdr`), `lgp_lift` distinguishes A57-UNDEFINED data words from real gaps. Limitation: fixtures without services (`__appInit` empty); service-based fixtures follow with P3.4. Knowledge doc: [`homebrew-rekompiliert-ausfuehren.md`](knowledge/homebrew-rekompiliert-ausfuehren.md).
- [x] **P2.9 Real `main` (+ `sdk` per ADR-0005; not `subsdk0`/`subsdk1`) compiles without errors**
  Location: PC · Deps: P2.8, P1.6 · Size: L
  Acceptance: generated C++ code (locally in `build/`) compiles and links with a stub runtime; statistics (functions, unsupported opcodes = 0, unresolved indirect targets) in `docs/analysis/`.
  Preliminary status (2026-10-11, before P2.8): `lgp_lift` over `main`+`sdk` compiles with clang without errors (93 translation units, `-O1`) and links with `lgp_runtime_core`; 4,117 indirect `BR` go through the dispatch table; 706 "unsupported" are trap filler words, not missing opcodes. Details: `06-recomp-gegen-main.md`.
  Status (2026-10-11): `lgp_lift --split N` distributes the code across N translation units, `-DLGP_GAME_LIFTED=<dir>` builds the boot program `lgp_game` directly from it (linked against the real runtime instead of a stub). Procedure and measurement template: `08-boot-test-pc.md`.

## P3 Runtime/HLE (desktop) — needs Paul's approval to start
  Met (2026-10-11, PC): `main`+`sdk` lifted in 8 s (92,139 functions, 0 unimplemented instructions, 706 A57-UNDEFINED filler words), 64 parts compile with clang 23 `-O2` in ~100 s without errors and link with the real runtime into `lgp_game` (125 MB). Figures: `09-boot-test-ergebnis.md`.

- [x] **P3.1 Guest memory + memory SVCs** (SetHeapSize, MapMemory, QueryMemory, …) per ADR-0001
  Location: Cloud · Deps: P2.6 · Size: M
  Acceptance: unit tests + fixtures for memory SVCs green.
  Status (2026-10-11): guest layout below 4 GiB per ADR-0001 (`Layout::standard`, `TotalMemorySize` as switch), block management, desktop reservation or Wasm block, SVCs `SetHeapSize`, `SetMemoryPermission`, `SetMemoryAttribute`, `MapMemory`/`UnmapMemory`, `QueryMemory`, `Map`/`UnmapSharedMemory`, `CreateTransferMemory`, `CloseHandle`, `GetInfo`, `OutputDebugString`, `Break`, `ExitProcess`. Tests: `runtime.guest_memory(_checked)`, `runtime.kernel_svc` (native and Node) and `runtime.kernel_unicorn` (our own A64 programs under Unicorn with the runtime as kernel). Checked off with PR #26 (P2.8): the libnx fixtures `basic` and `threads` (heap via `SetHeapSize`, thread stacks via `MapMemory`) run recompiled on the runtime identically to Unicorn (`ctest -L fixtures`, native and Node). The gate experiment "2.75 GiB" from ADR-0001 is answered: LGP needs at least ≈ 3040 MiB, the web value has been 3072 MiB since 2026-10-11. Knowledge doc: [`horizon-speicher-svcs-in-eigener-runtime.md`](knowledge/horizon-speicher-svcs-in-eigener-runtime.md).
- [x] **P3.2 Threads, sync, TLS** (CreateThread, WaitSynchronization, mutex/condvar via arbiter, events) per ADR-0002
  Location: Cloud · Deps: P3.1 · Size: L
  Acceptance: multi-thread fixtures (producer/consumer, priority) run deterministically in "baton" mode and correctly in parallel mode.
  Status (2026-10-11): scheduler with the three modes from ADR-0002 (free, core baton, baton; selected via `ProcessConfig::sched_mode` at process start, switching in a running process is still missing), one host thread per guest thread with its own stack (≥ 2× guest stack, at least 1 MiB), TLS areas, SVCs `CreateThread`, `StartThread`, `ExitThread`, `SleepThread`, `Get/SetThreadPriority`, `Get/SetThreadCoreMask`, `GetCurrentProcessorNumber`, `CreateEvent`, `SignalEvent`, `ClearEvent`, `ResetSignal`, `WaitSynchronization`, `CancelSynchronization`, `ArbitrateLock/Unlock`, `WaitProcessWideKeyAtomic`, `SignalProcessWideKey`, `GetSystemTick`, `GetThreadId`; process exit terminates blocked and computing threads (`LGP_BACKEDGE`). Acceptance: `runtime.threads_lifted(_checked)` lifts our own A64 programs (`tests/runtime/thread_programs.s`: producer/consumer via guest mutex and condvar following the Horizon protocol, priorities with preemption, spin waits without SVC) and runs them in all three modes; baton and core baton produce the same log in the expected order on every run, free mode the correct result. `runtime.threads_svc(_checked)` checks the SVC semantics individually, native and in Node (Emscripten now with pthreads). Limitations: the programs are hand-coded, not a libnx build (comes with P2.8/P3.3); no priority inheritance for mutexes. Knowledge doc: [`horizon-threads-sync-in-eigener-runtime.md`](knowledge/horizon-threads-sync-in-eigener-runtime.md).
- [x] **P3.3 IPC framework (HIPC/CMIF) + `sm`**
  Location: Cloud · Deps: P3.2 · Size: M
  Acceptance: a fixture opens a service via `sm` and calls a command.
  Status (2026-10-11): `runtime/hle/` (library `lgp_runtime_hle`): HIPC parser and response (all descriptor types X/A/B/W/C, copy/move handles, PID), CMIF sessions with domains (sub-objects, in-objects, object close), control commands (`ConvertCurrentObjectToDomain`, `CopyFromCurrentDomain`, `Clone`, `QueryPointerBufferSize`), `sm:` with `RegisterClient`/`GetServiceHandle`. Services are HLE and run synchronously on the calling guest thread; unknown commands halt the guest. Kernel: `ConnectToNamedPort`, `SendSyncRequest`, `SendSyncRequestWithUserBuffer`, `GetProcessId`. Acceptance: `runtime.ipc_lifted(_checked)` lifts `ipc_main` (`tests/runtime/thread_programs.s`), which opens the test service `lgp:test` via `sm:` and calls its command 0; `runtime.ipc_hle(_checked)` checks formats, domains and error cases, native and in Node. Limitation: hand-coded A64 instead of a libnx build (needs the loader, belongs to P3.1). Knowledge doc: [`horizon-ipc-hipc-cmif-sm.md`](knowledge/horizon-ipc-hipc-cmif-sm.md).
- [x] **P3.4 Base services**: `set`, `time`, `apm`, `am`/`applet` (as far as needed for boot), `fs` with RomFS from a local file
  Location: Cloud (code) + PC (test) · Deps: P3.3, P1.3 · Size: L
  Acceptance: milestone **"game boots up to the first graphics call"** on the PC, without crash, with log.
  Status (2026-10-11): all three code parts: `appletOE` (proxy, messages with event, launch parameters incl. PreselectedUser, storage also via transfer memory), `set`, `time:u` (one clock, fixed zone without tzdata, snapshots), `csrng` (deterministic), `pctl`, `acc:u0`; console values centralized in `lgp::hle::SystemConfig`, HLE events (`lgp::hle::Event`) and buffer helpers for all further services. Tests `runtime.hle_services(_checked)`. Part 2: `fsp-srv` with RomFS as image (`/romfs.bin` or virtual from the extracted `/romfs/`), system archives by DataId and SaveData on the save `IStorage` (without journaling, created on first open); tests `runtime.hle_fs(_checked)` check the image with a parser of our own. Part 3: the silent services `prepo:u`, `arp:r`, `btm:u`, `bt`, `lm` (success without effect, in `register_base_services`). Plus the software keyboard as library applet (`CreateLibraryApplet` 0x11, text check via interactive data, text from the host via `AppletState::set_keyboard_handler`). Boot program (`runtime/boot/`, `lgp_boot`/`lgp_game`): ties together loader, kernel, HLE services and lifted code, takes over rtld's tasks (relocations as when lifting, `nn::ro` globals, `nn::init::Start` with host callbacks, `DT_INIT` in reverse load order), checks the module layout against the module table emitted by the codegen and stops cleanly at the first graphics call (`nvnBootstrapLoader`, `nv::InitializeGraphics` & co.). Demonstrated with an SDK-like `main`+`sdk` pair and the libnx fixtures `basic`/`threads` (identical debug output) and new `services` (libnx clients against sm, applet, set, time, csrng, acc, fsp-srv with RomFS/save, swkbd); tests `ctest -L boot`. Added for this: `apm` (libnx' `appletInitialize` needs it), cloning of domain sessions. With `--graphics hle` the NVN HLE is connected (bootstrap loader replaced, `nv::` driver setup skipped, output via soft, WebGPU or window renderer, `--frames N`); in the test pair `main` renders the three frames of the P4.1 guest program. Still open for the check: the boot test on the PC per `08-boot-test-pc.md`, measurement point is the graphics stop. Knowledge doc: [`sdk-prozessstart-ohne-rtld.md`](knowledge/sdk-prozessstart-ohne-rtld.md). Knowledge docs: [`horizon-basisdienste-am-set-time-acc.md`](knowledge/horizon-basisdienste-am-set-time-acc.md), [`horizon-fsp-srv-romfs.md`](knowledge/horizon-fsp-srv-romfs.md).
  Met (2026-10-11, PC): the real game boots recompiled up to `nv::InitializeGraphics`, exit 0, in 0.5 s. Fixed along the way: wrong noreturn in function discovery, jump tables with a base before the loop head, `GetInfo(UserExceptionContextAddress)`, `hid` registered in the boot program, argument field (argc/argv). With `--graphics hle` new `vi:u/s/m`; the run goes through NVN initialization and then halts in game code (open, P4). Details: `09-boot-test-ergebnis.md`.

- [ ] **P3.5 Remaining services per `02-services.md`** (service by service, one substep per thread)
  Location: Cloud + PC · Deps: P3.4 · Size: XL
  Acceptance: no more "unimplemented service/command" in the log up to the title screen.
  Status (2026-10-13, part 1, Cloud): up to Route 1 and, according to play tests, until after the first gym, LGP only calls existing services; a log proof from the PC is still pending for the check. The gaps are in communication, internet and Pokémon GO. Part 1 turns the runtime into a console without internet, alone in the room: `nifm:u` (requests fail with 2110-1100, `GetAppletInfo` names netConnect, which returns 2110-1100), `ldn:u` (complete state machine, scan empty, connect 203-0065, own network with one node), `friend:u` (objects, play history open), `acc` without Nintendo Account (124-0200), library applets error (0x0E, logged and closes), netConnect (0x0F) and playerSelect (0x10, the one user or cancel 124-0001 when NSA is required or the user is excluded). Tests `runtime.hle_services(_checked)`. Open: PC run through the communication menu and internet features (checklist in Analysis 22), Pokémon GO (`bt` BLE), `bsd:u`/`bcat:u` if reached. Knowledge doc: [`horizon-offline-konsole-nifm-ldn-friend.md`](knowledge/horizon-offline-konsole-nifm-ldn-friend.md).
- [x] **P3.6 Platform interfaces** `IRenderer`, `IAudio`, `IInput`, `IStorage` + desktop backends (stub/SDL)
  Location: Cloud · Deps: P3.1 · Size: M
  Acceptance: the runtime compiles without direct platform dependencies outside `platform/`.
  Status (2026-10-11): interfaces in `platform/include/lgp/platform/` (`IRenderer` with WebGPU objects as opaque pointers, `IAudio`, `IInput`, `IStorage`, bundle `Platform`). The runtime's OS code (guest memory reservation, host threads, memory size per target) now lives in `lgp_platform`. Stub backends for all targets in `platform/common` (`NullRenderer`, `NullAudio`, `ManualInput`, `MemoryStorage`, `HostDirectoryStorage` with path sandbox). Acceptance: `platform.no_host_deps` checks on every ctest run that `runtime/` contains no OS, window, audio or browser headers and no target platform macros; `platform.backends` tests the backends natively and in Node. Limitation: SDL and WebGPU desktop backends are still missing, they come with P4.1/P5 (P4 chooses the WebGPU library). Design: [`platform-interfaces.md`](design/platform-interfaces.md).

## P4 Graphics — needs Paul's approval to start

- [ ] **P4.1 Graphics front end per ADR-0003** (NVN HLE or nvdrv/Maxwell LLE) up to "clear color visible"
  Location: Cloud (code) + PC (test) · Deps: P3.4, P3.6, P1.4 · Size: L
  Acceptance: first frame (even just a clear) in the desktop window.
  Status 2026-10-11 (part 1, Cloud): NVN HLE in `gpu/nvn/` (thunks for the proc slots in the dispatch table,
  Device/Queue/MemoryPool/Buffer/Texture/Window/Sync, `CommandBuffer` recording up to clear, CPU reference backend with
  present via `IRenderer`). 120 of the 230 functions used; a lifted test program of our own loads NVN via the
  bootstrap loader, clears and presents (`ctest -L nvn`). Acceptance open: real `main` up to graphics (P3),
  window backend (P4.4), verification of object sizes/enums/signatures against our own `sdk` (PC). Details:
  `docs/knowledge/nvn-hle.md`, `docs/analysis/06-nvn-hle-abdeckung.md`.
  Status 2026-10-11 (part 2, Cloud): drawing path in the HLE: state objects as 4-byte references (no guessed
  object size), programs with runtime translation to WGSL, samplers, texture/sampler pools, bindings, texel uploads,
  `CopyBufferToTexture`, draws (also indexed); the WebGPU backend executes blend, masks, depth/stencil, culling and
  textures. 205 of the 230 functions used. A lifted AArch64 test program of our own draws with our own
  Maxwell shaders and is checked pixel-exactly, natively (lavapipe) and as WASM in the browser (Chromium/SwiftShader).
  Open as above, plus checklist items 7–10 (array strides, enums/default values, handle base, state lifetime).
- [ ] **P4.2 Shader translator Maxwell → IR → WGSL/SPIR-V/MSL** (offline tool + runtime fallback)
  Location: Cloud (tool, own test shaders) + PC (real shaders, local) · Deps: P1.4 · Size: XL
  Acceptance: all shaders counted in P1.4 translate without errors; output stays local.
  Status 2026-10-11: early spike in `gpu/shader/` (decoder checked against envydis, IR, WGSL backend for VS/FS,
  `maxwell2wgsl`); our own test shaders run correctly on naga, Tint and wgpu-native. Acceptance open: no
  BNSH parser, coverage of the real shaders not measured. Details: `docs/knowledge/maxwell-nach-wgsl.md`.
  Status P4.2 (2026-10-11, PC): BNSH parser `tools/analyze/bnsh.py` reads all 8,401 containers (5,169 stages: 3,423 VS, 1,722 PS, 24 CS); measurement `tools/analyze/shader_coverage.py`. With 8 new opcodes, exact FMUL/I2F.RZ and TEXS.DC, `maxwell2wgsl` translates VS 99.9%, PS 93.7% (before 2.5% / 60.3%), all outputs naga-valid; of the BNSH shaders used up to Route 1, 95.6%. Open until acceptance: compute (24), TMML (94), TXQ/TLDS (17), TEX.AOFFI (19 boot shaders). Details: `07-shader-abdeckung.md`, format [`bnsh-format.md`](knowledge/bnsh-format.md).
  Status of gaps (2026-10-11, Cloud, PR #34): compute (global/shared/local memory, atomics, barriers also in divergent code), TMML (approximated), TXQ, TLDS, TEX.AOFFI/LB in the translator; compute dispatch in NVN HLE and WebGPU backend with write-back into guest memory. Demonstrated with our own test shaders (pixel/buffer check on lavapipe, Tint, guest program natively and in the browser). Open until acceptance: re-measurement on the real shaders (PC, `shader_coverage.py` now translates compute too), layout of the compute control area (workgroup/shared size) and `c0[0x110]` against the real `sdk` (checklist 11/12 in `nvn-hle.md`).
- [ ] **P4.3 Textures, buffers, formats** (BCn/ASTC → what WebGPU can do, swizzle/block linear)
  Location: Cloud + PC · Deps: P4.1 · Size: L
  Acceptance: title screen displayed correctly (visual comparison with Ryujinx, screenshots only local).
  Status 2026-10-11 (PC, before P4.3): the real game runs with `--graphics hle` in its render loop (600 frames in
  7 s, a good 35 draws per frame via WebGPU), the frame is black: texture formats 59–78/121 unknown, texture data from
  memory pools is not uploaded. Added for this: real `nvnDeviceGetInteger` values, `CallCommands`,
  `pl:u`/`hwopus`/`audout:u`, scheduler yield after IPC, WebGPU deadlock and attachment sizes. Details:
  `10-nvn-pruefliste-und-renderschleife.md`.
  Status 2026-10-11 (PC, P4.3 part 1): **first visible frame.** The game's controller selection appears in
  correct colors (1280×720 after crop, 100% non-black, ~12,800 colors). For this: measured format table and
  block-linear upload from memory pools (BCn, 16-bit widened), texture handles resolved at submit (slot stride 2),
  uniform block `i` = bank `3 + i` and compiler constants as `c[1]` (measured on the driver), crop in
  window coordinates, texture view swizzle in the shader. Open until acceptance: separable programs (1,074 calls),
  ASTC (20 textures), 3D textures, `CopyTextureToTexture`/`ClearTexture`, input up to the title screen. Details:
  `11-erstes-bild.md`, [`tegra-block-linear-texturen.md`](knowledge/tegra-block-linear-texturen.md).
- [ ] **P4.4 Renderer backend WebGPU (desktop via Dawn or wgpu-native)**
  Location: Cloud + PC · Deps: P4.1 · Size: L
  Acceptance: the same title screen via the WebGPU backend on the desktop.
  Status 2026-10-11 (part 1, Cloud): library chosen: wgpu-native 29.0.1.1 on the desktop, emdawnwebgpu in the
  browser (both standardized `webgpu.h`). `WebGpuRenderer` (`platform/webgpu/`, device, surface, all
  WebGPU calls on one GPU thread, in the browser the main thread), SDL3 window (`platform/desktop/`),
  `WebGpuBackend` (`gpu/webgpu/`) for the NVN HLE: textures, clears with scissor/mask/origin, depth/stencil
  clears, present with letterbox, draws with translated Maxwell shaders (vertex/constant buffers from guest memory,
  since P4.1 part 2 also textures, blend, depth/stencil and raster state). Tests: differential test against the CPU reference, lifted
  NVN guest program on WebGPU, draw test with our own shaders, demo in the window (Xvfb) and in the browser (Chromium),
  all on lavapipe/SwiftShader. Acceptance open: title screen needs the P3 boot and P4.3. Details: `docs/knowledge/webgpu-backend-desktop-und-browser.md`.
- [ ] **P4.5 In-game graphics**: overworld, battle, menus
  Location: PC · Deps: P4.3, P4.4 · Size: XL
  Acceptance: first route + one battle without gross graphics errors.

## P5 Audio, input, saves — needs Paul's approval to start

- [ ] **P5.1 Input** (`hid`, Joy-Con emulation incl. motion controls as fallback mapping) → `IInput`
  Location: Cloud + PC · Deps: P3.5 · Size: M
  Acceptance: game controllable via gamepad/keyboard up to the first route.
  Status (2026-10-11): part 1 of 2 as code: `hid` HLE (`register_hid`, `lgp::hle::HidState`) with the 0x40000-byte shared memory in libnx layout (offsets via `static_assert`, compiled once against libnx `hid.h`), npad configuration (supported styles/IDs, single/dual, hold type, `DisconnectNpad`, style set events), SixAxisSensor with start/stop, fusion, zero drift and rest detection, vibration to `IInput::set_vibration`, touchscreen/DebugPad/gesture activation. The host drives the LIFOs with `HidState::update()`. Tests `runtime.hle_hid(_checked)`, native and in Node. Status part 2 (2026-10-11): backends behind `IInput`. The shared mapping `PadInput`/`ControllerMapper` (`platform/common`) delivers handheld or a single Joy-Con, dead zones, triggers and a synthetic throw motion without gyro; the keyboard mapping follows Ryujinx's model. Desktop: SDL3 gamepads (buttons by label, gyro/acceleration, rumble) and keyboard, in the boot program with `--renderer window` (`--input`, `--controller`). Browser: Gamepad API and keyboard (`lgp_platform_web`). Tests: `platform.pad_input`, `platform.sdl_input` (virtual SDL gamepad) and `browser.web_input` (Playwright presses keys). Open until acceptance (checklist `11-pc-abnahme-p5.md`): Poké Ball Plus (deferred); check on the PC with the real game whether the throw gesture is recognized, how the sensor axes are oriented and whether sideways-held Joy-Cons need to be rotated. Knowledge docs: [`horizon-hid-npad-shared-memory.md`](knowledge/horizon-hid-npad-shared-memory.md), [`host-input-auf-switch-controller.md`](knowledge/host-input-auf-switch-controller.md).
- [ ] **P5.2 Audio** (`audren`/`audout`, Opus/ADPCM decoding) → `IAudio`
  Location: Cloud + PC · Deps: P3.5 · Size: L
  Acceptance: music and effects without dropouts over 60 s of play time.
  Status (2026-10-11): `hwopus` decodes with libopus 1.5.2 (`cmake/LgpOpus.cmake`, static for native and Emscripten, `LGP_ENABLE_OPUS`; without libopus, silence of the correct length): `OpenHardwareOpusDecoder`, `GetWorkBufferSize` (size assumed, not measured), `DecodeInterleaved(WithPerf)Old` with Nintendo's framing and final-range check. `audout:u` applies the volume and discards what the bounded backend queue (100 ms) does not accept (drift of host clock vs. sound card). Backends behind `IAudio`: SDL3 (`lgp_platform_sdl_audio`, `SDL_AudioStream`) and WebAudio (`lgp_platform_web`, an AudioWorklet reads an SPSC ring directly from the shared Wasm memory). Boot program: `--audio none|sdl`. Tests `runtime.hle_hwopus(_checked)` (our own signal encoded with the libopus encoder, bit-identical and correlation > 0.98), `runtime.hle_services` (5.1 downmix, volume, discarding), `platform.sdl_audio` and `boot.sdk_audio_sdl` (SDL dummy driver), `platform.audio_ring` (native and in Node, there with the real worklet code), `browser.web_audio` (Chromium). The browser boot program picks WebAudio automatically (`boot::host_devices`). Open (checklist `11-pc-abnahme-p5.md`): acceptance on the PC with the game (audible, 60 s without dropouts), check the 5.1 channel order in the game, work buffer size and the console's error codes. Knowledge doc: [`horizon-audout-hwopus.md`](knowledge/horizon-audout-hwopus.md).
- [ ] **P5.3 Saves** (`fs` SaveData, `account`) → `IStorage`
  Location: Cloud + PC · Deps: P3.5 · Size: M
  Acceptance: saving, restart, loading works; saves are only local.
  Status (2026-10-11): save data is journaled as on the console. The game works on a working copy, only `Commit` makes it permanent; without `Commit`, closing discards the changes. The switchover goes through a one-byte journal file instead of a folder rename, so it also works on OPFS. `Commit` with an open write file returns 0x327202 like the console. Saves from before P5.3 and files placed by hand are adopted at mount. On the desktop `commit()` syncs written files and changed folders down to disk (`fsync`, macOS `F_FULLFSYNC`, Windows `FlushFileBuffers`), so saves survive a power failure too. `RenameDirectory` copies where the storage cannot rename folders (WasmFS/OPFS: `EBUSY`). Browser: "Export saves" as ZIP (only the committed state, neutral layout) and "Import saves…" (also deflate ZIPs from OS tools). Tests: `runtime.hle_fs` (commit semantics, crash at every point of commit and adoption, rename by copy), `browser.opfs_storage` (journal across a page reload on real OPFS), `browser.save_backup`, `web.page_logic`. Open: acceptance on the PC with the game, see `11-pc-abnahme-p5.md`. Knowledge docs: [`horizon-fsp-srv-romfs.md`](knowledge/horizon-fsp-srv-romfs.md), [`opfs-und-dateiauswahl-im-browser.md`](knowledge/opfs-und-dateiauswahl-im-browser.md).
- [ ] **P5.5 In-game videos** (`movie::` from subsdk1 as HLE, ADR-0005) → `IVideo`
  Location: PC (real game) + Cloud (demuxer, backends) · Deps: P4.3 · Size: M
  Acceptance: title video plays on the desktop and in the browser, the game continues afterwards.
  Status (2026-10-11): desktop met. Our own WebM demuxer, `platform::IVideo` with an FFmpeg backend (optional, `LGP_FFMPEG_DIR`) and black frames without a backend; the HLE serves extractor, decoder, buffer and MediaData and sets the decoder events. LGP shows the title video and gets into the language selection with A. Browser backend: WebCodecs on its own worker pthread (`WebVideo`, test `browser.web_video` with a self-generated VP9 video); acceptance with the real game open, Safari unverified. Audio track: Vorbis decoding (libvorbis) and audio decoder in the HLE modeled on Android, on by default (`--no-movie-audio` turns it off, tests `runtime.hle_vorbis`, `runtime.movie_hle`); LGP plays it via its own 2-channel `audout` and clocks the video by it, checked on the PC. Open: behavior at the end of the video. Knowledge doc: [`movie-api-hle.md`](knowledge/movie-api-hle.md).
- [ ] **Way into the game (PC)**: as of 2026-10-12 LGP runs through the title video, language selection and intro into the player's room, the character is controllable; room still black, character in T-pose (compute programs, shader gaps, depth sampling, integer vertex formats). Procedure and causes: `17-intro-und-erste-ingame-szene.md`.
- [x] **P5.4 Desktop playthrough test** (samples: start, first gym, saving/loading)
  Location: PC · Deps: P4.5, P5.1–P5.3 · Size: L
  Acceptance: log in `docs/analysis/` without blocking errors.
  Status 2026-10-13: **passed.** Paul got until after the first gym without a blocking error ("ja alles super" (yes, all great)). Log: `24-durchspieltests-p5-4-p6-5.md`.

## P6 WASM/browser — main milestone — needs Paul's approval to start

Status 2026-10-13 (overview of all open P6 items with acceptance, status and ownership): `23-p6-abschluss-stand.md`. In short: P6.1 met; P6.2 met at its core, the manual check in the browser coincides with P6.5; P6.3 met (PC, warm 30.0 fps in three scenes; #137 merged; iPad only stable with COMPUTE_FIRST off); P6.2, P6.4 and ★ P6.5 checked off after Paul's playthrough test in the browser (2026-10-13, `24-durchspieltests-p5-4-p6-5.md`). **P6 is complete**; P7 needs Paul's approval to start.

- [x] **P6.1 Emscripten build of the whole project** (memory model and threads per ADR-0001/0002)
  Location: Cloud (fixtures) + PC (real game) · Deps: P2.8, P5.4 · Size: L
  Acceptance: game starts in the browser up to the first graphics call (local server with COOP/COEP, if threads).
  Status 2026-10-11 (Cloud): the boot program (`runtime/boot`, `lgp_add_boot_executable`, so also `lgp_game`) builds
  under Emscripten and becomes the page `<ziel>.html` with local folder selection there; it reads modules, NPDM, RomFS and saves
  from OPFS (`platform/web`, WasmFS). For this the project builds under Emscripten with
  `-fwasm-exceptions` throughout. Demonstrated with `browser.opfs_storage` and homebrew fixtures in the browser (`browser.boot_*`).
  Acceptance open: build `lgp_game` from the real lift output for WASM and start it on the PC in the browser up to the first
  graphics call. Knowledge doc: [`opfs-und-dateiauswahl-im-browser.md`](knowledge/opfs-und-dateiauswahl-im-browser.md).
  Status 2026-10-11 (PC): **acceptance met.** `lgp_game` (lift output main + sdk) builds to 133 MB of WASM, starts in
  Chromium from OPFS and shows the controller selection as natively (frame 300: 100% non-black, ~12,450 colors). For this
  `lgp_romfs_pack` (RomFS as one image instead of 27,818 files) and `--memory 3072` on the game page (LGP needed more
  than the then 2816 MiB from ADR-0001; since 2026-10-11, 3072 MiB is the web default). Details: `13-browser-abnahme.md`.
- [x] **P6.2 Web platform**: file picker/OPFS for game data (no server delivery), WebGPU, WebAudio (AudioWorklet), Gamepad API, OPFS saves
  Location: Cloud + PC · Deps: P6.1 · Size: L
  Acceptance: title screen in the browser, data only from a locally chosen file.
  Status 2026-10-11: file picker → OPFS, OPFS as `IStorage` for game data and saves, canvas binding of the
  WebGPU renderer (`--canvas`) and dev server with COOP/COEP are in place (P6.1). Open: WebAudio and Gamepad API
  (with P5.1/P5.2), title screen.
  Status 2026-10-13: **met.** The game runs in the browser from OPFS after local folder selection; Gamepad API, WebAudio and OPFS saves checked by hand in the P6.5 run.
- [x] **P6.3 Performance** (profiling, code size, startup time, shader cache, function splitting if needed)
  Location: PC + Mac · Deps: P6.2 · Size: XL
  Acceptance: stable 30 fps (the game's target rate) in Chrome on the PC.
  Status 2026-10-13 (PC): acceptance **passed on the #137 branch** (COMPUTE_FIRST on, full run accidentally with `LGP_FP_HOST_NAN=OFF`; control run with ON in Pallet Town also 30.0 fps, 0 windows below 27, no frame errors): Chromium with host clock and frame pacing, warm 30.0 fps each in Pallet Town, on a route and in battle, no 1-s window below 27 fps. Main without COMPUTE_FIRST reaches 37–39 fps unthrottled; acceptance on main is therefore likely, but not measured. Checked off 2026-10-13: #137 is merged (main a6b73a6); on touch devices COMPUTE_FIRST is off because the iPad crashes with it (open). What was measured was the #137 state before the merge; there is no separate measurement on main. Figures in `23-p6-abschluss-stand.md`.
- [x] **P6.4 Browser matrix**: Chrome/Edge, Firefox, Safari (Mac)
  Status 2026-10-11: Safari 27.2 and Firefox 157 (Linux, browser tests without the game, `docs/knowledge/firefox-als-zielbrowser.md`)
  measured; open: Firefox on Windows/macOS with the real game, Edge.
  Location: PC + Mac · Deps: P6.3 · Size: M
  Acceptance: matrix in `docs/analysis/`; at least Chrome fully playable. Scope (Paul, 2026-10-13): Chrome and Safari are enough, Firefox and Edge stay unverified.
  Status 2026-10-13: **met.** Matrix in `23-p6-abschluss-stand.md`; the P6.5 run demonstrates Chrome fully playable (Playwright Chromium 1223, not a retail Chrome).
- [x] **★ P6.5 Milestone "playable in the browser"**
  Location: PC · Deps: P6.3, P6.4 · Size: M
  Acceptance: playthrough test like P5.4, but in the browser.
  Status 2026-10-13: **passed** (PC, main a6b73a6, Playwright Chromium 1223 with real GPU). Paul played with a Pro Controller and sound, battle and catch, save/close tab/reload/load, export/import ("lädt korrekt" (loads correctly); "alles super (gui um das spiel aber spiel selbst super!" (all great (the GUI around the game, but the game itself great!)). No blocking errors; start 10–13 s; stutters the first time effects appear (pipelines are created on first use, windows down to 23.2 fps). Sample starting from Paul's save, not a run from a new game up to the gym; the gym part is demonstrated on the desktop (P5.4). Log: `24-durchspieltests-p5-4-p6-5.md`.

## P7 Native apps on all devices — planning only, start only after P6 and Paul's approval

Per [ADR-0008](decisions/0008-native-apps-auf-allen-geraeten.md) (accepted 2026-10-13): one app per platform from the same lift output, shared SDL3 shell, renderer = existing WebGPU renderer via wgpu-native (Metal/Vulkan/D3D12), Dawn only as a fallback. The apps contain translated game code: build only locally (PC/Mac), install only on one's own devices, never distribute from CI. Game data initially never went into the app package; since 2026-10-14 (Paul) it may (`LGP_APP_GAME_DIR`, Analysis 28), the packages then stay local. Order: desktop → iOS/iPadOS → Android → tvOS (conditional).

- [ ] **P7.1 Desktop apps Windows and macOS** (almost there: `lgp_game` with SDL3 + wgpu-native)
  Location: PC + Mac · Deps: P6.5 · Size: M
  Content: start by double-click without command line; start UI (choose and remember the game data folder, controller mode, back up/restore saves), saves and pipeline cache in the platform's user folders; macOS as `.app`, Windows as a folder with `.exe` and DLLs; video via libvpx instead of optional FFmpeg (ADR-0008).
  Acceptance: Paul starts on PC and Mac without command line and plays up to the first gym (log like P5.4).
  Start approved by Paul on 2026-10-13. Substeps (one PR each, knowledge doc in [`desktop-app-huelle.md`](knowledge/desktop-app-huelle.md)):
  - [ ] **P7.1a App mode**: `lgp_game` without arguments (or `--app`) reads `settings.ini` from the platform's user folders, asks for the game folder via a system dialog on first start and checks it; saves, log and pipeline cache in the user folders (pipeline cache also without `LGP_PRIVATE_DIR`); fullscreen (`--fullscreen`, F11). Size: S
  - [ ] **P7.1b Packages**: macOS `.app` (Info.plist, icon, ad-hoc signed, without console), Windows folder with `.exe` (GUI subsystem, icon) and all DLLs; a local CMake target builds the package. Size: S–M
  - [ ] **P7.1c Start UI** in the game window, operable with mouse and controller (proposal: Dear ImGui via the existing WebGPU renderer, MIT license, later usable on iOS/Android/tvOS too): game folder, controller mode and assignment, fullscreen, back up/restore saves, pause menu in the game. Size: M
  - [ ] **P7.1d Video via libvpx** instead of optional FFmpeg (ADR-0008), so the app shows videos without FFmpeg DLLs. Size: S–M
  - [ ] **P7.1e Acceptance** on PC and Mac as above.
- [ ] **P7.2 iOS/iPadOS native** ([ADR-0007](decisions/0007-ios-nativ-statt-wasm.md), accepted 2026-10-11): the same generated C++ with clang for `arm64-apple-ios`, no WASM. No JIT needed (everything translated ahead of time). Renderer = existing WebGPU renderer via wgpu-native (built from source, ADR-0008) or Dawn on Metal. Memory via the entitlement `increased-memory-limit` (possibly `extended-virtual-addressing`). Locked until ★ P6.5.
  Location: Mac · Deps: P6.5 (P7.1 recommended) · Size: L (sum of the substeps)
  Acceptance: game starts on an iPad or iPhone up to the title screen.
  - [ ] **P7.2a Memory feasibility** (first, before any port code): a small test app that, with the entitlements, prints `os_proc_available_memory()`, reserves the guest block (≈ 3.4 GiB, like `HostBlock`) and writes it step by step until iOS terminates the app.
    Status 2026-10-13: probe built (`tools/memprobe/`, procedure [`speicherprobe-mobil.md`](knowledge/speicherprobe-mobil.md)). iPad Pro M1 measured: with entitlements ≈ 4.2 GiB headroom above the guest block, without ≈ 1.6 GiB → `lgp_game` with both entitlements (assessment). iPhone 17 Pro Max open. Paul, 2026-10-13: mobile ports before the desktop apps (P7.1 rests after P7.1a).
    Location: Mac · Deps: P6.5 · Size: S
    Acceptance: table in `docs/analysis/` per test device: available memory with/without entitlement, whether the reservation succeeds, how much can be written. Decision whether the game (≥ 3048 MiB reported, 1.3–2.6 GiB real plus GPU memory) fits on the device; if not, stop and report to Paul.
  - [ ] **P7.2b Build path**: CMake toolchain/preset for iOS (Xcode generator), runtime, HLE and lift output compile for `arm64-apple-ios`; dependencies (wgpu-native or Dawn, SDL3, libopus, libvorbis, libvpx) built for iOS.
    Status 2026-10-14: preset `ios`, wgpu-native as a ready-made static iOS release (no own build needed), SDL3 with its own `main()`, app bundle with entitlements, `Documents/game` as the game folder ([`ios-app-bauen.md`](knowledge/ios-app-bauen.md)). First build on the Mac succeeded (full build 748 s), signed with both entitlements and installed on the iPad Pro M1. The first start ran until shortly before the first frame (limit error, fixed in main, retest on the device open). Open for acceptance: the `ctest` part in the iOS simulator.
    Location: Mac · Deps: P7.2a · Size: M
    Acceptance: `lgp_game` installable as a signed app on a device; the `ctest` part that runs without a device (runtime/HLE unit tests in the iOS simulator) green.
  - [ ] **P7.2c Platform backends** under `platform/ios/` or via SDL3: Metal surface for the WebGPU renderer, audio, controllers (MFi/Xbox/PlayStation/Joy-Con), `IStorage` for game data (selection from the Files app into the app container, never bundled in the app) and saves.
    Location: Mac · Deps: P7.2b · Size: M
    Acceptance: a homebrew fixture boots on the device; `platform.no_host_deps` stays green.
  - [ ] **P7.2d Game up to the title screen** on the device, incl. the video path (libvpx per ADR-0008, otherwise FFmpeg framework or VideoToolbox).
    Location: Mac · Deps: P7.2c · Size: M
    Acceptance: title screen; frame rate and memory peak (`os_proc_available_memory()`) in `docs/analysis/`.
  - [ ] **P7.2e Touch controls** (optional, if no controller): on-screen overlay as `IInput`.
    Location: Mac · Deps: P7.2d · Size: M
    Acceptance: game start up to Route 1 with touch only.
- [ ] **P7.3 Android native** (answers ADR-0007 question 6 via ADR-0008): NDK build arm64-v8a, wgpu-native with Vulkan (from source), SDL3 Java shell, game data via folder selection (SAF) into app storage.
  Location: PC (NDK, `adb`) · Deps: P6.5, P7.1 · Size: L
  - [ ] **P7.3a Memory probe** like P7.2a on the OnePlus 7 Pro (Snapdragon 855, 12 GB). The Shield is not a target. Paul, 2026-10-14: the Shield is a target after all; model, memory and open points in Analysis 26, addendum to ADR-0008 open.
    Status 2026-10-13: the same probe as an `adb shell` program (NDK build, see `speicherprobe-mobil.md`); needs the Android NDK on the PC.
    Acceptance: table in `docs/analysis/`, decision per device; if no device suffices, stop and report to Paul.
  - [ ] **P7.3b Build and shell**: toolchain/preset Android, Gradle project with SDL3, dependencies built for Android, a homebrew fixture boots on the device.
  - [ ] **P7.3c Game up to the title screen**, then up to Route 1 with a controller; frame rate and memory peak in `docs/analysis/`.
- [ ] **P7.4 tvOS (Apple TV), conditional**: start only if **P7.4a** shows that it works.
  Location: Mac · Deps: P7.2 · Size: L
  - [ ] **P7.4a Feasibility**: memory (according to Apple, the entitlement `increased-memory-limit` does not exist for tvOS), storage of the 4.2 GiB of game data (according to the archived tvOS guide only in the purgeable cache folder), saves (only ~500 KB persistent locally → iCloud or sync with PC/Mac), transfer without a Files app, wgpu-native for a tier 3 Rust target or Dawn.
    Acceptance: report in `docs/analysis/` with measurement on Paul's Apple TV 4K (3rd gen, A15, 4 GB according to press reports); Paul decides on P7.4b.
    Status 2026-10-14: Analysis 28, conditionally feasible. The Rust target is tier 2 (no nightly). Open are memory (suspected ≈ 2.5 GiB limit, LGP needs 2.6–2.8 GB in the browser) and persistent saves (1 MiB, only CloudKit or sync). Preset `tvos`, wgpu build script and memory probe for tvOS are ready, nothing has been measured on the device.
  - [ ] **P7.4b Port** like P7.2b–d for `arm64-apple-tvos`, controller input only.
- Open questions on P7 (ADR-0008): paid Apple Developer account; macOS runner in CI (CI change, needs approval).

## P8 Gameplay mod — planning only, start only after P6 and Paul's approval

- [ ] **P8.1 Analysis of encounter/catch logic** in LGP and the battle system (BDSP only as a behavioral reference)
  Location: PC · Deps: P6.5 · Size: L
  Acceptance: `docs/analysis/` describes the affected functions in our own words.
- [ ] **P8.2 Mod mechanism in the recompiled code** (hook/patch layer in the recompiler instead of binary patches)
  Location: Cloud + PC · Deps: P8.1 · Size: L
  Acceptance: hook mechanism with tests; mod source code contains no game code.
  Status 2026-10-20 (Paul's approval for P8 in the project chat): hooks (`lgp_lift --hook/--mods`, `lgp/runtime/hooks.hpp`), private mod files `<game>/mods/<id>.cfg`, registry `lgp::mods` with `--mods`/`settings.ini`/menu, call log `--hook-log`; tests `indirect.*_hooks`, `runtime.mods`, `boot.sdk_hook_log`/`sdk_mods_*`. Knowledge doc: [`spiel-mods-hooks.md`](knowledge/spiel-mods-hooks.md), plan: `51-p8-wilde-kaempfe.md`. Patch sites at instructions: #350.
- [ ] **P8.3 Wild battles instead of the catch minigame**
  Location: PC · Deps: P8.2 · Size: XL
  Acceptance: battle against a wild Pokémon on Route 1 playable.

---

## Core decisions (ADRs)

| ADR | Topic | Decision (accepted 2026-10-10) | Data from |
|---|---|---|---|
| [0001](decisions/0001-gast-adressraum-wasm.md) | Guest address space in WASM | wasm32, compact guest layout < 4 GiB on all platforms, `TotalMemorySize` as switch (web 3072 MiB); memory64 only as plan B | P1.3: real need 1.29–2.55 GiB |
| [0002](decisions/0002-threads.md) | Threads | 1:1 real host threads, core model (max. one thread per guest core) and deterministic baton mode switchable | P1.3: 10–11 threads, ≈ 30,000 sync/s |
| [0003](decisions/0003-grafik-hle-lle.md) | Graphics HLE/LLE, shaders | NVN HLE via the proc slots + offline shaders with runtime fallback; abort criterion in P4.1 | P1.4: 230/498 NVN functions, 95% of shaders from data |
| [0004](decisions/0004-indirekte-spruenge.md) | Indirect branches, SMC/JIT | directly indexed dispatch table over all functions, one jump table detector, `musttail`, `setjmp`/`longjmp` via host variant, no interpreter | P1.5 |
| [0005](decisions/0005-sdk-nsos.md) | `sdk`/`subsdk*` | recompile `main` + `sdk` (SVC/IPC boundary), not `subsdk0`/`subsdk1`, replace their imports instead; targeted replacements by symbol | P1.1–P1.5 |

Short version of the decision basis: [`decisions/P1.6-entscheidungsvorlage.md`](decisions/P1.6-entscheidungsvorlage.md).

Paul accepted all five on 2026-10-10 (P1.6).

Further ADRs on P7: [0007](decisions/0007-ios-nativ-statt-wasm.md) iOS/iPadOS port as a native arm64 build instead of WASM (accepted 2026-10-11).
[0008](decisions/0008-native-apps-auf-allen-geraeten.md) Native apps on all devices (SDL3 + wgpu-native, Android native, tvOS conditional), accepted 2026-10-13.

## Research sources (as of 2026-10-10)

- N64Recomp: <https://github.com/N64Recomp/N64Recomp> (function lookup for indirect calls via a runtime table)
- XenonRecomp: <https://github.com/hedge-dev/XenonRecomp> (jump table detection via an analysis tool, function table behind guest memory)
- suyu 0.0.4 Recompiler mode: reports e.g. <https://xenospectrum.com/en/suyu-static-recompiler-performance-evidence/>, <https://korben.info/en/suyu-bows-out-recompilation-mode-switch-games.html> (primary source/README not checked)
- Ryujinx/forks: <https://emulation.gametechwiki.com/index.php/Ryujinx> (Ryujinx MIT; original discontinued; Ryubing/Kenji-NX as forks, status unclear after DMCA notices Feb 2026)
- Eden/Citron: status contradictory according to secondary sources; Eden active (v0.2.1, distribution via eden-emu.dev), Citron unclear. Only relevant as a reference for this project, the yuzu heritage is GPLv2+.
- memory64: <https://caniuse.com/wf-wasm-memory64> (Chrome/Edge from 133, Firefox from 134, Safari not enabled), WebKit bugs <https://bugs.webkit.org/show_bug.cgi?id=300538>, Emscripten docs <https://emscripten.org/docs/tools_reference/settings_reference.html> (`-sMEMORY64` deprecated → `--target=wasm64`), costs: <https://spidermonkey.dev/blog/2025/01/15/is-memory64-actually-worth-using.html>
- WebGPU: <https://web.dev/blog/webgpu-supported-major-browsers>, Firefox 147 (Apple Silicon) <https://www.firefox.com/firefox/147.0/releasenotes/>; Firefox/Linux not yet confirmed
- JSPI: <https://v8.dev/blog/jspi-newapi>; stack switching: phase 3, only behind a flag (<https://github.com/WebAssembly/stack-switching>, <https://ocaml.org/backstage/2026-04-16-wasm-of-ocaml-what-changed-since-6-1>)
