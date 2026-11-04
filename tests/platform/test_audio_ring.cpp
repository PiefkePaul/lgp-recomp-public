// P5.2: the WebAudio sink's ring buffer (lgp/platform/web/audio_ring.hpp).
// Natively and under Emscripten/Node: wraparound, capacity, pause, underrun
// counting and a producer/consumer pair on two threads. Under Emscripten
// also the real consumer: audio_worklet.js's lgpAudioRender runs in Node on
// the ring in the wasm memory (the same SharedArrayBuffer views the
// AudioWorklet uses), and WebAudio reports no Web Audio in Node.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "../runtime/check.hpp"
#include "lgp/platform/web/audio_ring.hpp"

#if defined(__EMSCRIPTEN__)
#include <emscripten/em_js.h>

#include "lgp/platform/web/web_audio.hpp"
#include "web_audio_worklet_js.hpp"
#endif

using lgp::platform::web::AudioRing;

#if defined(__EMSCRIPTEN__)
// Two 128-frame quanta through lgpAudioRender into out (planar stereo per
// quantum); returns the frames taken from the ring, -1 if it stopped.
EM_JS(int, js_render_two_quanta, (const char* source, void* header, float* out), {
    const render = new Function(UTF8ToString(source) + '\nreturn lgpAudioRender;')();
    const buf = HEAPU8.buffer;
    const h = new Int32Array(buf, header, 8);
    const pcm = new Int16Array(buf, header + 32, h[2] * h[3]);
    const dst = new Float32Array(buf, out, 2 * 128 * 2);
    let taken = 0;
    for (let q = 0; q < 2; ++q) {
        const ch = [new Float32Array(128), new Float32Array(128)];
        const before = Atomics.load(h, 1);
        if (!render(h, pcm, ch)) return -1;
        taken += Atomics.load(h, 1) - before;
        dst.set(ch[0], q * 256);
        dst.set(ch[1], q * 256 + 128);
    }
    return taken;
});
// One quantum; 1 if its first 100 left samples are 201.., and the processor
// ends once the ring is closed.
EM_JS(int, js_render_wrapped, (const char* source, void* header), {
    const render = new Function(UTF8ToString(source) + '\nreturn lgpAudioRender;')();
    const buf = HEAPU8.buffer;
    const h = new Int32Array(buf, header, 8);
    const pcm = new Int16Array(buf, header + 32, h[2] * h[3]);
    const ch = [new Float32Array(128), new Float32Array(128)];
    render(h, pcm, ch);
    let ok = 1;
    for (let i = 0; i < 100; ++i) if (Math.round(ch[0][i] * 32768) !== 201 + i) ok = 0;
    Atomics.store(h, 6, 1);
    return ok && !render(h, pcm, ch) ? 1 : 0;
});
EM_JS_DEPS(test_audio_ring, "$UTF8ToString");
#endif

