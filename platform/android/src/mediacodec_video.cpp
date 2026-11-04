// MediaCodec video decoder (mediacodec_video.hpp).
//
// Synchronous use of AMediaCodec: decode() queues the frame (draining output
// while no input buffer is free) and appends every picture that is ready;
// the end of stream drains the rest. Output colour formats: semi-planar
// (COLOR_FormatYUV420SemiPlanar and Qualcomm's NV12 variants with aligned
// stride and slice height) and planar I420. Verdacht: other vendor formats
// (tiled ones) are not handled; such a decoder fails to convert and the
// movie HLE shows black pictures.
//
// LGP_MEDIACODEC_IMAGE_READER=1 (default off, for decoders whose buffer
// layout is unknown, e.g. NVIDIA's on a Shield; docs/analysis/33): the codec
// renders into an AImageReader surface in YUV_420_888 instead, and each
// picture is copied plane by plane as its strides say
// (yuv420_flexible_to_nv12), whatever the vendor's colour format.
#include "lgp/platform/android/mediacodec_video.hpp"

#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#include <android/log.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "lgp/platform/video_convert.hpp"

namespace lgp::platform::android {
namespace {

constexpr int32_t kColorPlanar = 19;               // COLOR_FormatYUV420Planar (I420)
constexpr int32_t kColorSemiPlanar = 21;           // COLOR_FormatYUV420SemiPlanar (NV12)
constexpr int32_t kColorQcomVenusNv12 = 0x7FA30C04;   // QOMX_COLOR_FORMATYUV420PackedSemiPlanar32m (NV12, aligned)

void log(const char* what, media_status_t status) {
    __android_log_print(ANDROID_LOG_WARN, "lgp", "MediaCodec: %s (%d)", what, int(status));
}

int32_t get_int(AMediaFormat* f, const char* key, int32_t fallback) {
    int32_t v = 0;
    return AMediaFormat_getInt32(f, key, &v) ? v : fallback;
}

bool image_reader_enabled() {
    const char* v = std::getenv("LGP_MEDIACODEC_IMAGE_READER");
    return v != nullptr && std::strcmp(v, "1") == 0;
}

// One YUV_420_888 image to tightly packed NV12, cropped to the image's crop
// rectangle (rounded to even values).
bool image_to_nv12(AImage* image, VideoPicture* pic) {
    int32_t format = 0, w = 0, h = 0, planes = 0;
    if (AImage_getFormat(image, &format) != AMEDIA_OK || format != AIMAGE_FORMAT_YUV_420_888 ||
        AImage_getWidth(image, &w) != AMEDIA_OK || AImage_getHeight(image, &h) != AMEDIA_OK ||
        AImage_getNumberOfPlanes(image, &planes) != AMEDIA_OK || planes != 3)
        return false;
    AImageCropRect crop{0, 0, w, h};
    if (AImage_getCropRect(image, &crop) != AMEDIA_OK || crop.right <= crop.left || crop.bottom <= crop.top)
        crop = {0, 0, w, h};
    uint8_t* data[3] = {};
    int len[3] = {};
    int32_t row[3] = {}, pixel[3] = {1, 1, 1};
    for (int i = 0; i < 3; ++i) {
        if (AImage_getPlaneData(image, i, &data[i], &len[i]) != AMEDIA_OK ||
            AImage_getPlaneRowStride(image, i, &row[i]) != AMEDIA_OK)
            return false;
        if (i > 0 && AImage_getPlanePixelStride(image, i, &pixel[i]) != AMEDIA_OK)
            return false;
    }
    // Cb and Cr share their strides in YUV_420_888.
    if (row[1] != row[2] || pixel[1] != pixel[2] || pixel[1] < 1 || row[0] < 1 || row[1] < 1)
        return false;
    const uint32_t cx = uint32_t(crop.left) & ~1u, cy = uint32_t(crop.top) & ~1u;
    const uint32_t cw = uint32_t(crop.right - crop.left) & ~1u, ch = uint32_t(crop.bottom - crop.top) & ~1u;
    if (cw == 0 || ch == 0)
        return false;
    // Every byte read must lie inside its plane.
    const size_t y_end = size_t(cy + ch - 1) * size_t(row[0]) + cx + cw;
    const size_t c_end = size_t(cy / 2 + ch / 2 - 1) * size_t(row[1]) + size_t(cx / 2 + cw / 2 - 1) * size_t(pixel[1]) + 1;
    if (y_end > size_t(len[0]) || c_end > size_t(len[1]) || c_end > size_t(len[2]))
        return false;
    const uint8_t* y = data[0] + size_t(cy) * size_t(row[0]) + cx;
    const size_t c_off = size_t(cy / 2) * size_t(row[1]) + size_t(cx / 2) * size_t(pixel[1]);
    yuv420_flexible_to_nv12(y, size_t(row[0]), data[1] + c_off, data[2] + c_off, size_t(row[1]), size_t(pixel[1]), cw,
                            ch, pic);
    return true;
}

class MediaCodecDecoder final : public IVideoDecoder {
public:
    explicit MediaCodecDecoder(AMediaCodec* codec, uint32_t width, uint32_t height, AImageReader* reader = nullptr)
        : codec_(codec), reader_(reader), width_(width), height_(height), stride_(width), slice_height_(height) {}
    ~MediaCodecDecoder() override {
        AMediaCodec_stop(codec_);
        AMediaCodec_delete(codec_);
        if (reader_ != nullptr)
            AImageReader_delete(reader_);  // after the codec: it renders into the reader's window
    }

