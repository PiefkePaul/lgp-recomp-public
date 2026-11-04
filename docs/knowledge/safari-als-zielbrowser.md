# Safari (macOS) as a target browser for the WebAssembly build

Game-independent. What an Emscripten build with threads, WasmFS/OPFS, WebGPU and AudioWorklet finds in Safari and
how Safari differs from Chromium. Measured on 2026-10-11 with Safari 27.2 on a Mac M1 (macOS 26.7.1)
via `tests/web/feature_probe.html` and the browser tests (`LGP_BROWSER=safari ctest --preset wasm -L browser`).
Where it says "derived", it is not measured.

## Summary

1. **Everything without which the build does not run is there:** cross-origin isolation with COOP/COEP from `localhost`,
   `SharedArrayBuffer`, `Atomics.wait` in workers, Wasm exceptions (`-fwasm-exceptions`), OPFS with synchronous
   access handles in workers (WasmFS), WebGPU via Metal (adapter `apple`), AudioWorklet with `SharedArrayBuffer`.
2. **Ran:** homebrew fixtures via the boot program (threads, OPFS, RomFS, saves), OPFS persistence across
   two page loads, keyboard input, the lifted NVN triangle via WebGPU and the WebGPU demo (600 frames,
   0 WebGPU errors, `requestAnimationFrame` ≈ 62/s with an unlocked screen) – identical to Chromium. All 11
   browser tests pass in Safari except `browser.web_audio` (skipped, needs a real gesture) and
   `browser.save_backup` (uses Playwright only).
3. **Locked screen:** Safari then pauses `requestAnimationFrame` and throttles timers very heavily. Everything that
   needs frames (render loop, WebGPU demo) stands still; tests only with an unlocked screen.

## Differences from Chromium

