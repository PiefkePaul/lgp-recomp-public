# Firefox as a target browser for the WebAssembly build

Game-independent. What an Emscripten build with threads, WasmFS/OPFS, WebGPU and AudioWorklet finds in Firefox, how
Firefox differs from Chromium and Safari, and what was changed in our own code for it. Measured on 2026-10-11
in a Linux container (x86_64, no GPU, Mesa lavapipe 25 as the Vulkan driver, Xvfb, PulseAudio with null sink) with
Playwright's Firefox builds: **Firefox 142.0.1** (Playwright 1.56.1, the version pinned in CI for Chromium)
and **Firefox 157.0** (Playwright 1.64.0). Measured via `tests/web/feature_probe.html` and the browser tests
(`LGP_BROWSER=firefox ctest --preset wasm -L browser`). Where it says "derived" or "according to the source", it is not measured.
Not measured: Firefox on Windows and macOS, Firefox with a real GPU, the real game (needs game data, so PC/Mac).

## Summary

1. **Everything without which the build does not run is there** (142 and 157): cross-origin isolation with COOP/COEP from
   `localhost`, `SharedArrayBuffer`, `Atomics.wait` in workers, Wasm exceptions (`-fwasm-exceptions`), OPFS with
   synchronous access handles in workers (WasmFS), AudioWorklet with `SharedArrayBuffer`, and a shared
   wasm32 memory that grows in steps up to **3750 MiB** (roughly what the game needs according to ADR-0001:
   3072 MiB guest plus host side).
2. **Ran (Firefox 157, all browser tests green):** homebrew fixtures via the boot program (pthreads with
   baton scheduler, OPFS, RomFS, saves, result identical to the Unicorn run), OPFS persistence across two
   page loads, save export/import, keyboard input, WebAudio sound via the AudioWorklet, VP9 video via WebCodecs, the lifted
   NVN triangle via WebGPU and the WebGPU demo (600 frames, 0 WebGPU errors, canvas visibly rendered).
   Firefox 142: everything except WebGPU (no adapter, see below); WebGPU tests are skipped there.
3. **WebGPU on Linux only via a switch.** In the Linux release builds `navigator.gpu` is absent without
   `dom.webgpu.enabled=true` (157). Firefox 142 rejects `requestAdapter()` even with the switch
   (`NotSupportedError: WebGPU is only available on Windows, and in Nightly and Early Beta builds on other platforms.`).
   According to the gpuweb implementation status, WebGPU in Firefox is on by default on Windows since 141 and on macOS since 145 (Apple
   Silicon) or 147 (all), on Linux only in Nightly. For Paul's devices (Windows PC, Mac) this means:
   Firefox should have WebGPU without a switch (derived, still to be measured there).
4. **Three problems in our own code fixed, one test adjusted** (section "Findings"): the OPFS copy hung on
   `navigator.storage.persist()`, the WebGPU demo took the whole browser down on exit (≈ 40 % of runs), the
   feature probe aborted on the WebGPU rejection.

## Differences from Chromium and Safari

| Topic | Firefox (142/157, Linux) | Consequence |
|---|---|---|
| `navigator.storage.persist()` | Asks the user (permission bar) and resolves the promise **only after the answer**; never in automated runs | The page waits at most 2 s for it and then continues copying (`prepareStorage` in `lgp_web.js`); the later answer is reported. Chromium and Safari answer immediately (usually `false`). |
| WebGPU | On Linux only with `dom.webgpu.enabled` (157); 142 not at all | Tests set the switch (`FIREFOX_PREFS` in `browser_test.mjs`). For users on Linux: `about:config` → `dom.webgpu.enabled` or Nightly. |
| WebGPU adapter (lavapipe) | `isFallbackAdapter` true, `info.vendor`/`device`/`description` empty; features incl. `texture-compression-bc`, `float32-filterable`, `depth32float-stencil8`, `shader-f16`, `dual-source-blending`, **no ASTC** | With BCn support like Chromium; ASTC falls back to the CPU decoder (`gpu/texture/`). Not measured with a real GPU. |
| WebGPU limits (lavapipe) | `maxTextureDimension2D` 16,384, `maxStorageBufferBindingSize` 134,217,728 (128 MiB), `maxBufferSize` 2,147,483,644, `maxBindGroups` 8, `maxColorAttachments` 8, `maxComputeWorkgroupStorageSize` 32,768 | `maxStorageBufferBindingSize` is considerably smaller than in Safari (≈ 1.33 GiB); shaders with large storage buffers must query it. |
| WebGPU presentation | Warning `WebGPU: disabling SharedTexture swapchain: missing GBM_FORMAT_ARGB8888 dmabuf format` (without GPU), then a copy via the CPU | Works, rAF ≈ 61/s. |
| WebGPU in workers | `navigator.gpu` present in a dedicated worker (157) | Like Safari; the renderer still forwards to the main thread. |
| Device teardown | `device.destroy()` right after the last present crashed the **parent process** (SIGSEGV in `libxul`, all tabs gone) | On the web the renderer waits for the queue and one frame before releasing (finding 2). |
| AudioContext | Every context starts `suspended` and only starts running after ≈ 2 s (null sink); **never without an audio output device** (`resume()` hangs). Chromium simulates a device. | In containers/CI start PulseAudio with `module-null-sink`. Autoplay without a gesture is allowed in Playwright's Firefox. |
| Wasm exceptions | Warning per module: "The WebAssembly exception handling 'try' instruction is deprecated … recompile to use the 'try_table' instruction" | Only a console warning. Emscripten still generates the old form with `-fwasm-exceptions`; the new one (`-sWASM_LEGACY_EXCEPTIONS=0`, exnref) needs a switch in Node 22, so not switched over for now. |
| WebCodecs `VideoDecoder` (VP9) | Present in the worker; frames arrive as **`BGRX`** (Firefox converts YUV→RGB itself), Chromium delivers `I420` | No exact access to the YUV planes: the way back RGB→NV12 deviates by up to 3 (Chromium: 0). `WebVideo::rgb_pictures()` counts such frames; `browser.web_video` then checks with the RGB tolerance. |
| memory64 | present | ADR-0001 stays with wasm32 because of Safari. |
| Folder selection | `<input webkitdirectory>` yes, `showDirectoryPicker()` no | Like Safari; the page uses `webkitdirectory`. |
| OPFS rename | `FileSystemHandle.move()` present for files and directories | WasmFS still reports `Io` on directory rename (`opfs: directory rename -> Io`, as in Safari); a limit of the WasmFS OPFS backend, not of the browser. |
| Storage quota | 10,240 MiB (Playwright profile), `persisted()` `false` | Enough for the ≈ 4.2 GiB of game data. Without persistent storage Firefox may evict the data. |
| Headless | No WebGPU adapter in headless mode | Tests run visibly under Xvfb (like Chromium). |