    bool decode(const uint8_t* data, size_t size, int64_t pts_us, std::vector<VideoPicture>* out) override {
        if (eos_sent_)
            return data == nullptr;
        const bool eos = data == nullptr;
        // Wait for an input buffer, draining pictures meanwhile (the decoder
        // only frees input once its output is taken).
        ssize_t in = -1;
        for (int tries = 0; tries < 200; ++tries) {
            in = AMediaCodec_dequeueInputBuffer(codec_, 5000);
            if (in >= 0)
                break;
            if (!drain(out, 0))
                return false;
        }
        if (in < 0) {
            log("no input buffer", AMEDIA_OK);
            return false;
        }
        size_t capacity = 0;
        uint8_t* buf = AMediaCodec_getInputBuffer(codec_, size_t(in), &capacity);
        if (buf == nullptr || size > capacity) {
            AMediaCodec_queueInputBuffer(codec_, size_t(in), 0, 0, uint64_t(pts_us), 0);
            return false;
        }
        if (size > 0)
            std::memcpy(buf, data, size);
        const media_status_t st = AMediaCodec_queueInputBuffer(
            codec_, size_t(in), 0, size, uint64_t(std::max<int64_t>(pts_us, 0)),
            eos ? AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM : 0);
        if (st != AMEDIA_OK) {
            log("queueInputBuffer", st);
            return false;
        }
        if (!eos)
            return drain(out, 0);
        eos_sent_ = true;
        // Everything the decoder still holds, until it reports the end.
        for (int tries = 0; tries < 400 && !eos_seen_; ++tries)
            if (!drain(out, 10000))
                return false;
        return true;
    }

private:
    // Takes the output that is ready (waiting up to timeout_us for the first).
    bool drain(std::vector<VideoPicture>* out, int64_t timeout_us) {
        for (;;) {
            AMediaCodecBufferInfo info{};
            const ssize_t idx = AMediaCodec_dequeueOutputBuffer(codec_, &info, timeout_us);
            if (idx == AMEDIACODEC_INFO_TRY_AGAIN_LATER)
                return true;
            if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
                read_format();
                continue;
            }
            if (idx == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED)
                continue;
            if (idx < 0) {
                log("dequeueOutputBuffer", media_status_t(idx));
                return false;
            }
            timeout_us = 0;
            if ((info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0)
                eos_seen_ = true;
            if (reader_ != nullptr) {
                // An empty buffer carries no picture.
                const bool picture = info.size != 0;
                AMediaCodec_releaseOutputBuffer(codec_, size_t(idx), picture);
                if (picture && !take_images(info.presentationTimeUs, out))
                    return false;
                if (eos_seen_)
                    return true;
                continue;
            }
            bool ok = true;
            if (info.size > 0) {
                size_t capacity = 0;
                const uint8_t* buf = AMediaCodec_getOutputBuffer(codec_, size_t(idx), &capacity);
                if (buf != nullptr && size_t(info.offset) + size_t(info.size) <= capacity) {
                    VideoPicture pic;
                    pic.pts_us = info.presentationTimeUs;
                    ok = convert(buf + info.offset, size_t(info.size), &pic);
                    if (ok)
                        out->push_back(std::move(pic));
                }
            }
            AMediaCodec_releaseOutputBuffer(codec_, size_t(idx), false);
            if (!ok)
                return false;
            if (eos_seen_)
                return true;
        }
    }