| Topic | Safari 27.2 | Consequence |
|---|---|---|
| Autoplay | `new AudioContext()` without a user gesture is `suspended`; a launch switch as in Chromium (`--autoplay-policy`) is missing | The web shell resumes the context on the first click/key press (`web_audio.cpp`). Automated tests without a real gesture hear nothing; `browser.web_audio` is therefore skipped with `LGP_BROWSER=safari` (`--needs-user-gesture`). |
| OPFS rename | `FileSystemDirectoryHandle.move()` exists, directories can be moved; `FileSystemFileHandle.move('neuer-name')` throws `TypeError` | Rename files via copy + delete, not via `move()`. In this build WasmFS reports `Io` when renaming a directory (output `opfs: directory rename -> Io` in the OPFS test; the test passes anyway). |
| Folder picker | `<input webkitdirectory>` yes, `showDirectoryPicker()` no | The page already uses `webkitdirectory` (P6.1). |
| Storage | quota ≈ 77 GiB on this disk, `persisted()` `false` without asking | Game data (≈ 4 GiB) fit; but the browser may evict them if no persistent storage is granted. |
| memory64 | not available | Confirms ADR-0001 (32-bit guest memory). |
| Wasm tail calls | available (from Safari 18.2) | The Wasm build needs them (`-mtail-call`, #115). Without them the engine stack grew with every instruction of the game's script VM, and Safari's worker stack overflowed in battle ("Maximum call stack size exceeded"). Older browsers (Safari < 18.2, iOS < 18.2, Chrome < 112, Firefox < 121) cannot load the module at all; the page and `browser.feature_probe` report that beforehand. |
| WebGPU in workers | `navigator.gpu` is available in a dedicated worker (measured in a real worker, probe after the correction) | The renderer still forwards WebGPU calls to the main thread (`PROXY_TO_PTHREAD`, necessary because of the canvas); rendering in the worker via `OffscreenCanvas` would be an option for P6.3. |
| WebGPU limits | `maxBufferSize`/`maxStorageBufferBindingSize` 1,431,655,764, `maxTextureDimension2D` 16,384, `maxBindGroups` 11, `maxColorAttachments` 8, `maxComputeWorkgroupStorageSize` 32,768 | More generous than the WebGPU minimums. |
| WebGPU features | among others `texture-compression-bc`, `-astc`, `-etc2`, `float32-filterable`, `depth32float-stencil8`, `shader-f16`, `timestamp-query` | BCn (94.5 % of the game textures) and ASTC are supported natively; no software unpacking needed. |
| Preferred canvas format | `bgra8unorm` | as Chromium on macOS. |
| Pthread output | lines from a pthread can arrive at the main thread after `onExit` | Test scripts wait briefly after the end (`browser_test.mjs`). |
| Gamepad | `navigator.getGamepads()` available | Real controllers not tested (none connected). |

## Findings (2026-10-11)

- **Output from pthreads was lost at program end (fixed).** In Emscripten a pthread prints via a
  `postMessage` to the main thread (`cmd: callHandler`, there `Module.print`). When `main()` returns in a
  `PROXY_TO_PTHREAD` build, the exit is handed to the main thread via Emscripten's call queue;
  that queue is signaled via `Atomics.waitAsync` and can overtake output messages still waiting. As soon as the
  main thread has started exiting, it no longer handles the remaining messages (they arrive, but
  `Module.print` is no longer called; observed with a counter on the worker messages). In Safari 27.2
  the end of the output (boot summary, `exit:` line) was therefore often missing, in Chromium not.
  Neither `fflush`, line buffering, stdout→stderr nor an `atexit` handler help (the latter already runs on
  the main thread during exit). **Solution:** before returning from `main()`, the pthread sends a marker line via
  the same output path and waits until the main thread has seen it (`Module.print` recognizes it, sets a flag in Wasm memory via
  `Atomics.store`/`notify` and discards the line). Implemented as
  `lgp::boot::finish_host()` in `platform/web/src/boot_host.cpp`; output from other threads still running at the end
  is not covered by this.
- **Real game:** runs in Safari in the render loop. Startup and frame rate depended almost entirely on reading from OPFS
  (WasmFS read paths, see [`opfs-und-dateiauswahl-im-browser.md`](opfs-und-dateiauswahl-im-browser.md)): before,
  frame 64 after 159 s and ≈ 41 fps; afterwards frame 64 after 7.8 s and ≈ 95 fps (numbers in
  `13-browser-abnahme.md`). The `svcBreak` seen first was not a Safari problem
  (too little `TotalMemorySize`, ADR-0001), nor was the following memory error (missing system fonts). The SVC trace
  (`--trace-svcs`) only appears together with `--log trace`; without it there are no trace lines, regardless of the
  browser.

- **WebGPU buffers must be destroyed, releasing is not enough (2026-10-12).** A `GPUBuffer` that the program
  only releases (`wgpuBufferRelease`, in JS: dropping the reference) lives until the garbage collector
  collects it. Safari does not count the GPU memory as pressure on the GC. If new upload buffers are created per command list,
  the web content process grows by several GB per minute (measured: ≈ 400 MB/s, 3.4 GB → 8.6 GB
  in just over 10 s). Once this led to 38 GB of memory and swapping and brought the Mac to a standstill; from the outside
  it looked like a hang at program end. Chromium did not show this. Solution: reuse buffers or call
  `wgpuBufferDestroy` before releasing (in the WebGPU backend with #90). After the submit this is allowed; the GPU still finishes the
  submitted commands.
- **A busy main thread looks like a hang.** If every proxied WebGPU task takes longer than the
  game tick allows (measured before #90: ≈ 50 ms per command list at ≈ 25 lists/s after the title screen),
  Safari's event loop no longer gets its turn. Timers, `fetch` reports and the pthreads' output
  stall, the process shows 99 % CPU, and the backed-up GPU work makes memory grow. Chromium managed
  the same scene. The test reports this as "no report from the page for 5 s"; with `LGP_SAFARI_SYNC_LOG=<Regex>`
  matching lines are sent synchronously via XHR and arrive anyway.

- **Hang at program end: deadlock in `exit()` (fixed, 2026-10-12).** After the scene change it looked as if
  Safari's main thread was hanging: 99 % CPU, flat memory, no more output. In fact the game had reached its
  frame limit and was exiting. In a `PROXY_TO_PTHREAD` build, `exit()` runs the static destructors on
  the main thread. A global `shared_ptr` of the movie HLE on `romfs.bin` closed its OPFS file there; WasmFS
  hands that via `emscripten_proxy_sync` to the OPFS thread and busy-waits. In Safari the answer never came, in
  Chromium it did. Found with `LGP_SAFARI_SPIN_PROBE=1` (stack of the busy-waiting main thread with
  wasm function names). `sample <pid>` on the web content process works too, but only shows the
  JS level (`performance.now` in a loop), no wasm names. Rule: files and other objects that wait for another thread
  when destroyed do not belong in static variables. They are released at the end of the run on their
  own thread (`MovieHle::release()`).

## Automating Safari

- `safaridriver` needs "Allow Remote Automation" in the developer settings (or
  `safaridriver --enable` with the admin password). Without this setting it works like this (`tests/web/browser_test.mjs`,
  `--browser safari`): open the page with `open -a Safari <url>`; the local test server injects into every HTML page a
  script that reports console lines, page errors and the brightness of the canvas back via `fetch` and fetches
  commands (folder selection via the page's own copy logic, key presses as synthetic `keydown`).
- Synthetic key events are not a user gesture: they do not resume an AudioContext.
- Memory watchdog: in Safari runs the test queries the memory of every web content process via `top` every 2 s.
  That is the footprint including compressed and swapped pages; the RSS from `ps` stays low precisely while
  swapping. If a process exceeds `LGP_SAFARI_MAX_MB` (default 10000), it is terminated and the test
  fails. `LGP_SAFARI_MEMLOG=1` prints the largest value every 10 s. After a failed run
  the test terminates game pages over 2 GB. As long as such a hanging page is running, it does not open another tab.
- **Disk space:** Safari 27.2 leaves the temporary file of every OPFS `createWritable()` in the temp folder of its
  network process (`$(getconf DARWIN_USER_TEMP_DIR)com.apple.WebKit.Networking+com.apple.Safari/FileSystemWritableStream*`),
  even after `close()`. Per prefill of the game data that is ≈ 4.4 GB; after a few runs the Mac's disk was full
  (33 GB). The test deletes these files while Safari is not running (before the start and after quitting).
  The game page itself copies via access handles in the worker (`opfs_copy_worker.js`) and is not affected by this.
- Cleanup: at the end of each test the test tab closes itself (`window.close()`). Safari only allows this with
  a single history entry, so the tab only navigates with `location.replace`. If the test started Safari,
  it quits it again afterwards (`LGP_SAFARI_QUIT=0` leaves it open). Test tabs from an earlier run,
  such as those restored by Safari after a restart, close themselves at the next poll; their
  output is discarded (every message carries the run ID).
- All tests share one tab: in Safari runs the test server runs on a fixed port (`LGP_SAFARI_PORT`, default 47816, so run them one after the other, `-j1`). The previous test's tab polls every 500 ms for the next page and loads it; only if nobody polls within 3 s does `open -a Safari` open a new tab. Before each test the tab loads a helper page that clears OPFS and `localStorage`, so that each test starts as in a fresh browser context (same port means same origin). Only `http://localhost` pages of the test server are opened.

## Remote-controlling Safari on the iPad

- The Mac starts pages in Safari on the iPad connected via USB using `safaridriver` (WebDriver). For this, the
  iPad needs developer mode and, in the Safari settings under "Erweitert" (Advanced), the Web Inspector and
  remote automation switched on. Then a run needs nobody at the device (LGP, October 2026).
- The automation session has **its own website storage**. It does not see OPFS, `localStorage` and saves of normal
  Safari use, and vice versa. Game data (romfs.bin etc.) must be copied to OPFS once within the automation session
  itself. A pipeline cache or save from the normal session does not apply there.

## WebGPU in WebKit: web process, GPU process and "Unable to …"

Read in the WebKit source code (branch `main`, October 2026). Whether the shipped Safari version on iPadOS 27.2 works exactly
like this has not been checked.

- **Structure:** every WebGPU object in the web process (`GPUDevice`, `GPUBuffer` …, `Source/WebCore/Modules/WebGPU/`)
  is a proxy (`Remote…Proxy`, `Source/WebKit/WebProcess/GPU/graphics/WebGPU/`). It sends every call over an
  IPC stream connection to the GPU process (`Source/WebKit/GPUProcess/graphics/WebGPU/Remote*.cpp`). There the
  Metal implementation `Source/WebGPU/WebGPU/*.mm` (partly Swift) carries out the call.
- **`InvalidStateError: … Unable to …`** (such as `GPUDevice.createBindGroup: Unable to make bind group.` or
  `GPUCommandEncoder.finish: Unable to finish.`) is not a validation error. WebCore throws it when the proxy
  returns `nullptr`. That only happens when the descriptor conversion fails or the IPC `send` fails.
  That is why error scopes and `uncapturederror` see nothing of it. If several such exceptions appear in quick
  succession, the connection to the GPU process is usually already gone, and the first cause lies earlier.
- **`getMappedRange` is synchronous:** `RemoteBufferProxy::getMappedRange` fetches the data via `sendSync`. If the
  call fails, the function silently returns an empty range (the code says "FIXME: Implement error handling").
  Emscripten's `wgpuBufferGetConstMappedRange` then returns `NULL`, although `mapAsync` reported success.
- **Connection abort through `MESSAGE_CHECK`:** the GPU process checks incoming messages, for example
  `RemoteBuffer::getMappedRange` with `MESSAGE_CHECK_COMPLETION(m_isMapped)`. If a check fails, the
  message counts as invalid and the stream connection is severed. After that all further calls fail with
  "Unable to …". Whether that was the trigger of the iPad abort is not proven (suspicion).
- **`mapAsync` completes with `onSubmittedWorkDone`:** `Buffer::mapAsync` reports back when all work of the queue
  submitted up to that point is done, not already after the command buffers that use the buffer. A
  writeback therefore only avoids waiting for later draws if its `mapAsync` is registered before their submit.
- **Finding LGP (iPad, Safari iPadOS 27.2):** this sequence made Safari on the iPad lose the GPU connection:
  1. create a second encoder (after the main encoder),
  2. submit it first,
  3. register `mapAsync`,
  4. then submit the main encoder.

  Mac Safari ran with the same sequence. If the second encoder is created before the main encoder, it runs stably;
  likewise without `mapAsync` before the last submit, but then without the gain (procedure and runs:
  `webgpu-backend-desktop-und-browser.md`, pitfall 12). The same picture (InvalidStateError at the encoder, only on the
  iPad) was shown by a separate GPU pthread and asynchronous submission.
- **Diagnosis:** in the Web Inspector console the "Unable to …" exceptions appear as unhandled rejections.
  No messages from the GPU process itself arrive there. The first conspicuous line before them is usually the
  actual trace; for LGP "storage buffer … not written back".

## Sources

- Measurement: `tests/web/feature_probe.html` (one line per feature) and `ctest --preset wasm -L browser` with
  `LGP_BROWSER=safari`.
- WebKit feature status: <https://webkit.org/status/>
- WebKit source code: <https://github.com/WebKit/WebKit> (`Source/WebCore/Modules/WebGPU/`, `Source/WebKit/WebProcess/GPU/graphics/WebGPU/`, `Source/WebKit/GPUProcess/graphics/WebGPU/`, `Source/WebGPU/WebGPU/`).
