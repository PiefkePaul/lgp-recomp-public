// P5: WebM demuxer of the movie:: HLE (lgp/hle/webm.hpp) against files built
// here with a small EBML writer from the Matroska specification (RFC 8794,
// RFC 9559): track list, timestamps, block kinds, all lacing kinds, clusters
// of unknown size, broken input.
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "check.hpp"
#include "lgp/hle/webm.hpp"

using lgp::hle::parse_webm;
using lgp::hle::WebmFile;
using Bytes = std::vector<uint8_t>;

namespace {

Bytes id_bytes(uint32_t id) {
    Bytes b;
    for (int s = 24; s >= 0; s -= 8)
        if ((id >> s) != 0 || s == 0)
            b.push_back(uint8_t(id >> s));
    return b;
}

// Size as an 8-byte vint (always valid), or the 1-byte "unknown" marker.
Bytes size_bytes(uint64_t n, bool unknown = false) {
    if (unknown)
        return {0xFF};
    Bytes b = {0x01};
    for (int s = 48; s >= 0; s -= 8)
        b.push_back(uint8_t(n >> s));
    return b;
}

Bytes el(uint32_t id, const Bytes& payload, bool unknown_size = false) {
    Bytes b = id_bytes(id);
    const Bytes s = size_bytes(payload.size(), unknown_size);
    b.insert(b.end(), s.begin(), s.end());
    b.insert(b.end(), payload.begin(), payload.end());
    return b;
}
Bytes uint_el(uint32_t id, uint64_t v) {
    Bytes p;
    for (int s = 56; s >= 0; s -= 8)
        if ((v >> s) != 0 || s == 0 || !p.empty())
            p.push_back(uint8_t(v >> s));
    return el(id, p);
}
Bytes float_el(uint32_t id, double v) {
    uint64_t bits;
    std::memcpy(&bits, &v, 8);
    Bytes p;
    for (int s = 56; s >= 0; s -= 8)
        p.push_back(uint8_t(bits >> s));
    return el(id, p);
}
Bytes str_el(uint32_t id, const std::string& s) { return el(id, Bytes(s.begin(), s.end())); }
Bytes cat(std::initializer_list<Bytes> parts) {
    Bytes b;
    for (const Bytes& p : parts)
        b.insert(b.end(), p.begin(), p.end());
    return b;
}
Bytes fill(size_t n, uint8_t v) { return Bytes(n, v); }

// Block payload: track (1-byte vint), relative timestamp, flags, then data.
Bytes block(uint8_t track, int16_t rel, uint8_t flags, const Bytes& data) {
    Bytes b = {uint8_t(0x80 | track), uint8_t(uint16_t(rel) >> 8), uint8_t(rel), flags};
    b.insert(b.end(), data.begin(), data.end());
    return b;
}

const Bytes kEbmlHeader = el(0x1A45DFA3, cat({uint_el(0x4286, 1), str_el(0x4282, "webm"), uint_el(0x4287, 4)}));

Bytes tracks() {
    const Bytes video = el(0xAE, cat({uint_el(0xD7, 1), uint_el(0x83, 1), str_el(0x86, "V_VP9"),
                                      el(0xE0, cat({uint_el(0xB0, 640), uint_el(0xBA, 360)}))}));
    const Bytes audio = el(0xAE, cat({uint_el(0xD7, 2), uint_el(0x83, 2), str_el(0x86, "A_VORBIS"),
                                      el(0x63A2, Bytes{1, 2, 3}), uint_el(0x23E383, 20000000),
                                      el(0xE1, cat({float_el(0xB5, 48000.0), uint_el(0x9F, 2)}))}));
    return el(0x1654AE6B, cat({video, audio}));
}

struct Source {
    Bytes data;
    lgp::hle::WebmRead read() const {
        return [this](uint64_t off, void* dst, size_t len) {
            if (off > data.size() || len > data.size() - off)
                return false;
            std::memcpy(dst, data.data() + off, len);
            return true;
        };
    }
};

void test_tracks_and_blocks() {
    // Info with a 1 ms scale (default) and 2.5 s; a void element; two clusters.
    const Bytes info = el(0x1549A966, cat({uint_el(0x2AD7B1, 1000000), float_el(0x4489, 2500.0)}));
    const Bytes c1 = el(0x1F43B675, cat({uint_el(0xE7, 0), el(0xA3, block(1, 0, 0x80, fill(100, 0xA1))),
                                         el(0xA3, block(1, 33, 0x00, fill(7, 0xA2))),
                                         el(0xA0, cat({el(0xA1, block(1, 66, 0, fill(5, 0xA3)))}))}));
    const Bytes c2 = el(0x1F43B675, cat({uint_el(0xE7, 1000), el(0xA3, block(1, -1, 0x80, fill(3, 0xA4)))}));
    Source src{cat({kEbmlHeader, el(0x18538067, cat({el(0xEC, fill(4, 0)), info, tracks(), c1, c2}))})};
    WebmFile f;
    std::string error;
    CHECK(parse_webm(src.read(), src.data.size(), &f, &error));
    CHECK(error.empty());
    CHECK_EQ(f.timestamp_scale_ns, 1000000u);
    CHECK_EQ(f.duration_ns, 2500000000);
    CHECK_EQ(f.tracks.size(), 2u);
    if (f.tracks.size() == 2) {
        CHECK(f.tracks[0].codec_id == "V_VP9");
        CHECK_EQ(f.tracks[0].type, 1u);
        CHECK_EQ(f.tracks[0].width, 640u);
        CHECK_EQ(f.tracks[0].height, 360u);
        CHECK(f.tracks[1].codec_id == "A_VORBIS");
        CHECK(f.tracks[1].codec_private == (Bytes{1, 2, 3}));
        CHECK(f.tracks[1].sampling_frequency == 48000.0);
        CHECK_EQ(f.tracks[1].channels, 2u);
        CHECK_EQ(f.tracks[1].default_duration_ns, 20000000u);
    }
    CHECK(f.track(2) != nullptr && f.track(3) == nullptr);
    CHECK_EQ(f.frames.size(), 4u);
    if (f.frames.size() == 4) {
        const int64_t pts[] = {0, 33000000, 66000000, 999000000};
        const uint32_t sizes[] = {100, 7, 5, 3};
        const uint8_t first[] = {0xA1, 0xA2, 0xA3, 0xA4};
        const bool key[] = {true, false, false, true};
        for (size_t i = 0; i < 4; ++i) {
            CHECK_EQ(f.frames[i].track, 1u);
            CHECK_EQ(f.frames[i].pts_ns, pts[i]);
            CHECK_EQ(f.frames[i].size, sizes[i]);
            CHECK_EQ(f.frames[i].keyframe, key[i]);
            CHECK_EQ(src.data[f.frames[i].offset], first[i]);
            CHECK_EQ(src.data[f.frames[i].offset + f.frames[i].size - 1], first[i]);
        }
    }
}

void test_lacing_and_unknown_sizes() {
    // Xiph: 3 frames of 300, 2, rest(4); EBML: 3 frames of 10, 10+(-3)=7, rest(1);
    // fixed: 2 frames of 6. Audio track 2 has a 20 ms default duration.
    Bytes xiph = {2, 255, 45, 2};  // count-1, 300 = 255 + 45, 2
    xiph.insert(xiph.end(), 300, 0x11);
    xiph.insert(xiph.end(), 2, 0x12);
    xiph.insert(xiph.end(), 4, 0x13);
    // EBML lacing: first size 10 (1-byte vint 0x8A), difference -3 as a
    // 1-byte signed vint: raw = -3 + 63 = 60 -> 0x80 | 60.
    Bytes ebml = {2, 0x8A, uint8_t(0x80 | 60)};
    ebml.insert(ebml.end(), 10, 0x21);
    ebml.insert(ebml.end(), 7, 0x22);
    ebml.insert(ebml.end(), 1, 0x23);
    Bytes fixed = {1};
    fixed.insert(fixed.end(), 6, 0x31);
    fixed.insert(fixed.end(), 6, 0x32);
    // A cluster of unknown size ends where the next cluster starts; the
    // segment has unknown size too (live-style file).
    const Bytes c1 = el(0x1F43B675,
                        cat({uint_el(0xE7, 10), el(0xA3, block(2, 0, 0x80 | 0x02, xiph)),
                             el(0xA3, block(2, 100, 0x80 | 0x06, ebml))}),
                        true);
    const Bytes c2 = el(0x1F43B675, cat({uint_el(0xE7, 500), el(0xA3, block(2, 0, 0x80 | 0x04, fixed))}));
    Source src{cat({kEbmlHeader, el(0x18538067, cat({tracks(), c1, c2}), true)})};
    WebmFile f;
    std::string error;
    CHECK(parse_webm(src.read(), src.data.size(), &f, &error));
    CHECK_EQ(f.duration_ns, 0);
    CHECK_EQ(f.frames.size(), 8u);
    if (f.frames.size() == 8) {
        const uint32_t sizes[] = {300, 2, 4, 10, 7, 1, 6, 6};
        const uint8_t first[] = {0x11, 0x12, 0x13, 0x21, 0x22, 0x23, 0x31, 0x32};
        const int64_t pts[] = {10, 30, 50, 110, 130, 150, 500, 520};  // ms; + i * default duration
        for (size_t i = 0; i < 8; ++i) {
            CHECK_EQ(f.frames[i].track, 2u);
            CHECK_EQ(f.frames[i].size, sizes[i]);
            CHECK_EQ(f.frames[i].pts_ns, pts[i] * 1000000);
            CHECK_EQ(src.data[f.frames[i].offset], first[i]);
            CHECK_EQ(src.data[f.frames[i].offset + f.frames[i].size - 1], first[i]);
        }
    }
}

void test_rejects() {
    WebmFile f;
    std::string error;
    Source empty{};
    CHECK(!parse_webm(empty.read(), 0, &f, &error));
    CHECK(!error.empty());
    Source mkv{cat({el(0x1A45DFA3, str_el(0x4282, "matroska")), el(0x18538067, tracks())})};
    CHECK(parse_webm(mkv.read(), mkv.data.size(), &f, &error));  // Matroska is fine
    Source other{cat({el(0x1A45DFA3, str_el(0x4282, "other")), el(0x18538067, tracks())})};
    CHECK(!parse_webm(other.read(), other.data.size(), &f, &error));
    CHECK(error.find("doctype") != std::string::npos);
    Source no_tracks{cat({kEbmlHeader, el(0x18538067, el(0x1549A966, uint_el(0x2AD7B1, 1000000)))})};
    CHECK(!parse_webm(no_tracks.read(), no_tracks.data.size(), &f, &error));
    // A lace that claims more bytes than the block holds.
    Bytes bad = {1, 200};
    bad.insert(bad.end(), 10, 0);
    Source broken{cat({kEbmlHeader, el(0x18538067, cat({tracks(), el(0x1F43B675, el(0xA3, block(2, 0, 0x02, bad)))}))})};
    CHECK(!parse_webm(broken.read(), broken.data.size(), &f, &error));
    // A truncated file keeps the frames read so far.
    Source cut{cat({kEbmlHeader, el(0x18538067, cat({tracks(), el(0x1F43B675, el(0xA3, block(1, 0, 0x80, fill(9, 1))))}),
                                     true)})};
    cut.data.insert(cut.data.end(), {0x1F, 0x43});
    CHECK(parse_webm(cut.read(), cut.data.size(), &f, &error));
    CHECK_EQ(f.frames.size(), 1u);
}

// WebmIndexer: open() stops at the first Cluster and reads nothing beyond its
// header; next() then adds at most one block at a time and ends with the same
// frames as parse_webm.
void test_incremental() {
    const Bytes info = el(0x1549A966, cat({uint_el(0x2AD7B1, 1000000), float_el(0x4489, 2500.0)}));
    const Bytes c1 = el(0x1F43B675, cat({uint_el(0xE7, 0), el(0xA3, block(1, 0, 0x80, fill(100, 0xA1))),
                                         el(0xA3, block(2, 5, 0x80, fill(7, 0xA2)))}));
    const Bytes c2 = el(0x1F43B675, cat({uint_el(0xE7, 1000), el(0xA3, block(1, 0, 0x80, fill(3, 0xA4)))}));
    Source src{cat({kEbmlHeader, el(0x18538067, cat({info, tracks(), c1, c2}))})};
    const uint64_t first_cluster = src.data.size() - c1.size() - c2.size();
    uint64_t furthest = 0;
    const lgp::hle::WebmRead counted = [&](uint64_t off, void* dst, size_t len) {
        furthest = std::max<uint64_t>(furthest, off + len);
        return src.read()(off, dst, len);
    };
    WebmFile f;
    std::string error;
    lgp::hle::WebmIndexer index;
    CHECK(index.open(counted, src.data.size(), &f, &error));
    CHECK(!index.done());
    CHECK_EQ(f.tracks.size(), 2u);
    CHECK_EQ(f.duration_ns, 2500000000);  // known before any frame
    CHECK(f.frames.empty());
    CHECK(furthest <= first_cluster + 12);  // the Cluster's id (4) and size (8), no payload
    size_t steps = 0, before = 0;
    while (!index.done()) {
        CHECK(index.next(&error));
        CHECK(f.frames.size() - before <= 1);
        before = f.frames.size();
        ++steps;
    }
    CHECK(!index.next(&error));  // nothing left
    CHECK(steps >= 5);  // a timestamp and a block per cluster, the ends
    WebmFile whole;
    CHECK(parse_webm(src.read(), src.data.size(), &whole, &error));
    CHECK_EQ(f.frames.size(), whole.frames.size());
    for (size_t i = 0; i < f.frames.size() && i < whole.frames.size(); ++i) {
        CHECK_EQ(f.frames[i].offset, whole.frames[i].offset);
        CHECK_EQ(f.frames[i].pts_ns, whole.frames[i].pts_ns);
        CHECK_EQ(f.frames[i].track, whole.frames[i].track);
    }
    // A broken block stops the indexer with an error; the frames before stay.
    Bytes bad = {1, 200};
    bad.insert(bad.end(), 10, 0);
    Source broken{cat({kEbmlHeader, el(0x18538067, cat({tracks(), el(0x1F43B675, cat({el(0xA3, block(1, 0, 0x80, fill(4, 1))),
                                                                                       el(0xA3, block(2, 0, 0x02, bad))}))}))})};
    CHECK(index.open(broken.read(), broken.data.size(), &f, &error));
    bool failed = false;
    while (!index.done())
        if (!index.next(&error))
            failed = true;
    CHECK(failed);
    CHECK(!error.empty());
    CHECK_EQ(f.frames.size(), 1u);
}

}  // namespace

int main() {
    RUN(test_tracks_and_blocks);
    RUN(test_lacing_and_unknown_sizes);
    RUN(test_rejects);
    RUN(test_incremental);
    return finish();
}