    // The pictures the codec has rendered into the reader: waits up to 100 ms
    // for one, then takes every image that is there. Each picture keeps its
    // own timestamp (the codec stamps the surface buffer with its
    // presentation time), so one that comes late does not shift the others.
    // false (decode fails, a FallbackVideo can take over) when an image does
    // not convert or three pictures in a row never arrive.
    bool take_images(int64_t fallback_pts_us, std::vector<VideoPicture>* out) {
        bool first = true;
        for (int waited_ms = 0;;) {
            AImage* image = nullptr;
            const media_status_t st = AImageReader_acquireNextImage(reader_, &image);
            if (st == AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE) {
                if (!first)
                    return true;  // took everything that was there
                if (waited_ms >= 100) {
                    if (!warned_) {
                        log("AImageReader: no image", st);
                        warned_ = true;
                    }
                    return ++missed_ < 3;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                ++waited_ms;
                continue;
            }
            first = false;
            missed_ = 0;
            if (st != AMEDIA_OK || image == nullptr) {
                log("AImageReader_acquireNextImage", st);
                return false;
            }
            VideoPicture pic;
            int64_t ns = 0;
            pic.pts_us = AImage_getTimestamp(image, &ns) == AMEDIA_OK ? ns / 1000 : fallback_pts_us;
            if (image_to_nv12(image, &pic)) {
                if (!logged_layout_) {
                    int32_t pixel = 0, row = 0;
                    AImage_getPlanePixelStride(image, 1, &pixel);
                    AImage_getPlaneRowStride(image, 1, &row);
                    __android_log_print(ANDROID_LOG_INFO, "lgp",
                                        "MediaCodec image reader: %ux%u chroma row %d pixel %d", pic.width,
                                        pic.height, int(row), int(pixel));
                    logged_layout_ = true;
                }
                out->push_back(std::move(pic));
                AImage_delete(image);
                continue;
            }
            log("AImageReader: unexpected image layout", AMEDIA_OK);
            AImage_delete(image);
            return false;
        }
    }

    void read_format() {
        AMediaFormat* f = AMediaCodec_getOutputFormat(codec_);
        if (f == nullptr)
            return;
        width_ = uint32_t(get_int(f, AMEDIAFORMAT_KEY_WIDTH, int32_t(width_)));
        height_ = uint32_t(get_int(f, AMEDIAFORMAT_KEY_HEIGHT, int32_t(height_)));
        color_ = get_int(f, AMEDIAFORMAT_KEY_COLOR_FORMAT, kColorSemiPlanar);
        stride_ = uint32_t(std::max<int32_t>(get_int(f, AMEDIAFORMAT_KEY_STRIDE, int32_t(width_)), int32_t(width_)));
        slice_height_ = uint32_t(
            std::max<int32_t>(get_int(f, AMEDIAFORMAT_KEY_SLICE_HEIGHT, int32_t(height_)), int32_t(height_)));
        // Crop rectangle (inclusive right/bottom), when the decoder gives one.
        int32_t l = 0, t = 0, r = 0, b = 0;
        if (AMediaFormat_getInt32(f, "crop-left", &l) && AMediaFormat_getInt32(f, "crop-top", &t) &&
            AMediaFormat_getInt32(f, "crop-right", &r) && AMediaFormat_getInt32(f, "crop-bottom", &b) && r > l &&
            b > t) {
            crop_x_ = uint32_t(l);
            crop_y_ = uint32_t(t);
            crop_w_ = uint32_t(r - l + 1);
            crop_h_ = uint32_t(b - t + 1);
        } else {
            crop_x_ = crop_y_ = 0;
            crop_w_ = width_;
            crop_h_ = height_;
        }
        if (color_ == kColorQcomVenusNv12) {
            // Venus NV12: stride aligned to 128, luma rows to 32 (the format
            // may report the unaligned values).
            stride_ = std::max(stride_, (width_ + 127) & ~127u);
            slice_height_ = std::max(slice_height_, (height_ + 31) & ~31u);
        }
        __android_log_print(ANDROID_LOG_INFO, "lgp", "MediaCodec output: %ux%u stride %u slice %u colour 0x%x",
                            width_, height_, stride_, slice_height_, unsigned(color_));
        AMediaFormat_delete(f);
    }

    bool convert(const uint8_t* src, size_t size, VideoPicture* pic) const {
        const uint32_t w = crop_w_ & ~1u, h = crop_h_ & ~1u;
        const size_t luma = size_t{stride_} * slice_height_;
        pic->width = w;
        pic->height = h;
        pic->nv12.resize(size_t{w} * h * 3 / 2);
        uint8_t* y = pic->nv12.data();
        uint8_t* uv = y + size_t{w} * h;
        const uint32_t cx = crop_x_ & ~1u, cy = crop_y_ & ~1u;
        switch (color_) {
        case kColorSemiPlanar:
        case kColorQcomVenusNv12: {
            if (luma + size_t{stride_} * ((cy + h) / 2) > size)
                return false;
            for (uint32_t r = 0; r < h; ++r)
                std::memcpy(y + size_t{r} * w, src + size_t{cy + r} * stride_ + cx, w);
            const uint8_t* src_uv = src + luma;
            for (uint32_t r = 0; r < h / 2; ++r)
                std::memcpy(uv + size_t{r} * w, src_uv + size_t{cy / 2 + r} * stride_ + cx, w);
            return true;
        }
        case kColorPlanar: {
            const size_t chroma_stride = stride_ / 2;
            const size_t chroma = chroma_stride * (slice_height_ / 2);
            if (luma + 2 * chroma > size)
                return false;
            for (uint32_t r = 0; r < h; ++r)
                std::memcpy(y + size_t{r} * w, src + size_t{cy + r} * stride_ + cx, w);
            const uint8_t* u = src + luma;
            const uint8_t* v = u + chroma;
            for (uint32_t r = 0; r < h / 2; ++r) {
                const size_t row = size_t{cy / 2 + r} * chroma_stride + cx / 2;
                uint8_t* dst = uv + size_t{r} * w;
                for (uint32_t c = 0; c < w / 2; ++c) {
                    dst[2 * c] = u[row + c];
                    dst[2 * c + 1] = v[row + c];
                }
            }
            return true;
        }
        default:
            return false;
        }
    }

    AMediaCodec* codec_;
    AImageReader* reader_;
    bool warned_ = false;
    bool logged_layout_ = false;
    int missed_ = 0;  // pictures in a row that never arrived
    uint32_t width_, height_, stride_, slice_height_;
    uint32_t crop_x_ = 0, crop_y_ = 0, crop_w_ = width_, crop_h_ = height_;
    int32_t color_ = kColorSemiPlanar;
    bool eos_sent_ = false;
    bool eos_seen_ = false;
};

}  // namespace

namespace {

// A started decoder for mime, rendering into window when one is given;
// nullptr if it does not configure or start.
AMediaCodec* start_codec(const char* mime, const VideoStreamInfo& info, ANativeWindow* window) {
    using Clock = std::chrono::steady_clock;
    const auto ms = [](Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); };
    const auto t0 = Clock::now();
    AMediaCodec* codec = AMediaCodec_createDecoderByType(mime);
    if (codec == nullptr)
        return nullptr;
    const auto t1 = Clock::now();
    AMediaFormat* format = AMediaFormat_new();
    AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, mime);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, int32_t(info.width));
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, int32_t(info.height));
    if (window == nullptr)
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT, kColorSemiPlanar);  // a wish, not a promise
    media_status_t st = AMediaCodec_configure(codec, format, window, nullptr, 0);
    AMediaFormat_delete(format);
    const auto t2 = Clock::now();
    if (st == AMEDIA_OK)
        st = AMediaCodec_start(codec);
    const auto t3 = Clock::now();
    // Where the time of opening a decoder goes (the movie HLE's Configure call).
    __android_log_print(ANDROID_LOG_INFO, "lgp", "MediaCodec: %s %ux%u: create %.1f ms, configure %.1f ms, start %.1f ms",
                        mime, info.width, info.height, ms(t1 - t0), ms(t2 - t1), ms(t3 - t2));
    if (st != AMEDIA_OK) {
        log(window != nullptr ? "configure/start with AImageReader" : "configure/start", st);
        AMediaCodec_delete(codec);
        return nullptr;
    }
    return codec;
}

}  // namespace

