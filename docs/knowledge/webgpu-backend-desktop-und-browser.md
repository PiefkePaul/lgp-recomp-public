# WebGPU as a renderer for desktop and browser: wgpu-native, emdawnwebgpu, threads

Cross-game knowledge from P4.4 (as of 2026-10-11). Code: `cmake/LgpWebGpu.cmake`, `cmake/LgpSdl.cmake`,
`platform/webgpu/` (`WebGpuRenderer`), `platform/desktop/` (`SdlWindow`), `gpu/webgpu/` (`WebGpuBackend` for the
NVN HLE), `platform/web/shell.html`, `tools/web/serve.py`. Tests: `ctest -L webgpu`, in the WASM build additionally
`-L browser`.

## One API, two implementations

Both sides implement the same standardized `webgpu.h` from
[webgpu-native/webgpu-headers](https://github.com/webgpu-native/webgpu-headers). The renderer code is therefore
identical except for a few `#if`s.

| | Desktop | Browser |
|---|---|---|
| Library | wgpu-native 29.0.1.1 (Rust/wgpu, Vulkan/Metal/DX12), prebuilt release zips, SHA-256 pinned | emdawnwebgpu (Dawn's `webgpu.h` over `navigator.gpu`), `--use-port=emdawnwebgpu` (built into Emscripten ≥ 4.0.10, downloads the package from GitHub on the first build) |
| Surface | `WGPUSurfaceSource{WindowsHWND,MetalLayer,XlibWindow,WaylandSurface}` from SDL3 window handles | `WGPUEmscriptenSurfaceSourceCanvasHTMLSelector` (`"#canvas"`) |
| Presenting | `wgpuSurfacePresent` | do **not** call it (aborts with `abort()`); the canvas presents when the main thread's JS task ends |
| Present modes | from the surface capabilities | only `Fifo` (emdawnwebgpu checks this via assertion) |
| Events | `wgpuInstanceProcessEvents` + `wgpuDevicePoll(device, wait=true)` (wgpu.h extension) | JS event loop; callbacks with `WGPUCallbackMode_AllowSpontaneous` fire there |

The Linux `.so` of wgpu-native has **no SONAME**. Linked as `IMPORTED` with a full path, the
build path ends up in `DT_NEEDED`. Solution: `IMPORTED_NO_SONAME TRUE`, then CMake links via `-l` and sets the RPATH.
The macOS dylib has `@rpath/libwgpu_native.dylib` as its install name (`IMPORTED_SONAME`). Under Windows,
`wgpu_native.dll` must go next to the exe (`lgp_webgpu_copy_runtime()`).

## Threads: all WebGPU calls on one thread

WebGPU objects in the browser belong to the JS realm (thread) that created them; they cannot be transferred
between workers. The game, however, calls NVN from several guest threads (= pthreads = Web Workers, ADR-0002). Solution:

- `WebGpuRenderer::run(task)` executes all GPU work on **one GPU thread**. In the browser this is
  by default the browser main thread; optionally a dedicated pthread (`RendererOptions::gpu_thread`, default
  off, switch `LGP_WEBGPU_GPU_THREAD=0/1`, in the game `?args=--env LGP_WEBGPU_GPU_THREAD=1`) that the renderer starts when
  it is created. The canvas moves there as an `OffscreenCanvas`
  (`emscripten_pthread_attr_settransferredcanvases`), the thread returns to its event loop with `emscripten_exit_with_live_runtime()`,
  and `run()` hands tasks to it via its own proxy queue
  (`em_proxying_queue_create`, not the system queue) with `emscripten_proxy_sync_with_ctx`. Without the
  pthread (default, or `pthread_create` fails) the GPU thread is the browser main thread: with `PROXY_TO_PTHREAD`, `main()` runs in a worker, and the main thread then owns canvas and device.
- What the dedicated pthread was meant for: the main thread also handles the page, input, audio control and video callbacks. Every
  handoff waits until it is free; in the probe (`browser.webgpu_worker_probe`,
  `docs/analysis/21-internet-recherche-werkzeuge.md`, A1) 5–7 ms per round trip under load, to the GPU pthread
  0.02–0.1 ms. In the game that is at least two handoffs per frame (`docs/analysis/20-route1-30fps-zeitaufteilung.md`, H2).
- Result in the game (Mac M1, Pallet Town, `--time-stats`): **no frame rate gain**. The handoff stayed at
  ~2.5–2.7 ms per call in Safari (Chromium ~1.2 ms), and most of the call is real work on the GPU thread
  (Safari ~4.4 ms, Chromium ~7 ms); the bottleneck is the WebGPU work (mainly uploads), not waiting for
  a busy main thread. That is why the switch is off by default; the path remains for later asynchronous
  handoffs and is tested via `browser.webgpu_demo_gpu_thread`.
- Teardown: everything that holds WebGPU objects (in the boot program the NVN backend) must be torn down **before** the renderer.
  With a dedicated pthread, `~WebGpuRenderer` ends the thread; an object released later then lands in
  a realm without these objects (`getJsObject(...).destroy` is `undefined`, the page hangs on exit).
- Build flags: `lgp_platform_webgpu` links `-sOFFSCREENCANVAS_SUPPORT=1` and an **empty**
  `-sOFFSCREENCANVASES_TO_PTHREAD=""`. The default `#canvas` would hand the canvas to `main()`'s thread at startup; then
  the renderer would not find it on another thread, the fallback to the main thread would no longer work, and
  pages without `#canvas` (Node tests) would abort at startup. In CMake as a bracket argument
  `[[-sOFFSCREENCANVASES_TO_PTHREAD=""]]`, otherwise the shell removes the quotes and emcc fails.
- The renderer passes the selector `#canvas` as `canvas` to `pthread_create`: `#canvas` is a special case there for
  `Module.canvas`, which does not exist on a pthread; Emscripten then logs an error before passing the call
  on to the main thread, which finds the canvas anyway.
- Presenting: without `wgpuSurfacePresent`; the canvas shows the frame when the GPU thread's task returns to the
  event loop (main thread as well as worker).
- The calling worker blocks until the task calls `emscripten_proxy_finish(ctx)`. This may happen **later** from
  a WebGPU callback (requesting adapter/device, `mapAsync`, `popErrorScope`), because the GPU thread
  returns to the event loop in between. This makes the asynchronous WebGPU API work from synchronous
  guest code without ASYNCIFY or JSPI.
- A mutex serializes `run()` so that two asynchronous tasks do not interleave on the GPU thread.
- Desktop: the same interface; the task runs on the calling thread (wgpu-native objects are
  thread-safe), waiting is done with `ProcessEvents`/`DevicePoll`.
- Cost: one synchronous round trip worker → GPU thread per NVN submit/present. The next step is to hand over commands asynchronously (only `present` and readbacks need to wait).
- How to tell the mode: `renderer: webgpu (…, own GPU thread)` in the boot program's log, `webgpu thread: own GPU thread`
  or `browser main thread` in `lgp_webgpu_demo` (`--gpu-thread` turns it on there).

Pumping window events (SDL), on the other hand, belongs on the process's main thread (macOS requires this). Guest
threads present; `end_frame()` only reads the "window closed" flag.

## Pitfalls

1. **wgpu-native aborts on invalid objects in a submit.** A validation error in
   `wgpuDeviceCreateRenderPipeline` is only reported, but a later `wgpuQueueSubmit` with this pipeline
   ends with a Rust panic and `abort()`. The backend therefore creates programs and pipelines inside a
   `wgpuDevicePushErrorScope`/`PopErrorScope` pair, waits for the result and skips draws with invalid
   pipelines. In the browser, on the other hand, invalid objects are just an error in the error callback.
   **Caution:** wgpu-native calls an `AllowSpontaneous` callback *inside* `wgpuDevicePopErrorScope` and
   holds internal locks while doing so. Anyone who keeps recording in the callback (`wgpuCommandEncoderFinish`) hangs forever. The
   callback may only remember the result; recording happens after returning (in the browser the callback comes later
   and may record itself). Found in the real game (P4), not in our own tests.
3. **Attachments of equal size:** WebGPU requires color and depth attachments of the same size; NVN draws into the common
   area (LGP: 1280×720 depth with 1920×1080 color). The backend then uses its own depth buffer in the color size.
4. **Stencil state for formats without stencil:** `stencilFront/Back` with `passOp = Replace` on `Depth32Float`
   is a validation error. For formats without stencil, leave the default values.
3. **Unused vertex buffer slots:** `WGPUVertexStepMode_VertexBufferNotUsed` no longer exists in the current header;
   "unused" is `stepMode = Undefined` without attributes.
4. **sRGB:** the console shows the bytes in memory. Presenting is therefore done via a *linear* view
   (`RGBA8Unorm` on an `RGBA8UnormSrgb` texture, needs `viewFormats` at creation) into a non-sRGB surface
   format (`BGRA8Unorm`/`RGBA8Unorm`). Clears and draws write into the sRGB view and thus encode like the GPU.
5. **Window coordinates:** WebGPU framebuffer row 0 is the first row in memory. NVN's default origin is
   bottom left: scissor and viewport y are mirrored with the target height (`h - (y + height)`). NDC +1 stays at the top;
   the shader does not need to mirror anything. The top-left origin (`nvnDeviceSetWindowOriginMode`) would additionally need
   a mirroring of NDC y in the vertex shader: open, as soon as a game uses it.
6. **Depth:** NVN can do `NearIsMinusW` (OpenGL, z in [-w, w]); WebGPU only accepts [0, w]. Open: conversion
   `z' = (z + w) / 2` in the translated vertex shader, depending on the device mode.
7. **Constant buffers per stage:** Maxwell has c[0..17] per shader stage. The translator puts them via
   `wgsl::Options::cbuf_group` into separate bind groups (vertex 0, fragment 1, textures 2/3); WebGPU guarantees exactly 4.
   The generated shaders declare 64 KiB per buffer, so the binding must be 64 KiB in size
   (`maxUniformBufferBindingSize` is exactly 65536 by default).
8. **Readback:** `bytesPerRow` when copying into a buffer must be a multiple of 256; `writeTexture`
   does not have this restriction.
9. **`depthClearValue` in the browser:** the default value in `webgpu.h` is NaN ("undefined"). wgpu-native ignores
   it with `LoadOp_Load`; Chromium rejects the render pass already during IDL conversion ("The provided float
   value is non-finite") – even if nothing is cleared at all. Always set a finite value. In general: what runs on
   wgpu-native is not automatically valid in the browser, which is why every draw test also runs as a browser test.
10. **Fixed function that WebGPU treats more strictly:** blend operation `Min`/`Max` requires factors `One`;
    alpha-to-coverage only with multisampling; depth compare and stencil states only if the target has such aspects;
    `u8` indices do not exist (the backend widens them to `u16`); buffer→texture copies need 256-byte rows
    (the backend recopies via an intermediate buffer). Logic op, polygon mode line/point, culling both sides,
    LOD bias and depth bounds are missing entirely. The alpha test (NVN `ColorStateSetAlphaTest` + `SetAlphaRef`) is generated by the
    translator in the fragment shader: `finish_fragment` discards if the alpha of target 0 fails the comparison with
    `lgp_alpha_ref`; the pipeline sets function and reference as overrides (`lgp_alpha_func`,
    `lgp_alpha_ref`); both belong to the pipeline key. Without alpha output to target 0 the test is dropped (warning).
11. **Writable storage bindings must not overlap within a dispatch.** Dawn (Chromium) rejects a
    dispatch in which the same buffer is attached to two `read_write` bindings, including a shared 16-byte dummy for
    unused slots; wgpu-native (lavapipe) let this through. The backend uses a separate dummy per slot and binds
    a guest range that is attached to two storage indices only once. Only found through the browser test.
12. **Compute results back into guest memory:** storage buffers are filled from guest memory per command list,
    copied into `MapRead` buffers after the submit and written back after `mapAsync`
    (`GuestMemoryView::write`). By default `execute()` waits for this. With `LGP_WEBGPU_ASYNC_WRITEBACK=1`
    the write-back runs asynchronously: `execute()` returns after the submit without waiting for the GPU. Later lists (`execute`) and `read_texture` first wait until all pending write-backs are in
    guest memory, because they read guest memory. `present` does not wait. `nvnSyncWait` only waits for write-backs
    of lists before its fence (`Backend::fence_mark`/`finish_until`), `nvnQueueFinish` for all; natively via
    `wgpuDevicePoll`, in the browser as a continuation in the map callback (the main thread does not block). A later list
    therefore always sees the results. If the guest itself writes into the range between submit and write-back, the
    write-back keeps its bytes (comparison with the uploaded bytes, diagnosis "writeback: guest wrote into the range
    meanwhile"). Why not the default: for LGP in the real game mode (cores, host clock) hardly anything waited for it
    natively (0.2 ms/frame); the 5–7 ms/frame of waiting only existed in the baton measurement mode. In headless Chromium on
    the Mac the game hung twice with it (cause open). The −16 % first measured in Safari were
    probably throttling of the fanless M1 Air: in the ABAB comparison (2026-10-13) both versions were within
    about 1 frame/s of each other per pair; the whole series dropped over time.
    Anyone who switches or detaches the guest memory view (`set_guest_memory`, `nullptr` at process end) first waits
    for pending write-backs; otherwise they write into freed memory after the end (crash on exit).
    `--frame-stats` shows "lists written back N, waits for them M in X ms". Draws in the same
    list do not see the results (their vertex/uniform data is uploaded during recording); dispatches
    among each other do (same range = same GPU buffer). For games that draw compute results in the same
    command buffer, GPU-resident buffers instead of upload arenas will be needed later.
    If a vertex stream lies entirely within the output of an earlier dispatch of the same list, its storage buffer
    is bound directly (`compute_vertex_streams`). If it only partially overlaps it (LGP, OnePlus drop: vertex buffer
    `+0x4bc0`, dispatch output `+0x180` inside it), the draw used to read the old guest bytes. With
    `LGP_WEBGPU_PATCH_DISPATCH_OUTPUT=1` (default off until the image check) the stream gets its own buffer:
    guest bytes via queue write, over them the overlapping parts from the storage buffer via `copyBufferToBuffer` (this ends
    the open draw pass). Overlaps not aligned to 4 bytes stay old (counter "vertex patch: … not
    4-byte aligned"), as do merged streams (VertexMerge). The same switch applies to dispatches among each other:
    if a later dispatch binds a different range that overlaps the output of an earlier one (LGP: storage
    `+0x100` over output `+0x80` at the same address), its buffer used to get the guest bytes from before the list.
    With the switch, the GPU copies the overlapping parts in at creation; if the earlier dispatch is in the main stream,
    the later one stays in order. Counter "dispatch output patch: …" in `--frame-stats`. Test
    `nvn.webgpu_draw_patch_dispatch_output`.
    **Compute first (default, `LGP_WEBGPU_COMPUTE_FIRST=0` turns it off):** without it, a synchronous `execute()` waits for
    the GPU time of the *whole* list, because the write-back copies are at the end of the same command buffer. Instead,
    dispatches that do not depend on any earlier GPU work of the list go into their own command buffer, which is submitted before
    the rest; the write-back copies come along, and `mapAsync` is registered between the two submits.
    The guest then only waits for the dispatches and the mapping; the draws continue on the GPU afterwards. A dispatch
    stays in place if it samples a texture that the list writes up to that point (render targets, including those carried over
    from the previous list, clear, copy into the texture), or uses a storage range that a command in the
    main stream already uses (graphics global memory, vertex data from dispatch output, copy, dispatch in place).
    The write-back stays at the end of the list as soon as a dispatch stayed in place or a copy wrote into an
    output range. The semantics stay synchronous: when `execute()` returns, everything is in guest memory;
    later lists see the same data as in list order. Counters in the line `gpu lists` (`--time-stats`).
    Compared to the default, only the GPU order within the list changes; whether frames stay the same is checked by
    `--frame-grid` in an A/B comparison. Effect for LGP (Safari, Mac): 24.6 → 30 fps (frame pacing cap), waiting for the
    write-back 16 → 2.5 ms per frame (`docs/analysis/20-route1-30fps-zeitaufteilung.md`, measurement H).
    **Natively, the blocking poll waits for the most recent submission.** `WebGpuRenderer::run` polls with
    `wgpuDevicePoll(device, true, nullptr)` until the map callback finishes the task. In wgpu-core this means
    `PollType::Wait` without an index: it waits for the most recently submitted work. With "compute first" this is the
    main submit with the list's draws. Only after that do the callbacks run. With `LGP_WEBGPU_POLL_SUBMIT_INDEX=1`
    (default off until a measurement shows the gain) the backend submits the submission with the
    readback copies via `wgpuQueueSubmitForIndex`. `run()` (`Call::wait_for_submission`) and `after_writebacks`
    then wait only for this submission. In the browser there is no blocking poll; nothing changes there.
    Mesa lavapipe blocks `wgpuQueueSubmit` as long as the queue is working, and therefore does not show the difference.
    The test `nvn.webgpu` only checks the order if the driver does not block like that; on the RTX 5070 Ti (Vulkan)
    the first version also reported "order not checked" (load too short); the load now grows up to 4M workgroups.
    More on this: `docs/analysis/39-rueckschrieb-warten-strukturell.md`.
    **Safari on the iPad: create the compute encoder first.** When the compute encoder was only created at the first dispatch,
    i.e. after the list's main encoder, Safari on the iPad lost the connection to the GPU process during the first dispatches.
    First came empty `getMappedRange` results despite a successful `mapAsync`, then
    `InvalidStateError` from `createBindGroup` or `GPUCommandEncoder.finish` ("Unable to …"). Mac Safari, Chromium and
    native ran. With the compute encoder created before the main encoder, the iPad ran stably (3000 frames, 29–30 fps);
    likewise when no `mapAsync` is registered before the main submit, but then without a gain.
    Only the following is backed by the WebKit source code: the "Unable to …" exceptions arise when the proxy in the
    web process delivers no object, for instance after a failed IPC call, and are not validation errors.
    Error scopes therefore do not see them. Why the order of encoder creation matters has not been clarified.
    Suspicion: WebKit already enqueues the Metal command buffer at creation, so that a buffer created later but
    submitted earlier waits for the one created earlier. Diagnosis via `LGP_WEBGPU_DEBUG`: `errorscope`
    (error scopes per list, with list number in the log) and `cfreadbackmain` (write-back in the main encoder, no
    `mapAsync` before the main submit).
13. **Do not assume workgroup memory is zero:** lavapipe+wgpu delivered values of the first workgroup in a second
    one, even though WebGPU promises zero initialization. Generated shaders reset their own counters themselves.
14. **Node package `webgpu` (Dawn):** the instance from `webgpu.create([])` must stay referenced. If the GC collects it,
    `ProcessEvents` crashes randomly (segfault or `std::system_error`), even though adapter and device are still alive.
15. **No texture swizzle in wgpu-native 29:** `WGPUFeatureName_TextureComponentSwizzle` is in `webgpu.h`, but the
    adapter (RTX, Vulkan) does not offer the feature. NVN view swizzles therefore run in the shader
    (`wgsl::Options::texture_swizzle`, uniform after the textures of each group), which also works in the browser without
    an extra feature. Details: [`tegra-block-linear-texturen.md`](tegra-block-linear-texturen.md).
16. **Narrowing down black draws:** `LGP_WEBGPU_DEBUG` (comma-separated) turns off guest state (`nocull`, `nodepth`,
    `noblend`, `nomask`, `noscissor`, `noviewport`), replaces the fragment shader with magenta (`solid`, shows pure
    coverage) or logs the first draws with state, vertex and constant buffers (`trace`). `noborder`
    turns off the border color emulation for color lookups (edge texel instead of border color), to attribute image differences
    to it. `noshadow` lets every depth comparison pass (1.0): if an artifact disappears with it, it comes from
    a shadow lookup. `halffill` (performance diagnosis only, wrong image) halves viewport and scissor of all draws
    into targets from 1280×720 up; they land in the top-left quarter, clears stay full; counter `halffill: draws into the
    top-left quarter` among the diagnostics. Together with `gputime` this shows what half resolution saves in fragment work
    on the large targets. Smaller targets stay unchanged, so a real scaling of all targets would probably save
    more.
    `LGP_WEBGPU_DUMP_DIR` writes the WGSL of each program. If even `solid` stays black, it is due to geometry or
    constants, not textures or blending (for LGP: wrong constant buffer banks, `nvn-hle.md`).
17. **In the browser there is no back-pressure by itself.** Natively, `wgpuSurfacePresent` (FIFO) and
    `wgpuDevicePoll` block when the GPU cannot keep up. In the browser, submit, `writeBuffer`/`writeTexture` and the
    canvas present return immediately; the browser copies the data of the queue writes and holds it until the GPU has
    processed it. If the GPU is slower than the guest, memory grows without limit and the image lags further and further
    behind. With `LGP_WEBGPU_FRAMES_IN_FLIGHT=N` the backend counts frames in flight
    (`wgpuQueueOnSubmittedWorkDone` after each present) and only lets `present()` return when at most N−1
    earlier frames are still running. The default is 0 = no limit (Safari, see point 12). The boot program sets its own default
    per device class in the window and in the browser when the environment names none: CMake
    `LGP_FRAMES_IN_FLIGHT_<DESKTOP|IOS|APPLE_TV|ANDROID|ANDROID_TV|BROWSER>` (Apple TV 3, otherwise 0, until measurements support a
    value; SDL detects Android TV and Shield at runtime), output `frames in flight: N (default on apple-tv)`.
    Reason: on the Apple TV, submitted command buffers piled up as driver memory without a limit (scene frames
    8000–9000, footprint 1705 MiB versus 1209 MiB with N = 1, `IOAccelerator` +376 MiB, Metal objects and textures the same).
    Per N, two runs, without HDMI (footprint increase frame 8370→8400, smallest `available`, wait time at the present per frame):
    N = 1 +46 MiB, 881 MiB, 27–40 ms; N = 2 +72 MiB, 841 MiB, 10–15 ms; N = 3 +216 MiB, 674–690 MiB, 5–8 ms;
    no limit +477 MiB, 393 MiB, none. With N = 16 (only counts) at most 4 frames ran at the same time, and that
    only in frames 8371–8460 (21 windows with 4, 211 with 2), memory as without a limit (`IOAccelerator` +440 MiB versus
    +26/+52/+190 MiB with N = 1/2/3: grows faster than linearly). `metal allocated` rose by only ≈ 13 MiB everywhere. N = 3 costs the least
    pacing and leaves ≈ 300 MiB more reserve than without a limit; between 2 and 3 only an fps comparison with HDMI will decide. To measure the natural depth, N = 16 practically does not limit, but does count
    (`max` in `gpu memory`). For LGP the Chromium GPU process stayed
    flat for over 10 min even without a limit (PC, 3.33–3.37 GB); in the browser as a continuation in the callback, the main thread does not block. Buffers that only live for
    one list or one readback are released with `destroy()`, not just dropped: a dropped
    buffer holds its memory in the browser until garbage collection. Counters: `gpu memory` in `--time-stats`
    (`zeitaufteilung-gast-threads.md`). What cannot be lowered, on the other hand, is the command buffer memory of the Metal driver:
    on the Mac it holds a fixed ≈ 170 MiB from the title screen on (`CommandBufferStorageAlloc` in `footprint`, flat from frame 1000, so
    no leak). It depends neither on the frame count nor on the size of the queue (`newCommandQueueWithMaxCommandBufferCount`
    64, 2048 and 4096 the same, own probe) nor on the number of simultaneous submits: with a limit of 1 or 4
    lists in flight it stayed at 168.9–169.1 MiB, and even without a limit never more than three lists ran at the same time
    (Mac, native, up to frame 1000). This is a fixed pool of the driver.
18. **`getContext('webgpu')` sporadically returns `null`.** Safari on iPadOS did this on the first start after
    loading in about every second attempt (cause unknown); reloading helped. emdawnwebgpu then returns `nullptr` from
    `wgpuInstanceCreateSurface`. The renderer therefore retries from the event loop up to
    25 times at 200 ms intervals before the start fails with "could not create a surface for the window (25 attempts)".
    Log: `webgpu: surface: getContext('webgpu') failed, trying again`, then `webgpu: surface created on
    attempt N`. Test `browser.webgpu_demo_surface_retry` (`browser_test.mjs --webgpu-context-fails 2`).
19. **A write-back at the end of the list serializes CPU and GPU.** According to the WebGPU specification, `mapAsync` only waits
    until the mapped buffer is free, i.e. until the commands that use it are done. But if the copy into the
    readback buffer is at the end of a command buffer behind the frame's draws, the buffer depends on the whole
    list: the guest thread waits for the full GPU time every frame, and the next frame only starts after that. On top of that,
    Dawn and WebKit in practice track completion per submit, not per command; so a buffer becomes
    free at the earliest when its whole submit is done. In `--time-stats` this looks like an expensive handoff: the
    guest thread stands in `gpu` for a long time, the GPU thread is barely busy, because waiting for the map callback does not count as
    `busy`. Remedy: put dispatches and their copies into a separate command buffer submitted earlier,
    register `mapAsync` in between, submit the draws afterwards (Analysis 20, "Nach #126" (after #126) and lever 1').

## Texture bindings: sample type and sampler kind match the shader

WebGPU fixes in the bind group layout what a binding expects, and checks this against the shader when building the pipeline
and against the texture when building the bind group:

- `texture_depth_2d(_array)` + `sampler_comparison` (shadow lookups, Maxwell `TEXS.DC`/`TEX.DC`/`TLD4.DC`) needs
  sample type `depth` and a sampler with `compare`. The translator reports this per binding
  (`wgsl::Bindings::texture_depth` → `ProgramDesc::*_texture_depth`); the comparison function comes from the NVN sampler
  (`nvnSamplerBuilderSetCompare`). The depth aspect is bound (`aspect = depth-only`, needed for
  depth+stencil formats). An RGBA8 or R8 texture from the memory pool (2D, not written by the GPU) is replaced
  by the backend with a Depth16Unorm copy of the red channel of level 0 (`compare_view`; Depth32Float may not be a
  copy destination). The assumption that the hardware compares against the red channel is UNVERIFIED. In the
  Pikachu catch scene, LGP binds a 16×16 RGBA8 texture to a shadow lookup (previously 1936 unbound textures in frame 9000).
  Other color textures in such a binding are replaced by the backend with a 1x1 depth texture (value 0); it counts the reason
  and reports the texture once (format, size, targets of the draw).
- WebGPU knows no border mode (`CLAMP_TO_BORDER`, NVN wrap mode 5). Shadow maps usually use it with
  border depth 1, so that everything outside the map stays lit. With `CLAMP_TO_EDGE` the edge texel decides instead,
  and a shadow at the map edge stretches across the image with a hard edge. That is why the backend emulates
  the border for 2D depth comparisons in the shader (`wgsl::Options::sampler_border`): bits 12 and 13 in the binding's swizzle word
  mark the border axes. If the coordinate leaves [0, 1] on such an axis, the lookup returns the result
  precomputed in bit 14, i.e. the comparison with the border depth (red of the border color, clamped to [0, 1]). The
  result is computed for a reference within (0, 1); border depths strictly between 0 and 1 are therefore approximated.
  Further approximations: at the map edge the filter only mixes edge texels instead of border and texels. 2D color lookups
  (`TEX`/`TEXS`, no gathers) return the border color outside with r = g = b = bit 14 and a = bit 15 (after the
  view swizzle; UNVERIFIED whether the hardware applies the swizzle to the border color). This is exact for black, white and
  transparent; other border colors and border on the W axis (3D) are rounded. Every border color used is reported once
  (info for exact, warning for approximated mapping).
- Reference value in the depth comparison: the hardware clamps it to [0, 1] for fixed-point depth (D16, D24, also the RGBA8 substitute),
  not for D32F. WebGPU may back `depth24plus` with float (wgpu does this where D24 is missing, e.g. on
  AMD/Vulkan); then a point behind the far plane of the shadow map (reference > 1) would always be
  shadowed with LESS_EQUAL, with a hard edge along this plane. The backend therefore sets bit 16 of the
  swizzle word for fixed-point formats (`kCompareClampRef`; comparison lookups do not use the LOD bias), and the shader clamps the reference.
  lavapipe backs D24 as fixed point, so the test there only checks that nothing breaks. The backend reports every
  format and every size on which depth comparisons run once (info). The HLE reports
  unmapped wrap modes once per value (LGP: 3 = `MirrorClampToEdge`, approximated as `CLAMP_TO_EDGE`). On the PC (Route 1,
  frame 7500) the emulation made a shadow at the far top edge of the image bright (2382 pixels).
- A comparison sampler must not be in a filtering sampler binding and vice versa: the backend removes `compare`
  or adds `LESS_EQUAL` when the NVN sampler and the shader do not match (UNVERIFIED what the hardware then
  does).
- 32-bit float textures (R32F, RG32F, RGBA32F) can only be bound with filtering samplers with the feature `float32-filterable`.
  The device requests it if the adapter has it (lavapipe, common desktop GPUs, Chrome mostly).
- Depth textures without comparison (`texture_2d<f32>` on a depth texture, including Depth24Stencil8) and 32-bit float
  without `float32-filterable` only work as sample type `unfilterable-float` with a `non-filtering` sampler. This depends
  on the bound texture, not on the shader: the backend computes per draw a mask of the affected bindings
  (`State::unfilterable_mask`, bit `32 * stage + k`), includes it in the pipeline key and builds a separate pipeline layout per program
  and mask (`Program::layout_variants`). For this, pass 1 of `execute()` tracks the
  texture bindings just like `record_list`. The depth aspect is bound; the depth value is in `.r` (WebGPU does not define the
  other channels); NVN filtering becomes `nearest` (UNVERIFIED how visible this is).
  Compute programs do not have variants yet: there it stays at substitute texture + counting.
- Pitfall: wgpu rejects a `depth-only` view of a Depth24PlusStencil8 texture with `format = Depth24PlusStencil8`
  ("view format is not present in the texture's viewFormat array"). `format = undefined` takes the aspect's format
  (Depth24Plus) and works.
- NVN allows sampling the currently bound depth texture at the same time (LGP does this with depth writes
  turned off). wgpu aborts with a panic ("conflicting usages RESOURCE vs DEPTH_STENCIL_WRITE"). The
  backend then creates the pass with `depthReadOnly`/`stencilReadOnly` (load/store ops stay `undefined`) if the
  draw writes neither depth nor stencil (as WebGPU counts it: stencil writes only with write mask ≠ 0 and an
  op ≠ `keep`). A draw that writes ends such a pass. If the draw writes and samples the target, the
  binding gets the substitute texture (reason "depth target sampled while the draw writes it"; on NVN the result would be
  undefined anyway). A substitute attachment of a different size (`proxy_depth_view`) is a different texture and
  does not collide.
- The translator does not generate integer textures (`uint`/`sint`); the backend also creates no
  textures for integer formats.

## Cost per draw

Uploads, bind groups and redundant calls per draw, and how the backend avoids them:
`webgpu-backend-draw-kosten.md` (benchmark `bench/webgpu/`).

## Render passes on tiled GPUs (phones, Apple TV)

Mobile GPUs (Adreno, Mali, Apple) load and store their attachments on every render pass. The number of passes costs
GPU time there, even if the passes contain few draws. On Vulkan there is also driver work per pass: wgpu-hal 29
recreates the framebuffers after every reset of a command encoder. Two consequences for the backend:

- Everything that unnecessarily ends an open draw pass is a candidate for savings. This includes hoisted
  dispatches in another encoder, separate clear passes and setting the same targets again. The line
  `gpu passes` of `--time-stats` names the causes (`webgpu-backend-draw-kosten.md`).
- Less CPU waiting alone brings no frame rate as long as the GPU queue is the bottleneck. That was the case for LGP: the
  wait for the write-back dropped by 99 %, the fps stayed the same.

LGP measurements (OnePlus 7 Pro, Apple TV) and what of it is suspicion: `docs/analysis/34-gpu-hebel-mobil-messstand.md`.

## Testing without a GPU

- **Desktop/CI:** Mesa lavapipe (`mesa-vulkan-drivers`) is enough for wgpu-native, also with an X11 window under
  `xvfb-run` (tested: window demo 120 frames, image checked via `xwd`).
- **Browser:** Playwright Chromium with `--enable-unsafe-webgpu --enable-features=Vulkan --use-vulkan=swiftshader
  --use-webgpu-adapter=swiftshader`. Important: start **headed** under `xvfb-run`. Headless loses the device immediately with
  these flags ("A valid external Instance reference no longer exists", even with pure JS); with
  `--use-angle=swiftshader` it stays, but screenshots show the canvas empty.
- The canvas is cleared at program end (runtime shutdown releases the context); therefore check pixels during the
  run (`tests/web/browser_test.mjs` takes screenshots every 200 ms).
- Node has no `navigator.gpu`: WebGPU tests in the WASM ctest report as skipped there (exit 77); the
  adapter request itself and the proxying are still exercised.
- Lifted guest code in the browser without the recompiler loader (it needs Wasm exceptions, which collide with the
  runtime's `setjmp`): the native build writes the fully linked image
  (`lgp_test_nvn_triangle --dump-image`), the WASM build embeds it together with the natively lifted C++
  (`LGP_NVN_GUEST_IMPORT_DIR`, test `browser.nvn_triangle`). Programs that check themselves and draw
  offscreen run with `browser_test.mjs --no-canvas-check`.
- Threads in the browser need cross-origin isolation (`COOP: same-origin`, `COEP: require-corp`):
  `python3 tools/web/serve.py <dir>` serves locally with these headers.
