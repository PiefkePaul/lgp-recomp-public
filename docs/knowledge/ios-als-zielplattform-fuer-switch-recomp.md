# iOS/iPadOS as a target platform for a Switch recompilation

Cross-game knowledge (status 2026-10-11). None of this has been measured on an iPhone or iPad; the statements
come from Apple's documentation or general knowledge and must be checked before use. The
LGP decision is in [`0007-ios-nativ-statt-wasm.md`](../decisions/0007-ios-nativ-statt-wasm.md).

## Why native instead of WASM

- **Static recompilation needs no JIT.** iOS does not allow normal apps pages that are first written and
  then executed. Emulators with a dynamic recompiler fall back to an interpreter there or need
  debugger tricks. Pre-translated C++ is ordinary app code and is not affected by this.
- **The same C++, a different target:** the lift output is host-neutral (guest memory via a base pointer, no
  host assumptions). What compiles for macOS arm64 compiles with `-target arm64-apple-ios` without changes to the
  generated code; differences lie only in the platform layer.
- **No Wasm surcharge:** Wasm costs roughly 1.9 times as much for the lift code (measured in Node/V8); in Safari,
  proxying to the main thread and a slow startup phase come on top.
- **Safari has no memory64.** A game that needs more than just under 4 GiB of linear memory has no way out in the browser on
  iOS.

## Memory on iOS

- iOS terminates apps that exceed their memory limit (jetsam), without swapping. The limit is device-dependent
  and undocumented; at run time `os_proc_available_memory()` returns the remainder.
- Entitlement `com.apple.developer.kernel.increased-memory-limit`: raises the limit on suitable devices.
- Entitlement `com.apple.developer.kernel.extended-virtual-addressing`: allows a larger virtual
  address space. Relevant because the runtime reserves the whole guest block in advance (`mmap` with `PROT_NONE`) and only
  commits it on writing.
- Apple devices have shared memory for CPU and GPU: the renderer's textures and buffers count towards the same
  limit as the guest memory.
- Reserved but not written costs hardly any real memory. What matters is what the game actually
  writes, plus GPU resources. Therefore measure early with a test app instead of estimating from the device size.

## Graphics, sound, input

- Native WebGPU: wgpu (via Metal) and Dawn support iOS. A WebGPU renderer that already runs on the desktop via
  wgpu-native only needs a different surface there (`CAMetalLayer`).
- SDL3 supports iOS (window/Metal view, audio, game controllers via the GameController framework).
- Touch controls have to be built yourself if no controller is used.

## Distribution

- Installation via Xcode with your own Apple Developer account onto your own devices. Game data never go into the
  app package; the app fetches them via file selection (`UIDocumentPickerViewController`) into its container.
