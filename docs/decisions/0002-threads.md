# 0002 – Threads: real WASM threads vs. cooperative scheduling

Status: **Accepted** (Paul, 2026-10-10) · Date: 2026-10-10 · Updated: 2026-10-10 (P1.6, measurement data from P1.3–P1.5) · Decides: Paul · Gate: data from P1.3

## Context

Switch games run on up to three cores with several threads (main/render thread, loading, audio, SDK system threads). Horizon schedules preemptively per core by priority. Recompiled code is ordinary C++: a guest thread waiting in `svcWaitSynchronization` or `svcArbitrateLock` **blocks on the native stack**. How many threads LGP uses and how they synchronize has been known since P1.3 (next section).

Browser situation (as of 2026-10):
- Real threads: Emscripten pthreads via Web Workers + `SharedArrayBuffer`. Needs cross-origin isolation (COOP/COEP headers). Since the build is only served locally anyway, we control the headers (local server or service worker workaround).
- Stack switching without threads: Asyncify (instruments every code path that can suspend, costs code size and run time, critical with megabytes of generated code), JSPI (available in Chrome from M126, Firefox by default from ~153 according to secondary sources, Safari unclear; JSPI only suspends at JS promise boundaries and is not a general fiber switch), Wasm Stack Switching (phase 3, only behind a flag, **not** usable).

## Measurement data (P1.3–P1.5)

Sources: `02-services.md` §1/§4/§5, `04-code-statistik.md`.

| Quantity | Value |
|---|---|
| Threads at the same time | **10–11 during gameplay**, max. 30 during the intro video (17 of them from `subsdk1`/multimedia) |
| Threads with real load | 3: `MainThread`, `JobThread1`, `JobThread2` (matching 3 cores) |
| Priorities | almost only 44 (34×) and 43 (4×); `SetThreadPriority` only 4×, all to 44 |
| Core affinity | 30× process default, 7× core 2, 1× core 1; cores 0–2 |
| Kernel sync per second (trace run, rather a lower bound) | `SignalProcessWideKey` ≈ 14,700, `ArbitrateLock`/`Unlock` ≈ 7,800 each, `WaitProcessWideKeyAtomic` ≈ 1,300, `WaitSynchronization` ≈ 130 |
| `GetCurrentProcessorNumber` | ≈ 4,800/s |
| Atomics in the code | no LSE atomics (ARMv8.0); `ldaxr`/`stlxr` 1,609× each in `main`, plus `ldar*`/`stlr*` |
| Where the SVCs are | **exclusively in `sdk`** (60 `SVC` instructions); `main` contains not a single one |
| Import call sites in `main` | `nn::os::LockMutex`/`UnlockMutex` 1,028/1,142, `nn::os::GetTlsValue` 4,488 |

### Critical review of the recommendation

