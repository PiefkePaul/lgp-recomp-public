# Platform layer (P3.6)

As of 2026-10-11. Code in `platform/`, tests in `tests/platform/` (`ctest -L platform`).

## Boundary

Everything the runtime needs from the host goes through `platform/`. `runtime/` (kernel, memory, scheduler, HLE services) contains no operating system, window, audio or browser code and no branching on `_WIN32`, `__EMSCRIPTEN__`, `__APPLE__`, `__linux__` etc. This is checked by `platform.no_host_deps` (`tests/platform/check_no_host_deps.py`, with self-test) on every ctest run; the test is the acceptance test of P3.6. Compiler macros (`__clang__`, `_MSC_VER`) and the C++ standard library remain allowed.

| Library | Content |
|---|---|
| `lgp_platform` (`platform/include`, `platform/os`, `platform/src`) | Interfaces, guest memory reservation (`HostBlock`: mmap / VirtualAlloc / Wasm allocation), host threads with stack size (`HostThread`: pthread / `_beginthreadex`), `kDefaultTotalMemorySize` per target (ADR-0001), `normalize_path` |
| `lgp_platform_common` (`platform/common`) | Backends without devices, on all targets: `NullRenderer`, `NullAudio`, `ManualInput`, `MemoryStorage`, `HostDirectoryStorage` |
| `lgp_platform_webgpu` (`platform/webgpu`) | `WebGpuRenderer`: `IRenderer` on WebGPU (wgpu-native or emdawnwebgpu), surface, GPU thread (P4.4) |
| `lgp_platform_desktop` (`platform/desktop`) | `SdlWindow`: SDL3 window as WebGPU surface (P4.4); audio and gamepads follow with P5 |
| `lgp_platform_web` (`platform/web`, Emscripten only) | OPFS via WasmFS as `IStorage` (`open_opfs_storage`), `lgp_add_web_page()`: page with local folder selection and copy to OPFS (P6.1, `docs/knowledge/opfs-und-dateiauswahl-im-browser.md`); `shell.html` for demos. AudioWorklet and Gamepad API follow with P5/P6.2 |

`lgp_runtime_core` links `lgp_platform`. HLE services get the services via `lgp::platform::Platform` (non-owning pointers; the embedding program owns the backends).

## Interfaces

- **`IRenderer`** (`renderer.hpp`): drawing surface plus WebGPU objects as opaque pointers (`GpuHandles`: instance, adapter, device, queue, surface, surface format). `gpu/` casts them to `webgpu.h` types, the runtime never does. In addition `present_rgba8()` as the CPU route (tests, early bring-up) and `end_frame()` (present, window events; `false` = window closed, process ends). Reason for the opaque pointers: ADR-0003 settles on WebGPU, but the library for it (Dawn, wgpu-native, Emscripten `webgpu.h`) is only chosen in P4; the runtime should not depend on it.
- **`IAudio`** (`audio.hpp`): `open_sink(AudioFormat)` → `IAudioSink` with interleaved s16. `write()` never blocks and reports how much was accepted; `queued_frames()` is the clock by which audout/audren release guest buffers. A real backend must therefore drain at playback speed; `NullAudioSink` only drains on `drain()`.
- **`IInput`** (`input.hpp`): `poll()` per hid sample, then `controller(i)` with style (handheld, single Joy-Con, pair, Pro), buttons in the `nn::hid::NpadButton` bit layout, sticks (±32767) and motion (G, revolutions/s like `SixAxisSensorState`). Vibration optional.
- **`IStorage`** / **`IFile`** (`storage.hpp`): synchronous file system with virtual paths. `Platform` has two instances: `game` (read-only, our own dump) and `save`. Errors as `StorageError`, no exceptions. Renaming never overwrites (Horizon behavior), writing beyond the end of a file pads with zeros.

All calls may block: they run on guest threads, and those may do so on every target (ADR-0002, `PROXY_TO_PTHREAD` in the browser).

## Safety of the storage backends

Paths come from game code. `normalize_path` only lets through absolute paths made of simple names (no `.`/`..`, no `\`, `:` or NUL); `HostDirectoryStorage` appends the normalized path to a canonical root and thus cannot reach anything outside it. Deleting or renaming the root is rejected. `MemoryStorage` limits files to 1 GiB, `HostDirectoryStorage` to 1 TiB.

**Bug found that was almost a deletion trap:** `root / ""` yields the root *with* a trailing separator in `std::filesystem`, and `path::operator==` considers `"root/"` and `"root"` different. The "do not delete the root" protection therefore did not trigger; `remove_directory("/", true)` deleted the entire root in the test. Now `host_path` explicitly maps the path `/` to the root itself, and `platform.backends` checks that the root still exists after the attempt.

## Open

- WebGPU library chosen (P4.4): wgpu-native on the desktop, emdawnwebgpu in the browser; details in `docs/knowledge/webgpu-backend-desktop-und-browser.md`. SDL audio and gamepads come with P5.
- Web backends: OPFS is in place (P6.1); AudioWorklet, Gamepad API, OffscreenCanvas with P5/P6.2.
- `HostBlock` still reports errors via exception (mmap/VirtualAlloc fails); under Wasm this aborts. Irrelevant for the result (no run without guest memory), but the error message is missing there.
