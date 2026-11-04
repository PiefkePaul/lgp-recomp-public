# 0008 – P7: native apps on all of our own devices (Windows, macOS, iOS/iPadOS, Android, tvOS)

Status: **Accepted** (Paul, 2026-10-13) · Date: 2026-10-13 · Decides: Paul · Roadmap: P7 (locked until ★ P6.5)

Supplements [ADR-0007](0007-ios-nativ-statt-wasm.md) (iOS native, accepted) and answers its open question 6
(Android). Does not start any work: P7 only begins after P6.5 and with Paul's approval. Paul accepted the recommendation (option C)
on 2026-10-13.

## Context

Paul, 2026-10-13: the actual goal is to play on all of our own devices: iOS, Android, macOS, Windows and
Apple TV. "Playable in the browser" was meant as a practical route, not as the end goal.

What exists today (as of main d1bf3a7):

- A host-neutral lift output (C++) that builds with clang for x86-64 (PC), arm64 (Mac) and wasm32. It needs
  `[[clang::musttail]]`, i.e. clang on every platform (MSVC is out for the lift output).
- Runtime, HLE and NVN HLE without OS code (`platform.no_host_deps`).
- The WebGPU renderer via `webgpu.h`: natively with wgpu-native 29 (Metal on the Mac, D3D12/Vulkan on the PC), in the browser with
  emdawnwebgpu.
- A native boot program `lgp_game` with an SDL3 window, SDL3 sound and SDL3 gamepads. It is operated via the
  command line (`--exefs`, `--game`, `--save`, …); a clickable app shell is missing.
- Video via optional FFmpeg (desktop) or WebCodecs (browser). LGP uses VP9 1920×1080 at 30 fps (Analysis 14).

Measured on the Mac M1: native ≈ 93 fps, Safari ≈ 41 fps in the same scene (ADR-0007). The native routes are therefore
faster and already mostly in place.

## Options

### A: One browser build for all devices

Pros: one build, no installation.
Cons: on Apple TV and Android TV there is no browser with WebGPU (tvOS has no browser at all); on iOS the
memory and speed limits from ADR-0007; ≈ 1.9× Wasm overhead everywhere. Does not meet Paul's goal.

### B: Native apps, a separate renderer per platform (Metal, Vulkan, D3D12)

Cons: three new renderers next to the tested WebGPU path, each with its own shader translation. No advantage
as long as wgpu covers the same APIs. Rejected.

### C: Native apps with a shared shell: SDL3 + the existing WebGPU renderer via wgpu-native (recommended)

One app per platform, built from the same parts:

| Part | Windows | macOS | iOS/iPadOS | Android | tvOS |
|---|---|---|---|---|---|
| Lift output + runtime | clang x86-64 | clang arm64 | clang arm64-apple-ios | NDK clang arm64-v8a | clang arm64-apple-tvos |
| Graphics (wgpu-native) | D3D12/Vulkan, ready-made packages | Metal, ready-made packages | Metal, build from source | Vulkan, build from source | Metal, Rust target only tier 3 (suspected: laborious) |
| Window, input, sound | SDL3 (present) | SDL3 (present) | SDL3 (GameController framework) | SDL3 (Java shell) | SDL3 |
| Game data | folder selection | folder selection | Files app → app container | SAF folder selection → app storage | no Files app; transfer over the network, storage only in the deletable cache (see below) |
| Video VP9 | libvpx or FFmpeg | libvpx or FFmpeg | libvpx | libvpx or MediaCodec | libvpx |

Pros:
- The renderer stays a single one; what has been tested on PC and Mac continues to hold. Only surface,
  build and shell are new per platform.
- SDL3 is already in the project and supports all five platforms (window, gamepads, sound). Input mapping
  (`PadInput`, `--stick`, `--throw` etc.) applies unchanged.
- No JIT needed (static recompilation), so no special entitlements on iOS/tvOS.

Cons:
- wgpu-native only ships ready-made packages for the desktop; for iOS and Android it must be built from source with cargo
  (example: MapLibre builds `libwgpu_native.a` for `aarch64-apple-ios` and `aarch64-linux-android`).
- tvOS is only a tier 3 target for Rust (suspected: `build-std` with nightly needed, not checked). Dawn would be
  the fallback there. *Addendum 2026-10-14:* obsolete. `aarch64-apple-tvos` is tier 2, `rustup target add` is enough;
  only a ready-made wgpu-native package is missing (Analysis 28, `tools/apple/build_wgpu_native.sh`).
- The game apps contain translated game code. They may only be built locally and only installed on our own devices,
  never distributed from CI (see consequences).

### Dawn instead of wgpu-native

Dawn would have the advantage that natively the same WGSL compiler (Tint) runs as in Chrome (Analysis 13: Tint rejected three
programs that naga accepted). Against it speak the considerably heavier build (GN/CMake, large repo) and the fact that
wgpu-native has already been tested in the project. Recommendation: wgpu-native stays; Dawn only if a target (most likely tvOS)
does not build with wgpu-native.

## Decision

**Option C** (accepted). In detail:

