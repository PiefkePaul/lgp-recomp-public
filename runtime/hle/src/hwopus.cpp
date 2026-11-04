// hwopus (P5.2): Opus decoders for the title. On the console the audio DSP
// decodes; here libopus does it on the host (LGP_HLE_OPUS, cmake/LgpOpus.cmake).
// Without libopus every packet decodes to silence of the right length, so a
// title's audio threads keep their timing.
//
// The input is Nintendo's framing: u32 big-endian payload size, u32
// big-endian final range of the encoder, then one Opus packet. The output
// is interleaved signed 16-bit PCM at the decoder's rate and channel count.
// Command ids and layouts from switchbrew "Audio services"; the commands are
// the ones of the Ryujinx reference run (docs/analysis/02-services.md):
// OpenHardwareOpusDecoder, GetWorkBufferSize, DecodeInterleavedWithPerfOld
// (and DecodeInterleavedOld, the same without the time). Everything else
// halts the guest. Knowledge: docs/knowledge/horizon-audout-hwopus.md.
#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <vector>

#include "lgp/hle/services.hpp"
#include "lgp/runtime/log.hpp"

#if defined(LGP_HLE_OPUS)
#include "opus.h"
#endif

namespace lgp::hle {
namespace {

using rt::LogLevel;

// Assumption, not measured on a console: the title allocates this much as
// transfer memory and hands it to OpenHardwareOpusDecoder; the HLE keeps its
// decoder state on the host and never touches it. Sized like a decoder that
// keeps its state there: room for libopus' state (kStateBound, about 18 KiB
// mono / 26 KiB stereo in libopus 1.5.2, the test checks the bound) plus
// one 120 ms frame of 16-bit PCM at the decoder's rate. The same in builds
// with and without libopus, so the title's memory layout does not change.
constexpr uint32_t kStateBound = 0x8000;
constexpr uint32_t kMaxFrameMs = 120;

uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

uint32_t be32(const uint8_t* p) { return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3]; }

bool valid_rate(uint32_t rate) {
    return rate == 8000 || rate == 12000 || rate == 16000 || rate == 24000 || rate == 48000;
}

}  // namespace

uint32_t hwopus_work_buffer_size(uint32_t sample_rate, uint32_t channels) {
    const uint32_t frame_bytes = sample_rate / 1000 * kMaxFrameMs * channels * 2;
    return align_up(kStateBound + frame_bytes, 0x1000);
}

bool hwopus_decodes_audio() {
#if defined(LGP_HLE_OPUS)
    return true;
#else
    return false;
#endif
}

// Samples per channel of one Opus packet at sample_rate, 0 if malformed.
uint32_t opus_packet_samples(const uint8_t* packet, size_t size, uint32_t sample_rate) {
    if (size < 1)
        return 0;
    const uint32_t config = packet[0] >> 3;
    // Frame duration in units of 2.5 ms.
    uint32_t units;
    if (config < 12)
        units = (config & 3) == 3 ? 24 : 4u << (config & 3);  // SILK: 10, 20, 40, 60 ms
    else if (config < 16)
        units = (config & 1) ? 8 : 4;                         // hybrid: 10, 20 ms
    else
        units = 1u << (config & 3);                           // CELT: 2.5, 5, 10, 20 ms
    uint32_t frames;
    switch (packet[0] & 3) {
    case 0: frames = 1; break;
    case 1:
    case 2: frames = 2; break;
    default:
        if (size < 2)
            return 0;
        frames = packet[1] & 0x3F;
        break;
    }
    if (frames * units > 48)  // more than 120 ms
        return 0;
    return frames * units * sample_rate / 400;
}

