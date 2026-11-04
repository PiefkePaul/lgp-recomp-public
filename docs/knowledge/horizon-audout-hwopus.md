# `audout:u` and `hwopus` as HLE: PCM output, Opus decoding, host backends

Cross-game knowledge from P5.2 (`runtime/hle/src/audout.cpp`, `runtime/hle/src/hwopus.cpp`, backends in `platform/desktop/src/sdl_audio.cpp` and `platform/web/src/web_audio.cpp`; tests `runtime.hle_services`, `runtime.hle_hwopus(_checked)`, `platform.sdl_audio`, `platform.audio_ring`, `boot.sdk_audio_sdl`, `browser.web_audio`). Command numbers and argument layouts come from switchbrew ("Audio services") and libnx `audout.h`/`hwopus.h` (only as an interface description, no code taken over). Which commands exist follows the Ryujinx reference run of LGP; everything else halts the guest.

## When a game needs which service

- **`audren:u`** (AudioRenderer) is the usual route: the game describes voices, mixers and effects, the DSP mixes. Games with their own audio engine (Wwise, FMOD, CRI), on the other hand, mix themselves and output finished PCM via **`audout:u`**. LGP (Wwise) opens `audren` only for device queries (`GetAudioDeviceServiceWithRevisionInfo`, device names, output volume) and never `OpenAudioRenderer`.
- **`hwopus`** decodes Opus packets. On the console this is a service with its own decoder; engines like Wwise send each packet individually via IPC. In the reference run LGP opens 594 decoders and calls `DecodeInterleavedWithPerfOld` 58,631 times, i.e. one IPC call per packet (typically 20 ms).

## `audout:u`

**Manager** (`IAudioOutManager`): 0/2 `ListAudioOuts(Auto)` writes a 0x100-byte name (`DeviceOut`) and returns the count. 1/3 `OpenAudioOut(Auto)`: input `u32 sample_rate, u16 channel_count, u16 reserved, u64 aruid`, PID, copy handle to the process, name as in-buffer; output `u32 sample_rate, u32 channel_count, u32 sample_format, u32 state` plus the `IAudioOut` object and the name as out-buffer. 0 for rate or channels means "default" (48 kHz, 2 channels). Format 2 = `PcmInt16`, state 0 = started, 1 = stopped.

**Per output** (`IAudioOut`): 0 `GetAudioOutState`, 1 `Start`, 2 `Stop`, 3/7 `AppendAudioOutBuffer(Auto)(u64 tag)` with the buffer description as in-buffer, 4 `RegisterBufferEvent` (copy handle), 5/8 `GetReleasedAudioOutBuffers(Auto)` (out-buffer with u64 tags, returns the count), 6 `ContainsAudioOutBuffer`, 9 `GetAudioOutBufferCount`, 10 `GetAudioOutPlayedSampleCount`, 11 `FlushAudioOutBuffers`, 12/13 `Set/GetAudioOutVolume` (f32).

