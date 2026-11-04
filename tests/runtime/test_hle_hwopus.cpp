// P5.2: hwopus through sm: and the IPC svcs. With libopus (LGP_TEST_OPUS)
// the test encodes a known stereo signal with libopus' encoder, sends the
// packets in Nintendo's framing (u32 BE size, u32 BE final range, packet)
// to the service and checks the PCM: bit-exact against a local libopus
// decoder, and close to the original signal (correlation, energy). Without
// libopus the service must answer with silence of the packet's length.
#include <cmath>
#include <cstring>
#include <vector>

#include "ipc_client.hpp"
#include "lgp/hle/services.hpp"

#if defined(LGP_TEST_OPUS)
#include "opus.h"
#endif

using namespace lgp_test;

namespace {

constexpr double kPi = 3.14159265358979323846;

struct OpusEnv : Env {
    Handle mgr = kInvalidHandle;
    OpusEnv() : Env([](ServiceManager& sm) { register_hwopus(sm); }) { mgr = open_service("hwopus"); }

    Reply call(Handle h, uint32_t command, Msg m = {}, SvcOutcome expect = SvcOutcome::Continue) {
        m.command = command;
        const std::optional<Reply> r = send(h, m, expect);
        return r ? *r : Reply{};
    }
    Handle open_decoder(uint32_t rate, uint32_t channels) {
        Msg m;
        m.arg<uint32_t>(rate).arg<uint32_t>(channels).arg<uint32_t>(hwopus_work_buffer_size(rate, channels));
        const Reply r = call(mgr, 0, m);
        CHECK_EQ(r.result, 0u);
        CHECK_EQ(r.move.size(), 1u);
        return r.move.empty() ? kInvalidHandle : r.move[0];
    }
    // Sends one framed packet; returns consumed/samples and the PCM.
    struct Decoded {
        uint32_t consumed = 0, samples = 0;
        Reply reply;
        std::vector<int16_t> pcm;
    };
    Decoded decode(Handle dec, const std::vector<uint8_t>& packet, uint32_t final_range, uint32_t channels,
                   bool perf = true) {
        std::vector<uint8_t> framed(8 + packet.size());
        const uint32_t size = static_cast<uint32_t>(packet.size());
        for (int i = 0; i < 4; ++i) {
            framed[i] = static_cast<uint8_t>(size >> (24 - 8 * i));
            framed[4 + i] = static_cast<uint8_t>(final_range >> (24 - 8 * i));
        }
        std::memcpy(framed.data() + 8, packet.data(), packet.size());
        CHECK(k.memory().copy_to_user(kBufA, framed.data(), framed.size()));
        std::vector<uint8_t> junk(kBufSize, 0x5A);
        CHECK(k.memory().copy_to_user(kBufB, junk.data(), junk.size()));
        Msg m;
        m.a = {{kBufA, framed.size(), 0}};
        m.b = {{kBufB, kBufSize, 0}};
        Decoded d;
        d.reply = call(dec, perf ? 4 : 0, m);
        CHECK_EQ(d.reply.result, 0u);
        d.consumed = d.reply.get<uint32_t>(0);
        d.samples = d.reply.get<uint32_t>(4);
        d.pcm.resize(size_t{d.samples} * channels);
        if (!d.pcm.empty())
            CHECK(k.memory().copy_from_user(d.pcm.data(), kBufB, d.pcm.size() * 2));
        return d;
    }
};

void test_packet_samples() {
    auto samples = [](std::initializer_list<uint8_t> packet, uint32_t rate) {
        const std::vector<uint8_t> v(packet);
        return opus_packet_samples(v.data(), v.size(), rate);
    };
    CHECK_EQ(samples({0xF8}, 48000), 960u);         // CELT 20 ms, 1 frame
    CHECK_EQ(samples({0x09}, 48000), 1920u);        // SILK 20 ms, 2 frames
    CHECK_EQ(samples({0x1B, 0x02}, 48000), 5760u);  // SILK 60 ms, 2 frames (code 3)
    CHECK_EQ(samples({0x1B, 0x03}, 48000), 0u);     // 180 ms: more than an Opus packet may hold
    CHECK_EQ(samples({0x80}, 24000), 60u);          // CELT 2.5 ms
    CHECK_EQ(samples({0x1B}, 48000), 0u);           // code 3 without count
}

void test_manager() {
    OpusEnv e;
    Msg params;
    params.arg<uint32_t>(48000).arg<uint32_t>(2);
    const uint32_t size = e.call(e.mgr, 1, params).get<uint32_t>(0);  // GetWorkBufferSize
    CHECK_EQ(size, hwopus_work_buffer_size(48000, 2));
    CHECK(size >= 48 * 120 * 4);  // at least one 120 ms stereo frame
    CHECK_EQ(size % 0x1000, 0u);
#if defined(LGP_TEST_OPUS)
    // The assumed size leaves room for libopus' own state (see hwopus.cpp).
    CHECK(size - 48 * 120 * 4 >= static_cast<uint32_t>(opus_decoder_get_size(2)));
    CHECK(hwopus_work_buffer_size(16000, 1) - 16 * 120 * 2 >= static_cast<uint32_t>(opus_decoder_get_size(1)));
#endif
    // Formats the console does not offer halt like any unknown request.
    Msg bad;
    bad.arg<uint32_t>(44100).arg<uint32_t>(2).arg<uint32_t>(0x10000);
    e.call(e.mgr, 0, bad, SvcOutcome::Unimplemented);
    Msg three;
    three.arg<uint32_t>(48000).arg<uint32_t>(3).arg<uint32_t>(0x10000);
    e.call(e.mgr, 0, three, SvcOutcome::Unimplemented);
}

void test_truncated_packet_halts() {
    OpusEnv e;
    const Handle dec = e.open_decoder(48000, 2);
    const uint8_t packet[] = {0, 0, 0, 9, 0, 0, 0, 0, 0xF8, 0xAA};  // says 9 bytes, has 2
    CHECK(e.k.memory().copy_to_user(kBufA, packet, sizeof(packet)));
    Msg m;
    m.command = 4;
    m.a = {{kBufA, sizeof(packet), 0}};
    m.b = {{kBufB, 0x2000, 0}};
    e.send(dec, m, SvcOutcome::Unimplemented);
}

#if defined(LGP_TEST_OPUS)

struct Encoded {
    std::vector<std::vector<uint8_t>> packets;
    std::vector<uint32_t> ranges;
    int lookahead = 0;
};

Encoded encode(const std::vector<int16_t>& pcm, uint32_t rate, int channels, int frame, int bitrate) {
    Encoded out;
    int err = 0;
    OpusEncoder* enc = opus_encoder_create(static_cast<opus_int32>(rate), channels, OPUS_APPLICATION_AUDIO, &err);
    CHECK_EQ(err, OPUS_OK);
    if (enc == nullptr)
        return out;
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(bitrate));
    opus_encoder_ctl(enc, OPUS_GET_LOOKAHEAD(&out.lookahead));
    const size_t frames = pcm.size() / channels / frame;
    for (size_t i = 0; i < frames; ++i) {
        std::vector<uint8_t> packet(1500);
        const int n = opus_encode(enc, pcm.data() + i * frame * channels, frame, packet.data(),
                                  static_cast<opus_int32>(packet.size()));
        CHECK(n > 0);
        packet.resize(n > 0 ? size_t(n) : 0);
        opus_uint32 range = 0;
        opus_encoder_ctl(enc, OPUS_GET_FINAL_RANGE(&range));
        out.packets.push_back(std::move(packet));
        out.ranges.push_back(range);
    }
    opus_encoder_destroy(enc);
    return out;
}

