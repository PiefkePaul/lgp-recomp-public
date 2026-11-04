#include "lgp/platform/vpx_video.hpp"

#include <algorithm>
#include <cstring>
#include <thread>

#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>

namespace lgp::platform {
namespace {

// Copies a decoded 8-bit 4:2:0 picture into tightly packed NV12 (crops odd
// sizes to even). Other formats (10-bit, 4:4:4) are not used by the games.
bool to_nv12(const vpx_image_t* img, VideoPicture* pic) {
    if (img->fmt != VPX_IMG_FMT_I420 && img->fmt != VPX_IMG_FMT_YV12)
        return false;
    const uint32_t w = img->d_w & ~1u, h = img->d_h & ~1u;
    pic->width = w;
    pic->height = h;
    pic->nv12.resize(size_t{w} * h * 3 / 2);
    uint8_t* y = pic->nv12.data();
    uint8_t* uv = y + size_t{w} * h;
    for (uint32_t r = 0; r < h; ++r)
        std::memcpy(y + size_t{r} * w, img->planes[VPX_PLANE_Y] + ptrdiff_t(r) * img->stride[VPX_PLANE_Y], w);
    for (uint32_t r = 0; r < h / 2; ++r) {
        const uint8_t* u = img->planes[VPX_PLANE_U] + ptrdiff_t(r) * img->stride[VPX_PLANE_U];
        const uint8_t* v = img->planes[VPX_PLANE_V] + ptrdiff_t(r) * img->stride[VPX_PLANE_V];
        uint8_t* dst = uv + size_t{r} * w;
        for (uint32_t c = 0; c < w / 2; ++c) {
            dst[2 * c] = u[c];
            dst[2 * c + 1] = v[c];
        }
    }
    return true;
}

class VpxDecoder final : public IVideoDecoder {
public:
    ~VpxDecoder() override {
        if (open_)
            vpx_codec_destroy(&ctx_);
    }

    bool open(const VideoStreamInfo& info) {
        vpx_codec_dec_cfg_t cfg{};
        // Tile threads: 1080p VP9 has up to 4 tile columns.
        cfg.threads = std::clamp(std::thread::hardware_concurrency(), 1u, 4u);
        cfg.w = info.width;
        cfg.h = info.height;
        open_ = vpx_codec_dec_init(&ctx_, vpx_codec_vp9_dx(), &cfg, 0) == VPX_CODEC_OK;
        return open_;
    }

    // VP9 has no reordering: every shown frame comes out of the call that
    // fed it (hidden alt-ref frames ride in a superframe with a shown one).
    bool decode(const uint8_t* data, size_t size, int64_t pts_us, std::vector<VideoPicture>* out) override {
        if (data == nullptr || size == 0)
            return true;  // nothing held back
        if (vpx_codec_decode(&ctx_, data, static_cast<unsigned int>(size), nullptr, 0) != VPX_CODEC_OK)
            return false;
        bool ok = true;
        vpx_codec_iter_t it = nullptr;
        while (const vpx_image_t* img = vpx_codec_get_frame(&ctx_, &it)) {
            VideoPicture pic;
            pic.pts_us = pts_us;
            if (to_nv12(img, &pic))
                out->push_back(std::move(pic));
            else
                ok = false;
        }
        return ok;
    }

private:
    vpx_codec_ctx_t ctx_{};
    bool open_ = false;
};

}  // namespace

std::unique_ptr<IVideoDecoder> VpxVideo::open_decoder(const VideoStreamInfo& info) {
    if (info.codec != VideoCodec::Vp9)
        return nullptr;
    auto dec = std::make_unique<VpxDecoder>();
    if (!dec->open(info))
        return nullptr;
    return dec;
}

}  // namespace lgp::platform
