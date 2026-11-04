# 0007 – iOS/iPadOS port: native ARM64 build instead of WASM in Safari

Status: **Accepted** (Paul, 2026-10-11) · Date: 2026-10-11 · Decides: Paul · Roadmap: P7.2 (locked until ★ P6.5)

This ADR only fixes the route for the later iOS/iPadOS port. It does not start any work: P7 only begins after the
browser milestone P6.5 and with Paul's approval. The browser remains target platform 1.

## Context

Today the game runs in three ways from the same lift output (`lgp_lift`, C++):

- natively on the PC (x86-64) and the Mac M1 (arm64), with `--renderer window` (SDL3 + wgpu-native),
- as WASM in Chromium and Safari on the Mac.

Measured on the Mac M1, same scene (`13-browser-abnahme.md`,
"frame rate Safari vs. native"): native with window ≈ 93 fps, Safari 27.2 ≈ 41 fps; start up to frame 64 natively 4–5 s,
Safari 159 s. The lifter benchmark measures WASM (Node/V8) at ≈ 1.89× the native run time (geometric mean)
([`lifted-code-performance.md`](../knowledge/lifted-code-performance.md)).

On iPhone and iPad there are two routes: the browser page in Safari (iOS browsers all use WebKit) or a native app.

## Options

### A: WASM build in Safari on iOS/iPadOS

Pros: no additional build, no installation, the same page as on the desktop.