// Normalized cross-correlation of channel ch of a (shifted by delay frames) and b.
double correlation(const std::vector<int16_t>& a, const std::vector<int16_t>& b, int channels, int ch, size_t from,
                   size_t to, int delay, double* energy_ratio) {
    double ab = 0, aa = 0, bb = 0;
    for (size_t i = from; i < to; ++i) {
        const double x = a[(i + delay) * channels + ch], y = b[i * channels + ch];
        ab += x * y;
        aa += x * x;
        bb += y * y;
    }
    *energy_ratio = bb > 0 ? aa / bb : 0;
    return aa > 0 && bb > 0 ? ab / std::sqrt(aa * bb) : 0;
}

void test_decode_stereo_48k() {
    constexpr uint32_t kRate = 48000;
    constexpr int kFrame = 960;    // 20 ms
    constexpr int kFrames = 50;    // 1 s
    std::vector<int16_t> signal(size_t{kFrame} * kFrames * 2);
    for (size_t i = 0; i < signal.size() / 2; ++i) {
        const double t = double(i) / kRate;
        signal[i * 2] = static_cast<int16_t>(16000 * std::sin(2 * kPi * 440 * t));
        signal[i * 2 + 1] = static_cast<int16_t>(10000 * std::sin(2 * kPi * 660 * t + 0.5));
    }
    const Encoded enc = encode(signal, kRate, 2, kFrame, 96000);
    CHECK_EQ(enc.packets.size(), size_t(kFrames));

    OpusEnv e;
    const Handle dec = e.open_decoder(kRate, 2);
    int err = 0;
    OpusDecoder* ref = opus_decoder_create(kRate, 2, &err);
    CHECK(ref != nullptr);
    std::vector<int16_t> decoded;
    bool exact = true;
    for (size_t i = 0; i < enc.packets.size(); ++i) {
        const auto d = e.decode(dec, enc.packets[i], enc.ranges[i], 2, i % 2 == 0);  // WithPerf and plain
        CHECK_EQ(d.consumed, 8 + enc.packets[i].size());
        CHECK_EQ(d.samples, uint32_t(kFrame));
        if (i % 2 == 0)
            CHECK(d.reply.data.size() >= 16);  // consumed, samples, u64 perf time
        std::vector<int16_t> want(size_t{kFrame} * 2);
        const int n = opus_decode(ref, enc.packets[i].data(), static_cast<opus_int32>(enc.packets[i].size()),
                                  want.data(), kFrame, 0);
        CHECK_EQ(n, kFrame);
        exact &= d.pcm == want;
        decoded.insert(decoded.end(), d.pcm.begin(), d.pcm.end());
    }
    opus_decoder_destroy(ref);
    CHECK(exact);  // the service is libopus, not an approximation of it
    CHECK_EQ(decoded.size(), signal.size());
    if (decoded.size() != signal.size())
        return;

    // The decoder output lags the input by the encoder's lookahead. Skip the
    // first 100 ms (the encoder settles), compare the rest.
    const size_t from = kRate / 10, to = signal.size() / 2 - size_t(enc.lookahead);
    for (int ch = 0; ch < 2; ++ch) {
        double energy = 0;
        const double c = correlation(decoded, signal, 2, ch, from, to, enc.lookahead, &energy);
        std::printf("  channel %d: correlation %.4f, energy ratio %.3f (lookahead %d)\n", ch, c, energy,
                    enc.lookahead);
        CHECK(c > 0.98);
        CHECK(energy > 0.8 && energy < 1.25);
    }
}