- Buffer description `AudioOutBuffer` (0x28 bytes): `next`, `sample_buffer` (guest address), `capacity`, `data_size`, `data_offset`. Played are `data_size` bytes from `sample_buffer + data_offset`, interleaved s16.
- The **tag** is an arbitrary u64 value of the SDK (which one does not matter to the runtime); the service returns it unchanged. The SDK holds the description until the tag is reported as released.
- **Release**: a buffer is "released" when it has finished playing. Then the buffer event is signaled; the SDK fetches the tags with `GetReleasedAudioOutBuffers`. The runtime clears the event as soon as there is nothing left to fetch. LGP's audio thread sits in a loop "wait, fetch, refill" (282,385 fetches in the reference run, 53,164 appends, i.e. several queries per buffer).
- **Pacing**: the runtime releases buffers according to the **host clock** (`AudioOutState::update(kernel, now_ns)`, in the boot program every 5 ms): the head of the queue starts "playing" when its turn comes and is done after `frames / 48000` s. If the queue runs empty, the next buffer starts on its arrival (underrun without catching up).
- **5.1**: LGP requests 6 channels (as when docked). The runtime accepts 2 and 6 and always gives the host backend stereo: left = FL + C/2 + RL/2, right = FR + C/2 + RR/2, LFE is dropped, with saturation to s16. Channel order in the 6-channel buffer: FL, FR, C, LFE, RL, RR (as in switchbrew/SDK; not yet checked audibly in the game).
- The volume from `SetAudioOutVolume` is applied before handing over to the backend.
- **At most 32 buffers per output**: a buffer counts from its append until the guest has fetched its tag with `GetReleasedAudioOutBuffers`. A 33rd append is refused with `ResultBufferRingFull` (module 153, description 8; as Ryujinx's `AudioDeviceSession`, `AudioDeviceBufferCountMax`), is not queued and does not reach the sink; the runtime warns once per output. This only bounds memory and sound when a guest keeps appending without fetching; it does not stop such a guest (LGP ignores the result).

## Host clock vs. sound card (drift)

The guest is paced by the host clock, the backend consumes samples at the sound card's rate. The two drift apart (typically a few tens of ppm, more with host stutters). Minimal rule in the runtime:

- Every backend has a **bounded queue** (SDL and WebAudio: 4800 frames = 100 ms; WebAudio rounds up to 8192 frames ≈ 170 ms, because the ring needs a power of two). `IAudioSink::write()` only takes what fits; `audout` discards the rest and reports this once as a warning. Latency thus cannot grow.
- An **underrun** (sound card faster than the host clock, or the guest delivers too late) plays silence; SDL and the worklet do that by themselves.
- Release to the guest stays tied to the host clock, not to the backend's fill level: without a device (`--audio none`, tests, Node) the game runs just as fast as with one. Releasing according to the backend's `queued_frames()` (sound card as clock source) would be the alternative if the acceptance test (60 s without dropouts) fails because of this.

## `hwopus`

**Manager** (`IHardwareOpusDecoderManager`): 0 `OpenHardwareOpusDecoder(s32 sample_rate, s32 channel_count, u32 work_buffer_size)` with a copy handle to transfer memory (the work buffer), output is the decoder object. 1 `GetWorkBufferSize(s32 sample_rate, s32 channel_count)` → u32. Allowed are the Opus rates 8000, 12000, 16000, 24000, 48000 Hz and 1 or 2 channels; LGP needs neither multistream (commands 2/3) nor the "Ex" variants of newer firmware.

**Decoder** (`IHardwareOpusDecoder`): 0 `DecodeInterleavedOld` and 4 `DecodeInterleavedWithPerfOld`. In-buffer (type A) with the framed packet, out-buffer (type B) for the PCM. Response: `u32 consumed` (bytes including the frame header), `u32 sample_count` (samples per channel), with "WithPerf" additionally `u64` decode time. The unit of the time is not documented; the runtime measures host time in microseconds (assumption). Later firmware has further variants (6 with a reset flag, 8 `DecodeInterleaved`), which LGP (SDK 5.4) does not use.

- **Framing (Nintendo format)**: `u32 big-endian payload_size`, `u32 big-endian final_range`, then exactly one Opus packet (RFC 6716). `final_range` is the encoder's final range-coder state (`OPUS_GET_FINAL_RANGE`); a decoder that decodes the same packet bit-exactly arrives at the same value. The runtime compares it after every packet and warns once on a mismatch; this exposes framing and transmission errors.
- **Length without a decoder**: the TOC byte (RFC 6716, 3.1) determines the frame duration (SILK 10/20/40/60 ms, hybrid 10/20 ms, CELT 2.5/5/10/20 ms) and code 0–3 the frame count; at most 120 ms per packet. The silence fallback (`opus_packet_samples`) is based on this.
- **Work buffer**: on the console the decoder state lives in the game's transfer memory. The runtime keeps its libopus state on the host and does not touch the buffer. `GetWorkBufferSize` answers with an **assumed**, not measured size: room for the decoder state (upper bound 32 KiB; libopus 1.5.2 needs about 18 KiB mono / 26 KiB stereo, the test checks the bound) plus one 120 ms frame of s16 at the decoder rate, rounded to 4 KiB (48 kHz stereo: 0xE000). The value is the same in builds with and without libopus, so that the game's memory layout does not change. In the reference run LGP creates 597 transfer memories totaling 44 MiB, i.e. around 76 KiB per decoder with Ryujinx's answer; the real console's value is open.
- An empty or truncated packet and errors from libopus halt the guest (`resp.unimplemented`), because the console's error codes are not known.

## libopus in the runtime

- `cmake/LgpOpus.cmake` downloads the 1.5.2 release tarball (SHA-256 pinned, offline via `LGP_OPUS_DIR`) and builds it statically for native and Emscripten: float build, without DNN/DRED/OSCE, without programs and tests. Switch `LGP_ENABLE_OPUS` (default on); without libopus, `hwopus` decodes to silence of the correct length (`hwopus_decodes_audio()` tells which case applies).
- One `OpusDecoder` per `IHardwareOpusDecoder` (`opus_decoder_create(rate, channels)`), `opus_decode` directly into s16, with room for 120 ms, limited to the guest's out-buffer. One mutex per decoder, because several guest threads could use the same session.
- The test encodes a stereo signal itself with the libopus encoder (440/660 Hz, 96 kbit/s, 20 ms frames), sends the packets in Nintendo's framing through IPC and checks: length, bit-identical with a local libopus decoder, correlation to the original > 0.98 (after subtracting the encoder lookahead of 312 samples) and energy ratio 0.8–1.25. Plus a mono decoder at 24 kHz.

## Host backends behind `IAudio`

- **Desktop (SDL3)**: `lgp::platform::desktop::SdlAudio` (target `lgp_platform_sdl_audio`, only with `LGP_SDL_AVAILABLE`). One `SDL_AudioStream` per sink via `SDL_OpenAudioDeviceStream` on the default device, `SDL_PutAudioStreamData` for writing, `SDL_GetAudioStreamQueued` for the fill level, pause/resume via the stream's device. SDL converts to the device format and pulls on its own thread. `SDL_AUDIO_DRIVER=dummy` consumes at device rate without hardware (tests, CI). Under Linux SDL only builds PipeWire/PulseAudio/ALSA if their headers are present at build time (`libpipewire-0.3-dev`, `libpulse-dev`, `libasound2-dev`); without them there are only `dummy` and `disk`.
- **Boot program**: `--audio none|sdl`, default `sdl` with `--renderer window`, otherwise `none`. If SDL audio does not start, the game keeps running without sound (message on stderr).
- **Browser (WebAudio)**: `lgp::platform::web::WebAudio` (target `lgp_platform_web`, Emscripten only). The sink places an SPSC ring (`audio_ring.hpp`) in Wasm memory; an AudioWorklet (`audio_worklet.js`, embedded as a string at build time and loaded via a Blob URL) reads the same `SharedArrayBuffer` with `Atomics`, without a message per buffer. Ring header: write and read position (frames mod 2³²), capacity (power of two), channels, pause, underrun counter, "closed". The `AudioContext` is created on the browser main thread (with `PROXY_TO_PTHREAD` via Emscripten's proxying queue). Browsers only start it after a user gesture; the sink resumes it on the first pointer/key event. Needs cross-origin isolation (COOP/COEP, `tools/web/serve.py`), no additional linker flags. The boot program in the browser (P6.1) takes Web Audio without an option (`boot::host_devices()`); the click on the folder selection happens before the sink is opened, so sound only becomes audible after the next key press or the next click on the page. Emscripten's Wasm audio worklets (`AUDIO_WORKLET`/`WASM_WORKERS`) would be the alternative; the pure JS worklet avoids a second thread type next to the pthreads.
- Growing Wasm memory is not a problem for the worklet: shared memory is never detached, and the `SharedArrayBuffer` passed at open time still covers the ring created earlier.

## Metrics in the log

On open, `audout` writes `OpenAudioOut(<rate>, <requested channels>) -> <channels>, open #<n> at guest <ms>` (n counts the opens since boot) and, for every 10 s of appended audio time,
an info line: discarded frames (sink full), underruns (queue ran empty while started) and
RMS/peak per guest channel. The underruns only count the runtime's guest queue; if the sink itself runs empty
(sound card slightly faster than the host clock, guest buffers still in the queue), that does not show up there. This way "runs without dropouts" and "which channels carry signal" can be checked without listening and without
a recording; the numbers contain no game content. With `SDL_AUDIO_DRIVER=dummy` the sink runs at real-time pace
without speakers.

The present-stall report (`--stall-report`) has one line per live output: open number, state and number of `Start` calls, queued and released-but-unfetched buffers, appends (refused ones in brackets), `GetReleased` calls with the time since the last one and the tags they returned. A guest that keeps appending while "released" grows and the call count stands still has stopped fetching on its side. For the order of events, `LGP_AUDOUT_LOG=N` logs the first N `Start`/`Stop`/`GetReleased` calls of every output with the guest time (the base of `svcGetSystemTick`) and the queue sizes.