## Findings (2026-10-11, finding 6 2026-10-14)

1. **OPFS copy hung (fixed).** Before copying, the page waited for `navigator.storage.persist()`. Firefox
   only resolves this promise once the user has decided in the permission bar; without an answer (automated
   runs, ignored bar) the copy never started. `prepareStorage()` now waits at most 2 s
   (`persistWaitMs`), reports `persisted: null` ("still asking") and passes the later answer on via `onPersisted`;
   test in `tests/web/lgp_web_test.mjs`.
2. **Browser crash when quitting the WebGPU demo (worked around).** In about 4 of 10 runs Firefox 157 disappeared
   exactly at program end (child processes: "Exiting due to channel error", core dump: SIGSEGV in an
   NSPR thread of the parent process in `libxul`, without symbols). Narrowed down by elimination: without the teardown of the
   renderer 10/10 green, without only `wgpuDeviceRelease` (which in emdawnwebgpu calls `device.destroy()`) 10/10 green,
   without only `wgpuSurfaceUnconfigure` still crashes. The lifted NVN triangle, which renders without a canvas, never
   crashed (8/8). So: `device.destroy()` shortly after the last present to a canvas. **Countermeasure** in
   `WebGpuRenderer::~WebGpuRenderer` (only Emscripten, only from a pthread): unregister the canvas, wait for
   `wgpuQueueOnSubmittedWorkDone` and then for a `requestAnimationFrame`, only then release. Afterwards
   15/15 runs green. A Firefox bug (content must not be able to crash the browser); not checked with a real GPU and
   on Windows/macOS. For the game this matters when closing or reloading the page.
   Addendum: in CI (xvfb, `--gpu-thread`) the demo once no longer ended ("no exit within 60000 ms" after
   the surface line), probably because that frame never came in the worker (not proven). Since then the renderer releases
   anyway after at most 2 s and reports `webgpu: no frame after the last present within 2000 ms`.
   These 2 s were too short (finding 6) and themselves led to the crash. Now the renderer waits at most 20 s for
   `OnSubmittedWorkDone` and after that at most 2 s for the frame. If work-done does not come, it does **not** release
   the device (`webgpu: GPU work not done within 20000 ms, device not released`); the page cleans that up when closing.
3. **`browser.web_video` (WebCodecs) failed (test adjusted).** The exact plane path expected an error ≤ 1,
   but Firefox delivers RGB frames (`BGRX`, BT.709, limited range) whose back-conversion deviates by up to 3. The test
   first checks exactly and, if the backend counted RGB frames, once more with tolerance 3.
4. **The last output lines of a pthread were lost at program end (fixed, 1 of 5 runs of
   `browser.web_video`).** The same race as in Safari ([`safari-als-zielbrowser.md`](safari-als-zielbrowser.md)):
   exiting overtakes output messages still pending. The boot program already had
   `lgp::boot::finish_host()` for that; for other programs there is now `lgp::platform::web::drain_output()`
   (`lgp/platform/web/output_drain.hpp`), which the video test calls before returning (afterwards 12/12).
