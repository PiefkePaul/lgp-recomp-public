# Time breakdown per guest thread (`--time-stats`)

Game-independent. Applies to any runtime that maps guest threads 1:1 onto host threads, schedules them via a baton and
hands graphics calls synchronously to a GPU thread (ADR-0002, ADR-0003). Code:
`runtime/core/include/lgp/runtime/time_stats.hpp`, measuring points in `runtime/core/src/scheduler.cpp`,
`runtime/core/src/kernel.cpp` (IPC), `gpu/nvn/src/hle.cpp` and `gpu/nvn/src/hle_impl.hpp` (`TimedBackend`),
GPU thread counters in `platform/webgpu/src/webgpu_renderer.cpp`. Test: `runtime.time_stats`.

## Purpose

A frame rate alone does not say *where* the time goes. A CPU profiler (V8 profiler, perf, callgrind) shows the self time
of functions, but not how long a guest thread waits for its baton slot, for another thread or for the
GPU thread. Yet exactly that decides whether faster lift code, fewer GPU hand-offs or more parallelism
helps. `--time-stats N` breaks the wall time of every guest thread down into fixed categories and prints them every N presented
frames, averaged per frame.

## Categories

| Category | When | What it means |
|---|---|---|
| `guest` | thread running, no other category | lifted code and SVCs without their own category; faster lift code helps here |
| `ready` | waiting for its scheduler slot | baton contention: another guest thread of the same slot is currently running |
| `wait` | blocked in the kernel | waits, sleeps, also the frame pacing (`--paced`) and idle under `--clock virtual` |
| `ipc` | `svcSendSyncRequest` | HLE service handles the request on the guest thread |
| `nvn` | in an NVN function | HLE records commands, reads guest memory (without `nvn-lock`, `gpu`) |
| `nvn-lock` | waiting for the NVN HLE lock | another thread is currently in an NVN function (the lock also applies during `gpu`) |
| `gpu` | in a renderer call | blocked until the GPU thread has recorded and submitted; in the browser plus the hand-off to the main thread |
| `movie` | in a `movie::` HLE method | extractor and decoder of in-game videos, including waiting for their lock (before 2026-10-30 counted as `guest`) |

Per thread there is additionally: how often per frame it went into `ready` and `wait`, with mean duration (`readies`, `waits`).
Many short `readies` point to hand-off costs, few long ones to another thread holding the slot. The line
`wakeups` counts the hand-offs in which another thread gave the slot to a waiting one, and measures the time from the
hand-off until it continues running (host wake-up latency plus scheduler lock; `busy` is the sum, `us each` the
mean). If it is large, the hand-offs themselves cost time (core idle states, P7.3 in
`android-ndk-build.md`). If it is small, `ready` is waiting for real work by other threads.
Plus GPU calls and IPC requests per frame with mean duration. The line `gpu thread` shows the
working time of the GPU thread (synchronous part of the tasks) and the number of tasks. `busy` ends when the task
returns synchronously; the guest thread's `gpu` only ends when it is reported finished. The difference `gpu` minus `busy`
therefore consists of three parts that the numbers do not separate: waiting for `run_mutex_` (another
guest thread is currently in a renderer call, `WebGpuRenderer::run`), the hand-off itself (in the browser
`emscripten_proxy_sync` → event loop of the main thread → back; natively it does not apply, the task runs on the
calling thread) and waiting for a completion after the return, in the browser mainly the map callback of a
compute write-back. Natively (desktop, Android, iOS), on the other hand, the write-back wait is in `busy`: `after_writebacks`
polls with `wgpuDevicePoll(device, true, …)` inside the task before it continues, as does the cap for
frames in flight (`LGP_WEBGPU_FRAMES_IN_FLIGHT`). So natively, first subtract the wait times from `gpu memory` from `busy`;
the rest is encode, submit and creation. Which part dominates is shown by the lines `gpu memory` (wait time for write-backs) and `nvn-lock`
(contention for the HLE lock, which usually also explains the mutex). With `LGP_WEBGPU_ASYNC_WRITEBACK=1` compute results are written back
asynchronously; the wait then only happens in the next renderer call or in `nvnSyncWait`/`nvnQueueFinish` (line
"lists written back … waits for them").

