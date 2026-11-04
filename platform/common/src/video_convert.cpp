#include "lgp/platform/video_convert.hpp"

#include <algorithm>
#include <cstring>

namespace lgp::platform {
namespace {

void prepare(uint32_t width, uint32_t height, VideoPicture* out) {
    out->width = width & ~1u;
    out->height = height & ~1u;
    out->nv12.resize(size_t{out->width} * out->height * 3 / 2);
}

uint8_t clamp8(int v) {
    return static_cast<uint8_t>(std::clamp(v, 0, 255));
}

}  // namespace

void planar_to_nv12(const uint8_t* y, size_t y_stride, const uint8_t* u, const uint8_t* v, size_t c_stride,
                    unsigned cw_shift, unsigned ch_shift, uint32_t width, uint32_t height, VideoPicture* out) {
    prepare(width, height, out);
    const uint32_t w = out->width, h = out->height;
    uint8_t* dy = out->nv12.data();
    uint8_t* duv = dy + size_t{w} * h;
    for (uint32_t r = 0; r < h; ++r)
        std::memcpy(dy + size_t{r} * w, y + r * y_stride, w);
    // Source chroma samples per 4:2:0 sample in each direction (1 or 2).
    const uint32_t nx = 2u >> cw_shift, ny = 2u >> ch_shift;
    for (uint32_t r = 0; r < h / 2; ++r) {
        uint8_t* dst = duv + size_t{r} * w;
        for (uint32_t c = 0; c < w / 2; ++c) {
            unsigned su = 0, sv = 0;
            for (uint32_t j = 0; j < ny; ++j) {
                const size_t row = size_t{r * ny + j} * c_stride;
                for (uint32_t i = 0; i < nx; ++i) {
                    su += u[row + c * nx + i];
                    sv += v[row + c * nx + i];
                }
            }
            const unsigned n = nx * ny;
            dst[2 * c] = uint8_t((su + n / 2) / n);
            dst[2 * c + 1] = uint8_t((sv + n / 2) / n);
        }
    }
}

void nv12_copy(const uint8_t* y, size_t y_stride, const uint8_t* uv, size_t uv_stride, uint32_t width,
               uint32_t height, VideoPicture* out) {
    prepare(width, height, out);
    const uint32_t w = out->width, h = out->height;
    uint8_t* dy = out->nv12.data();
    uint8_t* duv = dy + size_t{w} * h;
    for (uint32_t r = 0; r < h; ++r)
        std::memcpy(dy + size_t{r} * w, y + r * y_stride, w);
    for (uint32_t r = 0; r < h / 2; ++r)
        std::memcpy(duv + size_t{r} * w, uv + r * uv_stride, w);
}

void yuv420_flexible_to_nv12(const uint8_t* y, size_t y_stride, const uint8_t* cb, const uint8_t* cr,
                             size_t c_stride, size_t c_pixel_stride, uint32_t width, uint32_t height,
                             VideoPicture* out) {
    prepare(width, height, out);
    const uint32_t w = out->width, h = out->height;
    uint8_t* dy = out->nv12.data();
    uint8_t* duv = dy + size_t{w} * h;
    for (uint32_t r = 0; r < h; ++r)
        std::memcpy(dy + size_t{r} * w, y + r * y_stride, w);
    const bool nv12 = c_pixel_stride == 2 && cr == cb + 1;
    for (uint32_t r = 0; r < h / 2; ++r) {
        uint8_t* dst = duv + size_t{r} * w;
        const uint8_t* u = cb + r * c_stride;
        const uint8_t* v = cr + r * c_stride;
        if (nv12) {
            std::memcpy(dst, u, w);  // already interleaved CbCr
            continue;
        }
        for (uint32_t c = 0; c < w / 2; ++c) {
            dst[2 * c] = u[c * c_pixel_stride];
            dst[2 * c + 1] = v[c * c_pixel_stride];
        }
    }
}

void rgb32_to_nv12(const uint8_t* px, size_t stride, bool bgr, YuvMatrix matrix, bool full_range, uint32_t width,
                   uint32_t height, VideoPicture* out) {
    prepare(width, height, out);
    const uint32_t w = out->width, h = out->height;
    uint8_t* dy = out->nv12.data();
    uint8_t* duv = dy + size_t{w} * h;
    // Luma/chroma weights (x 2^16) of the two matrices.
    const double kr = matrix == YuvMatrix::Bt709 ? 0.2126 : 0.299;
    const double kb = matrix == YuvMatrix::Bt709 ? 0.0722 : 0.114;
    const double kg = 1.0 - kr - kb;
    const double ys = full_range ? 1.0 : 219.0 / 255.0, cs = full_range ? 1.0 : 224.0 / 255.0;
    const int yo = full_range ? 0 : 16;
    const int wr = int(kr * ys * 65536 + 0.5), wg = int(kg * ys * 65536 + 0.5), wb = int(kb * ys * 65536 + 0.5);
    // Cb = (B - Y) / (2 (1 - kb)), Cr = (R - Y) / (2 (1 - kr)), scaled to the range.
    const double cb_s = cs / (2 * (1 - kb)), cr_s = cs / (2 * (1 - kr));
    const int ur = int(-kr * cb_s * 65536), ug = int(-kg * cb_s * 65536), ub = int((1 - kb) * cb_s * 65536);
    const int vr = int((1 - kr) * cr_s * 65536), vg = int(-kg * cr_s * 65536), vb = int(-kb * cr_s * 65536);
    const int ri = bgr ? 2 : 0, bi = bgr ? 0 : 2;
    for (uint32_t r = 0; r < h; ++r) {
        const uint8_t* s = px + r * stride;
        uint8_t* d = dy + size_t{r} * w;
        for (uint32_t c = 0; c < w; ++c, s += 4)
            d[c] = clamp8(yo + ((wr * s[ri] + wg * s[1] + wb * s[bi] + 32768) >> 16));
    }
    for (uint32_t r = 0; r < h / 2; ++r) {
        const uint8_t* s0 = px + size_t{2 * r} * stride;
        const uint8_t* s1 = s0 + stride;
        uint8_t* d = duv + size_t{r} * w;
        for (uint32_t c = 0; c < w / 2; ++c) {
            const uint8_t* a = s0 + 8 * c;
            const uint8_t* b = s1 + 8 * c;
            const int R = a[ri] + a[4 + ri] + b[ri] + b[4 + ri];
            const int G = a[1] + a[5] + b[1] + b[5];
            const int B = a[bi] + a[4 + bi] + b[bi] + b[4 + bi];
            // Sums of four pixels: divide by 4 together with the 2^16 scale.
            d[2 * c] = clamp8(128 + ((ur * R + ug * G + ub * B + (1 << 17)) >> 18));
            d[2 * c + 1] = clamp8(128 + ((vr * R + vg * G + vb * B + (1 << 17)) >> 18));
        }
    }
}

int vp9_profile(const uint8_t* data, size_t size) {
    if (data == nullptr || size == 0 || (data[0] >> 6) != 2)  // frame_marker
        return -1;
    return ((data[0] >> 5) & 1) | (((data[0] >> 4) & 1) << 1);
}

bool is_keyframe(VideoCodec codec, const uint8_t* data, size_t size, unsigned nal_length) {
    if (data == nullptr || size == 0)
        return false;
    if (codec == VideoCodec::Vp9) {
        const int profile = vp9_profile(data, size);
        if (profile < 0)
            return false;
        // After marker and profile bits (and profile 3's reserved zero):
        // show_existing_frame, then frame_type (0 = KEY_FRAME).
        const int bit = profile == 3 ? 2 : 3;
        if ((data[0] >> bit) & 1)
            return false;
        return ((data[0] >> (bit - 1)) & 1) == 0;
    }
    if (nal_length < 1 || nal_length > 4)
        return false;
    for (size_t pos = 0; pos + nal_length < size;) {
        size_t len = 0;
        for (unsigned i = 0; i < nal_length; ++i)
            len = (len << 8) | data[pos + i];
        pos += nal_length;
        if (len == 0 || len > size - pos)
            return false;
        if ((data[pos] & 0x1f) == 5)  // coded slice of an IDR picture
            return true;
        pos += len;
    }
    return false;
}

unsigned avcc_nal_length(const std::vector<uint8_t>& avcc) {
    return avcc.size() >= 5 ? (avcc[4] & 3u) + 1 : 4;
}

}  // namespace lgp::platform
