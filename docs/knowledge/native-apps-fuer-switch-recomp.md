# Native apps for a Switch recompilation (Windows, macOS, iOS, Android, tvOS)

Knowledge across games (status 2026-10-13). None of this has been measured on a mobile or TV device; statements from
vendor documentation and third-party projects must be checked before use. iOS details:
[`ios-als-zielplattform-fuer-switch-recomp.md`](ios-als-zielplattform-fuer-switch-recomp.md). LGP decision:
[ADR-0008](../decisions/0008-native-apps-auf-allen-geraeten.md).

## Why native instead of the browser

- Statically recompiled code needs no JIT. That removes the biggest hurdle for emulators on iOS and tvOS.
- No Wasm overhead (roughly 1.9× in lifted code), no 4 GiB limit of wasm32, no proxying to the browser main thread.
- tvOS has no browser, Android TV no Chrome; there only the native route exists.

## One renderer for all: native WebGPU

A renderer against `webgpu.h` runs in the browser (emdawnwebgpu) and natively (wgpu-native or Dawn) with the same code.

| Target | wgpu-native | Dawn |
|---|---|---|
| Windows | ready-made release packages (D3D12, Vulkan) | from source |
| macOS | ready-made release packages (Metal) | from source |
| iOS | from source with cargo (`aarch64-apple-ios`, simulator `aarch64-apple-ios-sim`); bindgen needs the iOS headers via `BINDGEN_EXTRA_CLANG_ARGS` (MapLibre) | from source, Metal |
| Android | from source with cargo (`aarch64-linux-android`, NDK linker) | from source, Vulkan |
| tvOS | from source with cargo (`aarch64-apple-tvos`, Rust Tier 2, no nightly); bindgen needs the tvOS SDK via `BINDGEN_EXTRA_CLANG_ARGS` (`tvos-als-zielplattform.md`) | Metal backend, build for tvOS not checked |

The difference that matters: natively wgpu translates WGSL with naga, Chrome with Tint. The two reject different things
(e.g. Tint rejects `textureSampleBias` in non-uniform control flow). Anyone driving both routes should validate shaders
with both.

## Shell: SDL3

SDL3 covers Windows, macOS, iOS, tvOS and Android: window or Metal/Vulkan surface, gamepads (on Apple devices
via the GameController framework), sound, keyboard, and on the desktop file and folder dialogs. On Android SDL brings
a Java activity into which the native code is loaded as a library.

## Game data and saves per platform

- **Windows/macOS:** folder selection, data stays where it is.
- **iOS/iPadOS:** Files app or `UIDocumentPickerViewController` → copy into the app container.
- **Android:** Storage Access Framework (folder selection) → copy into the app's own storage, because direct
  file access to foreign folders has been restricted since Android 11 (commonly known, not checked).
- **tvOS:** according to Apple's archived tvOS guide only ~500 KB of persistent local storage (`NSUserDefaults`);
  larger data only in the cache folder, which the system may delete while the app is not running. No Files app:
  transfer over the network (e.g. a small upload server in the app). Saves would need iCloud/CloudKit or an
  own sync. Whether this still holds today is not checked.

## Memory

- Apple: jetsam without swapping, limit depends on the device. The entitlement
  `com.apple.developer.kernel.increased-memory-limit` exists according to Apple for iOS, iPadOS and visionOS, **not** for
  tvOS. CPU and GPU share the memory.
- Android: no fixed per-app limit for native memory, but the low-memory killer ends large processes
  (suspicion: on devices with ≤ 4 GB RAM a game with ~3 GiB of working memory is hardly stable).
- Always measure with a small probe app first (reserve a block, write it step by step) instead of inferring from the
  RAM figure.

## Distribution

Apps with translated game code are derived game code: build only locally, install only on your own devices
(Xcode or `adb install`), never distribute from CI. CI can build the shells with our own test programs.

## Sources

- MapLibre Native, WebGPU backends: <https://maplibre.org/maplibre-native/docs/book/platforms/ios/ios-webgpu.html>,
  <https://maplibre.org/maplibre-native/docs/book/platforms/android/android-webgpu.html>
- Apple, `increased-memory-limit`:
  <https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.developer.kernel.increased-memory-limit>
- Apple, App Programming Guide for tvOS (archived):
  <https://developer.apple.com/library/archive/documentation/General/Conceptual/AppleTV_PG/>
