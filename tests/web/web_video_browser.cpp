// P6 browser test: the WebCodecs video backend decodes the self-made VP9
// test video (tests/platform/data/vp9_pattern.webm, embedded at /vp9.webm)
// like the movie:: HLE drives it, from a pthread (PROXY_TO_PTHREAD), once per
// copy path: the frame's own planes (the browser's normal case) and the two
// RGB fallbacks (opaque frames, Safari). Tolerances: planes are exact
// (Chromium 1194: error 0); RGB goes through the browser's YUV->RGB
// conversion and back (error up to 3). Firefox 157 decodes to BGRX frames,
// so its "planes" path is RGB too and gets the RGB tolerance.
#include <cstdio>
#include <vector>

#include "../platform/video_pattern.hpp"
#include "lgp/platform/web/output_drain.hpp"
#include "lgp/platform/web/web_video.hpp"

namespace vp = lgp_test::video_pattern;
using lgp::platform::web::WebVideo;

int main() {
    if (!WebVideo::available()) {
        std::printf("web video: no VideoDecoder in workers\n");
        return 1;
    }
    std::vector<uint8_t> bytes;
    lgp::hle::WebmFile file;
    if (!vp::load("/vp9.webm", &bytes, &file))
        return 1;
    struct Path {
        const char* name;
        WebVideo::Copy copy;
        int tolerance;
    };
    int problems = 0;
    for (const Path& p : {Path{"planes", WebVideo::Copy::Auto, 1}, Path{"rgb", WebVideo::Copy::Rgb, 3},
                          Path{"canvas", WebVideo::Copy::Canvas, 3}}) {
        std::printf("[%s]\n", p.name);
        WebVideo video(p.copy);
        auto dec = video.open_decoder({lgp::platform::VideoCodec::Vp9, vp::kWidth, vp::kHeight, {}});
        if (!dec) {
            std::printf("  open_decoder failed\n");
            ++problems;
            continue;
        }
        if (p.copy != WebVideo::Copy::Auto) {
            problems += vp::decode_and_check(*dec, bytes, file, p.tolerance);
            continue;
        }
        // Check exactly first; a browser that only hands out RGB frames
        // (counted by the backend) gets the RGB tolerance instead.
        const uint64_t rgb_before = WebVideo::rgb_pictures();
        int found = vp::decode_and_check(*dec, bytes, file, p.tolerance);
        if (found != 0 && WebVideo::rgb_pictures() != rgb_before) {
            std::printf("  the browser decoded to RGB frames: checking again with tolerance 3\n");
            auto again = video.open_decoder({lgp::platform::VideoCodec::Vp9, vp::kWidth, vp::kHeight, {}});
            found = again ? vp::decode_and_check(*again, bytes, file, 3) : 1;
        }
        problems += found;
    }
    // A stream the browser cannot decode is refused, not fatal.
    lgp::platform::VideoStreamInfo bad{lgp::platform::VideoCodec::H264, 64, 48, {1, 0xff, 0xff, 0xff, 0xff}};
    if (WebVideo().open_decoder(bad) != nullptr)
        std::printf("  note: the browser accepts avc1.ffffff\n");
    if (problems != 0)
        std::printf("web video: %d problem(s)\n", problems);
    else
        std::printf("web video: ok\n");
    lgp::platform::web::drain_output();  // Firefox lost these last lines now and then
    return problems != 0 ? 1 : 0;
}
