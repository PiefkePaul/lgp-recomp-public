// Helpers for IVideo backends (P5/P6): pictures in the layouts decoders hand
// out (planar YUV, NV12, packed RGB) to the tightly packed NV12 the runtime
// expects (lgp/platform/video.hpp), plus keyframe detection on compressed
// frames for decoders that need the chunk type (WebCodecs). Pure functions,
// no host dependency, so they are tested natively.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "lgp/platform/video.hpp"

namespace lgp::platform {

// Planar 8-bit YUV with chroma subsampled by 2^cw_shift horizontally and
// 2^ch_shift vertically (I420: 1,1; I422: 1,0; I444: 0,0). width/height are
// rounded down to even values; chroma of 4:2:2/4:4:4 is averaged to 4:2:0.
void planar_to_nv12(const uint8_t* y, size_t y_stride, const uint8_t* u, const uint8_t* v, size_t c_stride,
                    unsigned cw_shift, unsigned ch_shift, uint32_t width, uint32_t height, VideoPicture* out);

// NV12 with strides (Y plane, interleaved CbCr plane) to tightly packed NV12.
void nv12_copy(const uint8_t* y, size_t y_stride, const uint8_t* uv, size_t uv_stride, uint32_t width,
               uint32_t height, VideoPicture* out);

// 8-bit YUV 4:2:0 in Android's flexible layout (YUV_420_888, AImage planes):
// separate Cb and Cr pointers that share a row stride and a pixel stride.
// Covers planar I420 (pixel stride 1), NV12 (2, cr == cb + 1) and NV21 (2,
// cb == cr + 1) and any padding; nothing about the layout is assumed beyond
// what the strides say. The caller offsets the pointers to the crop origin
// (an even position).
void yuv420_flexible_to_nv12(const uint8_t* y, size_t y_stride, const uint8_t* cb, const uint8_t* cr,
                             size_t c_stride, size_t c_pixel_stride, uint32_t width, uint32_t height,
                             VideoPicture* out);

enum class YuvMatrix { Bt601, Bt709 };

// Packed 8-bit RGB with 4 bytes per pixel (RGBA/RGBX or, with bgr, BGRA/BGRX;
// the fourth byte is ignored) to NV12 with the given matrix, limited range
// unless full_range. Chroma is the average of each 2x2 block.
void rgb32_to_nv12(const uint8_t* px, size_t stride, bool bgr, YuvMatrix matrix, bool full_range, uint32_t width,
                   uint32_t height, VideoPicture* out);

// True if the compressed frame is a keyframe a decoder can start from:
// VP9 (uncompressed header; a superframe starts with its first frame) or
// H.264 in AVCC form (length-prefixed NAL units, nal_length bytes each, an
// IDR slice inside). False for anything it cannot parse.
bool is_keyframe(VideoCodec codec, const uint8_t* data, size_t size, unsigned nal_length = 4);

// The VP9 profile (0..3) from a frame's uncompressed header; -1 if invalid.
int vp9_profile(const uint8_t* data, size_t size);

// NAL length size from an avcC record (lengthSizeMinusOne + 1); 4 if the
// record is too short.
unsigned avcc_nal_length(const std::vector<uint8_t>& avcc);

}  // namespace lgp::platform
