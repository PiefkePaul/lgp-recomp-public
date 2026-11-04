// IAudio: PCM output of the audio services (P3.6; filled in by P5.2).
// audout/audren mix in the runtime and hand over interleaved signed 16-bit
// frames; the backend plays them (SDL on desktop, an AudioWorklet in the
// browser).
//
// write() never blocks. The service paces the guest from queued_frames()
// (release a guest buffer once its frames have left the queue), so a backend
// must drain at the playback rate; the null backend drains on demand.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace lgp::platform {

struct AudioFormat {
    uint32_t sample_rate = 48000;  // Hz; audout's only rate
    uint32_t channels = 2;
};

class IAudioSink {
public:
    virtual ~IAudioSink() = default;
    virtual AudioFormat format() const = 0;
    // Queues up to `frames` frames (frames * channels samples); returns how
    // many were accepted (fewer when the backend's queue is full).
    virtual size_t write(const int16_t* samples, size_t frames) = 0;
    // Frames queued but not yet played.
    virtual size_t queued_frames() const = 0;
    virtual void set_paused(bool paused) = 0;
};

class IAudio {
public:
    virtual ~IAudio() = default;
    // nullptr if the backend cannot play this format.
    virtual std::unique_ptr<IAudioSink> open_sink(const AudioFormat& format) = 0;
};

}  // namespace lgp::platform