Six further lines break down `busy` and the difference to `gpu` (since 2026-10-15). Measuring always happens, output
only with `--time-stats`; the cost is one pair of clock reads per submit and present.
- `gpu lock wait` counts calls that found `run_mutex_` taken, with wait time.
- `gpu surface acquire` is the time in `wgpuSurfaceGetCurrentTexture`.
- `gpu surface submit` is the time in the `wgpuQueueSubmit` of the blit to the window.
- `gpu surface present` is the time in `wgpuSurfacePresent` (the last three only with a window).
- `gpu list submits` is the time in `wgpuQueueSubmit` of the command lists.
- `gpu warm-up` is the time for warming up from the pipeline cache at present.

The `gpu` column of the guest threads is broken down by the lines `gpu call <kind>` (since 2026-10-21), summed over all
guest threads (without the wait for the scheduler slot afterwards, which counts as ready): `programs` (creating/freeing programs), `textures` (creating/freeing textures), `texture writes`,
`lists` (recording command lists, including the texture checks and uploads from pools), `presents` and `finish`
(`nvnQueueFinish`, waiting on fences). Measured only with `--time-stats`. Occasion: on the Shield, during the
scene change ≈ 22 ms per frame of thread 2's `gpu` time remained unattributed; lock, swapchain and warm-up were not it.

What remains of `busy` after subtracting these lines and the uploads and creations from `gpu work` is encode plus
queue writes (constant and geometry arenas), `Finish` and creating bind groups. If a driver blocks in acquire, present or submit, GPU time is hidden there. On lavapipe, e.g.,
the submit already blocks while the queue is working. A frame that seems CPU-bound can thus in
truth be GPU-bound (Analysis 39, section 5).

Threads that almost only wait in the window (less than 0.5 % active) are summarized in one line.

