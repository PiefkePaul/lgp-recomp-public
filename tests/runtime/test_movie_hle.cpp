// P5: the movie:: HLE (runtime/boot/src/movie_hle.cpp) driven like the game's
// player drives it (docs/knowledge/movie-api-hle.md): Extractor on the
// self-made test video (argv[1], tests/platform/data/av_pattern.webm: VP9 +
// Vorbis), one Decoder per selected track, samples routed by
// GetSampleTrackIndex, outputs drained after every input. No video backend
// (black pictures of the right size); the sound track only when enabled,
// decoded to 16-bit PCM when libvorbis is built (LGP_TEST_VORBIS). One run
// plays like LGP's player with a hardware decoder: pictures come some frames
// after their input, and no sound is taken before the first picture.
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../../runtime/boot/src/movie_hle.hpp"
#include "ipc_client.hpp"
#include "lgp/platform/memory_storage.hpp"
#include "lgp/platform/video.hpp"

using namespace lgp_test;
using lgp::boot::MovieHle;
using lgp::CpuState;
using lgp::LiftedEntry;
using lgp::LiftedFn;

namespace {

const char* g_path = nullptr;

constexpr uint64_t kArea = 0x0900'0000, kAreaSize = 0x40000;
constexpr uint64_t kData = kArea;               // vtables + strings handed to the game
constexpr uint64_t kObjects = kArea + 0x1000;   // game objects (Extractor, Decoders, Buffers, MediaData)
constexpr uint64_t kStrings = kArea + 0x2000;   // our C strings
constexpr uint64_t kSample = kArea + 0x10000;   // ReadSampleData target (game memory)
constexpr uint64_t kOut = kArea + 0x20000;      // GetOutputBuffer target
constexpr uint64_t kOutSize = 0x20000;
constexpr uint64_t kVthunks = 0x0A00'0000;

struct Harness {
    Env env{[](ServiceManager&) {}};
    std::vector<LiftedEntry> host;
    std::map<uint64_t, LiftedFn> methods;
    uint64_t next_string = kStrings;

    explicit Harness(lgp::platform::IStorage* game, bool audio, lgp::platform::IVideo* video = nullptr) {
        CHECK(env.k.map_owned(kArea, kAreaSize, MemState::CodeData, kPermRW).ok());
        MovieHle::instance().reset(env.k, kData, kVthunks, &host);
        MovieHle::instance().configure(game, video, audio);
        for (const LiftedEntry& e : host)
            methods[e.va] = e.fn;
    }
    uint64_t str(const std::string& s) {
        const uint64_t va = next_string;
        CHECK(env.k.memory().copy_to_user(va, s.c_str(), s.size() + 1));
        next_string += (s.size() + 16) & ~uint64_t{7};
        return va;
    }
    std::string read_str(uint64_t va) {
        std::string s;
        char c;
        while (s.size() < 256 && env.k.memory().copy_from_user(&c, va + s.size(), 1) && c != 0)
            s.push_back(c);
        return s;
    }
    // A plain movie:: function by its mangled name.
    uint64_t call(const std::string& name, std::initializer_list<uint64_t> args) {
        CpuState ctx{};
        size_t i = 0;
        for (uint64_t a : args)
            ctx.x[i++] = a;
        auto h = MovieHle::instance().handler(name);
        CHECK(static_cast<bool>(h));
        if (h)
            h(&ctx);
        return ctx.x[0];
    }
    // Virtual method `slot` of the object at `self` (through its vtable).
    uint64_t vcall(uint64_t self, size_t slot, std::initializer_list<uint64_t> args) {
        uint64_t vtable = 0, key = 0;
        CHECK(env.k.memory().copy_from_user(&vtable, self, 8));
        CHECK(env.k.memory().copy_from_user(&key, vtable + 8 * slot, 8));
        CpuState ctx{};
        ctx.x[0] = self;
        size_t i = 1;
        for (uint64_t a : args)
            ctx.x[i++] = a;
        auto it = methods.find(key);
        CHECK(it != methods.end());
        if (it != methods.end())
            it->second(&ctx);
        return ctx.x[0];
    }
    template <typename T>
    T get(uint64_t va) {
        T v{};
        CHECK(env.k.memory().copy_from_user(&v, va, sizeof v));
        return v;
    }
    std::string find_string(uint64_t media, const char* key) {
        const uint64_t out = kObjects + 0xF00;
        if (call("_ZNK5movie9MediaData10FindStringEPKcPS2_", {media, str(key), out}) == 0)
            return "";
        return read_str(get<uint64_t>(out));
    }
    int64_t find_int(uint64_t media, const char* key) {
        const uint64_t out = kObjects + 0xF00;
        if (call("_ZNK5movie9MediaData9FindInt64EPKcPl", {media, str(key), out}) == 0)
            return -1;
        return get<int64_t>(out);
    }
};

std::vector<uint8_t> read_file(const char* path) {
    std::vector<uint8_t> bytes;
    if (std::FILE* f = path != nullptr ? std::fopen(path, "rb") : nullptr) {
        uint8_t buf[4096];
        for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;)
            bytes.insert(bytes.end(), buf, buf + n);
        std::fclose(f);
    }
    return bytes;
}

