// Browser sound output (P5.2): IAudio on the Web Audio API. Each sink owns
// an AudioRing in the wasm memory and an AudioContext on the page's main
// thread whose AudioWorklet ("lgp-audio", audio_worklet.js) reads the ring
// directly from the shared wasm memory. write() therefore never leaves the
// calling thread: no message per buffer, no copy through JavaScript.
//
// Needs a cross-origin isolated page (COOP/COEP, the wasm memory is a
// SharedArrayBuffer; the pthread build needs that anyway, ADR-0002) and
// works with PROXY_TO_PTHREAD: the AudioContext is created on the main
// thread through Emscripten's proxying queue. Browsers start an AudioContext only
// after a user gesture; the sink resumes it on the first pointer or key
// event on the page. Until then the ring fills up and write() accepts
// nothing more, like a full device. Node has no AudioContext: open_sink()
// returns nullptr there.
// Link flags: none beyond the project's -pthread ones.
#pragma once

#include <cstddef>
#include <memory>

#include "lgp/platform/audio.hpp"

namespace lgp::platform::web {

class WebAudio final : public IAudio {
public:
    // 100 ms at 48 kHz (rounded up to a power of two: 8192 frames = 170 ms);
    // the same drift policy as the SDL sink (lgp/platform/desktop/sdl_audio.hpp).
    static constexpr size_t kDefaultCapacityFrames = 4800;

    explicit WebAudio(size_t capacity_frames = kDefaultCapacityFrames) : capacity_(capacity_frames) {}
    // nullptr if the page has no Web Audio (Node) or the format is unusable.
    std::unique_ptr<IAudioSink> open_sink(const AudioFormat& format) override;
    // True if this JavaScript environment has an AudioContext.
    static bool available();

private:
    size_t capacity_;
};

}  // namespace lgp::platform::web
