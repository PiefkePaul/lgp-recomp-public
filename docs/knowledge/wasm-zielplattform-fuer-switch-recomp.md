# WebAssembly as a target platform for a Switch recompilation

Cross-game knowledge from P1.6 (as of 2026-10-10). Browser details come from secondary sources and our own
knowledge, **not** from our own measurements; they must be checked on real devices before use (P6.1). The
LGP-specific decisions are in `docs/decisions/0001`–`0005`.

## Memory

- **wasm32:** linear memory up to 4 GiB, bounds checks are dropped thanks to guard pages. Guest memory, runtime,
  Emscripten heap and dispatch table share these 4 GiB. A Switch game only fits if the runtime reports a
  smaller `TotalMemorySize` than the console (see `horizon-prozessverhalten-fuer-recomp.md`).
- **The lower bound comes from the game, not from usage:** a game can abort at startup if less is reported
  than a fixed minimum, even if it actually writes much less (LGP: abort below ≈ 3040 MiB with ≈ 2.6 GiB
  real need). So try the value early with the real game (bisection via `--memory`), do not derive it from
  Ryujinx measurements.
- **Compute the budget:** linear memory = guest block (reported memory + fixed regions before it) + host rest (Wasm
  data, Emscripten heap, thread stacks, buffers). The Wasm code itself does not count. For LGP: 3072 + 384 MiB guest,
  640 MiB host rest. Pages are only committed on write; so do not zero the guest block up front.
- **memory64:** part of Wasm 3.0, available in Chrome/Firefox, in Safari according to a search on 2026-10-10 without a
  confirmed release; browsers limit it to 16 GiB; engines need software bounds checks (slower). Rules out iOS/Safari.
- Only what is written occupies physical memory (pages are created lazily). A large, barely used
  guest heap therefore costs little real RAM.
- **No page protection in Wasm** (no `mprotect`). Everything emulators detect via write protection (changed
  GPU buffers, texture invalidation, self-modifying code) needs a different route in the browser: upload and hash
  ranges per draw, or a write marker in the memory abstraction of the generated code.
- Large game data (RomFS, several GB) never belongs in linear memory. From a worker, files can be read
  synchronously (OPFS sync access handles; a file chosen by the user via `FileReaderSync` or after copying into OPFS).

## Threads

- Emscripten pthreads = Web Workers + `SharedArrayBuffer`; needs cross-origin isolation (COOP/COEP headers or
  a service worker workaround for local serving).
- `Atomics.wait` is forbidden on the browser main thread. The whole game therefore runs in workers
  (`-sPROXY_TO_PTHREAD`), rendering via OffscreenCanvas from a worker (check WebGPU-in-worker support per
  browser), audio via AudioWorklet with a ring buffer in the `SharedArrayBuffer`.
- Emscripten threads have only **64 KiB of stack** by default. With native return, the host stack grows with the
  guest call depth; Switch guest stacks are up to 1 MiB. Set the stack size per thread accordingly.
- Create workers up front (thread pool), because creating a worker is asynchronous and a synchronous
  `CreateThread` would otherwise block.
- Wasm atomics are sequentially consistent, i.e. stricter than ARM64: correct, but possibly more expensive.
- Cooperative scheduling without threads is only possible in the browser with Asyncify (instruments every path that
  can suspend; unusable with millions of recompiled instructions and tens of thousands of switches per second), JSPI
  (only at Promise boundaries) or Wasm stack switching (as of 2026 only behind a flag).

## Control flow

- Indirect calls become `call_indirect` via a Wasm function table. A directly indexed table
  guest address → table index costs 4 bytes per 4-byte instruction slot in `.text` (≈ size of `.text`).
- Tail calls: `[[clang::musttail]]` becomes `return_call`/`return_call_indirect` (Wasm tail call proposal). According
  to secondary sources available in Chrome, Firefox and Safari from 18.2; check before use.
- `setjmp`/`longjmp`: Emscripten `-sSUPPORT_LONGJMP=wasm` (via Wasm exceptions), cost only in functions that
  call `setjmp`.
- C++ exceptions are **off** by default in Emscripten: `throw` aborts the program (`Aborted(undefined)` from
  `__cxa_throw`), `catch` never applies. Platform and runtime code therefore reports errors via return values; a
  `try`/`catch` around `std::bad_alloc` only helps natively. Found in P3.6 on a throwing constructor that was green
  when tested natively.

## Graphics (WebGPU) compared to NVN/Maxwell

- Missing in WebGPU: alpha test (→ `discard` in the shader), depth bounds, conservative raster, wide lines,
  rasterizer discard, geometry and tessellation shaders, bindless texture handles (→ own handle scheme via
  uniform buffers). `depthClamp` via the feature `depth-clip-control`.
- Textures: BCn on desktop via `texture-compression-bc` (optional, check per browser/GPU), ASTC practically only on mobile.
  Switch textures are block-linear (swizzled); do the deswizzling and re-encoding if needed offline.
