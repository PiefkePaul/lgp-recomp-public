// Backends without host devices (P3.6): CI, tests and headless runs. They
// record what they are given so tests can check it.
#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "lgp/platform/audio.hpp"
#include "lgp/platform/input.hpp"
#include "lgp/platform/renderer.hpp"
#include "lgp/platform/video.hpp"

namespace lgp::platform {

// No window, no GPU. Keeps the last presented image; end_frame() returns
// false after the configured number of frames (0 = never), standing in for a
// closed window.
class NullRenderer final : public IRenderer {
public:
    explicit NullRenderer(SurfaceSize size = {1280, 720}, uint64_t frame_limit = 0)
        : size_(size), frame_limit_(frame_limit) {}
    SurfaceSize surface_size() const override { return size_; }
    GpuHandles gpu() const override { return {}; }
    void present_rgba8(const FrameView& frame) override;
    bool end_frame() override;

    uint64_t frames() const { return frames_.load(); }
    // Last image from present_rgba8, tightly packed.
    std::vector<uint8_t> last_image(SurfaceSize* size) const;

private:
    SurfaceSize size_;
    uint64_t frame_limit_;
    std::atomic<uint64_t> frames_{0};
    mutable std::mutex mutex_;
    std::vector<uint8_t> image_;
    SurfaceSize image_size_;
};

// Discards samples. A sink queues up to `capacity` frames; drain() plays them
// (tests decide the pace, there is no clock).
class NullAudioSink final : public IAudioSink {
public:
    NullAudioSink(AudioFormat format, size_t capacity) : format_(format), capacity_(capacity) {}
    AudioFormat format() const override { return format_; }
    size_t write(const int16_t* samples, size_t frames) override;
    size_t queued_frames() const override;
    void set_paused(bool paused) override { paused_ = paused; }

    // Plays up to n queued frames (none while paused); returns how many.
    size_t drain(size_t n);
    uint64_t played_frames() const;
    // Sum of all written samples, so tests can tell data arrived intact.
    int64_t checksum() const;

private:
    AudioFormat format_;
    size_t capacity_;
    mutable std::mutex mutex_;
    size_t queued_ = 0;
    uint64_t played_ = 0;
    int64_t checksum_ = 0;
    std::atomic<bool> paused_{false};
};

class NullAudio final : public IAudio {
public:
    explicit NullAudio(size_t capacity_frames = 48000) : capacity_(capacity_frames) {}
    std::unique_ptr<IAudioSink> open_sink(const AudioFormat& format) override;

private:
    size_t capacity_;
};

// Input set by the program (tests, scripted runs): set() takes effect at the
// next poll(), like a real device sampled by hid.
class ManualInput final : public IInput {
public:
    explicit ManualInput(size_t controllers = 1)
        : pending_(controllers), current_(controllers), vibration_(controllers) {}
    void poll() override;
    size_t controller_count() const override { return current_.size(); }
    ControllerState controller(size_t index) const override;
    void set_vibration(size_t index, float low, float high) override;

    TouchScreenState touch_screen() const override;

    void set(size_t index, const ControllerState& state);
    void set_touch(const TouchScreenState& touch);
    // Last rumble request for a controller (low, high).
    std::pair<float, float> vibration(size_t index) const;
    // Slots hid reported disconnected by the game (npad_disconnected), in order.
    void npad_disconnected(size_t index) override;
    std::vector<size_t> disconnects() const;

private:
    mutable std::mutex mutex_;
    std::vector<size_t> disconnects_;
    std::vector<ControllerState> pending_;
    std::vector<ControllerState> current_;
    std::vector<std::pair<float, float>> vibration_;
    TouchScreenState pending_touch_, current_touch_;
};

// Black pictures (Y 16, Cb/Cr 128) at the stream's size, one per compressed
// frame, so a video runs for its full length without a decoder.
class NullVideoDecoder final : public IVideoDecoder {
public:
    NullVideoDecoder(uint32_t width, uint32_t height) : width_(width & ~1u), height_(height & ~1u) {}
    bool decode(const uint8_t* data, size_t size, int64_t pts_us, std::vector<VideoPicture>* out) override;

private:
    uint32_t width_, height_;
};

class NullVideo final : public IVideo {
public:
    std::unique_ptr<IVideoDecoder> open_decoder(const VideoStreamInfo& info) override;
};

}  // namespace lgp::platform