Cons:
- **Memory:** LGP aborts below ≈ 3044 MiB of reported memory and runs from 3048 MiB (ADR-0001, #61). The
  linear Wasm memory needs 3072 + 384 MiB of guest and 640 MiB of host rest for this. Whether Safari on iOS allows a tab that much
  memory has **not been measured**. It is known that iOS kills tabs with high memory consumption; the exact
  limit depends on the device and is not documented by Apple.
- **memory64** does not exist in Safari according to research from 2026-10-10 (ADR-0001). So there is no way out via
  a larger address space.
- **Speed:** ≈ 1.9× slower than native for the lifted code, in Safari on the Mac ≈ 2.3× fewer frames per second overall.
  On an iPhone with less cooling the gap is probably not smaller (inferred, not measured).
- **Start time:** in Safari on the Mac 159 s up to frame 64. The cause has not been profiled (candidates: pipeline creation,
  OPFS reading).
- WebGPU, OPFS and pthreads (COOP/COEP) must be available in Safari on iOS just as on the Mac. This has
  **not been checked**.

### B: Native arm64 build as an iOS/iPadOS app (chosen)

The same generated C++ is compiled with clang for `arm64-apple-ios`, as is done today for macOS arm64.

Pros:
- **No JIT needed.** All game code is translated in advance. This is the core advantage of static recompilation:
  iOS does not allow apps executable pages at runtime (except with a debugger or special entitlements); emulators with
  a JIT fail because of this. Our build does not need it.
- **Speed as native on the Mac:** the Wasm overhead (≈ 1.9×) disappears.
- **Memory controllable:** an app can request more memory via the entitlement `com.apple.developer.kernel.increased-memory-limit`;
  for large reservations there is `com.apple.developer.kernel.extended-virtual-addressing`.
  How much this yields on which device has **not been measured** (see open questions).
- **Graphics without a new backend:** on the desktop the WebGPU renderer runs via wgpu-native, which uses Metal on Apple devices.
  wgpu (or Dawn as an alternative) supports iOS. Only surface binding and the build are new.
- **The architecture fits:** platforms attach only via `IRenderer`, `IAudio`, `IInput`, `IStorage`, `IVideo`
  (`docs/design/platform-interfaces.md`); `runtime/` contains no OS code (`platform.no_host_deps`). An iOS port
  is essentially a platform backend plus an app shell.
- On the Mac, guest memory is already an `mmap` reservation with commit on write (`platform/os/src/host_block.cpp`).
  The same code can be used under iOS (Darwin), provided the address space suffices.

Cons:
- Own app shell (Xcode project or CMake generator for iOS), signing, installation only via Xcode or one's own
  Apple developer account. Public distribution (App Store, TestFlight for third parties) is ruled out by the project rules
  anyway.
- Game data has to get onto the device (file selection from the Files app into the app container). It is never packed into the
  app.
- Touch controls are missing; controllers (MFi, Xbox, PlayStation, Joy-Con) will probably work via SDL3 or the
  GameController framework (not checked).
- Video decoding (VP9 from the title video) runs on the desktop via optional FFmpeg (LGPL, dynamic). On iOS
  the route is open (FFmpeg as an embedded framework or VideoToolbox, if VP9 is available there).
- A second build path that CI does not cover as long as there is no macOS runner.

## Decision

**Option B.** The iOS/iPadOS port is built as a native arm64 build, not as WASM. Rationale: no JIT needed,
almost double the performance, controllable memory via entitlements, and most of it (lift output, runtime, HLE,
WebGPU renderer via wgpu) is already platform-neutral. The browser page may additionally work on iOS, but it is
not a goal there.

What applies:
- No WASM-specific work is done for iOS (e.g. Safari iOS memory tricks).
- Guest layout and `TotalMemorySize` stay as in ADR-0001 (compact layout < 4 GiB). Whether iOS needs `LGP_GUEST_MEM64`
  is decided by the measurement in P7.2a.
- Threads as in ADR-0002 (1:1 host threads).
- New platform parts belong in `platform/` (e.g. `platform/ios/`), not in `runtime/`.

## Open questions / data needed

1. **Memory (P7.2a, first):** how much memory does an app with `increased-memory-limit` get on the
   target devices (`os_proc_available_memory()`), and can the guest block (≈ 3.4 GiB reservation) be reserved without or with
   `extended-virtual-addressing`? LGP needs ≥ 3048 MiB **reported**, but up to Route 1 only writes
   1.29–2.55 GiB (ADR-0001). On Apple devices CPU and GPU share memory; the WebGPU buffers and textures
   therefore come on top of the guest consumption. Whether devices with 6 GB RAM are enough or only 8 GB is open.
2. **Entitlements and account:** which of the two entitlements can be used with a free or a paid
   developer account? Not checked.
3. **wgpu or Dawn on iOS:** wgpu-native does not ship ready-made release packages for iOS as it does for the desktop
   (to be checked); possibly build it ourselves. Dawn is the alternative.
4. **Video:** FFmpeg framework or VideoToolbox for VP9.
5. **tvOS:** Apple TV devices have considerably less RAM than current iPads (Apple TV 4K: 3–4 GB, not checked) and
   stricter memory limits. tvOS is therefore only an option if P7.2a shows headroom there.
6. **Android/NVIDIA Shield (P7.1), not decided:** the same arguments speak for native there too (NDK build
   for arm64-v8a, no JIT needed, no Wasm overhead). In addition: on Android TV (Shield) there is no Chrome, so the
   browser page would be out there anyway. Counter-argument or risk: according to the manufacturer the Shield TV has 2 GB
   (standard) or 3 GB (Pro) RAM, less than the Switch itself (4 GB). Whether LGP fits there at all is unclear and
   independent of the build route. Paul decides this question separately, before P7.1 starts. **Answered 2026-10-13 by [ADR-0008](0008-native-apps-auf-allen-geraeten.md): Android native (target device OnePlus 7 Pro), the Shield is not a target.**

## Consequences

- Roadmap P7.2 is run as a native port with its own sub-steps (P7.2a–P7.2e); the first step is the
  memory measurement.
- Platform neutrality of `runtime/`, `gpu/` and the lift output remains a hard rule; `platform.no_host_deps`
  already enforces this.
- Dependencies that are only built on the desktop (wgpu-native packages, SDL3, FFmpeg, libopus) will later need an
  iOS build. New dependencies should therefore not exclude iOS (e.g. no desktop-only library for
  core functions).

## Sources

- Own measurements: `13-browser-abnahme.md` (Safari vs. native on the Mac),
  [`lifted-code-performance.md`](../knowledge/lifted-code-performance.md) (WASM vs. native), ADR-0001 (memory values).
- Apple entitlements `com.apple.developer.kernel.increased-memory-limit` and
  `com.apple.developer.kernel.extended-virtual-addressing` as well as `os_proc_available_memory()`: known from the
  Apple developer documentation; concrete limits per device are not documented and not measured.
- iOS memory limits for Safari tabs: generally known behavior, **not measured** for this project.