With the WebGPU backend, each window is followed by a line `gpu memory, frames A-B:` with live buffers (count, MiB,
created/destroyed in the window), live textures, queue-write bytes per frame, frames in flight (now/max), how often and
how long `present` waited for an earlier frame, and how many compute write-backs there were and how long they were waited
for (pitfall 12 and "Backpressure" in `webgpu-backend-desktop-und-browser.md`). By default (synchronous write-back)
every write-back counts as waiting, from the submit until the results are in guest memory; that is the GPU time of the
whole list plus the map callback. With `LGP_WEBGPU_ASYNC_WRITEBACK=1` only later calls that still had to wait for open
write-backs count. Until 2026-10-13 the synchronous wait was missing from this number ("waited for 0"). If "buffers live" or the MiB
grow steadily, the backend is holding on to GPU memory; if they stay flat while the process grows, the cause lies elsewhere.
At the end of the line is the peak in the window ("peak in window"): the most live buffer and texture bytes
since the previous line, measured at every creation. It also captures buffers that live only for one list (storage,
readback, copy arena) and are already destroyed again at the end of the line. What the GPU still holds for running
lists after destruction is not counted.
A second line `gpu work, frames A-B:` shows what new content cost in the window: created programs and
draw pipelines with host time (plus prebuilt ones that were waited for or that came too late), dispatches that waited for a
still-compiling compute pipeline, with the wait time (asynchronous pipelines in the browser; this wait counts
for thread 1 as `gpu`, not as load of the GPU thread), new textures and the time
for texture uploads with MiB written. A third line `gpu lists, frames A-B:` counts the
submitted command lists, those among them with compute dispatches that write back guest memory (skipped
dispatches and those without a storage binding do not count), and their draws before the first, between the first and
last, and after the last dispatch. If many draws come after the last dispatch, a write-back waits for them
(`webgpu-backend-desktop-und-browser.md`, pitfall "Write-back in the same submit"). After that the same line counts,
for compute-first (default, `LGP_WEBGPU_COMPUTE_FIRST`), how many dispatches ran ahead and how many had to stay in place, in how
many lists that happened and how often the write-back came from the moved-ahead part (only then does the guest not wait
for the draws).
A fourth line `gpu load, frames A-B:` shows what the frames demanded of the backend: lists, commands per list
(mean and maximum), draws, render passes and
dispatches per frame, buffer→texture and texture→texture copies, newly created and cache-served bind groups,
uploaded constants and geometry in MiB per frame, and textures evicted for the texture budget. If the
process grows (line `host memory`) while `gpu memory` stays flat, the growth lies below the WebGPU calls,
e.g. in the driver's command buffer memory. If the load in this line rises at the same time, it grows along with it.
Last comes `host memory, frame N:` with the memory the operating system charges to the whole process: on
Apple `phys_footprint` from `task_info(TASK_VM_INFO)` (that is what jetsam compares against its limit) and its peak, on
iOS/tvOS additionally `available` from `os_proc_available_memory()` (bytes up to the limit), under Linux/Android `VmRSS`/`VmHWM`,
under Windows the private commit. With `--renderer window`, `guest resident` follows: how much of the guest block is currently in RAM (`mincore`, only Linux/Android/Apple; compressed pages are missing from it). Footprint minus `guest resident` is roughly the host share. On Apple, `metal allocated` follows: what the Metal device holds for the process (`MTLDevice.currentAllocatedSize`, all buffers, textures and heaps, including wgpu's own and the driver's via the device), in parentheses `recommendedMaxWorkingSetSize`. For this, `--time-stats` on Apple creates the default Metal device the first time and keeps it (also with `--renderer soft`). Then `purgeable` and `volatile` from the kernel ledgers (each with its compressed share): purgeable memory (Metal creates resources with a purgeable state this way); only the non-volatile part counts towards the footprint, the volatile part the system may discard without a kill. If graphics grows in the footprint without `metal allocated` rising with it, it is not Metal resources (suspicion then: IOSurfaces, shader code, driver caches). After that follow, where readable (Apple, Linux/Android), system-wide values: `system free` (free pages), `file cache` (file-backed pages, which also includes a file-backed guest block), `compressed` (physical memory occupied by the compressor, Apple only, otherwise 0) and `level` (share of RAM the system counts as available; Apple `kern.memorystatus_level`, Linux `MemAvailable`/`MemTotal`, -1 = unknown). If the footprint stays below the limit while `system free` plus `file cache` drops towards zero, a kill points to a system-wide shortage, not the process limit. In the browser the line is missing. If `available` drops towards zero, iOS/tvOS will terminate the app
soon, often without a jetsam report. With `LGP_HOST_BLOCK_REGIONS=<MiB>`, `guest regions` follows: the guest block per
section resident/dirty, dirty only in the file window (`tvos-als-zielplattform.md`).
With small windows (`--time-stats 30`) a stutter spike,
e.g. at the start of a battle, can thus be attributed to a cause.

## Reading

Example (test program `lgp_boot_sdkpair`, lavapipe, no game data):

```
time: frames 1-3: 0.02 s wall, 141.2 fps (7.1 ms per frame)
time:   thread 1 (prio 44, core 0): guest 0.14, ready 0.00, wait 0.00, ipc 0.00, nvn 0.27, nvn-lock 0.01, gpu 4.54 ms/frame; gpu calls 2.7 (1701 us each), ipc requests 0.0 (0 us each)
time:   gpu thread: busy 4.51 ms/frame (64 %), 2.7 calls (1693 us each)
```

Rules of thumb for a game with a 30 Hz target (33.3 ms per frame):

- **The thread that presents determines the frame rate.** Its sum of `guest + nvn + gpu + ipc + ready` is the
  frame time without waiting. If it has a lot of `wait`, it is waiting for other threads (job system) or for the frame pacing; then find the
  thread it is waiting for (high `guest` time on a job thread).
- **Large `ready`** means: baton contention. Under `--sched baton` (and thus under `--clock virtual`) all
  guest threads run one after another; work that the game spreads over three cores adds up. A frame rate from this
  mode is a lower bound for gameplay (`core-baton`, three slots), not a prediction.
