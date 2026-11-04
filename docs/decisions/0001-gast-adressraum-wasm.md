# 0001 – Guest address space in WASM: memory64 vs. remapping into 32-bit memory

Status: **Accepted** (Paul, 2026-10-10) · Date: 2026-10-10 · Updated: 2026-10-10 (P1.6, measurement data from P1.3–P1.5), 2026-10-11 (web value 3072 MiB, see "Change 2026-10-11") · Decides: Paul · Gate: data from P1.3

## Context

The guest (Horizon OS, AArch64) works with 64-bit pointers in a 39-bit address space. Code, heap, stacks, TLS and mapped regions lie wherever the kernel puts them. Because we implement the kernel (the SVCs) ourselves, **we determine the layout**: the game receives addresses from `svcSetHeapSize`, `svcMapMemory`, `svcQueryMemory` etc. and should not expect fixed addresses (to be checked in P1).

WebAssembly offers:
- **wasm32**: one linear memory of up to 4 GiB (browsers now allow 4 GiB; in practice large allocations fail on weaker devices). No bounds checks needed, because engines use guard pages.
- **memory64/wasm64**: part of Wasm 3.0. According to caniuse available in Chrome/Edge from 133 and Firefox from 134, **Safari has not shipped it yet** (WebKit bugs show progress until April 2026, but no confirmed release). Browsers limit it to 16 GiB. In Emscripten, `-sMEMORY64` is now deprecated; the way to go is `--target=wasm64`/`-m64`. Cost: engines have to insert software bounds checks; SpiderMonkey itself writes that memory64 cannot reach the bounds-check freedom of wasm32.

The Switch has 4 GB RAM, of which a game gets roughly 3.2 GB. How much LGP **actually** uses was unknown at the time of the draft; the measurement data is in the next section.

## Measurement data (P1.3–P1.5)

Sources: `02-services.md` §1/§6, `01-inventar.md` §2, `03-grafik.md` §7, `04-code-statistik.md`.

| Quantity | Value | Assessment |
|---|---:|---|
| `SetHeapSize` | 3.14 GiB (with `TotalMemorySize` 3.21 GiB) | LGP takes **everything** that is reported (with 12 GiB DRAM, 10.7 GiB). Not a measure of need. |
| Heap pages with data ≠ 0, peak up to Route 1 | **1.29 GiB** | Lower bound for CPU-written data |
| nvmap buffers at the same time, peak | **1.26 GiB** | live in the heap, overlap with the row above unknown |
| reliable range of the real need | **1.29 GiB … 2.55 GiB** (hard upper bound 3.21 GiB) | only game start up to Route 1, handheld |
| `UsedMemorySize` before the heap (images, stacks, BSS) | 65 MiB | images together ≈ 72 MiB |
| Relocations of `main` | 71,687 `RELATIVE`, 2,316 `ABS64` (symbol-related) | code is position-independent; fixed load addresses are not needed |
| Memory SVCs | `MapMemory` 38 (thread stacks), `CreateTransferMemory` 597 (44 MiB), `MapSharedMemory` 2, no `MapPhysicalMemory` | simple layout, no physical memory model needed |
| Dispatch table (ADR-0004), directly indexed | ≈ 13 MiB (`main`) or ≈ 29 MiB (all modules) | lives in the same linear memory |

### Critical review of the recommendation