// A decoder with the latency of a hardware one: each picture comes out
// kDelay inputs after its frame went in (black, the container's size).
class DelayedVideo final : public lgp::platform::IVideo {
public:
    static constexpr size_t kDelay = 6;
    std::unique_ptr<lgp::platform::IVideoDecoder> open_decoder(const lgp::platform::VideoStreamInfo& info) override {
        return std::make_unique<Decoder>(info);
    }

private:
    class Decoder final : public lgp::platform::IVideoDecoder {
    public:
        explicit Decoder(const lgp::platform::VideoStreamInfo& info) : info_(info) {}
        bool decode(const uint8_t* data, size_t, int64_t pts_us, std::vector<lgp::platform::VideoPicture>* out) override {
            if (data != nullptr)
                waiting_.push_back(pts_us);
            while (!waiting_.empty() && (data == nullptr || waiting_.size() > kDelay)) {
                lgp::platform::VideoPicture pic;
                pic.width = info_.width;
                pic.height = info_.height;
                pic.pts_us = waiting_.front();
                pic.nv12.assign(size_t(info_.width) * info_.height * 3 / 2, 0);
                out->push_back(std::move(pic));
                waiting_.erase(waiting_.begin());
            }
            return true;
        }

    private:
        lgp::platform::VideoStreamInfo info_;
        std::vector<int64_t> waiting_;
    };
};

struct Result {
    int tracks = 0;
    bool stuck = false;  // a decoder had no input buffer while the player waited for it
    std::vector<std::string> mimes;
    size_t pictures = 0, pcm_bytes = 0, ends = 0;
    int64_t rate = 0, channels = 0, width = 0;
};