- **Large `gpu`, small `gpu thread busy`**: first subtract the write-back wait time from `gpu memory` (next
  rule). If a lot remains and `nvn-lock` is small, in the browser it is the hand-offs that cost, not the work. Lever: bundle
  fewer calls per frame or hand off asynchronously.
- **`gpu thread busy` near 100 %**: the GPU thread is the bottleneck (recording, pipelines, uploads); then point the
  profiler at the GPU thread (`docs/knowledge/webgpu-backend-draw-kosten.md`).
- **Large `nvn-lock`**: threads record NVN commands at the same time and queue up at the single HLE lock, which is also
  held during the (synchronous) renderer call.
- **"gpu calls" counts every entry into `gpu`**, including `nvnSyncWait`/`nvnQueueFinish`, which return immediately
  without an open write-back. The mean duration per call is then too small. For the hand-off latency, compare the sum of `gpu`
  over all guest threads with `busy` and the call count of the `gpu thread` line.
- **Write-back wait time in `gpu memory` close to `gpu` minus `busy`**: then the call is waiting for the GPU itself, e.g.
  for a compute write-back that only finishes after the whole list (`webgpu-backend-desktop-und-browser.md`,
  pitfall 19). An expensive hand-off is then not the cause.
- **`ready` on threads with little work of their own**: a guest thread only gives up its core when it blocks in the
  kernel. HLE functions that the guest calls directly (NVN) hold the core even while they wait for the GPU thread
  or a lock. Other threads of the same core stay in `ready` for that long.
- **`guest` dominates**: the lift code is the lever (`lifted-code-performance.md`); in the browser, then point the V8 profiler
  (`tools/web/profile_chromium.mjs --start-after "time: frames <N>"`) at exactly that window.

## Asynchronous execution (`LGP_WEBGPU_ASYNC_EXECUTE`)

Without the switch, every list runs synchronously in the renderer call of the guest thread, which holds the HLE lock meanwhile: the
recording on the GPU thread (on the Apple TV around 25 µs per draw, ~14 ms per frame at ~540 draws) lies directly
in the frame timing of all threads waiting for it. With `LGP_WEBGPU_ASYNC_EXECUTE=N` (boot program, `0` turns it off),
`gpu::nvn::AsyncBackend` (`lgp/gpu/nvn/async_backend.hpp`) puts the backend calls into a queue that a dedicated
host thread processes in order:

- `nvnQueueSubmitCommands`, `nvnQueuePresentTexture`, creating/freeing programs and textures return immediately.
- `nvnSyncWait` only waits until the work before its fence has run, `nvnQueueFinish` for everything.
- At most N presents are in flight (default 1: the guest records frame N+1 while frame N runs).
- `nvnTextureWriteTexels` (host pointer) drains the queue and runs synchronously.
- Whoever reads the shown frame (`--frame-stats`, `stable:` trigger) drains the queue first; that costs the
  overlap in exactly those frames.

Consequences for reading: `gpu` now only contains enqueueing and waiting (fences, present limit). The line `gpu queue`
(source next to `gpu thread`) shows how long the queue thread spends in the backend; the line `gpu queue,
frames …` counts the wait times on presents and fences (including the drains for `--frame-stats`) and the longest
queue. If `gpu queue` is near 100 %, the recording is the bottleneck; then the overlap no longer helps.

Semantics: lists read vertex, index and constant buffers only when they run, i.e. later than synchronously. That
matches the console, on which the GPU runs behind the CPU; a game that rewrites a buffer without a fence
while the GPU is still reading it would see the same errors there. Counter reports
(`nvnCommandBufferReportCounter`) only land in guest memory after their list (`Backend::after_queued`, with
`AsyncBackend` on its thread), as on the GPU. A game that reads a timestamp as a progress marker
therefore does not see it before the list. Whether LGP adheres to this everywhere is only proven after runs with image comparison
(`--frame-grid`).

