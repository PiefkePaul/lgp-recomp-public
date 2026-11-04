// P7.3: MediaCodecVideo (platform/android), run on the phone over adb. The
// VP9 decoder opens, survives garbage and has nothing to flush; the VP9 test
// video (argv[1], tests/platform/data/vp9_pattern.webm) decodes to the
// expected pictures.
#include <vector>

#include "../runtime/check.hpp"
#include "lgp/platform/android/mediacodec_video.hpp"
#include "video_pattern.hpp"

using namespace lgp::platform;

namespace {

void test_open_and_garbage() {
    android::MediaCodecVideo video;
    std::unique_ptr<IVideoDecoder> dec = video.open_decoder({VideoCodec::Vp9, 1920, 1080, {}});
    CHECK(dec != nullptr);
    if (!dec)
        return;
    std::vector<VideoPicture> pics;
    std::vector<uint8_t> garbage(512);
    for (size_t i = 0; i < garbage.size(); ++i)
        garbage[i] = uint8_t(i * 37 + 11);
    dec->decode(garbage.data(), garbage.size(), 0, &pics);  // may or may not fail, must not crash
    dec->decode(nullptr, 0, 0, &pics);
}

const char* g_video_path = nullptr;

void test_pattern_video() {
    namespace vp = lgp_test::video_pattern;
    std::vector<uint8_t> bytes;
    lgp::hle::WebmFile file;
    CHECK(g_video_path != nullptr && vp::load(g_video_path, &bytes, &file));
    android::MediaCodecVideo video;
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