5. **Feature probe aborted (fixed).** In Firefox 142 `requestAdapter()` is rejected instead of returning `null`;
   the probe now reports that as a line. New in the probe: growth of the shared memory up to 3750 MiB (mandatory)
   and the AudioContext state after the asynchronous start.
6. **WebGPU demo sporadically red in CI (fixed, 2026-10-14).** Two pictures: the browser disappeared right after
   `no frame after the last present within 2000 ms` ("Target page, context or browser has been closed", without
   `--gpu-thread`), or the canvas stayed black in all samples, although all 600 frames were presented without errors
   (`--gpu-thread`). Notable: the samples came only every 3–6 s instead of every 200 ms.
   **Cause: no back pressure.** In the browser, submit and present return immediately
   ([`webgpu-backend-desktop-und-browser.md`](webgpu-backend-desktop-und-browser.md), point 17); the demo ran without a
   limit on frames in flight. On the busy CI runner lavapipe renders more slowly than the demo
   presents; at the end the GPU process was seconds behind. Consequences: (a) work-done only came after the 2 s timeout,
   the device was destroyed with work still running, Firefox crashed (finding 2); (b) the canvas stayed black and screenshots
   waited for seconds (suspicion: the compositor waits for a frame that the GPU process has not rendered yet; not
   looked up in Firefox). **Reproduced** in the container with Firefox 157 and
   lavapipe, limited to one core (`taskset -c 0`) with two load processes on the same core: without a limit,
   work-done was 1.1–6.8 s behind the last present; without `--gpu-thread` 3 of 4 runs with a browser crash, with
   `--gpu-thread` 2 of 4 with a black canvas. With at most 2 frames in flight (`LGP_WEBGPU_FRAMES_IN_FLIGHT=2`)
   4 of 4 green each, work-done ≤ 0.11 s after the last present, 28–35 instead of 2–5 samples. On 4 free cores
   (even with 16 load processes) the error did not occur. **Countermeasure:** in the browser the demo sets
   `LGP_WEBGPU_FRAMES_IN_FLIGHT=2` (if not already set), and the teardown waits longer (finding 2). For the game
   the default stays 0 (Safari ran slower with a limit); the changed teardown applies there too: when quitting
   `lgp_game` in the browser, the renderer waits up to 20 s if the GPU is behind and leaves the device unreleased afterwards.
   Since then the demo tests no longer test without back pressure; the teardown without a limit is only covered by the local cross-check
   (8/8).

## Automating Firefox

- Playwright only drives its own patched Firefox builds (Juggler protocol); a normal Firefox or
  Nightly does not work with it. The Firefox version depends on the Playwright version: 1.56.1 → 142, 1.64.0 → 157.
  `browser_test.mjs` takes another Playwright package with `LGP_PLAYWRIGHT_DIR` and another
  (Playwright) Firefox binary with `LGP_FIREFOX_PATH`.
- Switches (`firefoxUserPrefs`): `dom.webgpu.enabled`, `media.autoplay.default=0`,
  `media.autoplay.block-webaudio=false`. `gfx.webgpu.ignore-blocklist` and `dom.webgpu.workers.enabled` were not
  needed. A separate GPU process (`layers.gpu-process.enabled`) changed nothing about the crash from finding 2.
- Audio in the container: `apt-get install pulseaudio`, `pulseaudio --start --exit-idle-time=-1`,
  `pactl load-module module-null-sink`.
- `browser.save_backup` now also follows `LGP_BROWSER` (`firefox`, `webkit`).
- CI: the jobs `wasm` and `fixtures-wasm` additionally run the browser tests in Firefox 157 (Playwright 1.64.0
  in its own directory, PulseAudio null sink, no skipped tests allowed). Locally the
  11 tests took ≈ 90 s.

## Procedure (local, Linux)

```sh
npm install --prefix /opt/pw-firefox playwright@1.64.0
/opt/pw-firefox/node_modules/.bin/playwright install --with-deps firefox
pulseaudio --start --exit-idle-time=-1 && pactl load-module module-null-sink
LGP_BROWSER=firefox LGP_PLAYWRIGHT_DIR=/opt/pw-firefox/node_modules/playwright ctest --preset wasm -L browser
```

## Open

- Firefox on Windows (PC) and macOS (Mac) with a real GPU: feature probe and WebGPU demo, then the real game
  (`lgp_game`, procedure as in `13-browser-abnahme.md`); there it will also show
  whether the 3072 MiB guest block is allocated in a running program (so far only the growth
  of an empty shared memory has been measured).
- Whether Firefox offers ASTC with a real GPU and how large `maxStorageBufferBindingSize` is there.

## Sources

- Measurement: `tests/web/feature_probe.html`, `ctest --preset wasm -L browser` with `LGP_BROWSER=firefox`.
- WebGPU implementation status: <https://github.com/gpuweb/gpuweb/wiki/Implementation-Status>
- Firefox 141 with WebGPU on Windows: <https://simonwillison.net/2025/Jul/16/webgpu-firefox>
- Firefox 147 (WebGPU on all macOS versions): <https://www.firefox.com/firefox/147.0/releasenotes/>
