// Desktop sound output (P5.2): IAudio on SDL3's audio streams. Each sink is
// an SDL_AudioStream bound to the default playback device; SDL converts the
// 48 kHz 16-bit frames to the device format and pulls them on its own audio
// thread. write() puts frames into the stream (SDL_PutAudioStreamData) as
// long as fewer than capacity_frames are queued (SDL_GetAudioStreamQueued)
// and accepts fewer beyond that: audout drops them instead of letting the
// latency grow when the host clock runs ahead of the sound card.
//
// SDL_AUDIO_DRIVER selects the driver as usual ("dummy" plays nothing at the
// device's pace, for tests and machines without sound).
#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "lgp/platform/audio.hpp"

struct SDL_AudioStream;

namespace lgp::platform::desktop {

class SdlAudio final : public IAudio {
public:
    // 100 ms at 48 kHz: enough for the title's few queued buffers plus a
    // late host tick, small enough to keep the delay unnoticeable.
    static constexpr size_t kDefaultCapacityFrames = 4800;

    // nullptr and *error if SDL's audio subsystem does not start.
    static std::unique_ptr<SdlAudio> create(std::string* error, size_t capacity_frames = kDefaultCapacityFrames);
    ~SdlAudio() override;
    SdlAudio(const SdlAudio&) = delete;
    SdlAudio& operator=(const SdlAudio&) = delete;

    // nullptr if the device cannot be opened. The sink starts paused.
    std::unique_ptr<IAudioSink> open_sink(const AudioFormat& format) override;
    // SDL's audio driver ("pipewire", "pulseaudio", "alsa", "wasapi", "coreaudio", "dummy", ...).
    const std::string& driver() const { return driver_; }
    // Volume of every sink, open or opened later: 0 (mute) to 1 (as the game
    // sends it). The app's settings (P7).
    void set_gain(float gain);
    // The game stands (the app's pause menu) while the sinks keep running:
    // an empty queue then is no underrun. Pauses through audout
    // (IAudioSink::set_paused) need no call.
    void set_held(bool held) { streams_->held.store(held, std::memory_order_relaxed); }

    // Shared with the sinks, which outlive no SdlAudio but may close first.
    struct Streams {
        std::mutex mutex;
        float gain = 1.0f;
        std::vector<SDL_AudioStream*> open;
        std::atomic<bool> held{false};  // set_held
    };

private:
    explicit SdlAudio(size_t capacity_frames) : capacity_(capacity_frames) {}
    size_t capacity_;
    std::string driver_;
    std::shared_ptr<Streams> streams_ = std::make_shared<Streams>();
};

}  // namespace lgp::platform::desktop
