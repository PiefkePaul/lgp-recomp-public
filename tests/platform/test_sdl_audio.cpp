// P5.2: the SDL3 audio sink. Runs with whatever SDL_AUDIO_DRIVER says (ctest
// sets "dummy", which consumes at the device's pace without hardware).
// Exit 77 (skipped) when SDL has no usable audio device.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "../runtime/check.hpp"
#include "lgp/platform/desktop/sdl_audio.hpp"

using lgp::platform::AudioFormat;
using lgp::platform::desktop::SdlAudio;

int main() {
    std::string error;
    constexpr size_t kCapacity = 4800;  // 100 ms
    auto audio = SdlAudio::create(&error, kCapacity);
    if (!audio) {
        std::printf("skipped: %s\n", error.c_str());
        return 77;
    }
    std::printf("driver: %s\n", audio->driver().c_str());
    auto sink = audio->open_sink({48000, 2});
    if (!sink) {
        std::printf("skipped: no playback device\n");
        return 77;
    }
    CHECK_EQ(sink->format().sample_rate, 48000u);
    CHECK_EQ(sink->format().channels, 2u);
    CHECK(audio->open_sink({48000, 0}) == nullptr);

    // 250 ms of a 440 Hz square wave; the sink takes at most its capacity.
    std::vector<int16_t> pcm(12000 * 2);
    for (size_t i = 0; i < 12000; ++i)
        pcm[i * 2] = pcm[i * 2 + 1] = (i / 55) % 2 ? 8000 : -8000;
    CHECK_EQ(sink->queued_frames(), 0u);
    CHECK_EQ(sink->write(pcm.data(), 12000), kCapacity);
    CHECK_EQ(sink->queued_frames(), kCapacity);
    CHECK_EQ(sink->write(pcm.data(), 100), 0u);  // full: dropped

    // Paused (the initial state): nothing is consumed.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK_EQ(sink->queued_frames(), kCapacity);

    // Playing: the device drains the queue at its pace (about 100 ms here).
    const auto t0 = std::chrono::steady_clock::now();
    sink->set_paused(false);
    while (sink->queued_frames() != 0 && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5))
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    std::printf("drained %zu frames in %lld ms\n", kCapacity, static_cast<long long>(ms));
    CHECK_EQ(sink->queued_frames(), 0u);
    CHECK(ms >= 20);  // paced, not swallowed at once
    CHECK_EQ(sink->write(pcm.data(), 480), 480u);  // room again
    sink->set_paused(true);
    sink.reset();
    audio.reset();
    return finish();
}