1. **P7 goal:** native apps for Windows, macOS, iOS/iPadOS, Android and (conditionally) tvOS. The browser build is
   kept and stays in CI, but after P6.5 it is no longer specifically sped up.
2. **Android native** (ADR-0007, question 6): NDK build for arm64-v8a with Vulkan via wgpu-native. Rationale as for iOS
   (no JIT, no Wasm overhead); on Android TV there is no Chrome anyway. The target device is the OnePlus 7 Pro;
   the NVIDIA Shield is not a target.
3. **One shell for all:** SDL3, on top a small start UI of our own (choose or import game data,
   back up saves, settings such as controller mode), which sets the existing options of `lgp_game`.
   Platform code under `platform/<platform>/`, never in `runtime/`.
4. **Video:** libvpx (BSD license) as a single route for all native targets instead of FFmpeg. Whether a phone or the Shield
   manages VP9 1080p30 in software is open (suspected: Apple chips yes, Tegra X1 tight); otherwise Android could
   use MediaCodec.
5. **Order:** desktop apps (Windows, macOS) first, because they are almost done and usable right away; then iPad/iPhone
   (ADR-0007, strongest mobile device available); then Android; tvOS last and only after a feasibility probe.
6. **Every mobile and TV platform starts with a memory probe** (like P7.2a): a small app without game code that
   reserves and writes the guest block (≈ 3.4 GiB) until the system kills it.

## Open questions / data needed

1. **Target devices (Paul, 2026-10-13):** iPhone 17 Pro Max, iPad Pro M1 (12.9″, 5th gen), OnePlus 7 Pro, Apple TV 4K
   (3rd gen). RAM according to manufacturer or press data, not checked on the device: iPhone 17 Pro Max 12 GB (A19 Pro);
   OnePlus 7 Pro 12 GB (Paul's variant; Snapdragon 855, Adreno 640, Vulkan); Apple TV 4K (3rd gen) 4 GB (A15).
   Assessment (suspicion, until the memory probes run): the iPhone 17 Pro Max is certainly enough for the memory. For the
   OnePlus with 12 GB the memory is probably enough. The
   Snapdragon 855 is considerably slower than an M1; whether 30 fps is reachable is open. For the Apple TV with 4 GB and without
   a memory entitlement it is questionable whether LGP (≥ 3048 MiB reported, 1.3–2.6 GiB actually written plus
   GPU memory) fits at all; P7.4a clarifies this. The Shield is not a target.
2. **tvOS memory:** Apple only lists the entitlement `increased-memory-limit` for iOS, iPadOS and visionOS, not
   for tvOS. How much memory a tvOS app gets is open.
3. **tvOS storage:** according to Apple's (archived) tvOS guide an app only has ~500 KB of persistent local storage;
   larger data only in the cache folder, which the system may clear when the app is not running. For 4.2 GiB of game data
   that means: transfer again after every clearing. Saves would need iCloud/CloudKit or syncing with PC/Mac.
   Whether this still applies today has not been checked.
4. **Apple account:** whether the memory entitlements can be used with a free account has not been checked.
   Free profiles expire after 7 days (generally known, not checked for this project); a paid
   account ($99/year) would be practically necessary for permanent use on iPad/iPhone/Apple TV.
5. **Windows build with clang:** `lgp_game` is already built with clang on the PC; whether the app shell can stay MSVC-free
   (CI builds Windows in PRs only with MSVC) remains to be clarified.

## Consequences

- Roadmap P7 is restructured: P7.1 desktop apps, P7.2 iOS/iPadOS (content from ADR-0007 unchanged),
  P7.3 Android, P7.4 tvOS (conditional).
- **Game code stays local:** the finished apps contain translated game code. They are only built on the PC or Mac,
  never in CI, never uploaded or shared. CI builds the app shells at most with homebrew fixtures or the
  NVN test program (Android NDK works on Linux runners; iOS/tvOS would need a macOS runner, which is a
  CI change and needs Paul's approval).
- Game data never goes into the app package; every app fetches it via selection or import from Paul's own dump.
  *Addendum 2026-10-14 (Paul):* lifted. The whole game may go into the app package, for all apps
  (`LGP_APP_GAME_DIR`, Analysis 28). Such packages contain game data and, like the game code, stay local.
  Android is open, because APK assets are not files.
- New dependencies must build on all five platforms (rule from ADR-0007 extended).
- Cross-game knowledge doc: [`native-apps-fuer-switch-recomp.md`](../knowledge/native-apps-fuer-switch-recomp.md).

## Sources

- Own measurements: ADR-0007, Analyses 13, 14 and 20.
- MapLibre Native, WebGPU backend for iOS and Android (builds wgpu-native with cargo):
  <https://maplibre.org/maplibre-native/docs/book/platforms/ios/ios-webgpu.html>,
  <https://maplibre.org/maplibre-native/docs/book/platforms/android/android-webgpu.html>
- Apple, entitlement `increased-memory-limit` (platform list):
  <https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.developer.kernel.increased-memory-limit>
- Apple, App Programming Guide for tvOS (archived, storage and bundle size):
  <https://developer.apple.com/library/archive/documentation/General/Conceptual/AppleTV_PG/>
