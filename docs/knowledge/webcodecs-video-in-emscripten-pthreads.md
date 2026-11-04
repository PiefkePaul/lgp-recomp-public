# WebCodecs video from Emscripten pthreads

Status 2026-10-11. Across games: how an Emscripten program with `PROXY_TO_PTHREAD` (ADR-0002) decodes videos via the
WebCodecs `VideoDecoder` although the guest threads never return to their event loop, and how the
frames get into Wasm memory as NV12. Implementation: `platform/web/src/web_video.cpp` (`WebVideo`, behind
`platform::IVideo`), conversion `platform/common/src/video_convert.cpp`. The consumer is the `movie::` HLE
([`movie-api-hle.md`](movie-api-hle.md)).

## The basic problem

`IVideoDecoder::decode()` is a blocking call from a guest thread: frame in, finished frames out. WebCodecs
is asynchronous: `decoder.decode(chunk)` returns immediately, the frame comes later via the `output` callback, and
`VideoFrame.copyTo()` returns a promise. Both only run when the JavaScript thread that owns the decoder returns to
its event loop. A pthread that executes Wasm code (the game's player thread) never does that.
Creating the decoder there and waiting would be a deadlock: the waiting (`Atomics.wait`) blocks exactly the loop
that would have to deliver the result.

## Solution: an own "video worker" pthread

- On first use `WebVideo` starts a pthread that calls `emscripten_runtime_keepalive_push()`, reports via a
  semaphore that it is running, and **returns**. With keepalive the worker then stays alive and only serves
  its event loop: calls via Emscripten's proxying queue (`emscripten_proxy_sync`) and the
  WebCodecs callbacks. Without the semaphore a call can reach the worker before it is running.
- All decoders live on this worker; the page's main thread stays free (WebGPU presentation and
  the other proxy calls run there).
- The guest thread sends short calls via `emscripten_proxy_sync` (open, enqueue chunk, flush, close) and
  then waits for events with `std::condition_variable::wait_for`. Waiting with a time limit works in workers; on the
  page's main thread it does not (so `open_decoder` returns `nullptr` there).
- Events from the worker back into C++: `getWasmTableEntry(cb)(...)` with a function pointer that is passed
  at open (`EM_JS_DEPS(..., "$getWasmTableEntry")`). The callback runs on the worker, takes the mutex and
  wakes the guest thread.
- Closing runs on the same worker as the callbacks: after `lgp_web_video_close` (sets `closed`, closes the
  decoder) no event for this decoder is guaranteed to come any more, the C++ side may free its state. Copies still
  running check `closed` after every `await`.

## Synchronous interface over an asynchronous decoder

- `decode()` enqueues the chunk and waits up to 100 ms for the matching frame. A decoder can hold back frames
  (hidden VP9 alt-ref frames, H.264 reordering). Once the wait times out, the
  decoder remembers the backlog (`lag`) and from then on only waits until at most that many frames are pending. So there
  is no permanent stutter, only once up to 100 ms. Late frames come along with the next call.
- End of stream: `flush()` and wait (up to 5 s) until the copy chain is empty too. **After `flush()` WebCodecs requires
  a keyframe again** (`[[key chunk required]]`); so a flush per frame to force the latency does not work.
- `EncodedVideoChunk` needs `type: 'key' | 'delta'`; the first chunk and every one after an error must be `key`,
  otherwise `decode()` throws. The interface does not know the flag, so it is read from the bitstream
  (`is_keyframe`): VP9 from the uncompressed header (frame marker, profile bits, `show_existing_frame`, `frame_type`);
  a superframe starts with its first frame, byte 0 is enough. H.264 in AVCC form: a NAL unit of type 5
  (IDR), length field from the avcC (`lengthSizeMinusOne`).
- After a decoder error the `VideoDecoder` is closed. The backend creates it anew at the next keyframe
  and discards delta frames until then (`decode()` reports `false`, the HLE logs a warning).
- `VideoDecoder.isConfigSupported()` is asynchronous too; `open_decoder` waits up to 5 s for it and returns
  `nullptr` if the browser cannot handle the stream. The `movie::` HLE then plays black frames.