// sound_after_picture: the player leaves sound outputs alone until it has
// a picture (LGP starts its sound output with the first picture).
Result play(bool audio, lgp::platform::IVideo* video = nullptr, bool sound_after_picture = false) {
    lgp::platform::MemoryStorage game(true);
    game.add_file("/romfs/movie/av.webm", read_file(g_path));
    Harness h(&game, audio, video);
    Result r;
    const uint64_t ext = kObjects, media = kObjects + 0x100, buf = kObjects + 0x200;
    h.call("_ZN5movie9ExtractorC1ENS_13ContainerTypeE", {ext, 3});
    CHECK_EQ(h.vcall(ext, 15, {0, h.str("data:/movie/av.webm")}), 0u);  // SetDataSource
    CHECK_EQ(h.vcall(ext, 4, {kObjects + 0xF00}), 0u);                    // GetTrackCount
    r.tracks = h.get<int32_t>(kObjects + 0xF00);
    std::vector<uint64_t> decoders;
    for (int t = 0; t < r.tracks; ++t) {
        const uint64_t dec = kObjects + 0x400 + 0x100 * uint64_t(t);
        const uint64_t fmt = kObjects + 0x300 + 8 * uint64_t(t);
        h.call("_ZN5movie9MediaDataC1Ev", {fmt});
        CHECK_EQ(h.vcall(ext, 5, {uint64_t(t), fmt}), 0u);  // GetTrackConfiguration
        r.mimes.push_back(h.find_string(fmt, "mime"));
        CHECK_EQ(h.vcall(ext, 6, {uint64_t(t)}), 0u);  // SelectTrack
        h.call("_ZN5movie7DecoderC1ENS_11DecoderTypeENS_18DecoderOutputFormatENS_11DecoderModeEi", {dec, 3, 4, 1, 0});
        CHECK_EQ(h.vcall(dec, 2, {fmt}), 0u);  // Configure
        CHECK_EQ(h.vcall(dec, 5, {}), 0u);     // Start
        decoders.push_back(dec);
    }
    // Drains every ready output of a decoder.
    auto drain = [&](uint64_t dec) {
        if (sound_after_picture && r.pictures == 0 && dec != decoders[0])
            return;
        const uint64_t idx = kObjects + 0xF10, pts = kObjects + 0xF18, flags = kObjects + 0xF20, rest = kObjects + 0xF28;
        while (h.vcall(dec, 12, {idx, pts, flags, rest}) == 0) {  // DequeueOutputBuffer
            const int32_t i = h.get<int32_t>(idx);
            if (h.get<uint32_t>(flags) & 4) {
                ++r.ends;
            } else {
                const uint64_t of = kObjects + 0x380;
                h.call("_ZN5movie9MediaDataC1Ev", {of});
                h.vcall(dec, 18, {uint64_t(i), of});  // GetOutputBufferFormat
                h.call("_ZN5movie6BufferC1EPvm", {buf, kOut, kOutSize});
                CHECK_EQ(h.vcall(dec, 13, {uint64_t(i), buf}), 0u);  // GetOutputBuffer
                const uint64_t size = h.call("_ZNK5movie6Buffer4SizeEv", {buf});
                if (h.find_string(of, "mime") == "audio/raw") {
                    r.pcm_bytes += size;
                    r.rate = h.find_int(of, "sample-rate");
                    r.channels = h.find_int(of, "channel-count");
                } else {
                    ++r.pictures;
                    r.width = h.find_int(of, "width");
                    CHECK_EQ(size, uint64_t(r.width) * h.find_int(of, "height") * 3 / 2);
                }
            }
            CHECK_EQ(h.vcall(dec, 15, {uint64_t(i)}), 0u);  // ReleaseOutputBuffer
        }
    };
    // Feeds one sample to its track's decoder; false at the end of the file.
    const uint64_t idx = kObjects + 0xF30, rest = kObjects + 0xF38, track = kObjects + 0xF40, time = kObjects + 0xF48;
    auto feed = [&]() {
        if (h.vcall(ext, 9, {track}) != 0)  // GetSampleTrackIndex: 0x2000 at the end
            return false;
        const uint64_t dec = decoders[size_t(h.get<int64_t>(track))];
        if (h.vcall(dec, 9, {idx, rest}) != 0) {  // DequeueInputBuffer: none free
            drain(dec);
            if (h.vcall(dec, 9, {idx, rest}) != 0) {
                r.stuck = true;  // the game would wait for an input event that never comes
                return false;
            }
        }
        const uint64_t i = uint64_t(h.get<int32_t>(idx));
        h.call("_ZN5movie6BufferC1Ev", {buf});
        CHECK_EQ(h.vcall(dec, 10, {i, buf}), 0u);  // GetInputBuffer
        CHECK_EQ(h.vcall(ext, 10, {buf}), 0u);     // ReadSampleData
        const uint64_t size = h.call("_ZNK5movie6Buffer4SizeEv", {buf});
        CHECK_EQ(h.vcall(ext, 12, {time}), 0u);  // GetSampleTime
        CHECK_EQ(h.vcall(dec, 11, {i, 0, size, uint64_t(h.get<int64_t>(time)), 0}), 0u);  // QueueInputBuffer
        h.vcall(ext, 11, {});  // Advance
        drain(dec);
        return true;
    };
    size_t samples = 0;
    while (feed())
        ++samples;
    if (r.stuck) {
        std::printf("  stuck after %zu samples, %zu pictures\n", samples, r.pictures);
        return r;
    }
    // End of stream on every decoder: an empty buffer with flag 4.
    for (uint64_t dec : decoders) {
        const uint64_t i = kObjects + 0xF30;
        if (h.vcall(dec, 9, {i, rest}) != 0) {
            drain(dec);
            CHECK_EQ(h.vcall(dec, 9, {i, rest}), 0u);
        }
        CHECK_EQ(h.vcall(dec, 11, {uint64_t(h.get<int32_t>(i)), 0, 0, 0, 4}), 0u);
        drain(dec);
    }
    std::printf("  audio %s: %d tracks, %zu samples, %zu pictures, %zu PCM bytes, %zu ends\n", audio ? "on" : "off",
                r.tracks, samples, r.pictures, r.pcm_bytes, r.ends);
    (void)media;
    return r;
}

void test_video_only() {
    const Result r = play(false);
    CHECK_EQ(r.tracks, 1);
    CHECK(r.mimes.size() == 1 && r.mimes[0] == "video/x-vnd.on2.vp9");
    CHECK_EQ(r.pictures, 30u);
    CHECK_EQ(r.width, 64);
    CHECK_EQ(r.pcm_bytes, 0u);
    CHECK_EQ(r.ends, 1u);
}

