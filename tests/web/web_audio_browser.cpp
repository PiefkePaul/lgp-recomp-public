// P5.2 browser test: the WebAudio sink plays 500 ms of a 440 Hz tone through
// its AudioWorklet. Passes when the worklet has consumed every frame (the
// ring empties at the device's pace). Needs a browser that may start an
// AudioContext without a gesture (Chromium: --autoplay-policy=
// no-user-gesture-required, set by browser_test.mjs).
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include "lgp/platform/web/web_audio.hpp"

int main() {
    using clock = std::chrono::steady_clock;
    if (!lgp::platform::web::WebAudio::available()) {
        std::printf("web audio: no AudioContext\n");
        return 1;
    }
    lgp::platform::web::WebAudio audio;
    auto sink = audio.open_sink({48000, 2});
    if (!sink) {
        std::printf("web audio: open_sink failed\n");
        return 1;
    }
    constexpr size_t kFrames = 24000;
    std::vector<int16_t> pcm(kFrames * 2);
    for (size_t i = 0; i < kFrames; ++i)
        pcm[i * 2] = pcm[i * 2 + 1] = static_cast<int16_t>(6000 * std::sin(2 * 3.14159265 * 440 * double(i) / 48000));
    sink->set_paused(false);
    const auto t0 = clock::now();
    size_t written = 0;
    while ((written < kFrames || sink->queued_frames() != 0) && clock::now() - t0 < std::chrono::seconds(20)) {
        written += sink->write(pcm.data() + written * 2, kFrames - written);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0).count();
    if (written < kFrames || sink->queued_frames() != 0) {
        std::printf("web audio: stuck after %lld ms, %zu of %zu frames written, %zu queued\n",
                    static_cast<long long>(ms), written, kFrames, sink->queued_frames());
        return 1;
    }
    std::printf("web audio: ok, %zu frames played in %lld ms\n", kFrames, static_cast<long long>(ms));
    sink.reset();
    return 0;
}
