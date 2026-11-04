#include "lgp/platform/web/web_video.hpp"

#include <emscripten/em_js.h>
#include <emscripten/eventloop.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>
#include <pthread.h>
#include <semaphore.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <utility>

#include "lgp/platform/video_convert.hpp"

namespace lgp::platform::web {
namespace {

// Frame layouts the video worker hands over (low byte of `fmt`; JavaScript
// copies the visible rectangle with tightly packed planes). Bit 8: BT.709
// matrix (else BT.601), bit 9: full range. Only used by the RGB layouts.
enum FrameFormat : int { kI420 = 0, kI420A = 1, kNV12 = 2, kI422 = 3, kI444 = 4, kRGBX = 5, kBGRX = 6 };
constexpr int kFmtBt709 = 0x100, kFmtFullRange = 0x200;

// Events from the video worker (lgp_web_video_event).
enum Event : int { kSupport = 0, kFrame = 1, kFlushed = 2, kError = 3 };

// --- JavaScript, all of it on the video worker --------------------------------------

// Creates the decoder state `id`, asks isConfigSupported and configures a
// VideoDecoder; answers with event kSupport (fmt 1 = supported). Output
// frames are copied in order (one promise chain) and reported as kFrame with
// a malloc'd buffer the C++ side frees. `cb` is lgp_web_video_event.
EM_JS(void, lgp_web_video_open,
      (int id, const char* codec, int width, int height, int desc, int desc_len, int copy, int cb), {
    desc >>>= 0;  // wasm32 pointers above 2 GiB arrive as negative numbers
    const call = (kind, ptr, fmt, w, h, ts) => getWasmTableEntry(cb)(id, kind, ptr, fmt, w, h, ts);
    if (typeof VideoDecoder === 'undefined') { call(0, 0, 0, 0, 0, 0); return; }
    const config = { codec: UTF8ToString(codec), codedWidth: width, codedHeight: height, optimizeForLatency: true };
    if (desc_len > 0) config.description = HEAPU8.slice(desc, desc + desc_len);
    const st = { config, closed: false, needKey: true, decoder: null, chain: Promise.resolve(), canvas: null, call };
    if (!globalThis.lgpVideo) globalThis.lgpVideo = new Map();
    globalThis.lgpVideo.set(id, st);

    // Tightly packed planes of the visible rectangle: [stride, rows] each.
    const planes = { I420: (w, h) => [[w, h], [w / 2, h / 2], [w / 2, h / 2]],
                     I420A: (w, h) => [[w, h], [w / 2, h / 2], [w / 2, h / 2], [w, h]],
                     NV12: (w, h) => [[w, h], [w, h / 2]],
                     I422: (w, h) => [[w, h], [w / 2, h], [w / 2, h]],
                     I444: (w, h) => [[w, h], [w, h], [w, h]],
                     RGBA: (w, h) => [[w * 4, h]], RGBX: (w, h) => [[w * 4, h]],
                     BGRA: (w, h) => [[w * 4, h]], BGRX: (w, h) => [[w * 4, h]] };
    const codes = { I420: 0, I420A: 1, NV12: 2, I422: 3, I444: 4, RGBA: 5, RGBX: 5, BGRA: 6, BGRX: 6 };
    const deliver = async (frame) => {
        const vr = frame.visibleRect || { x: 0, y: 0, width: frame.displayWidth, height: frame.displayHeight };
        const w = vr.width & ~1, h = vr.height & ~1;
        if (w === 0 || h === 0) return;
        const rect = { x: vr.x, y: vr.y, width: w, height: h };
        const cs = frame.colorSpace || {};
        let flags = (cs.matrix === 'bt709' || (!cs.matrix && h >= 720)) ? 0x100 : 0;
        if (cs.fullRange) flags |= 0x200;
        let fmt = frame.format, bytes = null;
        if (copy === 0 && fmt in planes) {
            const layout = [];
            let size = 0;
            for (const [stride, rows] of planes[fmt](w, h)) { layout.push({ offset: size, stride }); size += stride * rows; }
            try {
                bytes = new Uint8Array(size);
                await frame.copyTo(bytes, { rect, layout });
            } catch (e) { bytes = null; }
        }
        if (!bytes) {
            // Opaque (GPU) frames or layouts without a mapping: let the browser
            // convert to RGB, by copyTo (Chromium 127+) or through a canvas.
            fmt = 'RGBA';
            flags &= 0x100;  // the browser's RGB is limited-range video converted to full-range RGB
            try {
                if (copy === 2) throw new Error('canvas forced');
                bytes = new Uint8Array(w * h * 4);
                await frame.copyTo(bytes, { rect, format: 'RGBA' });
            } catch (e) {
                if (typeof OffscreenCanvas === 'undefined') throw e;
                if (!st.canvas || st.canvas.width !== w || st.canvas.height !== h) st.canvas = new OffscreenCanvas(w, h);
                const ctx = st.canvas.getContext('2d', { willReadFrequently: true });
                ctx.drawImage(frame, vr.x, vr.y, w, h, 0, 0, w, h);
                bytes = new Uint8Array(ctx.getImageData(0, 0, w, h).data.buffer);
            }
        }
        if (st.closed) return;
        const ptr = _malloc(bytes.length);
        if (!ptr) return;
        HEAPU8.set(bytes, ptr);
        call(1, ptr, codes[fmt] | flags, w, h, frame.timestamp);
    };
    st.make = () => {
        st.decoder = new VideoDecoder({
            output: (frame) => {
                st.chain = st.chain.then(() => (st.closed ? undefined : deliver(frame)))
                               .catch((e) => err('web video: frame copy failed: ' + e))
                               .finally(() => frame.close());
            },
            error: (e) => {
                if (st.closed) return;
                err('web video: decoder error: ' + e);
                st.needKey = true;
                call(3, 0, 0, 0, 0, 0);
            },
        });
        st.decoder.configure(st.config);
    };
    VideoDecoder.isConfigSupported(config).then((r) => {
        if (st.closed) return;
        let ok = 0;
        if (r.supported) {
            try { st.make(); ok = 1; } catch (e) { err('web video: configure failed: ' + e); }
        }
        call(0, 0, ok, 0, 0, 0);
    }, (e) => {
        if (!st.closed) { err('web video: isConfigSupported failed: ' + e); call(0, 0, 0, 0, 0, 0); }
    });
});
EM_JS_DEPS(lgp_web_video, "$UTF8ToString,$getWasmTableEntry,malloc");

// Queues one chunk (the bytes are copied); 0 if it was not accepted (a
// delta frame while the decoder needs a keyframe, or a closed decoder).
EM_JS(int, lgp_web_video_decode, (int id, int data, int size, double ts, int key), {
    const st = globalThis.lgpVideo && globalThis.lgpVideo.get(id);
    if (!st) return 0;
    if (!st.decoder || st.decoder.state === 'closed') {
        if (!key) return 0;
        try { st.make(); } catch (e) { return 0; }  // after a decoding error: start again at a keyframe
    }
    if (st.needKey && !key) return 0;
    data >>>= 0;
    try {
        st.decoder.decode(new EncodedVideoChunk({ type: key ? 'key' : 'delta', timestamp: ts,
                                                  data: HEAPU8.slice(data, data + size) }));
        st.needKey = false;
        return 1;
    } catch (e) {
        err('web video: decode failed: ' + e);
        st.needKey = true;
        return 0;
    }
});

// Emits every held frame, then event kFlushed (also after an error).
EM_JS(void, lgp_web_video_flush, (int id), {
    const st = globalThis.lgpVideo && globalThis.lgpVideo.get(id);
    if (!st) return;
    const done = () => st.chain.then(() => { if (!st.closed) st.call(2, 0, 0, 0, 0, 0); });
    st.needKey = true;  // WebCodecs wants a keyframe after flush()
    if (!st.decoder || st.decoder.state !== 'configured') { done(); return; }
    st.decoder.flush().then(done, done);
});

// After this no event for `id` arrives any more (same thread as the events).
EM_JS(void, lgp_web_video_close, (int id), {
    const st = globalThis.lgpVideo && globalThis.lgpVideo.get(id);
    if (!st) return;
    st.closed = true;
    globalThis.lgpVideo.delete(id);
    try { if (st.decoder && st.decoder.state !== 'closed') st.decoder.close(); } catch (e) {}
});

EM_JS(int, lgp_web_video_has_decoder, (), { return typeof VideoDecoder !== 'undefined' ? 1 : 0; });

// --- Video worker -----------------------------------------------------------------

// The pthread that hosts the decoders: it signals that it runs, keeps its
// runtime alive and returns, so its event loop serves proxied calls and
// WebCodecs callbacks until the program exits.
pthread_t video_worker() {
    static const pthread_t thread = [] {
        static sem_t started;
        sem_init(&started, 0, 0);
        pthread_t t{};
        pthread_create(
            &t, nullptr,
            [](void*) -> void* {
                emscripten_runtime_keepalive_push();
                sem_post(&started);
                return nullptr;
            },
            nullptr);
        sem_wait(&started);
        return t;
    }();
    return thread;
}

// Runs fn(arg) on the video worker and waits for it.
void on_worker(void (*fn)(void*), void* arg) {
    emscripten_proxy_sync(emscripten_proxy_get_system_queue(), video_worker(), fn, arg);
}

// State shared between a decoder (guest thread) and its events (worker).
struct Shared {
    std::mutex m;
    std::condition_variable cv;
    int support = -1;  // -1 unanswered, 0 no, 1 yes
    bool flushed = false;
    uint64_t received = 0;  // pictures delivered
    uint64_t errors = 0;
    std::deque<VideoPicture> ready;
};

// Pictures that reached the wasm heap as RGB (WebVideo::rgb_pictures()).
std::atomic<uint64_t> g_rgb_pictures{0};

extern "C" void lgp_web_video_event(int id, int kind, int ptr, int fmt, int w, int h, double ts) {
    auto* s = reinterpret_cast<Shared*>(static_cast<uintptr_t>(id));
    if (kind == kFrame) {
        // Convert here, on the worker, so the guest thread only moves the picture.
        auto* src = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(ptr));
        const uint32_t uw = uint32_t(w), uh = uint32_t(h);
        const size_t luma = size_t{uw} * uh;
        VideoPicture pic;
        pic.pts_us = int64_t(ts);
        switch (fmt & 0xff) {
        case kI420:
        case kI420A:
            planar_to_nv12(src, uw, src + luma, src + luma + luma / 4, uw / 2, 1, 1, uw, uh, &pic);
            break;
        case kI422:
            planar_to_nv12(src, uw, src + luma, src + luma + luma / 2, uw / 2, 1, 0, uw, uh, &pic);
            break;
        case kI444:
            planar_to_nv12(src, uw, src + luma, src + 2 * luma, uw, 0, 0, uw, uh, &pic);
            break;
        case kNV12:
            nv12_copy(src, uw, src + luma, uw, uw, uh, &pic);
            break;
        case kRGBX:
        case kBGRX:
            g_rgb_pictures.fetch_add(1, std::memory_order_relaxed);
            rgb32_to_nv12(src, size_t{uw} * 4, (fmt & 0xff) == kBGRX,
                          (fmt & kFmtBt709) ? YuvMatrix::Bt709 : YuvMatrix::Bt601, (fmt & kFmtFullRange) != 0, uw,
                          uh, &pic);
            break;
        }
        std::free(reinterpret_cast<void*>(static_cast<uintptr_t>(ptr)));
        std::lock_guard<std::mutex> lock(s->m);
        if (!pic.nv12.empty())
            s->ready.push_back(std::move(pic));
        else
            ++s->errors;
        ++s->received;
    } else {
        std::lock_guard<std::mutex> lock(s->m);
        if (kind == kSupport)
            s->support = fmt != 0 ? 1 : 0;
        else if (kind == kFlushed)
            s->flushed = true;
        else if (kind == kError)
            ++s->errors;
    }
    s->cv.notify_all();
}

int id_of(Shared* s) {
    return static_cast<int>(reinterpret_cast<uintptr_t>(s));
}

void close_on_worker(Shared* s) {
    int id = id_of(s);
    on_worker([](void* p) { lgp_web_video_close(*static_cast<int*>(p)); }, &id);
}

// WebCodecs codec string for the stream.
std::string codec_string(const VideoStreamInfo& info) {
    char buf[32];
    if (info.codec == VideoCodec::H264) {
        const auto& c = info.codec_private;
        if (c.size() >= 4)
            std::snprintf(buf, sizeof buf, "avc1.%02x%02x%02x", c[1], c[2], c[3]);
        else
            std::snprintf(buf, sizeof buf, "avc1.42e01e");  // Baseline 3.0, Annex B without avcC
        return buf;
    }
    // vp09.<profile>.<level>.<bit depth>: profile 0, 8 bit (what the games
    // ship); the level after the picture size (VP9 level table, samples per frame).
    const uint64_t samples = uint64_t{info.width} * info.height;
    int level = 61;
    for (const auto& [max, l] : {std::pair<uint64_t, int>{36864, 10}, {73728, 11}, {122880, 20}, {245760, 21},
                                 {552960, 30}, {983040, 31}, {2228224, 41}, {8912896, 51}})
        if (samples <= max) {
            level = l;
            break;
        }
    std::snprintf(buf, sizeof buf, "vp09.00.%02d.08", level);
    return buf;
}

class WebDecoder final : public IVideoDecoder {
public:
    // How long decode() waits for the picture of the chunk it submitted, and
    // the end of stream for the rest.
    static constexpr auto kFrameWait = std::chrono::milliseconds(100);
    static constexpr auto kFlushWait = std::chrono::seconds(5);