void test_with_sound() {
    const Result r = play(true);
#if defined(LGP_TEST_VORBIS)
    CHECK_EQ(r.tracks, 2);
    CHECK(r.mimes.size() == 2 && r.mimes[1] == "audio/vorbis");
    CHECK_EQ(r.pictures, 30u);
    CHECK_EQ(r.rate, 48000);
    CHECK_EQ(r.channels, 2);
    // 48832 stereo frames (runtime.hle_vorbis), 16 bit.
    CHECK_EQ(r.pcm_bytes, 48832u * 4);
    CHECK_EQ(r.ends, 2u);
#else
    CHECK_EQ(r.tracks, 1);  // no decoder: the sound track stays hidden
#endif
}

// A late first picture (hardware decoder) and a player that takes no sound
// before it: the sound decoder must keep taking input, or the feeder stops at
// the next sound sample and the video never gets the frames it still needs.
void test_late_first_picture() {
    DelayedVideo video;
    const Result r = play(true, &video, true);
    CHECK(!r.stuck);
    CHECK_EQ(r.pictures, 30u);
#if defined(LGP_TEST_VORBIS)
    CHECK_EQ(r.pcm_bytes, 48832u * 4);
    CHECK_EQ(r.ends, 2u);
#endif
}

// Buffer(size_t): the library allocates (LGP's sound output buffer); the HLE
// hands out guest memory behind its data page and reuses it after the dtor.
void test_buffer_capacity() {
    Harness h(nullptr, false);
    const uint64_t b1 = kObjects + 0x800, b2 = kObjects + 0x840, b3 = kObjects + 0x880;
    h.call("_ZN5movie6BufferC1Em", {b1, 0x2000});
    const uint64_t base1 = h.call("_ZNK5movie6Buffer4BaseEv", {b1});
    CHECK(base1 >= kData + 0x1000 && base1 < kData + 0x1000 + MovieHle::kAllocPages * 0x1000);
    CHECK_EQ(h.call("_ZNK5movie6Buffer8CapacityEv", {b1}), 0x2000u);
    const uint32_t word = 0x12345678;
    CHECK(h.env.k.memory().copy_to_user(base1 + 0x2000 - 4, &word, 4));
    h.call("_ZN5movie6BufferC1Em", {b2, 0x2000});
    const uint64_t base2 = h.call("_ZNK5movie6Buffer4BaseEv", {b2});
    CHECK(base2 != 0 && (base2 >= base1 + 0x2000 || base2 + 0x2000 <= base1));
    h.call("_ZN5movie6BufferD1Ev", {b1});
    h.call("_ZN5movie6BufferC1Em", {b3, 0x2000});
    CHECK_EQ(h.call("_ZNK5movie6Buffer4BaseEv", {b3}), base1);  // reused
    // A copy keeps the storage alive after the original is destroyed.
    const uint64_t b4 = kObjects + 0x8C0, b5 = kObjects + 0x900;
    h.call("_ZN5movie6BufferC1ERKS0_", {b4, b3});
    h.call("_ZN5movie6BufferD1Ev", {b3});
    h.call("_ZN5movie6BufferC1Em", {b5, 0x2000});
    CHECK(h.call("_ZNK5movie6Buffer4BaseEv", {b5}) != base1);
    h.call("_ZN5movie6BufferD1Ev", {b4});  // last user: free again
    h.call("_ZN5movie6BufferC1Em", {b3, 0x2000});
    CHECK_EQ(h.call("_ZNK5movie6Buffer4BaseEv", {b3}), base1);
    h.call("_ZN5movie6BufferD1Ev", {b5});
    // More than the allocation pages hold: no memory, capacity 0.
    h.call("_ZN5movie6BufferC1Em", {b1, MovieHle::kAllocPages * 0x1000});
    CHECK_EQ(h.call("_ZNK5movie6Buffer4BaseEv", {b1}), 0u);
    CHECK_EQ(h.call("_ZNK5movie6Buffer8CapacityEv", {b1}), 0u);
}

}  // namespace

int main(int argc, char** argv) {
    g_path = argc > 1 ? argv[1] : nullptr;
    RUN(test_video_only);
    RUN(test_with_sound);
    RUN(test_late_first_picture);
    RUN(test_buffer_capacity);
    return finish();
}