Default: with the WebGPU renderer, N = 1 is the default where the CMake cache `LGP_ASYNC_EXECUTE_DEFAULT` sets it. That is
1 in macOS builds, otherwise 0 (iOS, tvOS, Android, Windows, Linux, browser). The environment wins. The reason for macOS is
an ABAB of LGP on an M1 with host clock, frames 1021–3000 (`docs/analysis/34-gpu-hebel-mobil-messstand.md`). The
fps only rose by 0.9 %, because the game paces at 30 frames. The slowest window rose from 24.5–25.0 to
26.4–27.6 fps, the gpu time of the game threads dropped from about 16.3 to 2.7 ms per frame. With the virtual clock the
frames were identical at all 12 checkpoints (the +4.4 % fps from that run do not count as fps evidence, because only one
guest thread runs at a time there). On the other platforms such evidence is missing. On the Apple TV the gpu time of the
game threads dropped clearly, but without HDMI no fps gain could be measured.

## Evaluating over a frame range and ABAB

`tools/analyze/perf_run_report.py` reads the output of one or more runs (console or `adb logcat`, prefixes
do not matter). It evaluates the lines `time:`, `gpu memory`, `gpu work`, `gpu lists`, `gpu time`, `gpu passes` (#249), `audout` and
`frame N:` for a frame range:

```sh
perf_run_report.py lauf.log --from 2401 --to 3600            # windows + sum over the range
perf_run_report.py --group A=a1.log,a2.log --group B=b1.log,b2.log --from 2401 --to 3600
perf_run_report.py --simpleperf report.txt [--top 25]        # `simpleperf report`, also per comm and library
```

- **fps over the range = frames ÷ wall time**, not the mean of the window fps. A slow window thus weighs
  with its duration. The other ms/frame values are weighted by frames, counters are summed.
- A window only counts if it lies entirely within the range. So choose `--time-stats N` such that the boundaries fall on
  window boundaries (range 2401–3600 with `--time-stats 300` or `100`). If the coverage differs, the
  report says so.
- `audout` lines carry no frame number. Their underruns count towards the next `time:` window.
- `--group`: mean ± sample standard deviation per variant and the difference to the first group. "beyond
  spread" only means |difference| > sd A + sd B, a rough rule and not a significance test. With two runs per variant
  the spread itself is uncertain. Do the runs in ABAB order (measurement discipline); the script does not check the
  order.
- For frame identity between two runs, keep using `frame_grid_compare.py` (`--frame-grid`).

## Limits

- Wall time, not CPU time: a thread in `guest` that the host OS preempts keeps counting as `guest`. On a
  busy machine (second run, browser processes) `guest` therefore looks larger.
- The report reads another thread's counters without stopping it, but consistently between two of its switches
  (seqlock); its running section counts up to the report. Windows of seconds are reliable, individual frames are not.
- The first window starts at program start and includes loading.
- A thread only counts once its host thread is running; the time between `svcStartThread` and the start of the host thread is missing from `ready`.
- Cost: two timestamps per switch (scheduler, every NVN function, every IPC call). With a few 10,000 switches
  per second, natively below 1 %; without the switch only one read of an atomic flag.
- Browser: every timestamp is a `performance.now()` across the JS boundary. In Safari the process grew with
  `--time-stats` by 11–20 MB/s ("WebKit Malloc", not Wasm or GPU memory); without the switch it stayed flat (Mac,
  battle with host clock). Cause inferred, not proven: the clock reads on the worker threads. That is why in the
  browser a helper thread reads the clock only every millisecond, and the switches take this value (`--time-stats-tick US`,
  `rt::set_time_stats_tick_us`; natively the default is 0 = exact clock). Sums over many switches stay correct on average;
  individual sections under 1 ms count as 0 or 1 ms. If the growth remains with the tick, the clock is not the cause.
- Callbacks that finish a GPU task later (readback) do not count towards the GPU thread's `busy`.
- The runtime's host threads (ticker, HLE helper threads, pipeline prebuild) do not appear; the exception is the
  queue thread of asynchronous execution (line `gpu queue`).