void test_decode_mono_24k() {
    constexpr uint32_t kRate = 24000;
    constexpr int kFrame = 480;  // 20 ms
    std::vector<int16_t> signal(size_t{kFrame} * 10);
    for (size_t i = 0; i < signal.size(); ++i)
        signal[i] = static_cast<int16_t>(12000 * std::sin(2 * kPi * 300 * double(i) / kRate));
    const Encoded enc = encode(signal, kRate, 1, kFrame, 32000);
    OpusEnv e;
    const Handle dec = e.open_decoder(kRate, 1);
    double energy = 0;
    for (size_t i = 0; i < enc.packets.size(); ++i) {
        const auto d = e.decode(dec, enc.packets[i], enc.ranges[i], 1);
        CHECK_EQ(d.samples, uint32_t(kFrame));
        if (i >= 5)
            for (int16_t s : d.pcm) energy += double(s) * s;
    }
    CHECK(energy > 0);  // not silence
}

#else

void test_decode_silence_without_libopus() {
    OpusEnv e;
    const Handle dec = e.open_decoder(48000, 2);
    const auto d = e.decode(dec, {0xF8, 0xAA, 0xBB}, 0x12345678, 2);  // 3-byte CELT 20 ms
    CHECK_EQ(d.consumed, 11u);  // header + payload
    CHECK_EQ(d.samples, 960u);  // samples per channel
    bool silent = true;
    for (int16_t s : d.pcm) silent &= s == 0;
    CHECK(silent);
}

#endif

}  // namespace

int main() {
    RUN(test_packet_samples);
    RUN(test_manager);
    RUN(test_truncated_packet_halts);
#if defined(LGP_TEST_OPUS)
    CHECK(hwopus_decodes_audio());
    RUN(test_decode_stereo_48k);
    RUN(test_decode_mono_24k);
#else
    CHECK(!hwopus_decodes_audio());
    RUN(test_decode_silence_without_libopus);
#endif
    return finish();
}
