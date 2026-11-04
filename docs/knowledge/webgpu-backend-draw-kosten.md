# Cost per draw in the WebGPU backend: uploads, bind groups, state

Concerns `gpu/webgpu/src/webgpu_backend.cpp` (`WebGpuBackend::record_list`). Measurement tool:
`bench/webgpu/webgpu_draw_bench.cpp` (`lgp_webgpu_draw_bench`, ctest `bench.webgpu_draws`). Applies to every
game whose NVN commands run on WebGPU via the HLE.

## Starting point

On the PC (RTX 5070 Ti, Route 1) the real game ran with WebGPU at 11.8 fps and about 587 draws per frame, but
with `--renderer soft` at 52 fps. So the backend cost about 65 ms per frame, roughly 110 µs per draw. Causes
in the code (all per draw):

1. **Constant buffers:** every bank read was uploaded as a full 64 KiB block (`kCbufBytes`, that is how large
   the array in the WGSL is), even if the guest had bound only 256 bytes. With three banks that was
   113 MiB per frame in the benchmark.
2. **Arenas per list:** for every command list new buffers of size "draws × banks × 64 KiB" were created.
   That costs memory, page faults and the zeroing by WebGPU.
3. **Vertex buffers:** every draw uploaded every bound vertex buffer completely, even if the same range had already
   been uploaded for the previous draw.
4. **Bind groups:** four new bind groups were created for every draw (constant buffers and textures for vertex and
   fragment).
5. **One `wgpuQueueWriteBuffer` per upload:** wgpu-native creates its own staging buffer for every call
   (`StagingBuffer::new`). In the browser every call copies from Wasm memory and crosses the JS boundary.
6. **Redundant calls:** pipeline, viewport, scissor, blend color and stencil reference were set anew on every
   draw. Natively that costs little; in the browser every call is a JS call.
7. **Pipeline key:** the key was built twice per draw as text with `std::to_string`.

## What the backend does now

- **Upload only the guest bytes, reuse identical ranges** (commit 53b0145 from the Mac, taken over): it uploads
  `min(size, 64 KiB)` instead of always 64 KiB. A range with the same address and size is uploaded only once within a
  list. All uploads read guest memory while the list is recorded, so the same
  range also has the same bytes there. The exception is `CmdCopyBuffer`, which changes guest memory in the middle of the list:
  after it the backend forgets the previous uploads (test in `nvn.webgpu_draw`).
- **Fixed arenas** (`State::UploadArena`, one for constant buffers, one for vertices and indices): the blocks
  (4 MiB each) persist for the lifetime of the device and are refilled by every list. No fence is
  needed for this: queue writes and submits run in queue order, so the write for the next list only lands
  after the GPU has executed the previous one.
- **One write per block:** uploads are first written into a host copy of the block. Before the submit follows a
  single `wgpuQueueWriteBuffer` per used block.
- **Dynamic offsets for constant buffers:** the bind group layouts of groups 0/1 use `hasDynamicOffset`. One
  bind group per (layout, arena block) is enough across all lists; the draw only passes the offsets. The
  device limit `maxDynamicUniformBuffersPerPipelineLayout` is 8 by default and applies to vertex and
  fragment banks together. Programs with more banks still use bind groups per draw (`Program::dynamic_cbufs`).
  Banks that were never bound point to a fixed 64 KiB zero buffer.
- **Cache for texture bind groups** (`State::texture_groups`): the key consists of the layout and the views,
  samplers and buffers of the entries. Exempt are groups with global memory, because their table buffers are created anew per
  list. The cache is cleared when a texture or a program is freed (otherwise
  a reused handle value could hit an old group), and likewise from 4096 entries.
- **State filter per pass** (`PassState` in `record_list`): pipeline, bind groups including offsets, vertex and
  index buffers, viewport, scissor, blend color and stencil reference are only set when they change.
- **Binary pipeline key:** the fields go into the key as bytes, without text formatting.

### Deviation: constant buffers beyond the guest size

A binding always covers the whole bank (64 KiB), because the WGSL declares `array<vec4<u32>, 4096>`. Previously
there was a zero behind the guest bytes. Now the bytes of later uploads lie there. On Maxwell reads
outside the bound size return 0. So a shader that reads beyond the bound size sees different values than
on the Switch. A correctly built shader only reads within its block. Whether the game adheres to this is
**UNVERIFIED**. It can be checked with `LGP_WEBGPU_DEBUG=cbufzero`: then every upload is again written with the full
64 KiB and zeros. If the frame hashes (`--frame-stats`) differ with and without this option, a shader
reads beyond the limit. An exact and still fast way would be to declare banks in the WGSL only as large
as the shader statically reads them. For that the translator (`gpu/shader`) would have to report the size per bank.

## Measurement (cloud container, Mesa lavapipe, release build)

`lgp_webgpu_draw_bench` (default: 600 draws in 4 lists, 8 programs, 16 textures, per draw its own object and
material constants of 256 bytes each, two textures). Median over 20–30 frames. "execute" is the time in
`WebGpuBackend::execute()`, "frame" the time including waiting for the GPU (`read_texture`). The checksum of the
target image is the same in all variants (`ff4a288be0f1a243`).

| State | execute, 4 lists | frame, 4 lists | execute, 1 list |
|---|---|---|---|
| main (94cf1ca) | 158 ms (264 µs/draw) | 170 ms | 94 ms (156 µs/draw) |
| + Mac commit 53b0145 | 82 ms (137 µs/draw) | 88 ms | 27 ms (44 µs/draw) |
| now | 13.4 ms (22 µs/draw) | 17.4 ms | 2.6 ms (4.3 µs/draw) |

Per frame now: 198 KiB of constant buffers and 320 KiB of vertex and index data instead of 113 MiB and 38 MiB, 88 newly
created bind groups (only during warm-up) instead of 2400.

How to read it: with one list "execute" is almost pure CPU time of the backend, i.e. about 4 µs per draw. With four lists
the submit blocks while lavapipe is still rasterizing the previous list on the CPU. That is why "execute" there
rises to 13 ms again. The frame time of about 17–20 ms is lavapipe's raster time in the cloud. On a real GPU
this share goes away. Absolute numbers from the cloud fluctuate by ±30 %. For comparisons only measurements on
the same machine count. With `LGP_WEBGPU_DEBUG=cbufzero` "execute" rises to about 27 ms (64 KiB per upload).

Further variants (now): 16 lists 20 ms, 1200 draws 26 ms (22 µs/draw), object 4 KiB and material 1 KiB 16 ms.

### Real game (PC, 2026-10-12)

Outdoors with 466 draws per frame the frame rate rises from 18.5 to 43.8 fps (2.4×). In steady operation no
new bind groups are created; about 1.27 MB of constant buffers go up per frame. Room and outdoor area look visually the same,
there were no graphics errors. The comparison of the frame hashes with and without `cbufzero` was part of the PC check.
The result was "merge ok".

## Measuring

```sh
cmake --preset release && cmake --build --preset release --target lgp_webgpu_draw_bench
build/release/bench/webgpu/lgp_webgpu_draw_bench [--draws 600] [--lists 4] [--frames 40] [--programs 8]
    [--textures 16] [--object-bytes 256] [--scene-bytes 4096] [--material-bytes 256] [--vertex-bytes 65536]
```

`WebGpuBackend::Stats` counts `cbuf_uploads`, `cbuf_upload_bytes`, `cbuf_reused`, `geometry_*` and
`bind_groups_created`/`bind_groups_reused`. On the real game these are the numbers that show whether the
reuse takes effect. `lgp_game --frame-stats N` prints them in the line `uploads:` (totals since the start).

## Stutter on first occurrence (shaders, pipelines, textures)

An effect that appears for the first time can stall for a frame. After that the same effect runs smoothly. On the
Switch the shaders already exist as machine code; here three costs arise on first use. All three
block the guest:

1. **Translation** Maxwell → WGSL (`set_program` in `gpu/nvn/src/procs_draw.cpp`, in the guest thread at
   `nvnProgramSetShaders`). From 2 ms the log line `nvn: … shader translated in X ms` appears.
2. **Creating the program** (shader modules, layouts, for compute also the pipeline; `create_program`). From 2 ms
   `nvn webgpu: … program N created in X ms (after present P)` appears.
3. **Draw pipelines** (`execute()`, before recording, per state combination of program, vertex layout, blend,
   depth, target format). Here the driver translates the shader into machine code. From 2 ms per list
   `nvn webgpu: N draw pipelines created in X ms (after present P)` appears. Measured up to the callback of
   `PopErrorScope`: in the browser it only comes when the GPU process has validated the pipelines, and the guest waits
   that long.

In addition there is checking and uploading **textures from the pools** (deswizzle, ASTC decoding on the CPU). From 2 ms per
list `textures from pools took X ms (…)` appears. The totals are in `--frame-stats` in the line `shaders:`.
The frame number `after present P` assigns a stutter to its cause.

First finding (cloud, lavapipe, `lgp_webgpu_draw_bench`): a draw pipeline costs 5–12 ms, a program under
2 ms.

