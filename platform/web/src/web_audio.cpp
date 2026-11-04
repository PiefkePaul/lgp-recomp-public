#include "lgp/platform/web/web_audio.hpp"

#include <emscripten/em_js.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>
#include <pthread.h>

#include <cstdlib>
#include <new>

#include "lgp/platform/web/audio_ring.hpp"
#include "web_audio_worklet_js.hpp"  // kAudioWorkletSource (generated from audio_worklet.js)

namespace lgp::platform::web {
namespace {

// Runs on the page's main thread. Creates an AudioContext at the ring's rate,
// loads the worklet from a Blob URL and connects an "lgp-audio" node that
// reads the ring at `header` in the wasm memory. Returns 0 without Web Audio.
EM_JS(int, lgp_web_audio_open, (const char* source, int header, int rate, int channels), {
    if (typeof AudioContext === 'undefined') return 0;
    const ctx = new AudioContext({ sampleRate: rate, latencyHint: 'interactive' });
    const url = URL.createObjectURL(new Blob([UTF8ToString(source)], { type: 'text/javascript' }));
    ctx.audioWorklet.addModule(url).then(() => {
        URL.revokeObjectURL(url);
        const node = new AudioWorkletNode(ctx, 'lgp-audio', {
            numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [channels],
            processorOptions: { buffer: HEAPU8.buffer, header: header },
        });
        node.connect(ctx.destination);
    }).catch((e) => console.error('web audio: ' + e));
    // Autoplay policy: start on the first user gesture if not allowed now.
    const resume = () => { if (ctx.state === 'suspended') ctx.resume(); };
    for (const ev of ['pointerdown', 'keydown', 'touchend'])
        globalThis.addEventListener && globalThis.addEventListener(ev, resume, { capture: true });
    resume();
    if (!globalThis.lgpAudioContexts) globalThis.lgpAudioContexts = new Map();
    globalThis.lgpAudioContexts.set(header, { ctx: ctx, resume: resume });
    return 1;
});
EM_JS_DEPS(lgp_web_audio, "$UTF8ToString");

EM_JS(void, lgp_web_audio_close, (int header), {
    const m = globalThis.lgpAudioContexts;
    const e = m && m.get(header);
    if (!e) return;
    m.delete(header);
    for (const ev of ['pointerdown', 'keydown', 'touchend'])
        globalThis.removeEventListener && globalThis.removeEventListener(ev, e.resume, { capture: true });
    e.ctx.close();
});

EM_JS(int, lgp_web_audio_available, (), { return typeof AudioContext !== 'undefined' ? 1 : 0; });

// Web Audio exists only on the page's main thread; with PROXY_TO_PTHREAD
// the program runs elsewhere, so the calls go through the system proxying
// queue (synchronously: the main thread is idle in its event loop).
void on_main_thread(void (*fn)(void*), void* arg) {
    if (emscripten_is_main_runtime_thread())
        fn(arg);
    else
        emscripten_proxy_sync(emscripten_proxy_get_system_queue(), emscripten_main_runtime_thread_id(), fn, arg);
}

struct OpenCall {
    int header, rate, channels, ok;
};

class WebAudioSink final : public IAudioSink {
public:
    WebAudioSink(void* memory, size_t capacity, const AudioFormat& format)
        : memory_(memory), ring_(memory, capacity, format.channels), format_(format) {
        ring_.set_paused(true);  // audout starts the sink with Start
    }
    ~WebAudioSink() override {
        ring_.close();  // the worklet stops reading before the memory goes
        int header = static_cast<int>(reinterpret_cast<uintptr_t>(memory_));
        on_main_thread([](void* h) { lgp_web_audio_close(*static_cast<int*>(h)); }, &header);
        std::free(memory_);
    }
    bool open() {
        OpenCall call{static_cast<int>(reinterpret_cast<uintptr_t>(memory_)), static_cast<int>(format_.sample_rate),
                      static_cast<int>(format_.channels), 0};
        on_main_thread(
            [](void* p) {
                auto* c = static_cast<OpenCall*>(p);
                c->ok = lgp_web_audio_open(kAudioWorkletSource, c->header, c->rate, c->channels);
            },
            &call);
        return call.ok != 0;
    }

    AudioFormat format() const override { return format_; }
    size_t write(const int16_t* samples, size_t frames) override { return ring_.write(samples, frames); }
    size_t queued_frames() const override { return ring_.queued(); }
    void set_paused(bool paused) override { ring_.set_paused(paused); }

private:
    void* memory_;
    AudioRing ring_;
    AudioFormat format_;
};

}  // namespace

bool WebAudio::available() {
    int ok = 0;
    on_main_thread([](void* p) { *static_cast<int*>(p) = lgp_web_audio_available(); }, &ok);
    return ok != 0;
}

std::unique_ptr<IAudioSink> WebAudio::open_sink(const AudioFormat& format) {
    if (format.channels == 0 || format.channels > 8 || format.sample_rate == 0 || !available())
        return nullptr;
    void* memory = std::aligned_alloc(16, (AudioRing::bytes(capacity_, format.channels) + 15) & ~size_t{15});
    if (memory == nullptr)
        return nullptr;
    auto sink = std::make_unique<WebAudioSink>(memory, capacity_, format);
    if (!sink->open())
        return nullptr;
    return sink;
}

}  // namespace lgp::platform::web
