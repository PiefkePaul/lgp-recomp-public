# wgpu-native and wgpu: upstream status and what an update brings

Cross-game knowledge (status 2026-10-14). Checked against the sources: clone of
[gfx-rs/wgpu-native](https://github.com/gfx-rs/wgpu-native) (trunk 589e6ca from 2026-10-05, tag `v29.0.1.1`) and
the `CHANGELOG.md` of [gfx-rs/wgpu](https://github.com/gfx-rs/wgpu) (trunk, 2026-10-14). How the backend integrates wgpu-native:
[`webgpu-backend-desktop-und-browser.md`](webgpu-backend-desktop-und-browser.md),
`cmake/LgpWebGpu.cmake`.

## Versions

| | Status | wgpu-core inside |
|---|---|---|
| wgpu-native, latest release | `v29.0.1.1` (2026-06-22) | 29.0.3 (according to the tag's `Cargo.lock`) |
| wgpu-native, trunk | 589e6ca (2026-10-05), no release | 29.0.4 |
| wgpu (Rust), latest release | `v30.0.1` (2026-08-21) | 30.0.1 |
| Move of wgpu-native to wgpu 30 | open: [wgpu-native#630](https://github.com/gfx-rs/wgpu-native/pull/630) (draft from 2026-09-12), #617 closed | – |

A project that downloads `v29.0.1.1` as a release zip is therefore on the latest release. There is no newer one.

## What trunk has beyond the release

- `WGPUDeviceExtras` gets `memoryHints` (`Performance`, `MemoryUsage`, `Manual` with custom block sizes), wgpu-native
  [#619](https://github.com/gfx-rs/wgpu-native/pull/619). `MemoryUsage` lets wgpu use smaller memory blocks when
  sub-allocating. Suspicion: on devices with little memory (3 GB Android TV, phones) it saves slack in
  half-full blocks; nothing is measured.
- wgpu-core 29.0.4: `vulkanMemoryModelDeviceScope` with the Vulkan memory model (validation error, no
  known visual bug), Metal `Queue::as_raw`.
- Release profile with fat LTO and `codegen-units = 1` (smaller, somewhat faster library, suspected).
- OpenHarmony window handles.

For that one would have to build wgpu-native oneself (Rust toolchain, target triples for Android and iOS). That alone is
not worth it.

## What wgpu 30 (not yet in wgpu-native) would bring

By platform, only points related to mobile devices and NVIDIA:

- **iOS/tvOS: BC textures.** wgpu ≤ 29 reports `texture-compression-bc` only on macOS. wgpu 30 queries
  `MTLDevice.supportsBCTextureCompression` on iOS from 16.4 on ([wgpu#9656](https://github.com/gfx-rs/wgpu/pull/9656)). According to Apple,
  from Apple9 (A17 Pro, M3) on all GPUs have BC, in Apple7/Apple8 (M1/M2, A14/A15) "some" under iPadOS. Suspicion: an
  iPad with M1 and an iPhone 17 Pro then get BC in hardware; an Apple TV with A15 probably not. Gain: no
  BCn CPU decoder at load time, BC textures stay 4–8× smaller in memory.
- **NVIDIA outside of Windows:** no more frame-time spikes from waiting on the fence of
  `vkAcquireNextImage` ([wgpu#9486](https://github.com/gfx-rs/wgpu/pull/9486)). Affects Linux PCs with NVIDIA and
  probably the NVIDIA Shield (not checked).
- Fixes for falsely reported out-of-memory ([#9643](https://github.com/gfx-rs/wgpu/pull/9643),
  [#9747](https://github.com/gfx-rs/wgpu/pull/9747)).
- API changes (among others `Queue::present`, color spaces of the surface). For users of `webgpu.h` this depends on what
  wgpu-native makes of it.

Still unreleased in wgpu (trunk): recovery from `VK_ERROR_FRAGMENTED_POOL`/`VK_ERROR_OUT_OF_POOL_MEMORY` when
creating descriptor sets instead of a panic ([#10264](https://github.com/gfx-rs/wgpu/pull/10264)), a crash during
shader compilation on some Mali drivers ([#10291](https://github.com/gfx-rs/wgpu/pull/10291)), a dedicated
Vulkan memory pool for short-lived buffers ([#10232](https://github.com/gfx-rs/wgpu/pull/10232)).

For Adreno there is no entry in the whole changelog since 29.0 (search for "Adreno", "Qualcomm").

## Pipeline cache

wgpu (Rust) has had `Device::create_pipeline_cache` since version 22 (Vulkan only): the application gets the data of a
`VkPipelineCache`, stores it and passes it back on the next start. wgpu-native knows in `ffi/wgpu.h` only the
feature `WGPUNativeFeature_PipelineCache` (0x0003002B), but no function to create or read out a cache
(trunk 589e6ca). So it is not usable via the C API.

Making it available would mean patching wgpu-native (functions for creation, `get_data` and the `cache` entry in
the pipeline descriptors) and building it oneself. Suspicion: on drivers with long compilation (Adreno: single
compute pipelines 40–60 s) a warm start would become short with it, because the cache data contains the finished driver binary format.
The cold start stays equally slow. This depends on the driver and is not measured.

## Known hurdles that an update does not solve

- Missing BC on Adreno and Mali: hardware, not wgpu.
- A driver bug like "dynamic index into a uniform `vec4` always returns x" (Adreno 640): workaround in the
  generated WGSL, not in wgpu.
- Downlevel limits: without requested limits wgpu-native gives the downlevel values (e.g. 16 KiB uniform binding).
  Workaround: request every limit up to the WebGPU default value when creating the device, as far as the adapter has it.
  No version changes anything about that.
