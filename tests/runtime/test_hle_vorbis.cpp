// P5: Vorbis sound tracks of in-game videos (lgp/hle/vorbis.hpp). Xiph
// lacing of the header packets; with libvorbis (LGP_TEST_VORBIS) the sound
// track of the self-made test video (argv[1], tests/platform/data/
// av_pattern.webm, tools/video/gen_test_webm.py --audio) decodes to its
// signal: 48 kHz stereo, 1 s, 440 Hz left and 660 Hz right at half scale.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "check.hpp"
#include "lgp/hle/vorbis.hpp"
#include "lgp/hle/webm.hpp"

using namespace lgp::hle;

namespace {

const char* g_path = nullptr;

void test_xiph_lacing() {
    std::vector<std::vector<uint8_t>> p;
    // Two laced sizes (300 = 255 + 45, 2), the last packet takes the rest (3).
    std::vector<uint8_t> cp = {2, 255, 45, 2};
    for (int i = 0; i < 300; ++i) cp.push_back(1);
    cp.insert(cp.end(), {2, 2, 3, 3, 3});
    CHECK(split_xiph_headers(cp, &p));
    CHECK(p.size() == 3 && p[0].size() == 300 && p[1].size() == 2 && p[2].size() == 3);
    CHECK(p.size() == 3 && p[2][0] == 3);
    CHECK(!split_xiph_headers({}, &p));
    CHECK(!split_xiph_headers({2, 255}, &p));      // lacing runs off the end
    CHECK(!split_xiph_headers({2, 10, 10, 1}, &p));  // sizes larger than the data
    VorbisDecoder dec;
    std::string error;
    CHECK(!dec.open({2, 1, 1, 0, 0, 0}, &error));  // not Vorbis headers
    CHECK(!error.empty());
}

void test_sound_track() {
    std::vector<uint8_t> bytes;
    if (std::FILE* f = g_path != nullptr ? std::fopen(g_path, "rb") : nullptr) {
        uint8_t buf[4096];
        for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;)
            bytes.insert(bytes.end(), buf, buf + n);
        std::fclose(f);
    }
    CHECK(!bytes.empty());
    WebmFile file;
    std::string error;
    const auto read = [&](uint64_t off, void* dst, size_t len) {
        if (off > bytes.size() || len > bytes.size() - off)
            return false;
        std::memcpy(dst, bytes.data() + off, len);
        return true;
    };
    CHECK(parse_webm(read, bytes.size(), &file, &error));
    const WebmTrack* audio = nullptr;
    for (const WebmTrack& t : file.tracks)
        if (t.type == 2)
            audio = &t;
    CHECK(audio != nullptr && audio->codec_id == "A_VORBIS");
    if (audio == nullptr)
        return;
    CHECK(audio->sampling_frequency == 48000 && audio->channels == 2);
    VorbisDecoder dec;
#if !defined(LGP_TEST_VORBIS)
    CHECK(!VorbisDecoder::available());
    CHECK(!dec.open(audio->codec_private, &error));
#else
    CHECK(VorbisDecoder::available());
    CHECK(dec.open(audio->codec_private, &error));
    CHECK_EQ(dec.sample_rate(), 48000u);
    CHECK_EQ(dec.channels(), 2u);
    std::vector<int16_t> pcm;
    size_t packets = 0;
    for (const WebmFrame& f : file.frames)
        if (f.track == audio->number) {
            CHECK(dec.decode(bytes.data() + f.offset, f.size, &pcm));
            ++packets;
        }
    const size_t frames = pcm.size() / 2;
    std::printf("  %zu packets, %zu frames\n", packets, frames);
    // Vorbis drops the first half block; WebM keeps no end trimming here.
    CHECK(frames > 46000 && frames < 50000);
    // Zero crossings and RMS of each channel between 0.2 s and 0.8 s.
    for (int ch = 0; ch < 2; ++ch) {
        size_t crossings = 0;
        double sum = 0;
        const size_t a = 9600, b = 38400;
        for (size_t i = a; i < b && i < frames; ++i) {
            const int s0 = pcm[(i - 1) * 2 + ch], s1 = pcm[i * 2 + ch];
            crossings += (s0 < 0) != (s1 < 0);
            sum += double(s1) * s1;
        }
        const double hz = double(crossings) / 2 / ((b - a) / 48000.0);
        const double rms = std::sqrt(sum / double(b - a));
        std::printf("  channel %d: %.1f Hz, rms %.0f\n", ch, hz, rms);
        CHECK(std::fabs(hz - (ch == 0 ? 440 : 660)) < 5);
        CHECK(std::fabs(rms - 0.5 / std::sqrt(2.0) * 32767) < 0.05 * 11585);
    }
    // Garbage is reported, not fatal.
    const uint8_t junk[] = {1, 2, 3, 4};
    dec.decode(junk, sizeof junk, &pcm);
#endif
}

}  // namespace

int main(int argc, char** argv) {
    g_path = argc > 1 ? argv[1] : nullptr;
    RUN(test_xiph_lacing);
    RUN(test_sound_track);
    return finish();
}
