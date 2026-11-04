// The test video tests/platform/data/vp9_pattern.webm (written by
// tools/video/gen_test_webm.py, which holds the same formulas): 64x48, 30
// frames at 30 fps, VP9 with hidden alt-ref frames. Checks a decoded NV12
// picture at the centres of its flat areas.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "lgp/hle/webm.hpp"
#include "lgp/platform/video.hpp"

namespace lgp_test::video_pattern {

constexpr uint32_t kWidth = 64, kHeight = 48, kFrames = 30;
constexpr int64_t kFrameUs = 33333;  // 30 fps; WebM stores milliseconds

// Inside the RGB gamut, so RGB round trips can reproduce them.
inline int luma(int frame, int qx, int qy) { return 70 + 20 * (qy * 2 + qx) + 3 * frame; }
inline int cb(int frame, bool right) { return right ? 137 - frame % 5 : 119 + frame % 5; }
inline int cr(int frame, bool bottom) { return bottom ? 130 + frame % 5 : 122; }

// Largest difference to the pattern of `frame` at the sample points; 1000
// for a picture of the wrong size.
inline int max_error(const lgp::platform::VideoPicture& p, int frame) {
    if (p.width != kWidth || p.height != kHeight || p.nv12.size() != size_t{kWidth} * kHeight * 3 / 2)
        return 1000;
    const uint8_t* y = p.nv12.data();
    const uint8_t* uv = y + kWidth * kHeight;
    int e = 0;
    for (int qx = 0; qx < 2; ++qx)
        for (int qy = 0; qy < 2; ++qy)  // rows 8 and 38 avoid the noise band (rows 20-27)
            e = std::max(e, std::abs(y[(8 + 30 * qy) * kWidth + 16 + 32 * qx] - luma(frame, qx, qy)));
    for (int r = 0; r < 2; ++r) {
        // NV12 chroma row 4 (top) / 20 (bottom), column 8 (left) / 24 (right), Cb then Cr.
        e = std::max(e, std::abs(uv[4 * kWidth + 2 * (8 + 16 * r)] - cb(frame, r != 0)));
        e = std::max(e, std::abs(uv[(4 + 16 * r) * kWidth + 2 * 8 + 1] - cr(frame, r != 0)));
    }
    return e;
}

// The file's bytes and its demuxed frame list; false (message printed) if
// it cannot be read.
inline bool load(const char* path, std::vector<uint8_t>* bytes, lgp::hle::WebmFile* file) {
    std::FILE* f = std::fopen(path, "rb");
    if (f == nullptr) {
        std::printf("video pattern: cannot open %s\n", path);
        return false;
    }
    uint8_t buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
        bytes->insert(bytes->end(), buf, buf + n);
    std::fclose(f);
    std::string error;
    const auto read = [bytes](uint64_t off, void* dst, size_t len) {
        if (off > bytes->size() || len > bytes->size() - off)
            return false;
        std::memcpy(dst, bytes->data() + off, len);
        return true;
    };
    if (!lgp::hle::parse_webm(read, bytes->size(), file, &error)) {
        std::printf("video pattern: %s: %s\n", path, error.c_str());
        return false;
    }
    return true;
}

// Feeds every frame of the video track to `dec` like the movie:: HLE does
// (one decode() per frame, then the end of stream) and checks the pictures:
// all frames, in order, at the pattern's timestamps, within `tolerance`.
// Returns the number of problems (each printed).
inline int decode_and_check(lgp::platform::IVideoDecoder& dec, const std::vector<uint8_t>& bytes,
                            const lgp::hle::WebmFile& file, int tolerance) {
    int problems = 0;
    std::vector<lgp::platform::VideoPicture> pics;
    size_t frames = 0, after_first_call = 0;
    for (const lgp::hle::WebmFrame& fr : file.frames) {
        const lgp::hle::WebmTrack* t = file.track(fr.track);
        if (t == nullptr || t->type != 1)
            continue;
        if (!dec.decode(bytes.data() + fr.offset, fr.size, fr.pts_ns / 1000, &pics)) {
            std::printf("  decode() failed at frame %zu\n", frames);
            ++problems;
        }
        if (++frames == 1)
            after_first_call = pics.size();
    }
    if (!dec.decode(nullptr, 0, 0, &pics)) {
        std::printf("  end of stream failed\n");
        ++problems;
    }
    std::printf("  %zu frames in, %zu pictures out (%zu after the first frame)\n", frames, pics.size(),
                after_first_call);
    if (pics.size() != kFrames) {
        std::printf("  expected %u pictures\n", kFrames);
        ++problems;
    }
    int worst = 0;
    for (size_t i = 0; i < pics.size() && i < kFrames; ++i) {
        const int e = max_error(pics[i], int(i));
        worst = std::max(worst, e);
        const int64_t want = int64_t(i) * kFrameUs / 1000 * 1000;  // millisecond timestamps
        if (e > tolerance || std::llabs(pics[i].pts_us - want) > 1000) {
            if (problems < 8)
                std::printf("  picture %zu: %ux%u pts %lld us (want %lld), error %d\n", i, pics[i].width,
                            pics[i].height, static_cast<long long>(pics[i].pts_us), static_cast<long long>(want), e);
            ++problems;
        }
    }
    std::printf("  largest pixel error %d (tolerance %d)\n", worst, tolerance);
    return problems;
}

}  // namespace lgp_test::video_pattern