namespace {

class HardwareOpusDecoder final : public Service {
public:
    HardwareOpusDecoder(uint32_t sample_rate, uint32_t channels)
        : Service("hwopus:IHardwareOpusDecoder"), sample_rate_(sample_rate), channels_(channels) {
#if defined(LGP_HLE_OPUS)
        int err = OPUS_OK;
        decoder_ = opus_decoder_create(static_cast<opus_int32>(sample_rate), static_cast<int>(channels), &err);
        if (err != OPUS_OK) {
            rt::log(LogLevel::Error, "hwopus: opus_decoder_create(%u, %u): %s", sample_rate, channels,
                    opus_strerror(err));
            decoder_ = nullptr;
        }
#endif
        on(0, [this](Request& req, Response& resp) { decode(req, resp, false); });  // DecodeInterleavedOld
        on(4, [this](Request& req, Response& resp) { decode(req, resp, true); });   // DecodeInterleavedWithPerfOld
    }
    ~HardwareOpusDecoder() override {
#if defined(LGP_HLE_OPUS)
        if (decoder_ != nullptr)
            opus_decoder_destroy(decoder_);
#endif
    }

private:
    // In: A buffer with the framed packet. Out: B buffer for the PCM.
    // Reply: u32 consumed bytes (header + payload), u32 samples per channel,
    // with perf also u64 decode time (microseconds, assumed unit).
    void decode(Request& req, Response& resp, bool perf) {
        std::vector<uint8_t> in;
        if (!req.read_in(0, &in, 0x10000) || in.size() < 8) {
            resp.result = rt::kernel_result::kInvalidPointer;
            return;
        }
        const uint32_t payload = be32(in.data());
        if (payload == 0 || payload > in.size() - 8) {
            // Truncated or empty packet (libopus would conceal a lost frame of
            // unknown length): no measured answer.
            rt::log(LogLevel::Error, "hwopus: packet of %u bytes in a %zu-byte buffer", payload, in.size());
            resp.unimplemented = true;
            return;
        }
        const uint8_t* packet = in.data() + 8;
        const HipcBuffer out = req.out_buffer(0);
        const auto t0 = std::chrono::steady_clock::now();
        std::lock_guard lock(mutex_);  // one libopus state, maybe several guest threads
        std::vector<int16_t> pcm;
        uint32_t samples = 0;
#if defined(LGP_HLE_OPUS)
        if (decoder_ == nullptr) {
            resp.unimplemented = true;
            return;
        }
        // Room for the largest packet (120 ms), capped by the guest's buffer.
        const uint32_t max_samples = std::min<uint64_t>(sample_rate_ / 1000 * kMaxFrameMs, out.size / (channels_ * 2));
        pcm.resize(size_t{max_samples} * channels_);
        const int n = opus_decode(decoder_, packet, static_cast<opus_int32>(payload),
                                  pcm.data(), static_cast<int>(max_samples), 0);
        if (n < 0) {
            rt::log(LogLevel::Error, "hwopus: opus_decode(%u bytes, room for %u samples): %s", payload, max_samples,
                    opus_strerror(n));
            resp.unimplemented = true;  // the console's error codes are not known
            return;
        }
        samples = static_cast<uint32_t>(n);
        pcm.resize(size_t{samples} * channels_);
        opus_uint32 range = 0;
        opus_decoder_ctl(decoder_, OPUS_GET_FINAL_RANGE(&range));
        if (range != be32(in.data() + 4) && !warned_) {
            warned_ = true;  // the encoder's range does not match: bit errors or a framing mistake
            rt::log(LogLevel::Warn, "hwopus: final range 0x%08x, packet says 0x%08x", range, be32(in.data() + 4));
        }
#else
        samples = opus_packet_samples(packet, payload, sample_rate_);
        if (uint64_t{samples} * channels_ * 2 > out.size) {
            resp.unimplemented = true;
            return;
        }
        pcm.assign(size_t{samples} * channels_, 0);
        if (!warned_) {
            warned_ = true;
            rt::log(LogLevel::Warn, "hwopus: built without libopus, packets decode to silence");
        }
#endif
        if (!pcm.empty() && !req.write_out(0, pcm.data(), pcm.size() * 2)) {
            resp.result = rt::kernel_result::kInvalidPointer;
            return;
        }
        resp.push<uint32_t>(8 + payload);  // consumed
        resp.push<uint32_t>(samples);
        if (perf) {
            const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0);
            resp.push<uint64_t>(static_cast<uint64_t>(us.count()));
        }
    }

    uint32_t sample_rate_, channels_;
    std::mutex mutex_;
    bool warned_ = false;
#if defined(LGP_HLE_OPUS)
    OpusDecoder* decoder_ = nullptr;
#endif
};

class HardwareOpusDecoderManager final : public Service {
public:
    HardwareOpusDecoderManager() : Service("hwopus") {
        // OpenHardwareOpusDecoder(s32 sample rate, s32 channels, u32 work buffer size; copy handle tmem)
        on(0, [](Request& req, Response& resp) {
            const uint32_t rate = req.pop<uint32_t>();
            const uint32_t channels = req.pop<uint32_t>();
            if (!valid_rate(rate) || channels < 1 || channels > 2) {
                rt::log(LogLevel::Error, "hwopus: OpenHardwareOpusDecoder(%u Hz, %u channels)", rate, channels);
                resp.unimplemented = true;
                return;
            }
            resp.push_object(std::make_shared<HardwareOpusDecoder>(rate, channels));
        });
        // GetWorkBufferSize(s32 sample rate, s32 channels) -> u32
        on(1, [](Request& req, Response& resp) {
            const uint32_t rate = req.pop<uint32_t>();
            const uint32_t channels = req.pop<uint32_t>();
            if (!valid_rate(rate) || channels < 1 || channels > 2) {
                resp.unimplemented = true;
                return;
            }
            resp.push<uint32_t>(hwopus_work_buffer_size(rate, channels));
        });
    }
};

}  // namespace

void register_hwopus(ServiceManager& sm) {
    sm.register_service("hwopus", [] { return std::make_shared<HardwareOpusDecoderManager>(); });
}

}  // namespace lgp::hle