On the real game (PC, battle session over about 11,600 frames, #102):

| Item | Count | Total | Maximum |
|---|---|---|---|
| Draw pipelines | 346 | 6758 ms (≈ 20 ms per pipeline) | 431.5 ms in one list |
| Programs | 310 | 2077 ms | 26.5 ms |
| Textures from pools | – | 1862 ms | 91 ms (once) |
| Translation ≥ 2 ms | 0 | – | – |

Of 89 lists with new pipelines, 58 were above one frame (16.7 ms). They account for 98 % of the stutter time. So the
cause is pipeline creation, during which the driver compiles shaders. Which native backend wgpu takes
is shown in the line `renderer: webgpu (…, Vulkan|D3D12|Metal)`. `LGP_WEBGPU_BACKEND=vulkan|dx12|metal|gl`
forces one. The wgpu-native feature `PipelineCache` exists only for Vulkan, and `wgpu.h` 29.0.1.1 offers no
function for it.

### How the Switch avoids this, and what of it works here

On the Switch the shaders lie in the RomFS as finished Maxwell machine code (BNSH). NVN has no
pipeline objects: blend, depth, vertex layout and target formats are dynamic state, the GPU executes the code directly.
No driver translates anything there. Here the chain is Maxwell → WGSL → (naga or Tint) → SPIR-V/HLSL/MSL →
driver. WebGPU also bakes vertex layout, blend, depth and target formats into the pipeline. Every new combination
therefore costs a driver translation.

| Route | What it saves | Assessment |
|---|---|---|
| (a) Translate shaders from the RomFS ahead of time (`bnsh.py`, `maxwell2wgsl`) | translation (measured: none ≥ 2 ms) and part of "creating the program" (2 s in total, max. 26 ms) | Does not hit the main costs. Pipelines need the runtime state that the shader alone does not reveal. |
| (b) Remember pipeline states and prebuild them at the next start | draw pipelines from the second run on | Implemented, see below. An effect never seen before still stutters the first time. |
| (c) Textures | 1.9 s in total, once 91 ms | Small item, but a noticeable stop when entering the route. Not ASTC, see "Texture upload" below. |

### Pipeline cache (route b)

`WebGpuBackend::set_pipeline_cache(path)` and the start option `--pipeline-cache FILE|none`
(`lgp/gpu/webgpu/pipeline_cache.hpp`):

- **What is stored:**
  - For every successfully validated draw pipeline a recipe, namely the pipeline key without the program ID
    (topology, target formats, vertex layout, fixed state including the alpha test from #105, depth mode `NEAR_IS_MINUS_W`
    from #99), together with a hash of the program. The hash covers WGSL and binding layout (`program_hash`).
  - Every program itself (WGSL and binding layout, `encode_program_desc`), compute programs included. On the PC
    creating programs also caused stutter (up to 53 ms).
- **Format:** `LGPPIPE1`, then a fingerprint of adapter, backend, limits and wgpu-native version, then the
  records. If the fingerprint does not match (different GPU, different driver or browser), the file starts anew. A
  truncated end (crash while appending) is cut off. Above 64 MiB (`kPipelineCacheFileLimit`) the file also starts anew
  (log line `… is larger than 64 MiB; starting over`), because records of older shader versions pile up. Until
  2026-10-29 the limit was 32 MiB: on the OnePlus 7 Pro a single Arena run wrote about 33 MiB, so every following run
  started over, and the log wrongly named another GPU as the reason.
- **Location:** the file is derived from game data and stays local.
  - The default is `$LGP_PRIVATE_DIR/cache/webgpu-pipelines.lgppipes`; without the variable the cache is off.
  - In the browser the page passes `/opfs/lgp/cache/webgpu-pipelines.lgppipes`. The folder is deliberately not
    under `save/`, so that "Export saves" does not take it along.
  - `open()` rejects paths without `.lgppipes` and paths in the source tree, build trees included.
  - The pre-commit guard blocks the extension and the magic `LGPPIPE` at offset 0, `.gitignore` the extension.
  - With `LGP_WEBGPU_DEBUG` overrides that change pipelines, the cache stays off.
- **Seed from another device:** a cache only applies to the adapter, driver and implementation that
  wrote it. So that a new device or a fresh browser does not experience all effects cold again,
  `set_pipeline_cache(path, error, seed)` takes the records of a second `.lgppipes` file with any
  fingerprint (`PipelineCacheFile::import_records`). Since 2026-10-26 the seed stays a read-only file of its own
  that is read again at every start and is not copied into the device's cache file: the cache file starts over above
  64 MiB, and a seed may be much larger (a whole catalog from `lgp_pipeline_seed catalog` is ≈ 185 MiB, limit 1 GiB).
  Its recipes are held like loaded ones; its program texts stay in the file and are read only when a program is
  created ahead (`PipelineCacheFile::program`), so a large seed does not cost its size in memory. Seed programs
  are created ahead only if they are compute programs or have a recipe (the device's own or the seed's): a catalog
  lists thousands of graphics programs that no run has drawn with yet. Records the seed already has are not
  appended to the device's file.
  - **Graphics screen before the game (app, 2026-10-26):** after the start screen the app creates all cached programs
    in 30 ms slices (`WebGpuBackend::warm_cached_programs`) and waits until their pipelines are built
    (`prebuild_progress`); if that takes longer than 300 ms (`LGP_PREPARE_SCREEN_MS`) it shows "Preparing graphics"
    with a progress bar and a "Start now" button that leaves the rest to the background. The pipelines stay alive and
    are adopted by the game; the driver caches (`.driver`, Metal's own) make later starts quicker.
    Measured on the PC (RTX 5070 Ti, Vulkan, released wgpu-native without the driver cache, seed v2, 2026-10-26):
    screen 8.9 s cold and 6.0 s at the next start for 1376 programs and pipelines; without the screen the game
    also built no draw pipeline during play (202 used from the background, none waited for). So it is off by
    default on the desktop and the Apple TV (all seed pipelines stay alive below its jetsam limit; not measured) and on for phones, tablets and Android TV (not measured there yet); `LGP_PREPARE_GRAPHICS=0|1`.
    With wgpu-native from `build_wgpu_native.py` (driver cache, 28.2 MiB `.driver` after the first start) the screen
    took 6.07 s cold and 5.99 s warm: the time is spent on the CPU side (WGSL→SPIR-V and pipeline objects for all
    1376 seed entries, of which a run up to the field uses about 200), not in the driver; the background time of
    the pipelines the game used fell from 2.7 s to 1.1 s. The first cold start of the day took 8.9 s (suspicion, not
    measured: NVIDIA's own shader cache works across folders).
  - Start option `--pipeline-cache-seed FILE`; without it `<--game>/pipeline-seed.lgppipes`, if present. The
    browser page copies a `.lgppipes` file that lies directly in the selected folder there
    (`/opfs/lgp/data/pipeline-seed.lgppipes`). Metric: `pipeline cache: … records from seed …` (every start, the seed is read again).
  - **Why this can carry over:** recipes only contain WebGPU enumeration values and NVN state. The values of
    `WGPUTextureFormat`, `WGPUVertexFormat`, `WGPUIndexFormat` etc. are the same in wgpu-native 29.0.1.1 and emdawnwebgpu
    (header files compared, 2026-10-13). `FixedState` has no padding bytes. Merging
    vertex buffers (`vertex_merge`) and `unclippedDepth` are decided by each device itself when building.
  - **What does not fit only costs time:** a recipe whose program hash never occurs here is never built. One
    that leads to a different key than the real draws (such as a different `unfilterable` without
    `float32-filterable`) is built for nothing. Recipes with a target in `RG11B10Ufloat` are skipped by a device without
    `rg11b10ufloat-renderable`, because the desktop worker builds without an error scope (see remaining risk).
  - **Unmeasured:** how many program hashes are the same between native PC, Chrome and Safari is not checked.
    If the WGSL texts differ per platform, the seed brings nothing.
  - **Measured (LGP, 2026-10-14, cold versus seed, ABAB, the same seed file on both devices):** the seed does not save
    the pipeline build, it moves it: `warm_step` builds every recipe in the background as its own
    `WGPURenderPipeline` (counter `pipelines_prebuilt`) before a draw needs it. Counted below is how many
    pipelines a draw had to build itself (`draw_pipelines_created`). Number and duration of the prebuilds are not evaluated in these
    runs.
    - Apple TV 4K (A15, tvOS, seed from the app bundle, 19.3 MB, 999 entries): 0 instead of 165 draw pipelines built in the draw,
      frames 701–800 at 23.5 instead of 20.7 fps. The Metal shader cache was warm in both arms.
    - Mac M1 (macOS, Metal shader cache emptied before each run): 0 instead of about 163 pipelines built in the draw, frames
      1–1200 in 42.6 instead of 49.0 s, frames 701–800 at 29.4 instead of 20.3 fps. A draw pipeline costs about 38 ms cold
      when a draw waits for it.
    - The cold translation of the Metal programs (about 720 ms on the Mac) is not taken off the game by the seed. It lies
      in the system's shader cache.
    - The recipes are backend-independent: the same file works under Metal on macOS and tvOS.
- **Prewarming the programs:** after every present the GPU thread creates stored programs until 3 ms per frame
  are used up (`warm_step`, order as in the file, i.e. as the game needed them). It validates them and
  builds their pipelines in the background. When the game then creates a program with the same hash,
  `create_program` takes over the finished one including pipelines (`adopt_warm`) and compiles nothing.
  - A program whose turn has not come yet or that is not yet validated in the browser is created as before.
  - Its stored pipelines are then prebuilt as soon as it exists.
  - **Limit:** the budget is only checked after each program. A single program whose creation takes longer
    (measured up to 53 ms on the PC) extends the frame in which it is prewarmed by this time. This only affects
    the first frames after the start, while programs are still to be prewarmed, and is derived, not measured on the
    game. Instead of a stutter at the effect there is then one early in the run.
- **Prebuilding:** for a validated program the GPU thread prepares the descriptors of the recipes (in an
  error scope; variant modules and layouts are created there). They are then created
  - **in the browser** with `createRenderPipelineAsync`. The callbacks arrive on the main thread, errors are reported by the
    callback itself. The guest thread writes the file after `execute()`, because OPFS is only reachable from a pthread.
  - **on the desktop** by worker threads with `wgpuDeviceCreateRenderPipeline`, by default half the cores,
    at most 4 (`LGP_WEBGPU_PREBUILD_THREADS=N` sets 1–16). wgpu-native does not implement
    `createRenderPipelineAsync`: version 29.0.1.1 aborts with "not implemented". With
    `LGP_WEBGPU_PREBUILD=async` the browser route runs synchronously on the desktop, for tests.
  - **Order of the workers:** the queue is sorted by the position of the recipe in the cache file, i.e.
    by first use in earlier runs, compute programs by the position of their program. Before, all
    recipes of a program came at once at the position of the program: a program seen early whose recipes are only
    needed in a later scene pushed them ahead of the recipes of the next scene. On slow devices
    (Adreno, ~127 ms per pipeline) the queue at startup is long with `LGP_WEBGPU_WARM_ALL`. The benefit is
    derived, not measured; it only shows when `prebuilds late`/`waited` > 0 appear in the window.
    In the browser the sorting does not apply (Dawn receives the jobs immediately).
- **Finished pipelines** are taken over by `execute()` before the pre-pass.
  - If a list needs a pipeline whose prebuild is still in the queue, it takes the job back and
    builds it itself.
  - If the worker is building the pipeline at the moment, the list waits for it.
  - In the browser the list waits for it (asynchronous pipelines, see below). With `LGP_WEBGPU_ASYNC_PIPELINES=0` it builds
    them synchronously, and the late result is discarded.
- **Remaining risk desktop:** error scopes in wgpu-native are a stack per device, not per thread; checked with
  an error from a second thread that the scope of the first catches. The worker therefore uses no scope.
  - A recipe comes from a pipeline that was validated with the same WGSL on the same adapter.
  - Should it nevertheless be invalid, the error lands in the currently open scope of the GPU thread (that list discards its
    new pipelines) or as an uncaught error in the log.
  - If the way pipelines are built from the key changes, `kPipelineRecipeVersion` must be increased.
- **Metrics** in `--frame-stats`, line `pipeline cache:`:
  - Programs and recipes at startup.
  - Prewarmed and adopted programs.
  - Prebuilt and used pipelines, of which waited-for and too late (then built as without a cache).
  - Failed prebuilds and the worker time.
  - Compute pipelines from the background (see cold start).
  - Many "too late" mean that prewarming does not get ahead of the game, for example directly after the start.
- **On the real game (PC, 2026-10-12, main 584b6c8, battle script, `--clock virtual`):** in the second run not a single
  draw pipeline was created in the game any more. 271 of 277 came prebuilt, no list had to wait for any. Programs
  took at most 12.9 ms, and only at startup. Without a cache there were 253 pipelines in the game with a peak of
  221 ms. The frames of the two runs are not compared.
- **Tests:**
  - `nvn.pipeline_cache_file`: file, program records, rejected paths, damaged end, foreign
    fingerprint, seed (read again each start, not copied, larger than the cache file, program texts read on demand,
    which seed programs are created ahead, no cache file).
  - `nvn.pipeline_cache`: worker and async route, prewarming and adoption (graphics and compute program), depth mode and
    alpha test in the recipe, outdated prebuilds after a newly created program, compute pipeline from the background including
    dispatch, and a run that does not wait for the prebuilds (every pipeline exactly once, waited for, taken back
    or built itself), plus a fresh cache with a seed of a foreign fingerprint (all pipelines prebuilt,
    the seed taken again at the second start, the fresh file holds only its own records).
  - In the WASM build `browser.pipeline_cache`: Chromium, real `createRenderPipelineAsync`.

Not checked: whether Chromium already uses its own shader caches (Dawn, driver) across runs and the
browser therefore stutters less than the desktop. A browser measurement on the game is pending.

### Cold start with an empty driver cache

The first run after a rebuild or driver change was clearly slower on the PC: compute programs up to 6.6 s,
pipelines up to 1.9 s. The driver then translates every shader anew instead of taking it from its own cache. The
expensive part is the pipeline: in wgpu-native `createShaderModule` only checks the WGSL (naga); machine code is only created
with the pipeline.

- **No cache at all (very first run):** nothing from this file helps against that. What the game needs for the first time
  is translated on first use.
- **Cache present, driver cache cold:** prewarmed compute programs now get their pipeline in the background, like
  draw pipelines (desktop: worker, browser: `createComputePipelineAsync`). Until it is there, the program counts as
  `compute_deferred`. The first dispatch takes back a still waiting job, waits for a running one or builds
  the pipeline itself in an error scope (browser: waits, see below).
- **Several workers:** with a cold driver cache a pipeline costs seconds. A single worker would have worked through the
  queue in order; now up to 4 build in parallel. wgpu-native allows creating
  pipelines from several threads.
- **Limits:**
  - Prewarming happens after presents, one program per 3 ms budget. If the game creates a compute program before
    prewarming has got there (for example before the first present), it is created as before including its pipeline in the guest call.
  - Whether the pipeline is ready before the first dispatch depends on how much time the game leaves between creation and
    use. This is not measured on the game; the line `pipeline cache:` shows it (compute pipelines from the
    background, waited for, too late).
  - Draw pipelines of programs that were not prewarmed are still only queued once their program exists.
- **PC counter-check (2026-10-12, each the first run after the rebuild, battle script):** main 584b6c8 needed 18.5 s for programs,
  peak 6.6 s; with this state 6.7 s, peak 3.0 s, and 4 compute stalls remained. Not quite comparable: the
  main run did not have a `.lgppipes` yet. Warm no difference, frames bit-identical (cold as warm).

### Saving the driver pipeline cache ourselves (Android)

Desktop drivers (NVIDIA, Mesa, Apple) keep translated pipelines in their own cache on disk. On Android
(Adreno) nothing of that remains between two runs: every draw pipeline costs about 127 ms there again in every run (profile OnePlus 7 Pro),
even if the `.lgppipes` already creates it in advance. Vulkan provides `VkPipelineCache` for this, which the app itself
saves and passes at the next start.

- **wgpu-native has no API for this.** wgpu-core has `create_pipeline_cache`/`pipeline_cache_get_data` and a field
  `cache` in the pipeline descriptors (Vulkan only, feature `PIPELINE_CACHE`); wgpu-native 29 hard-codes it to
  `None`; only the feature `WGPUNativeFeature_PipelineCache` is passed through.
- **Patch:** `tools/webgpu/wgpu-native-pipeline-cache.patch` adds `wgpuDeviceCreatePipelineCache` (data, size,
  `fallback`), `wgpuPipelineCacheGetData`, `wgpuPipelineCacheAddRef/Release` and the chain
  `WGPUPipelineCacheExtras` for render and compute pipeline descriptors; the header defines
  `WGPU_LGP_PIPELINE_CACHE`. `python3 tools/webgpu/build_wgpu_native.py --out <dir> [--android]` (default `--patches driver-cache`, with `--android` `driver-cache,lazy-naga-ir`) builds the
  pinned tag with it; the result goes into the build via `-DLGP_WGPU_NATIVE_DIR=<dir>`. The releases (CI,
  default) stay unpatched; nothing changes there.
- **Backend:** with a patched wgpu-native the renderer requests the feature if the adapter has it (Vulkan only).
  `set_pipeline_cache` loads `<cache>.driver` (`fallback`: if the file does not match device or driver, the
  cache starts empty; wgpu-core checks this with its own header), every pipeline gets it. It is written back when
  pipelines were created with the cache since the last time: at most every 30 s on its own thread
  (`lgp-pipe-save`, not in the guest thread) and at the end, each time via a temporary file with its own name per process.
  The file carries its own header with length and FNV-1a checksum before wgpu's data: wgpu-core only checks whether the data
  match device and driver, not whether they are intact, and some Android drivers crash on damaged caches.
  A damaged or older file is skipped (log `… is damaged or from an older build; ignored`); then
  the seed applies, if there is one, otherwise the cache starts empty. Switch off: `LGP_WEBGPU_DRIVER_CACHE=0`. Log: `driver pipeline cache …: N bytes from earlier runs` and
  `… saved: N bytes in X ms`.
- **Seed:** if `<cache>.driver` is missing (or empty) and there is a pipeline seed (`--pipeline-cache-seed`,
  `pipeline-seed.lgppipes` in the bundle), the driver cache starts with `<seed>.driver`, if present; it is written
  only to `<cache>.driver`. With `-DLGP_APP_DRIVER_SEED=ON` (default off) `LGP_APP_PIPELINE_SEED=<file.lgppipes>`
  also packs `<file.lgppipes>.driver` into the bundle or the APK; if the file is missing, configuring aborts. Only useful on the same GPU model with the same driver (otherwise
  `fallback`, empty); intended for our own APK on our own device after a reinstall or an emptied app folder.
  Log: `driver pipeline cache …: N bytes offered from seed …` (offered: whether the driver takes them, the line
  does not say). Test: `nvn.pipeline_cache` (under lavapipe only the header).
- **Limits:** the cache only saves the translation in the driver. The WGSL→SPIR-V translation (naga) still runs in wgpu
  for every pipeline. lavapipe puts only a header (96 bytes) into the
  cache, so the test `nvn.pipeline_cache` only checks that the file is written and
  loaded again. The `.driver` file contains machine code from game shaders and stays with the save data or in
  `$LGP_PRIVATE_DIR`.
- **Measurement (OnePlus 7 Pro, Adreno 640, vpress to frame 3000, `--clock virtual`, from 34.9 °C, ABAB, 2026-10-14):**
  with a warm `.lgppipes` and the driver cache on, 85/87 draw pipelines took 565/556 ms, out of 106 pipelines
  19,707/18,711 ms (3–5 ms instead of ~50 ms per pipeline); compute 36 ms instead of 1705 ms. The wall time dropped from
  215.2/218.0 to 204.8/204.6 s (−5 %). Cold (without both caches) it was 103 pipelines in 8014 ms and 229.0 s.
  The rest per pipeline is essentially naga.
- **Metal (`MTLBinaryArchive`), checked 2026-10-15, not built:** in wgpu 29.0.3 `create_pipeline_cache` is empty under
  Metal (`wgpu-hal/src/metal/device.rs`), `pipeline_cache_get_data` returns nothing, and the Metal adapter does not report
  `PIPELINE_CACHE` (only `vulkan/adapter.rs`). Feasible would be an own wgpu-hal patch next to `lazy-naga-ir`:
  objc2-metal 0.3.2 binds `MTLBinaryArchive` (`addRenderPipelineFunctionsWithDescriptor`, `serializeToURL`) and
  `setBinaryArchives` on both pipeline descriptors, the hal descriptors already carry `cache`. Our driver cache
  (`.driver`, seed) would stay unchanged and take effect as soon as the feature is reported. Estimate about 150–250 lines of Rust
  (archive via temp files, since the API wants URLs; adding after the pipeline build, presumably with another translation,
  so in the background; a mutex as long as thread safety is not clarified) plus maintenance with every wgpu update.
  Benefit small: Metal's own shader cache (`com.apple.metal`) saves the translation from the second start on, on iOS
  and tvOS until uninstall. The archive would only help at the first start after install, OS update or
  cache clearing; on the M1 that is once ~740 ms in frames 1–100. A shipped archive would only apply to
  the same GPU family and OS version, so it would have to be collected per device (iPad M1, Apple TV A15) and OS version.
  Build only when a cold first start on a device costs several seconds; that would only be measurable there via
  uninstalling, which is not intended.

### Asynchronous pipelines in the browser

**Measurement (PC, RTX 5070 Ti, Chromium with D3D12, battle script, `--clock virtual`, `--pipeline-cache none`,
2026-10-13, main b7a6406):**

| | cold (Dawn/driver cache empty) | Dawn cache warm |
|---|---|---|
| Graphics programs | 223, 1784 ms (peak 23 ms) | 174, 1387 ms |
| Compute programs | 6, 22,472 ms (peak 8464 ms) | 6, 8395 ms (peak 8350 ms) |
| Draw pipelines | 267, 15,900 ms | 267, 13,251 ms |
| Start to frame 1 | 13.7 s | 7.3 s |
| Start to frame 6500 | 201 s | 183.3 s |

`use_dxc` is already the default in Chromium; nothing changed with the switch. In frames 1–300, 75
programs are created taking 14.2 s.

**Cause:** the guest thread waits in `create_program` for `popErrorScope`, and the GPU process only answers
when it has finished the commands before it. For compute programs that includes the synchronously created pipeline, i.e. the
whole translation (HLSL, DXC). Draw pipelines of a list are likewise created synchronously, one after the other in an
error scope, and the list waits for the result. A compute shader takes 8.35 s even with a warm Dawn cache.

**Remedy (default in the browser, desktop with `LGP_WEBGPU_ASYNC_PIPELINES=1`, switch off with `=0`, in the browser as
`--env` in `?args=`):**
- For compute programs `create_program` only creates the module and layouts and starts `createComputePipelineAsync`.
  The guest thread only waits for the WGSL validation.
- If a list needs a compute pipeline that is still being translated, the list waits for it instead of creating it a second
  time synchronously.
- Missing draw pipelines of a list are all started with `createRenderPipelineAsync` and then awaited
  together. Dawn can then translate them side by side on helper threads; on the PC this brought nothing measurable
  (see below). Prebuilds from the
  pipeline cache that are still running are awaited by the list in the same way.
- A rejected pipeline now only affects its own draws or dispatches. It is marked as invalid and
  not tried again. Previously an error discarded the list's whole group. If a shader variant or a
  layout is invalid (own error scope), the list still discards all its new draw pipelines.
- If a callback does not come (in CI `browser.pipeline_cache` hung once after 60 s, cause not found), the
  list creates the rest synchronously after 20 s (`LGP_WEBGPU_PIPELINE_WAIT_MS`) and reports `async pipelines not done after …`.
  A late result is then discarded. `LGP_WEBGPU_DEBUG=dropasync` discards all results for lists and checks
  this route (`nvn.webgpu_draw_async_lost`; `wait_prebuilds` hangs in the process).
- On the desktop wgpu-native has no `create*PipelineAsync`. The switch takes the same route there with a
  synchronous substitute so that the tests cover it: `nvn.webgpu_draw_async` (including a compute pipeline that only fails when
  the pipeline is created) and `nvn.pipeline_cache_async`.
- **Metrics:**
  - The waiting time of a list counts towards `draw pipelines created`.
  - If a list only waits for compute pipelines, that appears in the `shaders:` line as "dispatches waited for one".
  - In the browser `compute program … created` now only measures the module and layouts.

**Limits:**
- The game frames do not change. Every list still waits until its pipelines are there, and there is no
  skipping of draws as with the "asynchronous shader compilation" of some emulators.
- There is only a gain in two cases. First, several pipelines of the same list are translated in parallel. Second,
  time passes between the creation of a compute program and its first dispatch during which the game keeps running.

**Without waiting (`LGP_WEBGPU_PIPELINE_NOWAIT=1`, only with async pipelines, default off):** a list whose
draw pipelines are still being translated is recorded immediately. Its affected draws are dropped until the pipeline
is there. This applies to new pipelines as well as to still running prebuilds from the pipeline cache. This is the
"asynchronous shader compilation" of some emulators: an effect is missing for a few frames on its first occurrence instead of the
game standing still. The list still waits for compute pipelines, because their results go back into guest memory.
- Finished pipelines are taken over by the next list (`harvest_prebuilds`). Only then does it enter them into the pipeline cache.
- If the preparation fails (shader variant, layout), the group's pipelines are marked invalid after the fact
  (`reject_nowait`).
- Metric: `draws skipped while compiling` in the `gpu work` line.
- Risk: a draw that renders into a texture the game keeps using over several frames (a reflection rendered once,
  shadow map, intermediate image) is missing there permanently until the game redraws it. Whether LGP has something
  like that is not checked. Hence the switch stays off until there is a game test with an image comparison.
- Desktop: the synchronous substitute delivers immediately, so no draw is dropped there. The tests
  `nvn.webgpu_draw_async_nowait` and `nvn.pipeline_cache_async_nowait` only check the flow; the effect only shows
  in the browser.

**PC measurement (2026-10-13, 5ffdffd, Chromium/D3D12, battle script, `--clock virtual`, `--pipeline-cache none`, ABAB;
A = `LGP_WEBGPU_ASYNC_PIPELINES=0`, B = default):**

| | A cold (2 runs) | B cold (2 runs) | A warm | B warm |
|---|---|---|---|---|
| Start to frame 1 | 13.7 s | 7.5 s | 7.4 s | 7.4 s |
| Start to frame 6500 | 198.9 / 198.8 s | 177.7 / 179.6 s | 188.9 s | 173.9 s |
| Creating programs | 24.4 / 24.2 s | 2.15 / 1.95 s | 17.4 s | 1.6 s |
| Draw pipelines | 16.3 / 15.9 s | 18.8 / 18.9 s | 14.0 s | 13.7 s |

- Frame 6500 has the same `--frame-stats` in all six runs.
- The compute pipelines disappear completely from the waiting time. Only 1–2 dispatches had to wait for one, each
  0.0 ms; so LGP creates its compute programs early enough before use.
- Cold, the first frame comes after about half the time, the battle runs about 20 s shorter.
- Draw pipelines do not get faster cold, but 2.5 s slower. Presumably they compete with the
  compute translations in the background; whether Chromium really translates the draw pipelines of a list in parallel the
  measurement does not show. Warm they are the same.
- Safari (MacBook Air M1, same state, cold, ABAB): neutral. Start to frame 1 5.3 versus 5.5 s, frames/s in battle
  the same. There creation costs little anyway (about 1.3 s in the first 1200 frames). At startup the log says
  `async pipelines on|off`.

**Prewarm everything at once (`LGP_WEBGPU_WARM_ALL`, default like `async pipelines`):** with a pipeline cache
`set_pipeline_cache` immediately creates all cached programs and starts their pipelines in the background, instead of only as many
after each frame as fit into 3 ms. In the browser creating a program on the GPU thread costs little (module
and layouts; Chrome only translates when building the pipeline), the pipelines are translated side by side by Dawn. This way even a
compute program whose Metal compilation in Chrome on the Mac takes 8.4 s despite a warm Chrome shader cache starts long
before the game needs it (Chrome only stores the translation, not the Metal compilation result). In the log: `N cached
programs warmed at once in X ms`. The order is that of the file (order of first use in earlier runs). On the
game (Mac M1, battle, warm cache, `docs/analysis/20-route1-30fps-zeitaufteilung.md`, part 2): the prewarming itself
takes 59 ms (Safari, 225 programs) or 80 ms (Chromium, 313); program creation in frames 1–600 drops in
Safari from ~240–310 ms (peaks up to 8.8 ms) to ~30–36 ms (≤ 3.5 ms), in Chromium from ~60 to ~14 ms. In exchange
in Chromium the first frame consistently comes 0.35–0.45 s later (Safari within noise). Less stutter in the game weighs more,
so the switch stays on by default.

**Time limit and program end:** `emscripten_set_timeout` keeps the runtime alive until the time limit expires. Until
the time limit was cleared at the end of the waiting time, `exit()` in the browser therefore came up to 20 s too late (every
browser test with asynchronous pipelines ran 20 s longer).

### Missing pipelines of a list side by side (desktop, phones)

Without asynchronous pipelines (desktop and Android/iOS native) the GPU thread created the draw and compute pipelines that
a list is missing one after the other. On the Adreno 640 a new draw pipeline costs about 127 ms cold
(62 pipelines in frames 5701–6000 taking 7.9 s, `--pipeline-cache none`), and the game stands still that long.

`LGP_WEBGPU_PIPELINE_THREADS=N` (1–16, default 4, on Android 1 = one after the other): the GPU thread prepares all descriptors
(variant modules and layouts are created there) and then creates the pipelines side by side on N threads; the list
waits for all of them. Errors of the helper threads land in the GPU thread's open error scope, because wgpu-native keeps one
scope stack per device and the GPU thread keeps it open until the helper threads finish. As before, an
error discards all new pipelines of the list. In the browser the switch does not apply (there `createRenderPipelineAsync`). At startup the
log says `pipeline threads N`.

That wgpu-native does not serialize creation is proven in the cloud container (lavapipe, 4 cores, 24 different
pipelines with 400 lines of fragment shader each, version 29.0.1.1): sequentially 500 ms, on 4 threads 171–196 ms. Whether the
Adreno driver translates in parallel too is not measured yet; there the default stays 1 until a
measured gain on the device.

On Metal the gain is measured (MacBook Air M1, game cold, ABAB):
with 4 instead of 1 thread the time of the draw pipelines drops from 31.0 to 13.4–13.5 s (−57 %) and the wall time from 252 to
235 s; the fps median rises from 45 to 51, the minimum from 20 to 28; the frames are bit-identical. On the PC (RTX 5070 Ti, Vulkan, game cold, ABAB, 2026-10-14) the time of the 200 draw pipelines drops from 8197/8024 to
3576/3548 ms (−56 %). The wall time goes from 61.4/58.5 to 54.7/54.9 s, the fps minimum from 29.3 to about 35; the
frames are bit-identical at all 20 checkpoints. Therefore 4 is the default everywhere except on Android, where the measurement
on the device is still pending.

**Measuring cold on the PC:** the NVIDIA driver has its own shader cache on disk, which `--pipeline-cache none`
does not switch off (`__GL_SHADER_DISK_CACHE=0` is ignored under Vulkan). With a warm driver cache
the same 200 pipelines only took 443–560 ms; that is the share of wgpu/naga, about 2.5 ms per pipeline. For cold runs
set `__GL_SHADER_DISK_CACHE_PATH` to an empty directory per run.

### Why a program needs several pipelines

A draw pipeline depends on the program and on the whole fixed state: topology, target formats, vertex layout, blend,
write masks, depth/stencil, culling, depth bias values, multisample, alpha test including reference value. If only one
field of it changes, a new pipeline is created, and the driver translates the shader again. Which fields these are
in the real game is shown by `LGP_WEBGPU_DEBUG=pipelinevariants`: every new pipeline is compared with the most similar
already requested one of the same program; the lines `diag: pipeline variant: <fields>` in
`--frame-stats` count what they differ in (`first of its program` = first pipeline of the program, nothing
can be saved for it). If `alpha ref` or `depth bias values` often appear there, it pays to take these values out of the
pipeline key into a uniform buffer; with `targets` or `vertex layout` that does not help. Test:
`nvn.webgpu_draw_pipeline_variants`.

### Finding expensive compute shaders

A few compute programs carry almost the whole cold start: Android (Adreno, P7.3) needed 107.9 of
108 s for four of them (one 56.8 s), PC Chromium cold 22.5 s for six (one 8.5 s), Chrome on the Mac 8.4 s for one even warm. Safari
is cheap (about 1.3 s in the first 1200 frames). Whether they are the same shaders on all devices was open, because the
program numbers are only the order of creation.

- **Log:** every compute pipeline that takes 2 ms or more appears in the log with program hash, WGSL size and route:
  `compute pipeline <hash> (WGSL N bytes) ready after X ms (async|worker|on use)`. With `async` the time counts from the
  job, waiting for other translations included. With `worker` the time counts from the start on the
  background thread; the waiting time before it follows as `X ms queued before`. Slow `create_program` calls also name the hash
  and WGSL size. The hash is the same across devices as long as the WGSL is the same (pipeline cache hash).
- **Draw pipelines:** every draw pipeline from 2 ms appears in the log likewise:
  `draw pipeline <hash> (WGSL N bytes) ready after X ms (on use|worker|async)`. `<hash>` is that of the graphics program,
  N the size of vertex and fragment WGSL together. A program has one pipeline per render state it is
  drawn with, and therefore appears several times. `on use` counts from the start of creation on the respective thread
  (so with `LGP_WEBGPU_PIPELINE_THREADS` without the waiting time for other pipelines of the list), `worker` from the start on
  the background thread, `async` from the job. `wgsl_stats.py --all` sums these times per program and counts the
  variants (column `<device> draw ms (n)`).
- **Dump:** `LGP_WEBGPU_DUMP_DIR` now also writes compute programs, as `prog<id>-<hash>.cs.wgsl`. Programs adopted from the
  cache (`adopt_warm`) are not written; for a complete dump use `--pipeline-cache none`. The backend creates the
  directory on first use (before, a missing directory on a device gave an empty dump without a message; now a
  directory that cannot be created gives a warning in the log).
- **Evaluation:** `python3 tools/analyze/wgsl_stats.py <dump> --log android=<log> --log chrome=<log> …` puts per
  program the shape (size, lines, blocks of the dispatcher loop, lockstep, loops, calls into global
  memory, registers, words of private arrays) next to the slowest time per device. Dump and output table with
  hashes stay in `$LGP_PRIVATE_DIR`; the numbers can be shared.
- **GPU run time instead of creation:** `LGP_WEBGPU_DEBUG=gputime` puts a timestamp pair around every compute and render pass
  (device feature `timestamp-query`, which the renderer only requests with this option; without it there is
  a warning in the log and nothing is measured). With `--time-stats N` the line
  `gpu time, frames A-B: compute X ms/frame (n passes), render Y ms/frame (m passes), untimed k; compute programs
  ms/frame: <hash> Z, …` follows per window (the five most expensive programs). `wgsl_stats.py` takes the highest value per hash from it as the
  column `<name> gpu ms/frame`. At most 256 passes are measured per list, the rest counts as `untimed`. The
  results come back one frame later; the list does not wait for them. Browsers round the timestamps (Chrome
  without a flag to 100 µs), so individual short passes are inaccurate there; sums over a window are sufficient. Test
  `nvn.webgpu_draw_gputime`.
  A second line `gpu time by target, frames A-B: N targets, ms/frame: draw 1280x720 c12 d5 9.10 (3.0 passes), clear 1280x720
  d5 0.40 (1.0 passes), …` ranks the render passes by kind and target (the ten most expensive, names as with
  `passlabels`, passes per frame in brackets; the line above stays as it is, tools parse it). It is a ranking, not a
  breakdown: where the GPU works on several passes at once (M1, see below; unchecked on Adreno and Mali), the times
  overlap and their sum exceeds the frame time. Texture ids are part of the name, so a target the game creates anew
  each frame appears under new names and the count `N targets` grows.
  A third line `gpu time by draw program, frames A-B: N programs, ms/frame (pass time split by draws): <hash> X (D
  draws), …` splits each draw pass's time over its graphics programs by draw count (the ten largest). That is an
  estimate for a ranking only: a full-screen draw and a small one get the same share, so a program with many cheap
  draws ranks too high and one expensive draw in a crowded pass too low (Analysis 34 §11 saw the estimate off by a
  factor of three). What a program really costs is shown by `LGP_WEBGPU_SOLID_PROGRAMS=<hash>,…` (hex prefixes):
  only these programs draw single-coloured, like `LGP_WEBGPU_DEBUG=solid` for all (wrong image, no pipeline cache;
  log line `program … solid (LGP_WEBGPU_SOLID_PROGRAMS)`; a listed vertex-only program, such as a depth-only pass,
  has no fragment work to take away and stays as it is, log line `… vertex-only, no fragment work, not solid`). What the
  shadow lookups of all programs cost together shows `LGP_WGSL_COMPARE_CONST=1`: every depth-compare sample yields 1
  (lit) without reading the texture (wrong image, no pipeline cache); compare the target's time with and without, best with
  `--clock virtual`.
- **Effects at half resolution** (`WebGpuBackend::set_halfres_effects`, in the boot program `--effects half`, for measuring
  `LGP_WEBGPU_HALFRES_PROGRAMS=<hash>,…`): the draw runs into a substitute target of half
  width and height (cleared to zero, the same pipeline, depth via a substitute of that size), then an
  own pass adds it bilinearly onto the real target. That gives the same sum apart from resolution, as long as the draw only
  adds: one color target, destination factor one and operation Add for color and alpha, source factors without destination value, no
  depth or stencil test, one sample, and the shader does not read its screen position. Otherwise the draw stays at
  full resolution, with a counter `half-res: <reason>` in the diagnostic lines; successful ones are counted by
  `half-res: draws at half resolution`. The half draw and the adding up are each their own render pass; so what is compared
  is the render time per frame (`gputime`), not that of a single pass.
  The effect becomes softer; whether that is disturbing is decided by looking at the image, not by the frame grid comparison alone.
- **GPU time per target in the profiler:** the `gputime` timestamps per pass overlap on the M1 (sum about twice
  the frame time and 2.5 times the busy GPU time in Metal System Trace, Analysis 37 §5; suspicion: the GPU
  works on several passes at the same time), so a breakdown by target is not possible with them.
  `LGP_WEBGPU_DEBUG=passlabels` gives every pass a name after its target, which wgpu passes on to the backend:
  `draw 1280x720 c12 d5 present` (draw pass, size of the first target, color and depth texture, `present` = color target is
  one of the textures of the last four presents, i.e. one of the rotating window images), `clear 240x135 c3`, `clear 1280x720 d5`, `blit 640x360 c7`, compute
  `cs <program-hash> 40x23x1` (groups). Instruments (Metal System Trace) and AGI then break down the GPU time per encoder
  or debug label. Metal always takes over pass names (`setLabel`), Vulkan only with the instance option `Debug`
  (`VK_EXT_debug_utils`); with this option the renderer sets it as `Debug | ValidationIndirectCall` instead of `Default`
  (suspicion, not checked: otherwise equivalent with release libraries; the environment variables `WGPU_DEBUG`,
  `WGPU_VALIDATION` then possibly no longer take effect, and under DX12 `Debug` can do more). Without the option the passes stay nameless (the blit is called `nvn blit`). Behind the
  switch because the names have costs that are not measured here (suspicion: one `snprintf` per pass, in wgpu a
  `String` and under Metal an `NSString`, a few microseconds at 20–60 passes per frame), and because `Debug` under
  Vulkan can change more than the names. Test `nvn.webgpu_draw_passlabels` (only runs through, does not check the names).
- **Suspicious shapes (assumption, unmeasured on these shaders):**
  - Code with branches runs as `loop { switch pc { … } }` with one case per block. Driver compilers (Metal, Adreno)
    can poorly recover structured control flow from that.
  - With a barrier in branching code the lockstep loop is added, with two barriers per step.
  - Every access to global memory calls `gmem_ld`/`gmem_st` with a `switch` over all slots; the
    address resolution `gmem_addr` searches a table.
  - Without a size from the SPH (compute has none) the local memory per thread is a private array of 4096 bytes;
    in addition there are the SSY/PBK stacks with 16 words each.
- **Static SSY/PBK stacks (`wgsl::Options::static_stacks`, `LGP_WGSL_STATIC_STACKS=1`, default off):**
  structured Maxwell code has the same stack content at every point, no matter by which path it is reached.
  The translator computes the stacks forward over all instructions. If the content is unambiguous everywhere, no push
  is predicated, no pop happens on an empty stack and the depth is at most 16, then `SYNC`/`BRK` jumps directly to the target, and
  the private arrays `ssy_stack`/`pbk_stack` (32 words) go away. Otherwise the dynamic form stays. Occasion: on the Mac
  (Metal, cold) the most expensive draw pipelines cost 250–350 ms. All of them have the dispatcher loop and these 32 private
  words. Whether the arrays are the cost driver there is an assumption that the measurement still has to show. Tests:
  `gpu.translate_static_stacks`, `gpu.naga_validate_static_stacks`, `gpu.render_static_stacks` (same pixels).
- **Structured control flow (`wgsl::Options::structured`, default on, `LGP_WGSL_STRUCTURED=0` switches it off):** instead of the
  dispatcher loop `loop { switch pc { … } }` the translator writes `if`/`else` and `loop { }` following the
  control flow graph. Precondition are statically resolvable stacks (as above). The procedure:
  - The blocks (the same leaders as for the dispatcher) form a graph. Back edges from a depth-first search
    must close natural loops with exactly one entry, the header. If the way back from the loop end reaches
    the shader's entry, the header does not dominate it (second entry, irreducible): dispatcher.
  - Every loop may only be left towards one block, otherwise only via `EXIT`.
  - The post-dominators are computed without back edges, with a sink for `EXIT` and the end of the code.
  - A conditional branch becomes `if`/`else` up to its immediate post-dominator. A header becomes
    `loop { }`, edges to the innermost header become `continue`, edges to the loop exit `break`.
  - If the traversal reaches a block twice, the regions overlap. Then the dispatcher stays, likewise for
    compute in lockstep (barrier with branches) and with more than 128 nesting levels (the traversal
    recurses per level, threads under Emscripten have small stacks). The fallback is checked by `fs_irreducible`.
  - The header line of the WGSL names the shape, for counting over many shaders: `// Control flow: structured`, `dispatcher`
    or `dispatcher, static stacks`, for the dispatcher with the reason in parentheses (`structured off`, `lockstep barrier`,
    `stacks not static`, `not structurable`). The boot program logs every stage translated this way
    (`nvn: fragment shader in the dispatcher form: (…)`).
  - On the Adreno 640 (OnePlus 7 Pro, Vulkan) the dispatcher form computes wrongly: with `LGP_WGSL_STRUCTURED=0`
    bushes in Pallet Town were magenta from frame ~5100 (frame 6000: 52334 magenta pixels, mean deviation from the PC 38.4),
    structured not (≤ 0.6). Affected are fragment shaders in dispatcher form with `discard` (24–31 KB of WGSL). The
    cause in the driver is not narrowed down (suspicion: `discard` inside `loop`/`switch`). Therefore the
    dispatcher must not be a silent fallback on Android; the log line shows whether one occurs.
  Occasion: on the Mac the most expensive draw pipelines all have the dispatcher loop.
  - Measured on the PC (RTX 5070 Ti, Vulkan, cold with an empty driver cache, 4 pipeline threads, 6000 frames, ABAB):
    draw pipelines 3744/3643 ms → 3290/3248 ms (−11 %), programs 2499/2390 → 2176/2080 ms (−12 %).
  - The most expensive program dropped from 402 to 298 ms (WGSL 112 → 89 KB). The frames are the same at all 20 checkpoints.
  - Static stacks alone brought −4 % for the pipelines and nothing for the programs.
  - Coverage over all 5170 stages of the game: the 3856 with branches are all structured, no fallback to the
    dispatcher, the rest is straight-line.
  - Measured on the Mac (M1, Metal, cold with an emptied Metal cache, 10500 frames, ABAB): draw pipelines (273)
    13.83 → 12.43 s (−10 %), programs (296) 2.58 → 2.22 s (−14 %), of which compute 1.62 → 1.29 s (−20 %). The
    slowest draw pipeline dropped from 363 to 254 ms, the largest shaders from 90–107 to 67–84 KB.
  - The Mac frames deviate by at most 1 in the mean color of a grid cell, mean 0.00. Presumably
    the Metal compiler orders floating-point computations differently in the new code.
  - Adreno (Android) is still unmeasured.
  Tests: `gpu.structured_shape` (every test shader with branches except the lockstep one comes out structured).
  The dispatcher form is still checked by `gpu.translate_dispatcher`, `gpu.naga_validate_dispatcher`, `gpu.tint_compile_dispatcher`
  and `gpu.render_dispatcher`, as are the `*_static_stacks` tests (same pixels). Own test shaders for this: `fs_branches` (nested `if`/`else`, two
  loops with `BRK`) and `fs_exit_paths` (`SSY` without `SYNC`, both paths end in `EXIT`).
- **Measured (lavapipe, synthetic):** the search in `gmem_addr` is the largest item. The driver compiler
  (Mesa/NIR) inlines every helper function at every call, so the loop over 12 table entries and the
  `switch` helpers appear once per access in the code. Shader with 200 accesses: 12.2 s with the search, 22 ms without the
  loop; `cs_memory` twenty times in a row (200 accesses, 100 KB of WGSL, about as large as program 77)
  32–37 s with the search, 0.34 s with direct access. A dispatcher loop with many blocks, on the other hand, cost little, almost
  linearly. Pipeline-overridable constants with a dead fallback branch only helped half (2.1 s): the dead branch is only
  removed after inlining.
- **Direct access (`wgsl::Options::storage_desc_offset`, `uniform_desc_offset`):** the translator tracks via
  data flow over the registers (forward analysis over the blocks, `Pop` to all `Push` targets) whether an address comes from
  exactly one storage descriptor `c0[0x310 + 16 k]` or uniform descriptor `c0[0x210 + 16 s]` (load, `MOV`,
  additions with non-pointers, `LEA`/`LEA.HI`, `XMAD` via the addend, `SEL`). Then the access only checks one
  table entry (`gmem_addr_at`) and reads the binding directly: storage slot k via `gm<k>_ld` etc., uniform block s
  via a fixed constant window (`gu<s>_ld`, at most 4 blocks per program; writing there is dropped as
  before). Other accesses search as before and carry a comment `// gmem search: <reason>` in the WGSL (not
  resolved, 32-bit address, address from data, several sources, low/high disagree, more uniform blocks than windows).
  If a dispatch binds a range in two slots (WebGPU forbids writable ranges bound twice, the
  backend binds it once) or a directly read uniform block lies in a bound storage slot, the
  backend synchronously builds a variant with `wgsl::move_static_slots` (the target reads the other entry) and counts
  `diag: dispatch: a range the shader reads directly is bound in another slot`. `LGP_WEBGPU_DEBUG=gmemsearch`
  switches back to searching everywhere, for A/B measurements. `wgsl_stats.py` shows per program the columns `gmem search`,
  `gmem direct` and `search why` (reasons counted).
- **Measured on the game:** with uniform windows, in the six most expensive programs (67–70, 76, 77) only 4
  of a total of 284 accesses still search, all in 77 (`mixed`: address from several sources); before, 77 and 76 searched in 57 and
  50 accesses respectively. PC (Vulkan): frames bit-identical to before (virtual clock, 10 frames). With storage slots direct only: Mac
  native (Metal), cold with an emptied Metal compiler cache: all expensive compute programs together 2868 ms before (sum of the
  logged creation times), 2218 ms direct (13–45 % less per shader); warm both about 40–46 ms. Caution when measuring on macOS: the
  Metal compiler keeps its own cache under `$(getconf DARWIN_USER_CACHE_DIR)/com.apple.metal`; a shader it
  already knows takes a few milliseconds. Comparing cold against warm at first looked like a 40–65-fold
  deterioration.
- **Android (OnePlus 7 Pro, Adreno 640, cold with `--pipeline-cache none`, ABAB, every start below 35 °C):**
  compute pipelines together 449/444 s before, 5.3/2.8 s with direct access (program 77: 188 s → 3.6/1.8 s, 76:
  155 s → 0.7/0.4 s). Frames 1–300: 134 s (2.2 fps) → 27–30 s (10–11 fps); wall time to frame 5700: 1236/1270 s →
  800/858 s. Memory peak (VmHWM) unchanged. So the Adreno compiler suffers particularly from the search loop
  over all slots.

### Upload geometry only on change

Mac (Safari, Pallet Town, still scene, frames 3000–4500): 18.2 MiB of uploads per frame, of which 16.3 MiB vertex and
index data (782 uploads of Ø 22 KB each), 1.5 MiB constant buffers, textures 0. The arena re-uploaded what every list
needed; reuse only happened within the same list.

Now the backend keeps an own GPU buffer per guest range (address, length, kind: vertex or index with type)
and a copy of the guest bytes it was written from (`State::geometry_cache`). If the bytes in
guest memory still match (`GuestMemoryView::equal`, in the boot program a `memcmp` on the host mapping), the buffer is
bound without an upload; otherwise it is rewritten (`queue.writeBuffer` before the submit of the list).

- A queue write takes effect before the whole list. If a range was already bound in the same list and has
  changed since (a `CmdCopyBuffer` writes into guest memory during recording), the new content goes into
  the arena; the first draw keeps the old bytes. Test in `nvn.webgpu_draw`.
- Index data: the guest bytes are compared; the buffer holds the indices (widened from 8 bit to 16 bit where applicable).
- Merged vertex streams (device with only 8 vertex buffers) and constant buffers still go through the arena.
- Upper limit 256 MiB (GPU buffers plus copies). If a new entry no longer fits, the least recently
  used entries of earlier lists are dropped until 3/4 is reached. If the current list fills the cache alone,
  its rest goes through the arena.
- Metric: `unchanged since an earlier list` in the `uploads:` line of `--time-stats`/`--frame-stats`.
  Switch off with `LGP_WEBGPU_GEOMETRY_CACHE=0`.
- Cost: the comparison reads the guest bytes once (16 MiB per frame, natively about 1 ms; Wasm not measured) instead of
  copying and uploading them. In the benchmark (`lgp_webgpu_draw_bench --vertex-bytes 262144`, cloud, lavapipe, debug)
  the vertex/index uploads drop from 1088 to 6 KiB per frame; the frame time stays the same, the CPU is bound
  elsewhere there.
- In the browser (LGP, Pallet Town): queue writes 18.2 → 1.9 MiB per frame, Safari on the M1 +11 % frames/s,
  Chromium on the PC +7–10 % (measurement in `docs/analysis/20-route1-30fps-zeitaufteilung.md`).
- Fingerprint instead of a copy (default on under Android/iOS/tvOS, otherwise off; `LGP_WEBGPU_GEOMETRY_FP=0/1`) stores per entry only a
  64-bit fingerprint of the guest bytes (the same function as for textures) instead of the copy. Hashing happens directly in
  guest memory (`GuestMemoryView::view`, in the boot program the host mapping), without direct access after a
  `read`. This halves the cache and takes its whole CPU share out of `malloc` (Mac native: about 1 GB of malloc in the
  footprint, share of the cache still open; tvOS jetsam). Cost: hashing instead of `memcmp`, equally fast on the cloud machine
  (16 MiB in pieces of 4–128 KiB: ~3.4 ms each, both limited by memory bandwidth); not measured on
  ARM. A collision would leave stale geometry in place, as with textures. Test
  `nvn.webgpu_draw_geometry_fp`. ABAB on the Mac (native, save up to frame 8500, two runs each): footprint
  1703–1722 → 1629 MB (−75 to −93 MB), geometry cache copies 71.4 → 0 MiB, frames/s the same (56.5–56.9 and
  21.0–21.2 respectively). Hence the default on the mobile devices (jetsam, little RAM); off on the desktop, because memory is rarely tight there
  and the frame time on weaker desktop CPUs is not measured.

### Texture upload when entering the route

When entering the route one frame on the PC stalls 85–93 ms in `textures from pools`. The log line names 321 uploads
and 0 ASTC levels decoded on the CPU. So the stop comes from the uploads themselves (reading from guest memory,
fingerprint, deswizzle, `writeTexture`), not from the ASTC decoder. LGP has only a few small ASTC textures.
An attempt to decode large ASTC images on helper threads therefore changed nothing on the PC and was not
taken over.

PC run (route trip to frame 10,500, native): `textures from pools` 5235 ms in total, of which reading and fingerprinting
949 ms for 3.3 GiB and `writeTexture` 43 ms. About 4.2 s were not assigned to any item, and the largest single frame
was at 26 ms. The log line and the `shaders:` line at the end of `lgp_game` now break down the time completely:
- checks of block linear textures (count), reading and fingerprinting.
- deswizzle and conversion (ASTC included).
- `writeTexture` (count, time, MiB).
- pitch linear textures (count, time, MiB). As CPU-written caches (glyphs) they are checked every frame,
  block linear textures only every 60 frames.

The deswizzle now computes the GOB address once per GOB instead of per texel; in the container that is about
1.5 times faster cold, warm no measurable difference.

Second PC run with this breakdown (same route): 4772 ms, of which 77 % (3683 ms) pitch linear checks
(4269 checks, 8.5 GiB, i.e. about 2 MiB and 0.86 ms per check), 19 % reading and fingerprinting of the
block linear textures, 2 % `writeTexture`, 1 % deswizzle. A large stop when entering the route no longer
appeared (largest single frame 26 ms); the costs are ongoing. Countermeasures:
- Pitch linear textures keep the bytes of their last upload and compare with `memcmp` instead of hashing.
- Reading uses a reused buffer; a fresh buffer of megabytes cost page faults and zeroing at every check.
- The fingerprint computes four independent chains (about twice as fast in the container).
Micro-measurement in the container for 2 MiB: before about 950 µs (new buffer, copy, hash), with a reused buffer
and `memcmp` about 340 µs.

Third PC run (same route, with `memcmp`): `textures from pools` 2143 ms, of which 70 % (1505 ms) pitch linear.
Because of different load the absolute values are not directly comparable with the second run. Spread over the route of
10,500 frames that is about 0.15 ms per frame on average.

After that the check compares directly in guest memory (`GuestMemoryView::equal`, in the boot program a `memcmp` on the
host mapping) and only copies on a change. In the container that costs about 150 µs for 2 MiB instead of 335 µs
for copy and comparison. Views without direct access compare in pieces of 4 KiB.

Rejected as long as measurements do not demand it:
- Checking less often (every N frames): new glyphs would then appear up to N−1 frames too late, new text would
  flicker or appear with gaps.
- Write tracking of the runtime (`LGP_GUEST_MEM_WRITE_WATCH`): it is a compile-time switch and costs
  a page table read on every store of the lifted code. Writes by the HLE via
  `GuestMemory::host_ptr` (such as reading files into guest memory) mark nothing; they would be missed.

### Buffer pool for list buffers (switch, off)

Besides the fixed arenas every list creates further buffers anew and frees them after the submit: copy arena,
storage buffers of the dispatches, slot tables for global memory, blit uniforms, intermediate buffers for copies into
dispatch outputs and read-back blocks. On Vulkan that costs `vkCreateBuffer`, memory allocation
(gpu-allocator) and `vkDestroyBuffer` per list. In the PC profile (Route 1) gpu-allocator allocations show up. That they come from
these buffers is a suspicion, not measured.

`LGP_WEBGPU_BUFFER_POOL=1` returns these buffers to a pool after the list instead of destroying them. The
return waits for the list's `wgpuQueueOnSubmittedWorkDone` (read-back blocks after their map callback); the next
list takes a free buffer with the same usage and size. The pool keeps at most 64 MiB free, on Android,
iOS/iPadOS and tvOS 16 MiB (estimated, not measured; `LGP_WEBGPU_BUFFER_POOL_MB=N`), and discards the
oldest beyond that. A read-back block whose map failed is destroyed instead of returned. With `--time-stats` the line
`gpu buffer pool` shows reused and newly created buffers and the free memory in the pool. Test:
`nvn.webgpu_draw_buffer_pool`.

The default stays off until a measurement on the game shows a gain. The hit rate is in the same line
(reused versus newly created).

## Render passes and their causes

On tiled GPUs (Adreno, Mali, Apple) every render pass loads and stores its attachments. Many short passes therefore
cost GPU time, even if they contain few draws. With `--time-stats N` there is per window the line

```
gpu passes, frames 2701-3000: render passes 17188 (57.3/frame), draw passes D (X draws each), new attachment sets K; draw pass ended: render targets changed N, own pass: colour clear M, …
```

The causes are listed in descending order of count.

- `draw pass ended: …` says why a pass with draws ended: new render targets (`render targets changed`), the same
  targets set again (`render targets set again (same)`), depth becomes readable or writable again, copies, clears,
  a dispatch in list order (`dispatch (in order)`), a hoisted dispatch (`dispatch (ahead)`) and the
  end of the list (`end of list`). A dispatch that records nothing (no compute program, zero or too many groups)
  does not end the pass.
- `new attachment sets` counts passes whose set of attachment views the backend has not seen yet. That is an
  estimate of how often the Vulkan driver creates a framebuffer object (`vkCreateFramebuffer` showed up in the simpleperf of the
  Android dip). View handles reused after a release count as known; swapchain images change
  per frame.
- `own pass: …` counts passes that serve only one command: color clears (whole via load op, partial as a triangle),
  depth/stencil clears and blits.
- A hoisted dispatch (`LGP_WEBGPU_COMPUTE_FIRST`, default) runs in its own encoder and would not need to end the
  open draw pass. `LGP_WEBGPU_KEEP_PASS_ON_DISPATCH=1` keeps it open (default under Android,
  on the OnePlus 7 Pro +7.5 % fps in frames 2401–3600, within the spread, slower in no run (Analysis 34); otherwise off, `=0` switches it off; test `nvn.webgpu_draw_keep_pass`).
- Measurement on the OnePlus 7 Pro (P7.3, frames 2401–3600, `--clock host`, first ABAB pair; both pairs together +7.5 %, Analysis 34): with
  `LGP_WEBGPU_KEEP_PASS_ON_DISPATCH=1` the frame rate rose from 14.50 to 16.57 fps, and the passes per frame dropped from
  57.6 to 52.5. In the dip, per 300 frames mainly `render targets changed` (9300) remained, 1800 own clear passes each
  for color and depth, `render targets set again (same)` (~1680) and `copy texture to texture` (~880).
- `LGP_WEBGPU_FOLD_CLEARS=1` (default under Android, otherwise off; `=0` switches it off): a clear over the whole texture
  (color with all channels, depth or stencil entirely) no longer gets its own pass. It waits for the next
  draw pass that has the texture as an attachment and becomes the load op `clear` there (counter
  `folded: colour clear (no own pass)` or `folded: depth/stencil clear (no own pass)`). If something else uses
  the texture first, it gets the clear as its own pass as before, immediately before that. This applies to draws that
  sample the texture (the open pass then ends with `sampled texture has a clear pending`), copies from or into the texture,
  partial clears (with scissor or mask), dispatches in list order and the end of the list. A read-only
  depth attachment and a substitute depth attachment of a different size do not take the clear either. Hoisted dispatches
  never read a texture that the list writes or clears before, and therefore stay unaffected.
- `LGP_WEBGPU_KEEP_PASS_SAME_TARGETS=1` (default under Android, otherwise off; `=0` switches it off): if the same render targets are set again, the open
  draw pass stays open (counter `kept: render targets set again (same)`). If a draw then samples its own color attachment,
  that is just as invalid without the switch, because the new pass would have the same attachment.
- Measurement of both together on the OnePlus 7 Pro (Adreno 640, 2026-10-29, Arena, frames 1501–3000, `--clock host`,
  ABAB ×3): 8.87 → 9.33 fps (+5.2 %, every run with the switches above every run without), `gpu call finish` 62.6 → 56.5
  ms, render passes 48.7 → 38.0 per frame. With `--clock virtual` the frame grid at frame 1500 was identical (max cell
  difference 0, as between two runs without the switches); later frames were not compared (the runs with
  `--frame-grid` reached only about frame 2400 in 10 minutes). A second series on 2026-10-30 (same scene and window,
  ABAB ×2, build a26bf56) gave 9.09 → 9.24 fps (+1.7 %), with one run without the switches (9.41) above both runs with
  them; the spread within one arm was up to 7 %. Over both series (5 runs per arm) that is 8.96 → 9.29 fps (≈ +4 %).
  The fewer passes are certain; the fps gain is small and only partly above the spread. The switches stay on because
  no run showed a cost and the image is unchanged. On the Shield both were neutral (Analysis 49), so they
  are the default under Android only.
- `target set: back to an earlier one` counts draw passes whose attachments (color and depth) an earlier pass
  of the same list already had, i.e. ping-pong between a few targets. `…, draws could join it` counts among these the cases in
  which the draws could have joined the earlier pass without changing a result. For that,
  no pass in between may write or sample the targets of the later pass, none may write what it samples,
  and no copy and no clear may touch these textures. In addition there must be no dispatch in list order and
  no copy into a dispatch output in between, and the later pass must not begin with a folded clear.
  This is diagnosis only; nothing is reordered. It shows whether reordering the draws pays off.
- `store: colour|depth attachment, …` counts per draw pass the attachments it stores (StoreOp Store). It only counts
  real attachments. A read-only depth attachment and a substitute depth attachment of a different size do not count. For every
  attachment the counter states what happens to it next in the same list:
  - `read later`: a pass loads or samples it, a copy or a partial clear touches it, or a dispatch in
    list order lies in between. As a precaution the diagnosis counts the dispatch as a read.
  - `overwritten later before any read (discard candidate)`: a whole clear overwrites it, folded or as its
    own pass.
  - `not used again in the list`: nothing more in the list.

  On tiled GPUs (Adreno, Mali, Apple) every stored attachment costs bandwidth. A discard candidate could
  get StoreOp Discard. What comes after the end of the list, i.e. the next frame or present, the counter does not see.
  Here too only counting happens; no StoreOp changes.
- The counters `target set: …` and `store: …` exist only with `LGP_WEBGPU_DEBUG=passdiag` (or
  `WebGpuBackend::set_pass_diagnosis`), printed in the line `gpu passes` of `--time-stats`. Without `passdiag`
  `--time-stats` costs nothing for this, and measurement runs stay comparable with older builds.
  Without the switch the GPU thread creates no pass recording, and the comparison at the end of the list is dropped too.
- Both switches are checked by `nvn.webgpu_draw_fold_clears`. It runs through all image comparisons of `lgp_test_nvn_webgpu_draw`
  and checks the counters in the section on pass causes as well as in the depth comparison with a sampled target.

### Keeping framebuffers across encoder resets (Vulkan, own wgpu-native build)

`new attachment sets` underestimates how often `vkCreateFramebuffer` runs. wgpu-hal 29 (Vulkan, `command.rs`) keeps the
framebuffers in a cache per hal command encoder and destroys all of them at every `reset_all`. That comes as soon as the
encoder's command buffers are done, i.e. roughly every list. wgpu-core reuses the encoders from a pool.
So almost every render pass creates its framebuffer anew, even if its attachments have been the same for frames.

`tools/wgpu/vulkan-keep-framebuffers.patch` (patch `keep-framebuffers` in `tools/webgpu/build_wgpu_native.py`) lets
the framebuffers survive the reset with `LGP_WGPU_KEEP_FRAMEBUFFERS=1`. At the reset only those are removed that were not used for 16 resets,
and all of them if more than 256 remain. Destruction still only happens in `reset_all` or
at the end of the encoder, when no command buffer uses them any more. The key contains the identity of the views, which
wgpu-hal never reuses. A framebuffer for a released view is therefore never hit and ages out.
Without the variable the build behaves like upstream.

Counter-check (cloud, lavapipe, temporarily counted): `lgp_webgpu_demo --frames 200 --headless` created 201
framebuffers without the switch (1599 hits), with the switch 3 (1797 hits). How much time that saves on Android is not measured.
Build for example with `LGP_WGPU_NATIVE_PATCHES=driver-cache,keep-framebuffers tools/android/build_wgpu_native.sh …`. The
patch is not in the default set, and the switch stays off until a measurement shows a gain.

## 30 fps despite `--unthrottled` (window)

With `--renderer window` the surface used to always run in present mode FIFO. There every present waits for the
vsync of the screen. A frame that takes 17–33 ms then occupies two vsyncs, and the frame rate locks in at 30 fps.
That fits the PC measurements with #90 (5 of 6 runs at 30.1–30.5 fps).

Now `--unthrottled` requests mailbox in the window, alternatively immediate (with tearing), otherwise FIFO. The line
`present: unthrottled (window: …)` shows what the surface got. With `--paced` and in the browser it stays
at FIFO. A canvas knows nothing else; there the browser determines the pace.

The PC measurements, however, ran without a window (`--renderer webgpu`, no surface), where FIFO has no effect. The
runtime does not pace without `--paced` and without the virtual clock (`present_thunk` in `runtime/boot/src/boot.cpp`, the
`vi` vsync event is never signaled). According to the PC log LGP sets present interval 2, i.e. aims, as on the console,
at 30 fps and apparently holds this pace itself by guest time. How exactly is not investigated. For
performance comparisons ms per frame or runs with `--clock virtual` are therefore better suited than fps that stick to 30.

## Open

- A measurement on the real game in the browser (Chromium) is still pending. For comparison the Mac value after 53b0145:
  Chromium 63–67 fps.
- `pipeline_request` still runs twice per draw (pre-pass in `execute()` and `prepare_draw`), but can be cached
  between draws without a state change.
- Groups with global memory (graphics stages that read storage buffers) still create a bind group and
  a table buffer per draw.
- Exact constant buffers without `cbufzero`: declare banks only as large as the shader reads them (see above).
