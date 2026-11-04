# Horizon process behavior that shapes a recompilation

Game-independent knowledge from P1.6 (status 2026-10-10, NintendoSDK 5.x, clang-built retail games). Concrete numbers for
LGP are in `docs/analysis/02-services.md` and `04-code-statistik.md`, not here. What is marked as a conclusion
was only observed on one game.

## Memory: heap size is not a measure of need

- Games query the available memory via `svcGetInfo(TotalMemorySize)` and take almost all of it with **one**
  `svcSetHeapSize`. Their own allocator then manages the heap itself. An emulator with more
  emulated DRAM simply gets a larger heap; the heap size says nothing about the need.
- For a custom runtime, `TotalMemorySize` is therefore the adjusting knob: if it reports less, the game gets a
  smaller heap. Whether a game runs stably with that depends on the game and cannot be tested with Ryujinx (no DRAM
  below 4 GiB). The custom runtime has to try that out via a switch.
- Estimating the real need: count pages in the heap that contain a byte ≠ 0 (Ryujinx `HostMappedUnsafe` maps the
  guest linearly, see `tools/analyze/ryujinx_heap_scan.ps1`). That is a lower bound; buffers written only by the GPU
  stay zero in guest memory in Ryujinx and live in the nvmap buffers (debug log of `nvdrv`). The upper bound
  is the sum of both.
- Retail code is position-independent (almost only `R_AARCH64_RELATIVE`); there are no fixed load addresses. The runtime
  may choose the layout freely, including putting everything below 4 GiB.

## Where system calls are

- In games using the NintendoSDK, typically **only `sdk`** contains `SVC` instructions and system register accesses
  (`tpidrro_el0`, `cntpct_el0`, FPCR/FPSR). The game's `main` calls the SDK, never the kernel. Observed on LGP; check for other
  games with statically linked third-party libraries.
- Consequence for the architecture: whoever recompiles `sdk` along with the game needs SVC and system register support only for `sdk`.
  Whoever replaces `sdk` at the API level does not need it at all, but must instead rebuild the SDK semantics.
- The IPC side is manageable: a game is a pure client (`SendSyncRequest`, one `ConnectToNamedPort("sm:")`,
  no `ReplyAndReceive`).

## Synchronization: many kernel halves

- `nn::os::Mutex`/`ConditionVariable` have a user-mode fast path (atomics) and only go into the
  kernel under contention: `ArbitrateLock`/`ArbitrateUnlock` (mutex), `SignalProcessWideKey`/`WaitProcessWideKeyAtomic` (condvar).
  Still, in job-system-heavy games these SVCs reach tens of thousands per second. A runtime must map them as
  direct function calls onto host futexes, not as an expensive trap or JS round trip.
- `GetCurrentProcessorNumber` is also called very often (per-core structures in the job system/allocator,
  conclusion). The runtime should return a stable guest core number 0–2, not a host CPU number.
- Horizon schedules per core by priority; threads of equal priority on one core never run at the same time. Whoever
  maps guest threads 1:1 onto host threads removes this guarantee. A core model (at most one running guest thread
  per guest core) restores it.
- NintendoSDK 5.x code uses no LSE atomics (ARMv8.0), only `ldaxr`/`stlxr` pairs and `ldar`/`stlr`. Emulation
  via compare-and-swap has the known ABA problem compared to real LL/SC; uncritical for common lock/counter patterns.

## Control flow in clang-built retail games

Supplements `ghidra-nso-headless.md` (section "Code patterns of NintendoSDK clang"):

- `RET` always via `x30`; C++ exceptions practically unused in game code (games mostly build with
  `-fno-exceptions`), but present in `sdk` (libc++). A native C++ return per guest function holds.
- Virtual calls dominate the indirect branches; more than half of all functions have their address taken. A
  dispatch table must therefore cover all function starts and be O(1).
- Indirect tail calls (`BR` from a loaded pointer at the end of a function) are frequent. Emulated as "call + return",
  the host stack grows; `[[clang::musttail]]` in the generated code avoids that.
- `setjmp`/`longjmp` occurs occasionally (libraries such as libpng/script VMs, conclusion). Because the host stack
  mirrors the guest stack, it can be mapped onto host `setjmp`/`longjmp`, without a block dispatcher.
- No JIT: retail NPDMs do not allow code memory SVCs. Dynamically loaded NROs (`ldr:ro`) are possible but rare;
  check via NPDM, RomFS (NRO/NRR files) and imports from `nn::ro`.

## Modules besides `main` and `sdk`

- `subsdk*` often contain large middleware that the game hardly needs. Examples: `glslc` (runtime GLSL compiler from
  the NVN SDK, often linked, but only reachable via an optional loader path) and `multimedia` (NVIDIA Stagefright
  port for hardware video via `nvdrv`/`mm:u`). Before recompiling, check with the Ryujinx PTC profile whether anything
  from it runs at all besides the module initialization (`ryujinx-caches-als-laufzeitdaten.md`); often it is enough to replace the few
  imports from `main`.
- `rtld` does not need to be recompiled if all imports can be resolved within the ExeFS: the recompiler links
  statically and calls the `.init_array` entries itself.