- Codec strings: VP9 `vp09.00.LL.08` (profile 0, 8 bit, level by frame size, 1080p → 41), H.264 `avc1.PPCCLL` from
  bytes 1–3 of the avcC, which goes along as `description`.

## Frames into Wasm memory

- `frame.copyTo(ziel, { rect, layout })` with a self-chosen, dense layout (offset and stride per plane) delivers
  the planes without padding. `rect` = visible area, rounded down to even dimensions (the chroma planes require even
  coordinates).
- Formats (`frame.format`): software decoders in Chromium deliver `I420` (for VP9 with alpha `I420A`), hardware decoders
  often `NV12`; also possible `I422`, `I444`, `RGBA/RGBX/BGRA/BGRX`. The conversion to dense NV12 runs on the
  worker (`planar_to_nv12`, `nv12_copy`, `rgb32_to_nv12`), the guest thread only moves the finished frame.
- `frame.format === null` (opaque GPU frames, possible with Safari) and unknown formats such as
  `I420P10`: fallback to RGB, first `copyTo(ziel, { rect, format: 'RGBA' })` (Chromium from 127), otherwise via an
  `OffscreenCanvas` (`drawImage` + `getImageData`). Back to YUV with the matrix from `frame.colorSpace.matrix`
  (`bt709`, otherwise BT.601; without a value BT.709 from 720 lines up) in limited range. This costs time and some
  accuracy (in the test 1 step on flat areas, Chromium), but is only the emergency route. YUV values outside the
  RGB color space are lost in the process (a first version of the test video had such values and deviated by up to 33);
  real video material lies within the color space.
- The target is first an ordinary `Uint8Array`, then `_malloc` + `HEAPU8.set`. Copying directly into a view of the
  shared Wasm memory is allowed by the specification (`AllowSharedBufferSource`), but is not safe in every
  browser; the copy is about 3 MB per frame at 1080p and uncritical.
- Pointers from C++ arrive in `EM_JS` as signed numbers; above 2 GiB (the game uses ≈ 3.4 GiB of
  Wasm memory, ADR-0001) they are negative, and `HEAPU8.slice(p, …)` then reads from the end. Hence `p >>>= 0` before every
  access. Emscripten's growth pass (`GROWABLE_HEAP_*`) also covers `EM_JS` code, so `HEAPU8` is current there
  after a `memory.grow`.

## Browsers

| Browser | VideoDecoder in a worker | VP9 | H.264 | Evidence |
|---|---|---|---|---|
| Chromium (Playwright 1.56.1, CI) | yes | yes (libvpx, `I420`) | no (without proprietary codecs) | `browser.web_video` |
| Chrome | yes (from 94) | yes | yes | documentation, not measured |
| Safari | from 16.4 (WebCodecs video in Window and DedicatedWorker) | depends on hardware decoding and macOS version | yes | **not checked**, Mac: `LGP_BROWSER=safari ctest --preset wasm -R browser.web_video` |
| Firefox | from 130 | yes | depending on the system | not checked (P6.4) |

Whether Safari decodes VP9 1080p in WebCodecs and which `frame.format` it delivers is open. If it fails,
`open_decoder` reports it (console: `web video: the browser cannot decode …`) and the video stays black; one route would then be
libvpx to Wasm.

## Test without game data

`tests/platform/data/vp9_pattern.webm` (6 KB, `tools/video/gen_test_webm.py`): 64×48, 30 frames, VP9 two-pass with
alt-ref (7 hidden frames in superframes), flat areas with frame-dependent values. `browser.web_video` decodes
it via all three copy routes (planes, `copyTo` RGBA, canvas) and checks count, order, timestamps and the
values at the area centers; `platform.ffmpeg_video` checks the same with FFmpeg, `platform.video_convert` the
conversion and keyframe detection. Result in Chromium (Playwright 1.56.1): all 30 frames, plane route exact,
both RGB routes deviation 1; every frame already arrived in the `decode()` call of its chunk (the backlog mechanism
did not kick in). FFmpeg on the other hand only releases frames of the same video at the end of the stream (frame threads); the HLE copes
with both.

Not covered: pointers above 2 GiB (only the real game gets there), the backlog route with a
decoder that holds back frames, H.264 (Playwright Chromium cannot do it).
