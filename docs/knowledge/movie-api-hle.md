# movie:: (multimedia module) as HLE

Status 2026-10-11. Game-independent knowledge on the `movie::` API of the Switch SDK multimedia module, measured from the
calls of one game (LGP, see `../analysis/14-titelvideo-movie-hle.md`),
not from SDK code. Implementation: `runtime/boot/src/movie_hle.cpp`, ADR-0005 (the multimedia module is not
recompiled).

## Structure

`movie::` is closely modeled on Android's `MediaExtractor`/`MediaCodec`, with the same key names in the
format descriptions. A game plays a video like this:

1. `movie::SetAllocator(alloc, free, realloc, user)`, `SetCoreMask`, `GetThreadPriority`: settings. The HLE
   does not need them (host memory, host threads).
2. `Extractor(ContainerType)`, then virtual methods: `SetDataSource(reader, path)`, `GetTrackCount`,
   `GetTrackConfiguration(i, MediaData*)`, `SelectTrack(i)`.
3. `Decoder(DecoderType, DecoderOutputFormat, DecoderMode, priority)`, then `SetEvents`, `Configure(MediaData*)`,
   `Start`.
4. A player thread waits on the decoder events (nn::os MultiWait) and processes whatever has become free:
   - Input free: `DequeueInputBuffer` → `GetInputBuffer(i, Buffer*)` → `Extractor::ReadSampleData(Buffer*)` →
     `GetSampleTime` → `QueueInputBuffer(i, offset, size, pts, flags)` → `Extractor::Advance`.
   - Output free: `DequeueOutputBuffer(&i, &pts, &flags, &rest)`; the game puts the entry into its own
     queue and shows it when `pts` is due: `GetOutputBufferFormat(i, MediaData*)`,
     `GetOutputFormat(MediaData*)`, `Buffer::SetDataAndCapacity(target memory)`, `GetOutputBuffer(i, Buffer*)` (copies
     the frame), `ReleaseOutputBuffer(i)`.
5. End: `Extractor::GetSampleTrackIndex` reports 0x2000, `ReadSampleData` fails, the game submits an empty
   buffer with flag 4 (end of stream); the decoder then returns an output with flag 4.

`DequeueInputBuffer`/`DequeueOutputBuffer` return via `rest` the number of further available buffers; the player
then fetches them in a loop. The player clears the events itself after waking up, so an additional signal
does no harm.

## Virtual methods (index = byte offset / 8 in the vtable)

| Class | Index | Method (name chosen by us) | Arguments |
|---|---|---|---|
| Extractor | 0, 1 | Destructors | |
| Extractor | 4 | GetTrackCount | `int*` |
| Extractor | 5 | GetTrackConfiguration | `int index, MediaData*` |
| Extractor | 6 | SelectTrack | `int index` |
| Extractor | 9 | GetSampleTrackIndex | `int64_t*`; status 0x2000 at the end |
| Extractor | 10 | ReadSampleData | `Buffer*` |
| Extractor | 11 | Advance | |
| Extractor | 12 | GetSampleTime | `int64_t*` (µs) |
| Extractor | 15 | SetDataSource | `reader*, const char* path` (`"data:/…"` = RomFS) |
| Decoder | 2 | Configure | `MediaData*` (track format from the extractor) |
| Decoder | 3 | SetEvents | `nn::os::EventType* [4]`; [0] input free, [1] output free, [2] output format known (the player waits for it, see below), [3] unused by the player (presumably error) |
| Decoder | 4 | GetOutputFormat | `MediaData*` |
| Decoder | 5 | Start | |
| Decoder | 6 | (called on shutdown, presumably Stop) | |
| Decoder | 9 | DequeueInputBuffer | `int* index, int* rest` |
| Decoder | 10 | GetInputBuffer | `int index, Buffer*` |
| Decoder | 11 | QueueInputBuffer | `int index, int offset, int size, int64_t pts, uint32_t flags` |
| Decoder | 12 | DequeueOutputBuffer | `int* index, int64_t* pts, uint32_t* flags, int* rest`; status 0 or 0x2000 counts as a hit |
| Decoder | 13 | GetOutputBuffer | `int index, Buffer*` (copy into the target) |
| Decoder | 15 | ReleaseOutputBuffer | `int index` |
| Decoder | 18 | GetOutputBufferFormat | `int index, MediaData*` |

Status values: 0 = success, 0x2000 = no more data. The measured player treats other values only as "not 0";
the HLE uses 0x1003 for "no buffer free right now" and 0x1001 for errors (our own choice). The measured player does not check the result of
`SetDataSource`: if the video is missing, it crashes later. So the HLE must deliver a
plausible video.

## Format keys (`MediaData`)