1. **The old gate value ("peak > 3 GiB → re-evaluate B") cannot be checked as stated.** Because LGP always takes almost everything via `SetHeapSize`, the "peak" is exactly what our runtime reports as `TotalMemorySize`. The actual question is: **does LGP run stably if we report less** (e.g. 2.5–2.75 GiB instead of 3.21 GiB)? Ryujinx cannot test this (no DRAM below 4 GiB); it becomes an experiment in P3.1.
2. **The data speaks for A rather than against it.** Even the pessimistic range (2.55 GiB) leaves around 1.4 GiB in 4 GiB for runtime, Emscripten heap, dispatch table, audio and upload buffers. With 2.75 GiB of reported memory ≈ 1.2 GiB would remain; with today's web value of 3072 MiB, 640 MiB remain (see "Change 2026-10-11"). That is tight but not critical: with WebGPU, GPU resources live in the GPU process, not in Wasm memory, and the 4.4 GB RomFS must never be loaded completely into linear memory anyway, but is read on demand from the file chosen by the user.
3. **Physical memory ≠ reserved memory.** Browser and operating system only back Wasm memory page by page on first write. Heap parts that are never written therefore cost hardly any real RAM. For devices with 8 GiB RAM (Paul's Mac) what counts is the range 1.3–2.6 GiB plus the host share, not the 3.14 GiB.
4. **New risk from P1.4 that also affects this ADR:** the game writes vertex/index/uniform data directly into mapped GPU memory (`03-grafik.md` §7.1). On the desktop, changed pages can be detected via page protection; **in Wasm there is no page protection** (no `mprotect`). That leaves: uploading the ranges a draw references (possibly with a hash), or a write marker in the memory abstraction. The latter would make every guest store more expensive. This ADR therefore specifies that the memory abstraction is built so that such a write path can later be switched on **only for pages in GPU pools** (e.g. via a page table with flags), without rebuilding the recompiler. The decision on the method is made in P4.1.
5. **As far as Safari is concerned, memory64 has not improved since the draft.** A search on 2026-10-10 found no source for a Safari release with memory64. For P7 (iOS/tvOS), B would therefore still be an exclusion criterion in the browser.

Conclusion: recommendation A stays, the gate is made more precise (see below).

## Options

### A: wasm32, compact guest layout with offset

Our SVC implementation places all guest regions densely in a range < 4 GiB. Every guest access becomes `mem_base + (uint32_t)(addr - guest_base)` (or identity, if `guest_base = 0`).

- Pros: best performance (no bounds checks), runs in **all** browsers incl. Safari/iOS, smaller code.
- Cons: hard limit. Guest memory + runtime + Emscripten heap + staging buffers for textures must stay below 4 GiB together. Pointer truncation is a silent error if the game does use high address bits after all (tagging/TBI); needs debug assertions in the desktop build.

### B: memory64, guest addresses 1:1

- Pros: no layout trick, guest addresses stay unchanged, no 4 GiB limit.
- Cons: no Safari (today), measurably slower due to bounds checks, younger toolchain paths (more bugs). Does not solve the actual problem if browsers do not really hand out several GiB.

### C: wasm32 + multi-memory (guest memory in a second memory object)

- Pros: separates guest and host heap, each up to 4 GiB.
- Cons: C/C++ code cannot address a second memory directly (Clang support only via special builtins); generated code would have to go through helper functions or inline Wasm. Very little tooling experience in the ecosystem. Plan C only.

## Recommendation

**A** (unchanged from the draft, gate made more precise), with four stipulations:
1. The recompiler emits **no** direct pointer accesses, only macros/inline functions (`lgp_mem_read32(va)` etc., see `docs/design/p2-interfaces.md`). Desktop build: 64-bit mapping (range reserved via `mmap`/`VirtualAlloc`) plus optional debug checks. WASM build: offset variant. memory64 thus becomes a build flag, not a rebuild.
2. **Same guest layout on desktop and web:** all guest regions (images ≈ 72 MiB, stacks, heap, shared/transfer memory) lie below 4 GiB in the desktop build too. Then guest addresses in logs, differential tests and config files are identical on all platforms.
3. **`TotalMemorySize` is a runtime switch.** Initial value for WASM: 3072 MiB (until 2026-10-11: 2816 MiB = 2.75 GiB). Desktop may report 3.21 GiB (like the console).
4. The memory abstraction gets an optional page-flag path for GPU pools (point 4 above), off by default.

**Gate (replaces "peak > 3 GiB"):** in P3.1/P3.3 the recompiled game boots on the desktop with 2.75 GiB reported up to Route 1 without memory errors (and, as a control, with 3.21 GiB). If that also fails with 3.0 GiB, B is re-evaluated. Until then memory64 stays working as a build flag, but untested.

**Result (2026-10-11):** 2.75 GiB is not enough; LGP aborts at startup below ≈ 3040 MiB; 3072 MiB runs natively and in Chromium. A therefore stays in force with 3072 MiB, B is not needed (see below).

Rationale: the measured need (1.3–2.6 GiB) fits into wasm32; performance (no software bounds checks) is the scarcest factor in the browser; Safari/iOS (P7) would be excluded for now with B.

## Open questions / data needed

- ~~Peak memory need (P1.3)~~ → answered: 1.29–2.55 GiB real, heap size follows the reported memory.
- ~~Fixed addresses? (P1.5)~~ → no, `main` is position-independent (`RELATIVE` relocations). **High pointer bits/TBI** were not measured; the debug assertion in the desktop build remains mandatory.
- ~~Double allocation of GPU memory (P1.4)~~ → GPU pools live in the guest heap (nvmap peak 1.26 GiB); the WebGPU copy lies outside Wasm memory. Only how changes are detected is still open (P4.1).
- ~~Does LGP run with 2.75 GiB `TotalMemorySize`?~~ → no, lower bound ≈ 3040 MiB; web value now 3072 MiB (change 2026-10-11).
- **New:** the need in later areas and in docked mode (1080p) has not been measured; a jump by gigabytes is considered unlikely, but this is only an estimate.
- **New:** measure on Paul's devices the largest wasm32 memory that Chrome, Firefox and Safari actually allocate with `SharedArrayBuffer` (because of ADR-0002) (P6.1).

## Change 2026-10-11: web value 3072 MiB

Accepted by Paul on 2026-10-11, subject to review (below).

**Reason:** the real game does not start with 2816 MiB. Right after `SetHeapSize` it ends in `svcBreak(reason 0)`; measured natively it aborts with 3025 MiB and runs with 3050 MiB. The game page therefore already ran with `--memory 3072`. The measured real need (P1.3: heap data ≈ 1.3 GiB plus GPU buffers ≈ 1.26 GiB, `SetHeapSize` 3.14 GiB with 3.21 GiB reported) is unchanged; the limit probably comes from a check by the game right after `SetHeapSize` (inferred from the time of the abort, not looked up in the code), not from real consumption.

**Measured more precisely (Mac M1, native release build, 2026-10-11):** in 4 MiB steps around the limit: up to
3044 MiB `svcBreak`, from 3048 MiB on the game runs up to `InitializeGraphics`. At 3044 MiB it requests via `SetHeapSize`
`0xBB60'0000` (aborts), at 3048 MiB `0xBBA0'0000` (runs). The lower bound for `TotalMemorySize` therefore lies
between 3044 and 3048 MiB; 3072 MiB leaves 24 MiB of margin. **Safari 27.2** allocated the guest block with `--memory 3048`
(≈ 3.4 GiB of shared wasm32 memory) and ran up to the NVN start.

**Stipulation:** `kDefaultTotalMemorySize` in the browser is 3072 MiB (`0xC000'0000`, `platform/include/lgp/platform/host_block.hpp`). The game page no longer needs its own `--memory`. Desktop stays at ≈ 3.21 GiB.

**Memory budget in wasm32 (4096 MiB):**

| Part | before (2816 MiB) | now (3072 MiB) |
|---|---:|---:|
| Guest block = `TotalMemorySize` + 384 MiB (code, stacks, alias, misc from `0x0800'0000`, heap from `0x2000'0000`) | 3200 MiB | 3456 MiB |
| Rest for Wasm data, Emscripten heap (runtime, HLE, dispatch table ≈ 13–29 MiB), thread stacks, audio ring, upload buffers | 896 MiB | 640 MiB |

The code of the Wasm module (≈ 133 MB `.wasm`) does not live in linear memory, only its data segments. Only what is written is physically backed; the guest block is allocated once in the browser, but not zeroed in advance. Chromium went along with growing to this size without errors, and so did Safari 27.2 (real game 600 frames with 3072 MiB, `13-browser-abnahme.md`). Firefox 157 (Linux) lets an empty shared wasm32 memory grow up to 3750 MiB ([`firefox-als-zielbrowser.md`](../knowledge/firefox-als-zielbrowser.md)). **Not measured:** the real game in Firefox, and how much of the 640 MiB the host really needs while the game runs.

**Review condition:** the value is re-evaluated as soon as it turns out to be too tight or too large, in particular if
- the game shows memory errors with 3072 MiB in later areas, in docked mode or during videos (too little; then more guest memory at the expense of the host rest, or re-evaluate B),
- the host in the browser uses up its remaining 640 MiB (`malloc` fails, memory growth fails; too much guest memory), or
- a target browser (Safari, Firefox) does not allocate the required ≈ 3.6 GiB.

## Consequences

- Memory SVCs (P3.1) implement their own layout allocator; `TotalMemorySize`/`SetHeapSize` are configurable.
- The RomFS is streamed (file handle or OPFS access from a worker), never loaded completely into Wasm memory.
- All runtime parts that read guest memory (IPC buffers, graphics) go through the same abstraction.

## Sources

- <https://caniuse.com/wf-wasm-memory64>
- <https://bugs.webkit.org/show_bug.cgi?id=300538>
- <https://emscripten.org/docs/tools_reference/settings_reference.html>
- <https://spidermonkey.dev/blog/2025/01/15/is-memory64-actually-worth-using.html>
- XenonRecomp places its function table behind guest memory instead of reserving 8 GB virtually: <https://github.com/hedge-dev/XenonRecomp>