std::unique_ptr<IVideoDecoder> MediaCodecVideo::open_decoder(const VideoStreamInfo& info) {
    const char* mime = info.codec == VideoCodec::Vp9 ? "video/x-vnd.on2.vp9" : "video/avc";
    if (info.codec == VideoCodec::H264)
        return nullptr;  // avcC -> csd-0/csd-1 is not done (LGP's videos are VP9)
    if (image_reader_enabled()) {
        // Four images: the one being copied plus the codec's queue.
        AImageReader* reader = nullptr;
        ANativeWindow* window = nullptr;
        const media_status_t rs =
            AImageReader_new(int32_t(info.width), int32_t(info.height), AIMAGE_FORMAT_YUV_420_888, 4, &reader);
        if (rs == AMEDIA_OK && AImageReader_getWindow(reader, &window) == AMEDIA_OK) {
            if (AMediaCodec* codec = start_codec(mime, info, window))
                return std::make_unique<MediaCodecDecoder>(codec, info.width, info.height, reader);
        } else {
            log("AImageReader_new", rs);
        }
        if (reader != nullptr)
            AImageReader_delete(reader);
        // Falls back to the decoder's own buffers.
    }
    AMediaCodec* codec = start_codec(mime, info, nullptr);
    if (codec == nullptr)
        return nullptr;
    return std::make_unique<MediaCodecDecoder>(codec, info.width, info.height);
}

}  // namespace lgp::platform::android