namespace {

struct Ring {
    std::vector<uint32_t> storage;  // 4-byte aligned
    AudioRing ring;
    Ring(size_t capacity, uint32_t channels)
        : storage((AudioRing::bytes(capacity, channels) + 3) / 4), ring(storage.data(), capacity, channels) {}
};

std::vector<int16_t> ramp(size_t frames, uint32_t channels, int start) {
    std::vector<int16_t> v(frames * channels);
    for (size_t i = 0; i < frames; ++i)
        for (uint32_t c = 0; c < channels; ++c)
            v[i * channels + c] = static_cast<int16_t>((start + int(i)) * (c == 0 ? 1 : -1));
    return v;
}

void test_layout_and_capacity() {
    CHECK_EQ(AudioRing::round_capacity(4800), 8192u);
    CHECK_EQ(AudioRing::round_capacity(256), 256u);
    CHECK_EQ(AudioRing::bytes(256, 2), 32u + 256 * 4);
    Ring r(100, 2);  // rounded to 128
    CHECK_EQ(r.ring.capacity(), 128u);
    CHECK_EQ(r.ring.channels(), 2u);
    const uint32_t* h = r.storage.data();
    CHECK_EQ(h[2], 128u);  // the worklet reads capacity/channels at these words
    CHECK_EQ(h[3], 2u);
    const auto data = ramp(200, 2, 1);
    CHECK_EQ(r.ring.write(data.data(), 200), 128u);  // full
    CHECK_EQ(r.ring.queued(), 128u);
    CHECK_EQ(r.ring.write(data.data(), 1), 0u);
    CHECK_EQ(h[0], 128u);
}

void test_wraparound() {
    Ring r(64, 2);
    std::vector<int16_t> out(48 * 2);
    int next_write = 0, next_read = 0;
    for (int round = 0; round < 20; ++round) {  // 20 * 48 frames: wraps many times
        const auto data = ramp(48, 2, next_write);
        CHECK_EQ(r.ring.write(data.data(), 48), 48u);
        next_write += 48;
        CHECK_EQ(r.ring.read(out.data(), 48), 48u);
        bool ok = true;
        for (int i = 0; i < 48; ++i)
            ok &= out[i * 2] == int16_t(next_read + i) && out[i * 2 + 1] == int16_t(-(next_read + i));
        CHECK(ok);
        next_read += 48;
    }
    CHECK_EQ(r.ring.queued(), 0u);
    CHECK_EQ(r.ring.underruns(), 0u);
}

void test_pause_and_underrun() {
    Ring r(64, 2);
    const auto data = ramp(16, 2, 100);
    r.ring.write(data.data(), 16);
    std::vector<int16_t> out(32 * 2, 7);
    r.ring.set_paused(true);
    CHECK_EQ(r.ring.read(out.data(), 32), 0u);  // silence, nothing consumed
    CHECK_EQ(out[0], 0);
    CHECK_EQ(r.ring.queued(), 16u);
    CHECK_EQ(r.ring.underruns(), 0u);  // pausing is no underrun
    r.ring.set_paused(false);
    CHECK_EQ(r.ring.read(out.data(), 32), 16u);
    CHECK_EQ(out[0], 100);
    CHECK_EQ(out[16 * 2], 0);  // the rest is silence
    CHECK_EQ(r.ring.underruns(), 16u);
}

void test_threads() {
    // A producer writes 200k frames in uneven pieces while a consumer reads in
    // 128-frame quanta (the AudioWorklet's); the consumer must see every
    // frame once, in order.
    constexpr int kTotal = 200000;
    Ring r(1024, 2);
    std::atomic<bool> ok{true};
    std::thread consumer([&] {
        std::vector<int16_t> out(128 * 2);
        int expect = 0;
        while (expect < kTotal) {
            const size_t n = r.ring.read(out.data(), 128);
            for (size_t i = 0; i < n; ++i, ++expect)
                if (out[i * 2] != int16_t(expect) || out[i * 2 + 1] != int16_t(-expect))
                    ok = false;
            if (n == 0)
                std::this_thread::yield();
        }
    });
    int written = 0;
    size_t piece = 1;
    while (written < kTotal) {
        const size_t want = std::min<size_t>(piece, size_t(kTotal - written));
        const auto data = ramp(want, 2, written);
        written += int(r.ring.write(data.data(), want));
        piece = piece % 700 + 37;
        std::this_thread::yield();
    }
    consumer.join();
    CHECK(ok.load());
    CHECK_EQ(r.ring.queued(), 0u);
}

#if defined(__EMSCRIPTEN__)
void test_worklet_render() {
    // Ring of 256 stereo frames, 300 frames written in two steps so the read
    // position wraps; lgpAudioRender fills 128-frame quanta into Float32Arrays,
    // which are copied to `out` (2 channels, planar).
    Ring r(256, 2);
    auto data = ramp(200, 2, 1);
    CHECK_EQ(r.ring.write(data.data(), 200), 200u);
    std::vector<float> out(2 * 128 * 2);
    const int taken = js_render_two_quanta(kAudioWorkletSource, r.storage.data(), out.data());
    CHECK_EQ(taken, 200);  // 128 + the remaining 72
    CHECK(out[0] == 1.0f / 32768);
    CHECK(out[128] == -1.0f / 32768);
    CHECK(out[256 + 71] == 200.0f / 32768);  // last written frame
    CHECK(out[256 + 72] == 0.0f);            // then silence
    CHECK_EQ(r.ring.underruns(), 56u);
    // Wrap: 100 more frames start at index 200 of the 256-frame ring.
    data = ramp(100, 2, 201);
    CHECK_EQ(r.ring.write(data.data(), 100), 100u);
    const int wrapped = js_render_wrapped(kAudioWorkletSource, r.storage.data());  // also closes the ring
    CHECK_EQ(wrapped, 1);
}

void test_web_audio_in_node() {
    lgp::platform::web::WebAudio audio;
    CHECK(!lgp::platform::web::WebAudio::available());  // Node has no AudioContext
    CHECK(audio.open_sink({48000, 2}) == nullptr);
}
#endif

}  // namespace

int main() {
    RUN(test_layout_and_capacity);
    RUN(test_wraparound);
    RUN(test_pause_and_underrun);
    RUN(test_threads);
#if defined(__EMSCRIPTEN__)
    RUN(test_worklet_render);
    RUN(test_web_audio_in_node);
#endif
    return finish();
}