1. **Cooperative scheduling (B) is out with these numbers.** Each of the roughly 30,000 kernel sync operations per second is a possible thread switch. With Asyncify, every switch would mean unwinding and rebuilding the complete call stack, and Asyncify would have to instrument ≈ 4.6 million recompiled instructions for that. On top of that, the job system deliberately spreads the load over three threads. On a single host thread we would only have a third of the compute time, with Wasm code that is slower than native anyway.
2. **1:1 real threads (A) are feasible in the browser.** 10–13 workers (video via HLE, i.e. without the 17 multimedia threads) are within limits; Emscripten can create them in a pool in advance. 30,000 futex operations per second are within a few percent of one core, natively as in Wasm (`Atomics.wait/notify`).
3. **New risk: `GetCurrentProcessorNumber` and core exclusivity.** 4,800 queries per second point to per-core data structures (job system, allocator; conclusion, not verified). On Horizon, threads of the same priority on the same core do not run at the same time and, to our knowledge, are not time-sliced against each other (Ryujinx's scheduler models it this way; not checked against real hardware). Code may rely on this. With 1:1 host threads, by contrast, any number of "core 0" threads run in parallel. Countermeasure built into P3.2: a **core model** in the runtime. At most one guest thread per guest core runs at the same time (core baton, three slots), handover at blocking SVCs or sync calls. This is the baton mode from option C, only with three slots instead of one, and models the hardware more accurately. Whether this is necessary or whether free parallel operation suffices will only be shown by the real run; both modes stay switchable.
4. **The sync path must be cheap, but it need not be an SVC path.** Because all SVCs are in `sdk`, ADR-0005 co-determines where we intercept: if we recompile `sdk`, `ArbitrateLock` etc. become direct runtime calls (no trap, no JS round trip). If we replace `nn::os` at API level, `LockMutex` etc. land directly on host primitives. Both are fast enough; the SVC variant is closer to the original and is recommended here (see ADR-0005).
5. **Exclusive pairs → CAS.** `ldaxr`/`stlxr` are modeled as compare-and-swap. This has the well-known ABA problem (CAS does not see an intermediate change A→B→A, LL/SC does). For usual lock/counter patterns this is uncritical; differences show up reproducibly in the differential test only in baton mode.
6. **Host stack size.** With native `RET` (ADR-0004) the host stack grows with the guest call depth. Emscripten threads only have 64 KiB of stack by default; guest stacks are up to 1 MiB (main thread according to the NPDM). Per thread, the host stack must be at least as large as the guest stack, plus reserve.

## Options

### A: 1:1 real host threads

Every guest thread becomes a host thread (desktop: `std::thread`; web: pthread/Web Worker). Horizon sync primitives are mapped onto futex/atomics; exclusive accesses (LDXR/STXR) onto C++ atomics/CAS.

- Pros: no stack switching problem, uses several cores, same model on desktop and web.
- Cons: not deterministic (differential tests and debugging harder), priorities can only be modeled roughly, Web Workers are more expensive than native threads, WebGPU/WebAudio calls must come from the right context (WebGPU in a worker works in Chrome; AudioWorklet separate).

### B: Cooperative scheduling (one host thread, fibers)

- Pros: deterministic, no `SharedArrayBuffer` needed.
- Cons: in the browser only realistic with Asyncify (too expensive for the code size) or with stack switching (not available). Spin waits in the game would hang without yield points. Uses only one core.

### C: Hybrid "baton" mode on real threads

Like A, but a global scheduler optionally lets **only one** guest thread run at a time and hands over at SVC boundaries according to Horizon priorities. Switchable at runtime.

- Pros: deterministic debug/test mode without fibers, also works in the browser (threads block on a futex), parallel mode for performance.
- Cons: two modes to test; spin waits without an SVC can hang in baton mode (needs timeout-based preemption or detection).

## Recommendation

**A with C as a built-in mode** (unchanged), extended by the core model:
- Recompiler and runtime assume real host threads (desktop `std::thread`, web pthreads/Web Workers).
- The runtime knows three operating modes, switchable at runtime: **free** (all guest threads in parallel), **core baton** (at most one running thread per guest core, i.e. at most three; default for the real game until measurements show otherwise) and **baton** (exactly one thread, deterministic; mandatory for differential tests according to ADR-0006).
- `GetCurrentProcessorNumber` returns the thread's guest core from the core model, not a host CPU number, and is an inline path.
- Host stack per thread ≥ guest stack + reserve.
- B is only re-evaluated if Wasm stack switching is available in all target browsers.

## Open questions / data needed

- ~~Number of threads, priorities, affinities, sync SVCs (P1.3)~~ → answered (table above).
- Spin waits without an SVC (P1.5): **not measured directly.** The statistics only count exclusive pairs (1,609 in `main`), not loops without an SVC. In baton mode such a spin shows up as a hang; the remedy is a timeout after which the baton is passed on.
- **New:** does LGP need core exclusivity, or does it also run in free mode? (P3.2/P3.3, comparison of both modes)
- **New:** check WebGPU calls from a worker (OffscreenCanvas) in Chrome, Firefox and Safari (P6.1), because the guest render thread cannot be a browser main thread (`Atomics.wait` is forbidden there).
- Cost of ~13 Web Workers (measurement in P6.1).

## Consequences

- Emscripten build with `-pthread` and `-sPROXY_TO_PTHREAD` (the game runs entirely in workers), pre-created thread pool, stack size per thread matching the guest; local serving with COOP/COEP headers (P6.1).
- P3.2 implements the core model (three operating modes) as part of the scheduler.
- Runtime sync (P3.2) must map guest atomics correctly onto Wasm memory (Wasm atomics are sequentially consistent, stricter than ARM64; correct, but possibly more expensive).

## Sources

- <https://v8.dev/blog/jspi-newapi>
- <https://github.com/WebAssembly/stack-switching>
- <https://ocaml.org/backstage/2026-04-16-wasm-of-ocaml-what-changed-since-6-1> (stack switching phase 3, Chrome only behind a flag)