Android names: `mime` (`video/x-vnd.on2.vp9`, `video/avc`), `width`, `height`, `durationUs`, `sample-rate`,
`channel-count`. For the output additionally `nv12-y-stride`, `nv12-y-offset`, `nv12-uv-offset`, `nv12-colorspace`.
The measured player reads the three layout values but does not use them: it expects NV12 tightly packed
(`w*h` luma, then `w*h/2` Cb/Cr interleaved, size `w*h*1,5`). It reads `nv12-colorspace` only from
`GetOutputFormat`; if the key is missing, its default stays.

`MediaData` (constructor, `Clear`, `Find{String,Int32,Int64}`) and `Buffer` (`Base`, `Size`, `Offset`, `Capacity`,
`SetRange`, `SetDataAndCapacity`, copies, metadata) are ordinary functions, not inline accesses. The HLE therefore keeps
both entirely on the host (table by guest address). Input buffers also live on the host: the
game only touches them via `ReadSampleData`/`QueueInputBuffer`.

## Implementation in the runtime

- Container: own WebM demuxer (`lgp/hle/webm.hpp`, per RFC 8794/9559), reads only the track list and the positions of the frames,
  data on demand. Paths `mount:/pfad` are looked up in the RomFS (`romfs/<pfad>` or in `romfs.bin` via
  `hle::romfs_find`).
- Decoding: `platform::IVideo` (`lgp/platform/video.hpp`). Desktop: FFmpeg libavcodec (`lgp_platform_ffmpeg`,
  optional via `LGP_FFMPEG_DIR` or pkg-config, dynamically linked). libvpx (`lgp_platform_vpx`, VP9 only, software,
  up to 4 tile threads, `--video vpx`, `cmake/LgpVpx.cmake` builds libvpx 1.17.0 with its own `configure`) is
  the default only under iOS (before that, videos stayed black there, because `LgpFfmpeg.cmake` deliberately searches for
  nothing in cross builds); under macOS/Linux and with the Android NDK it can be enabled via `-DLGP_ENABLE_VPX=ON`. tvOS and the
  iOS simulator do not work yet: for Apple devices `configure` only knows the iPhoneOS SDK and only x86 simulators
  (rejected at configure time, as are Windows hosts, also for Android, and multiple `CMAKE_OSX_ARCHITECTURES`).
  Only the compiler, target and, under Apple, the deployment target are passed on (`--extra-cflags`, replaces the
  iOS 7 minimum of `configure`), not `CMAKE_C_FLAGS`; current Xcode ignores the `-fembed-bitcode` of `configure`.
  `make` runs with the host's core count. `LGP_VPX_DIR` must be 1.17.0 (checked via `CHANGELOG`). Android: MediaCodec (default), libvpx only as a fallback
  in case a device delivers tiled output formats (NDK build checked on the PC, libvpx.a about 580 KB). Without a backend: black frames for the full length
  (`NullVideo`). Browser: WebCodecs `VideoDecoder` on its own worker pthread (`lgp_platform_web`, `WebVideo`,
  [`webcodecs-video-in-emscripten-pthreads.md`](webcodecs-video-in-emscripten-pthreads.md)).
- Flow: 4 input and 4 output buffers per decoder. `QueueInputBuffer` decodes immediately; frames wait until an
  output buffer is free. Input buffers only come back when at most one frame is waiting (back pressure, otherwise
  the feeder decodes the whole video at once). For the audio decoder only from 64 waiting packets: LGP does not take
  any audio before the first frame, and a hardware decoder (MediaCodec) only releases the first frame a few inputs
  later. With the tight back pressure also on audio, the feeder stood at the next audio sample, the video got
  no more input, and the movie start hung (Shield: 10 s without a present, test `runtime.movie_hle`
  `test_late_first_picture`). The video still throttles the feeder, so the audio runs ahead by at most the
  interleaving distance. Events are set after releasing the HLE lock via the
  SDK function `nn::os::SignalEvent` in the calling guest thread.
- Audio tracks (Vorbis) on by default (`MovieHle::configure(..., audio_tracks)`, turn off with `--no-movie-audio`;
  without libvorbis they stay hidden): the track
  appears with `mime` `audio/vorbis`, `sample-rate`, `channel-count`. `SelectTrack` is additive as on Android;
  multiple selected tracks deliver their samples in file order, `GetSampleTrackIndex` names the track. A decoder
  with an `audio/…` format decodes with libvorbis (`lgp/hle/vorbis.hpp`, `LGP_ENABLE_VORBIS`) to interleaved
  16-bit PCM, one output buffer per packet; output format `audio/raw`, `pcm-encoding` 2. That is the Android model;
  measured on the game: for the audio track LGP creates a
  second decoder with `DecoderType 5`, `DecoderOutputFormat 1`, `DecoderMode 1` (video: 3, 4, 1) and outputs the PCM
  itself via its own `audout` with 2 channels.