    WebDecoder(const VideoStreamInfo& info, int copy)
        : shared_(std::make_unique<Shared>()), info_(info), copy_(copy) {}
    ~WebDecoder() override { close_on_worker(shared_.get()); }

    bool open() {
        struct Call {
            int id;
            std::string codec;
            const VideoStreamInfo* info;
            int copy;
        } call{id_of(shared_.get()), codec_string(info_), &info_, copy_};
        on_worker(
            [](void* p) {
                auto* c = static_cast<Call*>(p);
                const auto& cp = c->info->codec == VideoCodec::H264 ? c->info->codec_private : std::vector<uint8_t>{};
                lgp_web_video_open(c->id, c->codec.c_str(), int(c->info->width), int(c->info->height),
                                   static_cast<int>(reinterpret_cast<uintptr_t>(cp.data())), int(cp.size()), c->copy,
                                   static_cast<int>(reinterpret_cast<uintptr_t>(&lgp_web_video_event)));
            },
            &call);
        std::unique_lock<std::mutex> lock(shared_->m);
        shared_->cv.wait_for(lock, std::chrono::seconds(5), [&] { return shared_->support >= 0; });
        if (shared_->support != 1)
            std::fprintf(stderr, "web video: the browser cannot decode %s %ux%u\n", call.codec.c_str(),
                         info_.width, info_.height);
        return shared_->support == 1;
    }

