// P6: NV12 conversion and keyframe detection for the video backends
// (lgp/platform/video_convert.hpp), natively and in Node. The keyframe test
// runs on the VP9 test video (argv[1], tests/platform/data/vp9_pattern.webm).
#include <vector>

#include "../runtime/check.hpp"
#include "lgp/platform/video_convert.hpp"
#include "video_pattern.hpp"

using namespace lgp::platform;

namespace {

const char* g_video_path = nullptr;

void test_planar() {
    // 4x2 I420: luma 0..7, Cb 10 11, Cr 20 21.
    const uint8_t y[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    const uint8_t u[2] = {10, 11}, v[2] = {20, 21};
    VideoPicture p;
    planar_to_nv12(y, 4, u, v, 2, 1, 1, 4, 2, &p);
    const std::vector<uint8_t> want{0, 1, 2, 3, 4, 5, 6, 7, 10, 20, 11, 21};
    CHECK(p.width == 4 && p.height == 2);
    CHECK(p.nv12 == want);
    // 4:4:4 with padded strides: chroma averaged over 2x2 (rounded).
    const uint8_t y4[2 * 6] = {1, 2, 3, 4, 99, 99, 5, 6, 7, 8, 99, 99};
    const uint8_t u4[2 * 6] = {10, 11, 20, 20, 99, 99, 12, 12, 30, 31, 99, 99};
    const uint8_t v4[2 * 6] = {0, 0, 255, 255, 99, 99, 1, 0, 255, 254, 99, 99};
    planar_to_nv12(y4, 6, u4, v4, 6, 0, 0, 4, 2, &p);
    const std::vector<uint8_t> want4{1, 2, 3, 4, 5, 6, 7, 8, 11, 0, 25, 255};
    CHECK(p.nv12 == want4);
    // Odd sizes are cropped to even ones.
    planar_to_nv12(y4, 6, u4, v4, 6, 0, 0, 5, 3, &p);
    CHECK(p.width == 4 && p.height == 2 && p.nv12.size() == 12);
}

void test_nv12_copy() {
    const uint8_t y[2 * 5] = {1, 2, 3, 4, 0, 5, 6, 7, 8, 0};
    const uint8_t uv[5] = {9, 10, 11, 12, 0};
    VideoPicture p;
    nv12_copy(y, 5, uv, 5, 4, 2, &p);
    const std::vector<uint8_t> want{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    CHECK(p.nv12 == want);
}

// YUV_420_888 as AImageReader hands it out: the same 4x2 picture (luma
// 0..7, Cb 10 11, Cr 20 21) in each layout a decoder may choose.
void test_flexible() {
    const std::vector<uint8_t> want{0, 1, 2, 3, 4, 5, 6, 7, 10, 20, 11, 21};
    VideoPicture p;
    // Planar (I420), tight.
    {
        const uint8_t y[8] = {0, 1, 2, 3, 4, 5, 6, 7};
        const uint8_t u[2] = {10, 11}, v[2] = {20, 21};
        yuv420_flexible_to_nv12(y, 4, u, v, 2, 1, 4, 2, &p);
        CHECK(p.width == 4 && p.height == 2);
        CHECK(p.nv12 == want);
    }
    // Planar with padded rows (row stride 8 for luma, 4 for chroma).
    {
        const uint8_t y[16] = {0, 1, 2, 3, 99, 99, 99, 99, 4, 5, 6, 7, 99, 99, 99, 99};
        const uint8_t u[4] = {10, 11, 99, 99}, v[4] = {20, 21, 99, 99};
        yuv420_flexible_to_nv12(y, 8, u, v, 4, 1, 4, 2, &p);
        CHECK(p.nv12 == want);
    }
    // Semi-planar NV12 (Cb first, pixel stride 2), padded rows: the fast path.
    {
        const uint8_t y[12] = {0, 1, 2, 3, 99, 99, 4, 5, 6, 7, 99, 99};
        const uint8_t uv[6] = {10, 20, 11, 21, 99, 99};
        yuv420_flexible_to_nv12(y, 6, uv, uv + 1, 6, 2, 4, 2, &p);
        CHECK(p.nv12 == want);
    }
    // Semi-planar NV21 (Cr first): Cb and Cr swap places in the output.
    {
        const uint8_t y[8] = {0, 1, 2, 3, 4, 5, 6, 7};
        const uint8_t vu[4] = {20, 10, 21, 11};
        yuv420_flexible_to_nv12(y, 4, vu + 1, vu, 4, 2, 4, 2, &p);
        CHECK(p.nv12 == want);
    }
    // Chroma in separate buffers with pixel stride 2 (a gap byte between
    // samples, not interleaved with the other plane).
    {
        const uint8_t y[8] = {0, 1, 2, 3, 4, 5, 6, 7};
        const uint8_t u[4] = {10, 99, 11, 99}, v[4] = {20, 99, 21, 99};
        yuv420_flexible_to_nv12(y, 4, u, v, 4, 2, 4, 2, &p);
        CHECK(p.nv12 == want);
    }
    // Crop: the caller offsets the planes to an even origin (2,2) of a 6x4
    // picture; odd crop sizes round down.
    {
        uint8_t y[6 * 4], u[3 * 2], v[3 * 2];
        for (int i = 0; i < 24; ++i)
            y[i] = uint8_t(i);
        for (int i = 0; i < 6; ++i) {
            u[i] = uint8_t(100 + i);
            v[i] = uint8_t(200 + i);
        }
        yuv420_flexible_to_nv12(y + 2 * 6 + 2, 6, u + 1 * 3 + 1, v + 1 * 3 + 1, 3, 1, 3, 2, &p);
        const std::vector<uint8_t> crop{14, 15, 20, 21, 104, 204};
        CHECK(p.width == 2 && p.height == 2);
        CHECK(p.nv12 == crop);
    }
}

// One flat 2x2 RGB(X) picture converted; returns {Y, Cb, Cr}.
std::vector<int> convert_flat(uint8_t r, uint8_t g, uint8_t b, bool bgr, YuvMatrix m, bool full) {
    std::vector<uint8_t> px;
    for (int i = 0; i < 4; ++i) {
        px.insert(px.end(), {bgr ? b : r, g, bgr ? r : b, 0});
    }
    VideoPicture p;
    rgb32_to_nv12(px.data(), 8, bgr, m, full, 2, 2, &p);
    CHECK(p.nv12.size() == 6);
    CHECK(p.nv12[0] == p.nv12[3]);
    return {p.nv12[0], p.nv12[4], p.nv12[5]};
}

void test_rgb() {
    CHECK((convert_flat(255, 255, 255, false, YuvMatrix::Bt601, false) == std::vector<int>{235, 128, 128}));
    CHECK((convert_flat(0, 0, 0, false, YuvMatrix::Bt709, false) == std::vector<int>{16, 128, 128}));
    CHECK((convert_flat(255, 255, 255, true, YuvMatrix::Bt709, true) == std::vector<int>{255, 128, 128}));
    // Pure red, the textbook values (BT.601: 81/90/240, BT.709: 63/102/240).
    CHECK((convert_flat(255, 0, 0, false, YuvMatrix::Bt601, false) == std::vector<int>{81, 90, 240}));
    CHECK((convert_flat(255, 0, 0, false, YuvMatrix::Bt709, false) == std::vector<int>{63, 102, 240}));
    CHECK((convert_flat(255, 0, 0, true, YuvMatrix::Bt709, false) == std::vector<int>{63, 102, 240}));
    // Blue in BGRX order.
    CHECK((convert_flat(0, 0, 255, true, YuvMatrix::Bt601, false) == std::vector<int>{41, 240, 110}));
}

void test_keyframes() {
    // H.264 in AVCC form: IDR slice (type 5) or not.
    const uint8_t idr[] = {0, 0, 0, 2, 0x67, 0x42, 0, 0, 0, 2, 0x65, 0x88};
    const uint8_t non_idr[] = {0, 0, 0, 2, 0x41, 0x9a};
    const uint8_t broken[] = {0, 0, 0, 9, 0x65};
    CHECK(is_keyframe(VideoCodec::H264, idr, sizeof idr));
    CHECK(!is_keyframe(VideoCodec::H264, non_idr, sizeof non_idr));
    CHECK(!is_keyframe(VideoCodec::H264, broken, sizeof broken));
    const uint8_t idr2[] = {0, 2, 0x65, 0x88};
    CHECK(is_keyframe(VideoCodec::H264, idr2, sizeof idr2, 2));
    CHECK(avcc_nal_length({1, 0x64, 0, 0x1f, 0xff}) == 4);
    CHECK(avcc_nal_length({1, 0x64, 0, 0x1f, 0xfd}) == 2);
    // VP9: invalid frame marker.
    const uint8_t bad[] = {0x00};
    CHECK(!is_keyframe(VideoCodec::Vp9, bad, 1));
    CHECK(vp9_profile(bad, 1) == -1);

    std::vector<uint8_t> bytes;
    lgp::hle::WebmFile file;
    CHECK(g_video_path != nullptr && lgp_test::video_pattern::load(g_video_path, &bytes, &file));
    size_t frames = 0, keys = 0;
    for (const auto& f : file.frames) {
        const bool key = is_keyframe(VideoCodec::Vp9, bytes.data() + f.offset, f.size);
        CHECK(key == f.keyframe);  // the container's flag agrees
        CHECK(vp9_profile(bytes.data() + f.offset, f.size) == 0);
        keys += key;
        if (frames++ == 0)
            CHECK(key);
    }
    CHECK(frames == lgp_test::video_pattern::kFrames);
    CHECK(keys == 1);
}

}  // namespace

int main(int argc, char** argv) {
    g_video_path = argc > 1 ? argv[1] : nullptr;
    RUN(test_planar);
    RUN(test_nv12_copy);
    RUN(test_flexible);
    RUN(test_rgb);
    RUN(test_keyframes);
    return finish();
}