- `Buffer(size_t capacity)` allocates the memory in the library itself (LGP: once 0x2000 bytes for the
  audio output, then `GetOutputBuffer(i, &buffer)` with this buffer). For this the HLE hands out guest memory from
  `MovieHle::kAllocPages` pages behind its data page and takes it back in the destructor. Without that, the
  buffer had capacity 0 and every packet reported "N bytes do not fit in 0".
- If there is an audio track, LGP paces the video by the audio (the audio clock sets the video renderer's clock). Without an
  audio track, the 23 s title video ran through in just under 8 s in the HLE; so without audio the game applies no
  real-time brake of its own. An HLE should therefore offer the audio track even if nobody is listening.

## Vorbis in WebM

- The track's `CodecPrivate` contains the three Vorbis headers (identification, comment, setup) in Xiph lacing: byte 0 =
  packet count − 1 (i.e. 2), then the lengths of the first two packets as a sequence of 255 bytes plus remainder; the last
  packet takes the rest (`hle::split_xiph_headers`). Each WebM block is then exactly one Vorbis packet.
- The first audio packet does not yet deliver samples (overlap of the MDCT windows); after that, per packet half a
  window of short/long block (at 48 kHz typically 128/1024 frames, mixed 576). Without evaluating the
  granule position or `DiscardPadding`, a few hundred frames of silence more come out at the end than the track is long
  (test video: 48,832 instead of 48,000).
- libvorbis/libogg have no CMake in the release tarballs that we want to use; `cmake/LgpVorbis.cmake` builds
  only the decoder sources itself (like `LgpOpus.cmake`) and generates `ogg/config_types.h`.

## Pitfalls

- A video frame ends up in an NVN buffer via `nvnBufferMap` and goes from there via
  `nvnCommandBufferCopyBufferToTexture` into R8 and RG8 textures. The WebGPU backend previously only copied 4-byte formats
  from buffers; since then it takes the bytes per texel from the NVN format table.
- Uniformly green image (RGB 0, 136, 0) = luma and chroma 0: the textures receive no data.

## Slow calls and the scheduler slot

- `movie:` logs every HLE call over 50 ms (`movie: slow call <class>:<slot> took X ms (Y ms waiting for the lock)`, at most 64 lines).
- Shield measurements, S14 and S15 (title video, 6 runs). These calls are slow, mostly without waiting for the lock:
  - Extractor `SetDataSource` (slot 15, mostly indexing the WebM): 0.6–2.4 s;
  - Decoder `Configure` (slot 2, MediaCodec): 0.5–1.3 s;
  - `QueueInputBuffer` (slot 11): up to 0.4 s.
- Giving the guest thread's scheduler slot up during these calls (the way `HostWorkScope` does) was tried in S15, 2 runs per arm, ABAB. It removed the other threads' `ready` time, but the stall did not get shorter (9.1–10.9 s against 9.4–9.5 s), and lock waits appeared. So it was dropped.
- About 6 s of the stall are pure waiting, on the PC as well.

## Event [2]: output format known

- `Player::Start` starts both decoders and then waits on one event of its own, with `TimedWaitEvent` and a 5 s limit. After that it reads `GetOutputFormat`.
- The event-handler thread ("MovieDecoderEventHandler") sets a "format changed" flag for a decoder on that decoder's event [2]. It signals the player's event only once the flag is set for every decoder. Event [3] is ignored.
- If the HLE never sends [2], every movie start waits out the full 5 s with no movie call. This was measured on the PC (S14-PC, title video) and confirmed in Ghidra.
- The HLE now sends [2] once per decoder, together with that decoder's first output event.
- `LGP_MOVIE_FORMAT_EVENT=0` leaves [2] out, for comparison.

## Indexing as far as the player reads

- `SetDataSource` reads only the EBML header, Info and Tracks, up to the first Cluster (`lgp::hle::WebmIndexer`).
- Every later extractor call indexes until 8 samples of the selected tracks lie ahead of the cursor. Each step reads at most one block header, mostly from the 64 KiB read window.
- Before this, `SetDataSource` read the whole file. For the title video (18.8 MiB) that took 0.3–1.5 s on the Shield with the file-backed guest block, and 125 ms without it (S17), all of it with the guest stopped.
- Log line: `movie: <file> header read in X ms (N reads, M MiB) after Y ms to find it`.
- `LGP_MOVIE_LAZY_INDEX=0` indexes the whole file in `SetDataSource`, as before. The log line then says `indexed in`.
