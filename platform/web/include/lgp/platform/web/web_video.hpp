// Browser video decoding (P6, in-game videos of the movie:: HLE): IVideo on
// the WebCodecs VideoDecoder.
//
// WebCodecs is asynchronous: frames come out through a callback of the
// JavaScript event loop, while IVideoDecoder::decode() is a blocking call
// from a guest thread (a pthread busy in wasm code whose event loop never
// runs). The backend therefore owns one extra pthread, the "video worker",
// that returns to its event loop right after starting (runtime keepalive)
// and only runs JavaScript: every decoder lives there, calls reach it through
// Emscripten's proxying queue, and its output callback copies each VideoFrame
// into the wasm heap and converts it to NV12 (lgp/platform/video_convert.hpp)
// before the waiting guest thread picks it up. Nothing runs on the page's
// main thread.
//
// decode() submits the chunk and waits briefly for its picture; a decoder
// that holds frames back (hidden VP9 frames, H.264 reordering) makes the wait
// time out once, after which the backend accepts that many frames of delay
// instead of waiting each time. The end of stream flushes the decoder.
//
// Needs VideoDecoder in a DedicatedWorker (Chromium 94+, Safari 16.4+;
// Firefox 130+) and a cross-origin isolated page (the pthread build needs that
// anyway, ADR-0002). Without it open_decoder() returns nullptr and the
// movie:: HLE plays black pictures. Link flags: none beyond the project's
// -pthread ones.
#pragma once

#include <cstdint>
#include <memory>

#include "lgp/platform/video.hpp"

namespace lgp::platform::web {

class WebVideo final : public IVideo {
public:
    // How a VideoFrame reaches the wasm heap. Auto: its own planes when the
    // browser exposes them (I420/NV12/...), otherwise RGB. The others force
    // the fallbacks for tests: Rgb = copyTo(format "RGBA"), Canvas = drawn
    // into an OffscreenCanvas and read back.
    enum class Copy { Auto, Rgb, Canvas };

    explicit WebVideo(Copy copy = Copy::Auto) : copy_(copy) {}
    // nullptr if this environment has no VideoDecoder or the browser cannot
    // decode the stream (VideoDecoder.isConfigSupported). Blocks until the
    // browser answered; must not run on the page's main thread.
    std::unique_ptr<IVideoDecoder> open_decoder(const VideoStreamInfo& info) override;
    // True if the video worker has a VideoDecoder (starts the worker).
    static bool available();
    // Pictures so far (all decoders) that the browser handed over as RGB:
    // forced by Copy::Rgb/Canvas, opaque frames, or a decoder that outputs
    // RGB itself (Firefox 157 on Linux: BGRX frames, no YUV planes).
    static uint64_t rgb_pictures();

private:
    Copy copy_;
};

}  // namespace lgp::platform::web
