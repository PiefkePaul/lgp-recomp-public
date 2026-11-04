#include "lgp/hle/vorbis.hpp"

#include <algorithm>
#include <cmath>

#if defined(LGP_HLE_VORBIS)
#include <vorbis/codec.h>
#endif

namespace lgp::hle {

bool split_xiph_headers(const std::vector<uint8_t>& cp, std::vector<std::vector<uint8_t>>* packets) {
    packets->clear();
    if (cp.empty())
        return false;
    const size_t count = size_t{cp[0]} + 1;
    size_t pos = 1;
    std::vector<size_t> sizes;
    for (size_t i = 0; i + 1 < count; ++i) {  // the last size is implicit
        size_t n = 0;
        for (;;) {
            if (pos >= cp.size())
                return false;
            const uint8_t b = cp[pos++];
            n += b;
            if (b != 255)
                break;
        }
        sizes.push_back(n);
    }
    size_t total = 0;
    for (size_t n : sizes)
        total += n;
    if (total > cp.size() - pos)
        return false;
    sizes.push_back(cp.size() - pos - total);
    for (size_t n : sizes) {
        packets->emplace_back(cp.begin() + std::ptrdiff_t(pos), cp.begin() + std::ptrdiff_t(pos + n));
        pos += n;
    }
    return true;
}

#if defined(LGP_HLE_VORBIS)

struct VorbisDecoder::Impl {
    vorbis_info info;
    vorbis_comment comment;
    vorbis_dsp_state dsp;
    vorbis_block block;
    bool dsp_ready = false;
    int64_t packetno = 0;

    Impl() {
        vorbis_info_init(&info);
        vorbis_comment_init(&comment);
    }
    ~Impl() {
        if (dsp_ready) {
            vorbis_block_clear(&block);
            vorbis_dsp_clear(&dsp);
        }
        vorbis_comment_clear(&comment);
        vorbis_info_clear(&info);
    }
};

VorbisDecoder::VorbisDecoder() : impl_(std::make_unique<Impl>()) {}
VorbisDecoder::~VorbisDecoder() = default;

bool VorbisDecoder::available() {
    return true;
}

bool VorbisDecoder::open(const std::vector<uint8_t>& codec_private, std::string* error) {
    std::vector<std::vector<uint8_t>> headers;
    if (!split_xiph_headers(codec_private, &headers) || headers.size() != 3) {
        *error = "CodecPrivate is not three Xiph-laced Vorbis headers";
        return false;
    }
    for (size_t i = 0; i < 3; ++i) {
        ogg_packet p{};
        p.packet = headers[i].data();
        p.bytes = long(headers[i].size());
        p.b_o_s = i == 0;
        p.packetno = ogg_int64_t(i);
        if (vorbis_synthesis_headerin(&impl_->info, &impl_->comment, &p) != 0) {
            *error = "Vorbis header " + std::to_string(i) + " rejected";
            return false;
        }
    }
    if (vorbis_synthesis_init(&impl_->dsp, &impl_->info) != 0 || vorbis_block_init(&impl_->dsp, &impl_->block) != 0) {
        *error = "Vorbis decoder set-up failed";
        return false;
    }
    impl_->dsp_ready = true;
    impl_->packetno = 3;
    rate_ = uint32_t(impl_->info.rate);
    channels_ = uint32_t(impl_->info.channels);
    return true;
}

bool VorbisDecoder::decode(const uint8_t* data, size_t size, std::vector<int16_t>* out) {
    if (!impl_->dsp_ready || data == nullptr || size == 0)
        return false;
    ogg_packet p{};
    p.packet = const_cast<unsigned char*>(data);
    p.bytes = long(size);
    p.packetno = impl_->packetno++;
    p.granulepos = -1;
    if (vorbis_synthesis(&impl_->block, &p) != 0)
        return false;
    vorbis_synthesis_blockin(&impl_->dsp, &impl_->block);
    float** pcm = nullptr;
    int n;
    while ((n = vorbis_synthesis_pcmout(&impl_->dsp, &pcm)) > 0) {
        const size_t base = out->size();
        out->resize(base + size_t(n) * channels_);
        int16_t* dst = out->data() + base;
        for (int i = 0; i < n; ++i)
            for (uint32_t c = 0; c < channels_; ++c) {
                const float v = std::clamp(pcm[c][i], -1.0f, 1.0f);
                *dst++ = int16_t(std::lrint(v * 32767.0f));
            }
        vorbis_synthesis_read(&impl_->dsp, n);
    }
    return true;
}

void VorbisDecoder::reset() {
    if (impl_->dsp_ready)
        vorbis_synthesis_restart(&impl_->dsp);
}

#else  // no libvorbis

struct VorbisDecoder::Impl {};
VorbisDecoder::VorbisDecoder() : impl_(std::make_unique<Impl>()) {}
VorbisDecoder::~VorbisDecoder() = default;
bool VorbisDecoder::available() {
    return false;
}
bool VorbisDecoder::open(const std::vector<uint8_t>&, std::string* error) {
    *error = "this build has no Vorbis decoder (LGP_ENABLE_VORBIS)";
    return false;
}
bool VorbisDecoder::decode(const uint8_t*, size_t, std::vector<int16_t>*) {
    return false;
}
void VorbisDecoder::reset() {}

#endif

}  // namespace lgp::hle
