// P7.3: FallbackVideo (lgp/platform/fallback_video.hpp) with fake decoders:
// a primary that fails before its first picture hands the whole stream to
// the fallback; a working primary stays, also after a later error.
#include <cstring>
#include <vector>

#include "../runtime/check.hpp"
#include "lgp/platform/fallback_video.hpp"

using namespace lgp::platform;

namespace {

// Outputs one picture per frame after `delay` frames (pts of the input);
// fails at frame `fail_at` (1-based, 0 = never) or when `refuse` opens.
struct FakeVideo final : IVideo {
    int delay = 0;
    int fail_at = 0;
    bool refuse = false;
    int opened = 0;
    std::vector<int64_t> fed;  // pts of every frame a decoder of this backend got

    struct Decoder final : IVideoDecoder {
        FakeVideo* v;
        int n = 0;
        std::vector<int64_t> pending;
        explicit Decoder(FakeVideo* video) : v(video) {}
        bool decode(const uint8_t* data, size_t, int64_t pts, std::vector<VideoPicture>* out) override {
            if (data == nullptr) {
                for (int64_t p : pending) out->push_back(VideoPicture{2, 2, p, std::vector<uint8_t>(6)});
                pending.clear();
                return true;
            }
            ++n;
            v->fed.push_back(pts);
            if (v->fail_at != 0 && n == v->fail_at)
                return false;
            pending.push_back(pts);
            if (n > v->delay) {
                out->push_back(VideoPicture{2, 2, pending.front(), std::vector<uint8_t>(6)});
                pending.erase(pending.begin());
            }
            return true;
        }
    };
    std::unique_ptr<IVideoDecoder> open_decoder(const VideoStreamInfo&) override {
        if (refuse)
            return nullptr;
        ++opened;
        return std::make_unique<Decoder>(this);
    }
};

const uint8_t kFrame[4] = {1, 2, 3, 4};

void test_fails_before_first_picture() {
    FakeVideo primary, fallback;
    primary.delay = 2;
    primary.fail_at = 2;  // like MediaCodec with an output format we cannot convert
    FallbackVideo video(primary, fallback);
    auto d = video.open_decoder({});
    CHECK(d != nullptr);
    std::vector<VideoPicture> out;
    CHECK(d->decode(kFrame, 4, 0, &out));
    CHECK(d->decode(kFrame, 4, 1000, &out));  // primary fails: both frames to the fallback
    CHECK(d->decode(kFrame, 4, 2000, &out));
    CHECK(d->decode(nullptr, 0, 0, &out));
    CHECK_EQ(fallback.opened, 1);
    CHECK((fallback.fed == std::vector<int64_t>{0, 1000, 2000}));
    CHECK_EQ(out.size(), size_t{3});
    CHECK_EQ(primary.fed.size(), size_t{2});
}

void test_working_primary_stays() {
    FakeVideo primary, fallback;
    primary.delay = 1;
    primary.fail_at = 4;  // after its first picture: an ordinary error, no switch
    FallbackVideo video(primary, fallback);
    auto d = video.open_decoder({});
    std::vector<VideoPicture> out;
    CHECK(d->decode(kFrame, 4, 0, &out));
    CHECK(d->decode(kFrame, 4, 1, &out));
    CHECK_EQ(out.size(), size_t{1});
    CHECK(d->decode(kFrame, 4, 2, &out));
    CHECK(!d->decode(kFrame, 4, 3, &out));
    CHECK_EQ(fallback.opened, 0);
}

void test_primary_cannot_open() {
    FakeVideo primary, fallback;
    primary.refuse = true;
    FallbackVideo video(primary, fallback);
    auto d = video.open_decoder({});
    CHECK(d != nullptr);
    CHECK_EQ(fallback.opened, 1);
}

void test_no_picture_within_bound() {
    FakeVideo primary, fallback;
    primary.delay = 1000;  // never shows anything within the bound
    FallbackVideo video(primary, fallback);
    auto d = video.open_decoder({});
    std::vector<VideoPicture> out;
    const int n = int(FallbackVideo::kMaxKeptFrames) + 10;
    for (int i = 0; i < n; ++i)
        CHECK(d->decode(kFrame, 4, i, &out));
    CHECK_EQ(fallback.opened, 1);
    CHECK_EQ(fallback.fed.size(), size_t(n));  // from the first frame on
    CHECK_EQ(out.size(), size_t(n));
}

// A decoder that drops what it cannot output without an error: nothing by
// the end of the stream.
void test_no_picture_at_end() {
    FakeVideo primary, fallback;
    primary.delay = 1000;
    FallbackVideo video(primary, fallback);
    auto d = video.open_decoder({});
    std::vector<VideoPicture> out;
    for (int i = 0; i < 5; ++i)
        CHECK(d->decode(kFrame, 4, i, &out));
    // The fake flushes its pending frames at the end; one that dropped them
    // would give none (test_silent_to_end).
    CHECK(d->decode(nullptr, 0, 0, &out));
    CHECK_EQ(fallback.opened, 0);  // the flush showed pictures: the primary worked
    CHECK_EQ(out.size(), size_t{5});
}

// The end-of-stream call itself fails before any picture.
void test_fails_at_end() {
    struct Silent final : IVideo {
        struct D final : IVideoDecoder {
            bool decode(const uint8_t* data, size_t, int64_t, std::vector<VideoPicture>*) override {
                return data != nullptr;
            }
        };
        std::unique_ptr<IVideoDecoder> open_decoder(const VideoStreamInfo&) override {
            return std::make_unique<D>();
        }
    } primary;
    FakeVideo fallback;
    FallbackVideo video(primary, fallback);
    auto d = video.open_decoder({});
    std::vector<VideoPicture> out;
    for (int i = 0; i < 3; ++i)
        CHECK(d->decode(kFrame, 4, i * 1000, &out));
    CHECK(d->decode(nullptr, 0, 0, &out));
    CHECK_EQ(fallback.opened, 1);
    CHECK((fallback.fed == std::vector<int64_t>{0, 1000, 2000}));
    CHECK_EQ(out.size(), size_t{3});
}

// Ends without an error and without a single picture.
void test_silent_to_end() {
    struct Silent final : IVideo {
        struct D final : IVideoDecoder {
            bool decode(const uint8_t*, size_t, int64_t, std::vector<VideoPicture>*) override { return true; }
        };
        std::unique_ptr<IVideoDecoder> open_decoder(const VideoStreamInfo&) override {
            return std::make_unique<D>();
        }
    } primary;
    FakeVideo fallback;
    FallbackVideo video(primary, fallback);
    auto d = video.open_decoder({});
    std::vector<VideoPicture> out;
    for (int i = 0; i < 3; ++i)
        CHECK(d->decode(kFrame, 4, i, &out));
    CHECK(d->decode(nullptr, 0, 0, &out));
    CHECK_EQ(fallback.opened, 1);
    CHECK_EQ(out.size(), size_t{3});
}

}  // namespace

int main() {
    RUN(test_fails_before_first_picture);
    RUN(test_working_primary_stays);
    RUN(test_primary_cannot_open);
    RUN(test_no_picture_within_bound);
    RUN(test_no_picture_at_end);
    RUN(test_fails_at_end);
    RUN(test_silent_to_end);
    return finish();
}
