#include "lgp/platform/ffmpeg_video.hpp"

#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace lgp::platform {
namespace {

// Copies a decoded frame into tightly packed NV12 (crops odd sizes to even).
bool to_nv12(const AVFrame* f, VideoPicture* pic) {
    const uint32_t w = uint32_t(f->width) & ~1u, h = uint32_t(f->height) & ~1u;
    pic->width = w;
    pic->height = h;
    pic->nv12.resize(size_t{w} * h * 3 / 2);
    uint8_t* y = pic->nv12.data();
    uint8_t* uv = y + size_t{w} * h;
    switch (f->format) {
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUVJ420P:
        for (uint32_t r = 0; r < h; ++r)
            std::memcpy(y + size_t{r} * w, f->data[0] + ptrdiff_t(r) * f->linesize[0], w);
        for (uint32_t r = 0; r < h / 2; ++r) {
            const uint8_t* u = f->data[1] + ptrdiff_t(r) * f->linesize[1];
            const uint8_t* v = f->data[2] + ptrdiff_t(r) * f->linesize[2];
            uint8_t* dst = uv + size_t{r} * w;
            for (uint32_t c = 0; c < w / 2; ++c) {
                dst[2 * c] = u[c];
                dst[2 * c + 1] = v[c];
            }
        }
        return true;
    case AV_PIX_FMT_NV12:
        for (uint32_t r = 0; r < h; ++r)
            std::memcpy(y + size_t{r} * w, f->data[0] + ptrdiff_t(r) * f->linesize[0], w);
        for (uint32_t r = 0; r < h / 2; ++r)
            std::memcpy(uv + size_t{r} * w, f->data[1] + ptrdiff_t(r) * f->linesize[1], w);
        return true;
    default:
        return false;  // 10-bit or 4:4:4 streams: not used by the games so far
    }
}

class FfmpegDecoder final : public IVideoDecoder {
public:
    ~FfmpegDecoder() override {
        av_frame_free(&frame_);
        av_packet_free(&packet_);
        avcodec_free_context(&ctx_);
    }

    bool open(const VideoStreamInfo& info) {
        const AVCodec* codec = avcodec_find_decoder(info.codec == VideoCodec::Vp9 ? AV_CODEC_ID_VP9 : AV_CODEC_ID_H264);
        if (codec == nullptr || (ctx_ = avcodec_alloc_context3(codec)) == nullptr)
            return false;
        ctx_->width = int(info.width);
        ctx_->height = int(info.height);
        ctx_->thread_count = 0;  // automatic
        ctx_->pkt_timebase = AVRational{1, 1000000};
        if (!info.codec_private.empty()) {
            ctx_->extradata = static_cast<uint8_t*>(av_mallocz(info.codec_private.size() + AV_INPUT_BUFFER_PADDING_SIZE));
            if (ctx_->extradata == nullptr)
                return false;
            std::memcpy(ctx_->extradata, info.codec_private.data(), info.codec_private.size());
            ctx_->extradata_size = int(info.codec_private.size());
        }
        frame_ = av_frame_alloc();
        packet_ = av_packet_alloc();
        return frame_ != nullptr && packet_ != nullptr && avcodec_open2(ctx_, codec, nullptr) == 0;
    }

    bool decode(const uint8_t* data, size_t size, int64_t pts_us, std::vector<VideoPicture>* out) override {
        int err;
        if (data == nullptr || size == 0) {
            err = avcodec_send_packet(ctx_, nullptr);
        } else {
            if (av_new_packet(packet_, int(size)) != 0)
                return false;
            std::memcpy(packet_->data, data, size);
            packet_->pts = pts_us;
            err = avcodec_send_packet(ctx_, packet_);
            av_packet_unref(packet_);
        }
        bool ok = err == 0 || err == AVERROR_EOF;
        while (avcodec_receive_frame(ctx_, frame_) == 0) {
            VideoPicture pic;
            pic.pts_us = frame_->best_effort_timestamp != AV_NOPTS_VALUE ? frame_->best_effort_timestamp : pts_us;
            if (to_nv12(frame_, &pic))
                out->push_back(std::move(pic));
            else
                ok = false;
            av_frame_unref(frame_);
        }
        return ok;
    }

private:
    AVCodecContext* ctx_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* packet_ = nullptr;
};

}  // namespace

std::unique_ptr<IVideoDecoder> FfmpegVideo::open_decoder(const VideoStreamInfo& info) {
    auto dec = std::make_unique<FfmpegDecoder>();
    if (!dec->open(info))
        return nullptr;
    return dec;
}

}  // namespace lgp::platform