    bool decode(const uint8_t* data, size_t size, int64_t pts_us, std::vector<VideoPicture>* out) override {
        Shared& s = *shared_;
        uint64_t errors_before;
        {
            std::lock_guard<std::mutex> lock(s.m);
            errors_before = s.errors;
        }
        if (data == nullptr || size == 0) {
            {
                std::lock_guard<std::mutex> lock(s.m);
                s.flushed = false;
            }
            int id = id_of(shared_.get());
            on_worker([](void* p) { lgp_web_video_flush(*static_cast<int*>(p)); }, &id);
            std::unique_lock<std::mutex> lock(s.m);
            const bool done = s.cv.wait_for(lock, kFlushWait, [&] { return s.flushed; });
            // Everything submitted is out (or lost to an error): start counting afresh.
            submitted_ = s.received;
            lag_ = 0;
            drain(out);
            return done && s.errors == errors_before;
        }
        struct Call {
            int id;
            const uint8_t* data;
            size_t size;
            double ts;
            int key;
            int accepted;
        } call{id_of(shared_.get()), data, size, double(pts_us),
               is_keyframe(info_.codec, data, size, avcc_nal_length(info_.codec_private)) ? 1 : 0, 0};
        on_worker(
            [](void* p) {
                auto* c = static_cast<Call*>(p);
                c->accepted = lgp_web_video_decode(c->id, static_cast<int>(reinterpret_cast<uintptr_t>(c->data)),
                                                   int(c->size), c->ts, c->key);
            },
            &call);
        std::unique_lock<std::mutex> lock(s.m);
        if (call.accepted == 0) {
            drain(out);
            return false;
        }
        ++submitted_;
        // Wait for this chunk's picture unless the decoder is known to lag.
        if (!s.cv.wait_for(lock, kFrameWait, [&] { return s.received + lag_ >= submitted_ || s.errors != errors_before; }))
            lag_ = submitted_ - s.received;
        drain(out);
        return s.errors == errors_before;
    }

private:
    // Caller holds shared_->m.
    void drain(std::vector<VideoPicture>* out) {
        while (!shared_->ready.empty()) {
            out->push_back(std::move(shared_->ready.front()));
            shared_->ready.pop_front();
        }
    }

    std::unique_ptr<Shared> shared_;
    VideoStreamInfo info_;
    int copy_;                // WebVideo::Copy
    uint64_t submitted_ = 0;  // chunks accepted by the decoder
    uint64_t lag_ = 0;        // pictures the decoder may hold back
};

}  // namespace

uint64_t WebVideo::rgb_pictures() {
    return g_rgb_pictures.load(std::memory_order_relaxed);
}

bool WebVideo::available() {
    int ok = 0;
    on_worker([](void* p) { *static_cast<int*>(p) = lgp_web_video_has_decoder(); }, &ok);
    return ok != 0;
}

std::unique_ptr<IVideoDecoder> WebVideo::open_decoder(const VideoStreamInfo& info) {
    if (emscripten_is_main_browser_thread())
        return nullptr;  // would wait for the worker on the thread that serves it
    auto dec = std::make_unique<WebDecoder>(info, static_cast<int>(copy_));
    if (!dec->open())
        return nullptr;
    return dec;
}

}  // namespace lgp::platform::web
