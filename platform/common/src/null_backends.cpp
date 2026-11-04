#include "lgp/platform/null_backends.hpp"

#include <algorithm>
#include <cstring>

namespace lgp::platform {

void NullRenderer::present_rgba8(const FrameView& frame) {
    std::lock_guard lock(mutex_);
    image_.resize(size_t{frame.width} * frame.height * 4);
    for (uint32_t y = 0; y < frame.height; ++y)
        std::memcpy(image_.data() + size_t{y} * frame.width * 4, frame.pixels + size_t{y} * frame.stride,
                    size_t{frame.width} * 4);
    image_size_ = {frame.width, frame.height};
}

bool NullRenderer::end_frame() {
    const uint64_t n = ++frames_;
    return frame_limit_ == 0 || n < frame_limit_;
}

std::vector<uint8_t> NullRenderer::last_image(SurfaceSize* size) const {
    std::lock_guard lock(mutex_);
    if (size != nullptr)
        *size = image_size_;
    return image_;
}

size_t NullAudioSink::write(const int16_t* samples, size_t frames) {
    std::lock_guard lock(mutex_);
    const size_t n = std::min(frames, capacity_ - queued_);
    for (size_t i = 0; i < n * format_.channels; ++i)
        checksum_ += samples[i];
    queued_ += n;
    return n;
}

size_t NullAudioSink::queued_frames() const {
    std::lock_guard lock(mutex_);
    return queued_;
}

size_t NullAudioSink::drain(size_t n) {
    if (paused_)
        return 0;
    std::lock_guard lock(mutex_);
    const size_t played = std::min(n, queued_);
    queued_ -= played;
    played_ += played;
    return played;
}

uint64_t NullAudioSink::played_frames() const {
    std::lock_guard lock(mutex_);
    return played_;
}

int64_t NullAudioSink::checksum() const {
    std::lock_guard lock(mutex_);
    return checksum_;
}

std::unique_ptr<IAudioSink> NullAudio::open_sink(const AudioFormat& format) {
    if (format.channels == 0 || format.channels > 6 || format.sample_rate == 0)
        return nullptr;
    return std::make_unique<NullAudioSink>(format, capacity_);
}

void ManualInput::poll() {
    std::lock_guard lock(mutex_);
    current_ = pending_;
    current_touch_ = pending_touch_;
}

TouchScreenState ManualInput::touch_screen() const {
    std::lock_guard lock(mutex_);
    return current_touch_;
}

void ManualInput::set_touch(const TouchScreenState& touch) {
    std::lock_guard lock(mutex_);
    pending_touch_ = touch;
}

ControllerState ManualInput::controller(size_t index) const {
    std::lock_guard lock(mutex_);
    return index < current_.size() ? current_[index] : ControllerState{};
}

void ManualInput::set_vibration(size_t index, float low, float high) {
    std::lock_guard lock(mutex_);
    if (index < vibration_.size())
        vibration_[index] = {low, high};
}

void ManualInput::set(size_t index, const ControllerState& state) {
    std::lock_guard lock(mutex_);
    if (index < pending_.size())
        pending_[index] = state;
}

void ManualInput::npad_disconnected(size_t index) {
    std::lock_guard lock(mutex_);
    disconnects_.push_back(index);
}

std::vector<size_t> ManualInput::disconnects() const {
    std::lock_guard lock(mutex_);
    return disconnects_;
}

std::pair<float, float> ManualInput::vibration(size_t index) const {
    std::lock_guard lock(mutex_);
    return index < vibration_.size() ? vibration_[index] : std::pair<float, float>{0, 0};
}

bool NullVideoDecoder::decode(const uint8_t* data, size_t size, int64_t pts_us, std::vector<VideoPicture>* out) {
    if (data == nullptr || size == 0)
        return true;  // end of stream: nothing held back
    VideoPicture pic;
    pic.width = width_;
    pic.height = height_;
    pic.pts_us = pts_us;
    const size_t luma = size_t{width_} * height_;
    pic.nv12.assign(luma + luma / 2, 128);
    std::fill(pic.nv12.begin(), pic.nv12.begin() + ptrdiff_t(luma), uint8_t{16});
    out->push_back(std::move(pic));
    return true;
}

std::unique_ptr<IVideoDecoder> NullVideo::open_decoder(const VideoStreamInfo& info) {
    return std::make_unique<NullVideoDecoder>(info.width, info.height);
}

}  // namespace lgp::platform
