// P5: VpxVideo (platform/vpx, libvpx). The VP9 decoder opens (H.264 does not), rejects garbage
// without crashing and has nothing to flush; the VP9 test video (argv[1],
// tests/platform/data/vp9_pattern.webm) decodes to the expected pictures.
#include <vector>

#include "../runtime/check.hpp"
#include "lgp/platform/vpx_video.hpp"
#include "video_pattern.hpp"

using namespace lgp::platform;

namespace {

void test_open_and_garbage() {
    VpxVideo video;
    CHECK(video.open_decoder({VideoCodec::H264, 1920, 1080, {}}) == nullptr);  // VP8/VP9 only
    for (const VideoCodec codec : {VideoCodec::Vp9}) {
        std::unique_ptr<IVideoDecoder> dec = video.open_decoder({codec, 1920, 1080, {}});
        CHECK(dec != nullptr);
        if (!dec)
            continue;
        std::vector<VideoPicture> pics;
        std::vector<uint8_t> garbage(512);
        for (size_t i = 0; i < garbage.size(); ++i)
            garbage[i] = uint8_t(i * 37 + 11);
        dec->decode(garbage.data(), garbage.size(), 0, &pics);  // may or may not fail, must not crash
        CHECK(dec->decode(nullptr, 0, 0, &pics));
        CHECK(pics.empty());
    }
}

const char* g_video_path = nullptr;

void test_pattern_video() {
    namespace vp = lgp_test::video_pattern;
    std::vector<uint8_t> bytes;
    lgp::hle::WebmFile file;
    CHECK(g_video_path != nullptr && vp::load(g_video_path, &bytes, &file));
    VpxVideo video;
    std::unique_ptr<IVideoDecoder> dec = video.open_decoder({VideoCodec::Vp9, vp::kWidth, vp::kHeight, {}});
    CHECK(dec != nullptr);
    if (dec)
        CHECK(vp::decode_and_check(*dec, bytes, file, 2) == 0);
}

}  // namespace

int main(int argc, char** argv) {
    g_video_path = argc > 1 ? argv[1] : nullptr;
    RUN(test_open_and_garbage);
    RUN(test_pattern_video);
    return finish();
}
