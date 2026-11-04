#include "lgp/platform/desktop/sdl_audio.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cinttypes>

namespace lgp::platform::desktop {
namespace {

class SdlAudioSink final : public IAudioSink {
public:
    SdlAudioSink(SDL_AudioStream* stream, const AudioFormat& format, size_t capacity,
                 std::shared_ptr<SdlAudio::Streams> streams)
        : stream_(stream), format_(format), capacity_(capacity), frame_bytes_(format.channels * 2),
          streams_(std::move(streams)) {
        std::lock_guard lock(streams_->mutex);
        streams_->open.push_back(stream_);
        SDL_SetAudioStreamGain(stream_, streams_->gain);
        SDL_SetAudioStreamGetCallback(stream_, on_get, this);
        SDL_AudioSpec device{};
        int device_frames = 0;
        if (SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(stream_), &device, &device_frames))
            SDL_Log("lgp audio: device %d Hz, %d channels, %d frames per buffer (%.1f ms); queue up to %zu frames "
                    "(%.0f ms)",
                    device.freq, device.channels, device_frames,
                    device.freq > 0 ? 1000.0 * device_frames / device.freq : 0.0, capacity_,
                    format_.sample_rate > 0 ? 1000.0 * double(capacity_) / format_.sample_rate : 0.0);
    }
    ~SdlAudioSink() override {
        {
            std::lock_guard lock(streams_->mutex);
            std::erase(streams_->open, stream_);
        }
        SDL_DestroyAudioStream(stream_);  // also closes the device; no callback after this
        log_starved(true);
    }

    AudioFormat format() const override { return format_; }

    size_t write(const int16_t* samples, size_t frames) override {
        writing_.store(true, std::memory_order_relaxed);
        log_starved(false);
        const size_t queued = queued_frames();
        const size_t n = std::min(frames, queued < capacity_ ? capacity_ - queued : 0);
        if (n == 0 || !SDL_PutAudioStreamData(stream_, samples, static_cast<int>(n * frame_bytes_)))
            return 0;
        return n;
    }

    size_t queued_frames() const override {
        const int bytes = SDL_GetAudioStreamQueued(stream_);
        return bytes > 0 ? static_cast<size_t>(bytes) / frame_bytes_ : 0;
    }

    void set_paused(bool paused) override {
        if (paused) {
            SDL_PauseAudioStreamDevice(stream_);
            writing_.store(false, std::memory_order_relaxed);  // an empty queue in a pause is no underrun
        } else {
            SDL_ResumeAudioStreamDevice(stream_);
        }
    }

private:
    // On SDL's audio thread, before the device takes data from the stream:
    // additional_amount > 0 means the queue holds less than the device wants,
    // so it plays silence for that part (an underrun on our side; the guest's
    // audout did not deliver in time). Counted only once frames were written.
    static void SDLCALL on_get(void* userdata, SDL_AudioStream*, int additional_amount, int) {
        auto* self = static_cast<SdlAudioSink*>(userdata);
        if (additional_amount <= 0 || !self->writing_.load(std::memory_order_relaxed) ||
            self->streams_->held.load(std::memory_order_relaxed))
            return;
        self->starved_.fetch_add(1, std::memory_order_relaxed);
        self->starved_bytes_.fetch_add(uint64_t(additional_amount), std::memory_order_relaxed);
    }

    // At most every 10 s (and at the end) when the count grew: how often the
    // device found the queue short and how much silence that was (Verdacht:
    // SDL may ask a little more than needed, so this is an upper bound).
    void log_starved(bool final) {
        const uint64_t now_ms = SDL_GetTicks();
        if (!final && now_ms - last_log_ms_ < 10'000)
            return;
        last_log_ms_ = now_ms;
        const uint64_t count = starved_.load(std::memory_order_relaxed);
        const uint64_t bytes = starved_bytes_.load(std::memory_order_relaxed);
        if (count == logged_count_)
            return;
        const double ms = format_.sample_rate > 0 ? 1000.0 * double((bytes - logged_bytes_) / frame_bytes_) /
                                                        format_.sample_rate
                                                  : 0.0;
        SDL_Log("lgp audio: queue ran short %" PRIu64 " times (%.0f ms silence) %s, %" PRIu64 " in total",
                count - logged_count_, ms, final ? "until the end" : "in the last 10 s", count);
        logged_count_ = count;
        logged_bytes_ = bytes;
    }

    SDL_AudioStream* stream_;
    AudioFormat format_;
    size_t capacity_;
    size_t frame_bytes_;
    std::shared_ptr<SdlAudio::Streams> streams_;
    std::atomic<bool> writing_{false};
    std::atomic<uint64_t> starved_{0};
    std::atomic<uint64_t> starved_bytes_{0};
    uint64_t last_log_ms_ = 0;  // the writer's thread only
    uint64_t logged_count_ = 0;
    uint64_t logged_bytes_ = 0;
};

}  // namespace

std::unique_ptr<SdlAudio> SdlAudio::create(std::string* error, size_t capacity_frames) {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        if (error != nullptr)
            *error = std::string("SDL audio: ") + SDL_GetError();
        return nullptr;
    }
    std::unique_ptr<SdlAudio> audio(new SdlAudio(capacity_frames));
    const char* driver = SDL_GetCurrentAudioDriver();
    audio->driver_ = driver != nullptr ? driver : "";
    return audio;
}

SdlAudio::~SdlAudio() { SDL_QuitSubSystem(SDL_INIT_AUDIO); }

std::unique_ptr<IAudioSink> SdlAudio::open_sink(const AudioFormat& format) {
    if (format.channels == 0 || format.channels > 8 || format.sample_rate == 0)
        return nullptr;
    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_S16;
    spec.channels = static_cast<int>(format.channels);
    spec.freq = static_cast<int>(format.sample_rate);
    // Opens the default device paused, with a stream bound to it.
    SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (stream == nullptr)
        return nullptr;
    return std::make_unique<SdlAudioSink>(stream, format, capacity_, streams_);
}

void SdlAudio::set_gain(float gain) {
    std::lock_guard lock(streams_->mutex);
    streams_->gain = std::clamp(gain, 0.0f, 1.0f);
    for (SDL_AudioStream* stream : streams_->open)
        SDL_SetAudioStreamGain(stream, streams_->gain);
}

}  // namespace lgp::platform::desktop
