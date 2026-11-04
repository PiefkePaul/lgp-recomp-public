// WebGPU renderer for the NVN-HLE (webgpu_backend.hpp).
#include "lgp/gpu/webgpu/webgpu_backend.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <string>
#include <functional>
#include <cinttypes>
#include <limits>
#include <list>
#include <map>
#include <mutex>
#include <numeric>
#include <set>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "lgp/gpu/nvn/hle.hpp"
#include "lgp/gpu/shader/wgsl.h"
#include "lgp/gpu/texture/astc.hpp"
#include "lgp/gpu/texture/bc.hpp"
#include "lgp/gpu/texture/parallel.hpp"
#include "lgp/gpu/webgpu/pipeline_cache.hpp"
#include "lgp/gpu/webgpu/poll_watch.hpp"
#include "lgp/platform/host_env.hpp"
#include "lgp/platform/host_thread.hpp"
#include "lgp/runtime/log.hpp"

#if defined(__APPLE__)
#include <TargetConditionals.h>  // TARGET_OS_IPHONE
#endif
#if defined(__EMSCRIPTEN__)
#include <emscripten/eventloop.h>  // emscripten_set_timeout (async pipeline wait)
#else
#include <webgpu/wgpu.h>  // wgpuGetVersion
#endif
// The driver's own pipeline cache (VkPipelineCache), kept between runs. Only
// a wgpu-native built with tools/webgpu/wgpu-native-pipeline-cache.patch has
// it (WGPU_LGP_PIPELINE_CACHE); the releases have no API for it.
#if defined(WGPU_LGP_PIPELINE_CACHE)
#define LGP_DRIVER_PIPELINE_CACHE 1
#else
#define LGP_DRIVER_PIPELINE_CACHE 0
#endif

namespace lgp::gpu::webgpu {

namespace pw = platform::webgpu;
using nvn::Format;

namespace {

// Clears that do not cover the whole target (scissor, channel mask, partial
// stencil mask) are drawn: a full-screen triangle, clipped by the scissor
// rectangle, writes the clear colour or depth.
constexpr const char* kClearWgsl = R"(
struct Clear {
    color: vec4f,
    depth: f32,
};
@group(0) @binding(0) var<uniform> c: Clear;

@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {
    let uv = vec2f(f32((i << 1u) & 2u), f32(i & 2u));
    return vec4f(uv * 2.0 - 1.0, c.depth, 1.0);
}

@fragment fn fs() -> @location(0) vec4f {
    return c.color;
}
)";

// Texture-to-texture blit (scaled/mirrored copies): one triangle covering the
// viewport (= destination region), sampling the source region at a level.
constexpr const char* kBlitWgsl = R"(
struct Blit {
    src: vec4f,  // u0, v0, u1, v1 (normalized, may be mirrored)
    lod: f32,
};
@group(0) @binding(0) var t: texture_2d<f32>;
@group(0) @binding(1) var s: sampler;
@group(0) @binding(2) var<uniform> b: Blit;

struct Out {
    @builtin(position) pos: vec4f,
    @location(0) uv: vec2f,
};

@vertex fn vs(@builtin(vertex_index) i: u32) -> Out {
    let k = vec2f(f32((i << 1u) & 2u), f32(i & 2u));  // (0,0) (2,0) (0,2)
    var o: Out;
    o.pos = vec4f(k.x * 2.0 - 1.0, 1.0 - k.y * 2.0, 0.0, 1.0);
    o.uv = k;
    return o;
}

@fragment fn fs(in: Out) -> @location(0) vec4f {
    return textureSampleLevel(t, s, mix(b.src.xy, b.src.zw, in.uv), b.lod);
}
)";

// "0x263baa93, ABC" -> {"263baa93", "abc"}: hex prefixes of program hashes
// (as in "gpu time by draw program" and pipeline dumps).
std::vector<std::string> parse_hash_prefixes(const std::string& list) {
    std::vector<std::string> out;
    std::string item;
    for (size_t i = 0; i <= list.size(); ++i) {
        const char c = i < list.size() ? list[i] : ',';
        if (c == ',') {
            if (item.rfind("0x", 0) == 0) item.erase(0, 2);
            if (!item.empty() && item.size() <= 16 &&
                item.find_first_not_of("0123456789abcdef") == std::string::npos)
                out.push_back(item);
            item.clear();
        } else if (c != ' ') {
            item += char(std::tolower(static_cast<unsigned char>(c)));
        }
    }
    return out;
}

// Ids of the half-resolution stand-in targets (State::halfres_target), from
// here down; the HLE numbers textures upwards from 1.
constexpr nvn::TextureId kHalfResTextureBase = 0xFFFFF000u;

// Whether the blit pipeline (float texture, filtering sampler) can read the format.
bool blit_filterable(WGPUTextureFormat f) {
    switch (f) {
    case WGPUTextureFormat_R8Uint: case WGPUTextureFormat_R8Sint: case WGPUTextureFormat_R16Uint:
    case WGPUTextureFormat_R16Sint: case WGPUTextureFormat_RG8Uint: case WGPUTextureFormat_RG8Sint:
    case WGPUTextureFormat_R32Uint: case WGPUTextureFormat_R32Sint: case WGPUTextureFormat_RG16Uint:
    case WGPUTextureFormat_RG16Sint: case WGPUTextureFormat_RGBA8Uint: case WGPUTextureFormat_RGBA8Sint:
    case WGPUTextureFormat_RGB10A2Uint: case WGPUTextureFormat_RG32Uint: case WGPUTextureFormat_RG32Sint:
    case WGPUTextureFormat_RGBA16Uint: case WGPUTextureFormat_RGBA16Sint: case WGPUTextureFormat_RGBA32Uint:
    case WGPUTextureFormat_RGBA32Sint: case WGPUTextureFormat_R32Float: case WGPUTextureFormat_RG32Float:
    case WGPUTextureFormat_RGBA32Float:
        return false;
    default:
        return true;
    }
}

struct BlitUniform {
    float src[4];
    float lod;
    float pad[3];
};

struct ClearUniform {
    float color[4];
    float depth;
    float pad[3];
};
constexpr uint64_t kUniformStride = 256;  // minUniformBufferOffsetAlignment default

bool has_depth(WGPUTextureFormat f) {
    return f == WGPUTextureFormat_Depth16Unorm || f == WGPUTextureFormat_Depth24Plus ||
           f == WGPUTextureFormat_Depth24PlusStencil8 || f == WGPUTextureFormat_Depth32Float ||
           f == WGPUTextureFormat_Depth32FloatStencil8;
}
bool has_stencil(WGPUTextureFormat f) {
    return f == WGPUTextureFormat_Stencil8 || f == WGPUTextureFormat_Depth24PlusStencil8 ||
           f == WGPUTextureFormat_Depth32FloatStencil8;
}

uint32_t bytes_per_texel(WGPUTextureFormat f) {
    switch (f) {
    case WGPUTextureFormat_RGBA8Unorm:
    case WGPUTextureFormat_RGBA8UnormSrgb:
    case WGPUTextureFormat_BGRA8Unorm:
    case WGPUTextureFormat_RGB10A2Unorm: return 4;
    default: return 0;
    }
}

bool is_bc(WGPUTextureFormat f) { return f >= WGPUTextureFormat_BC1RGBAUnorm && f <= WGPUTextureFormat_BC7RGBAUnormSrgb; }

// NVN ASTC formats in NVN order (4x4, 5x4, ..., 12x12), linear then sRGB.
constexpr WGPUTextureFormat kAstcFormats[2][14] = {
    {WGPUTextureFormat_ASTC4x4Unorm, WGPUTextureFormat_ASTC5x4Unorm, WGPUTextureFormat_ASTC5x5Unorm,
     WGPUTextureFormat_ASTC6x5Unorm, WGPUTextureFormat_ASTC6x6Unorm, WGPUTextureFormat_ASTC8x5Unorm,
     WGPUTextureFormat_ASTC8x6Unorm, WGPUTextureFormat_ASTC8x8Unorm, WGPUTextureFormat_ASTC10x5Unorm,
     WGPUTextureFormat_ASTC10x6Unorm, WGPUTextureFormat_ASTC10x8Unorm, WGPUTextureFormat_ASTC10x10Unorm,
     WGPUTextureFormat_ASTC12x10Unorm, WGPUTextureFormat_ASTC12x12Unorm},
    {WGPUTextureFormat_ASTC4x4UnormSrgb, WGPUTextureFormat_ASTC5x4UnormSrgb, WGPUTextureFormat_ASTC5x5UnormSrgb,
     WGPUTextureFormat_ASTC6x5UnormSrgb, WGPUTextureFormat_ASTC6x6UnormSrgb, WGPUTextureFormat_ASTC8x5UnormSrgb,
     WGPUTextureFormat_ASTC8x6UnormSrgb, WGPUTextureFormat_ASTC8x8UnormSrgb, WGPUTextureFormat_ASTC10x5UnormSrgb,
     WGPUTextureFormat_ASTC10x6UnormSrgb, WGPUTextureFormat_ASTC10x8UnormSrgb, WGPUTextureFormat_ASTC10x10UnormSrgb,
     WGPUTextureFormat_ASTC12x10UnormSrgb, WGPUTextureFormat_ASTC12x12UnormSrgb},
};
constexpr uint32_t kNvnAstcFirst = uint32_t(nvn::Format::ASTC_4x4), kNvnAstcLast = uint32_t(nvn::Format::ASTC_12x12_SRGB);
bool is_nvn_astc(uint32_t f) { return f >= kNvnAstcFirst && f <= kNvnAstcLast; }
bool is_astc(WGPUTextureFormat f) {
    for (const auto& row : kAstcFormats)
        for (WGPUTextureFormat a : row)
            if (a == f) return true;
    return false;
}
bool is_compressed(WGPUTextureFormat f) { return is_bc(f) || is_astc(f); }

// BCn the CPU decoder covers (all but BC6H), with the RGBA8 host format of the
// decoded texels; false for other formats.
bool bc_cpu_format(WGPUTextureFormat f, lgp::gpu::texture::BcFormat* bc, WGPUTextureFormat* host) {
    using lgp::gpu::texture::BcFormat;
    switch (f) {
    case WGPUTextureFormat_BC1RGBAUnorm: *bc = BcFormat::BC1, *host = WGPUTextureFormat_RGBA8Unorm; return true;
    case WGPUTextureFormat_BC1RGBAUnormSrgb: *bc = BcFormat::BC1, *host = WGPUTextureFormat_RGBA8UnormSrgb; return true;
    case WGPUTextureFormat_BC2RGBAUnorm: *bc = BcFormat::BC2, *host = WGPUTextureFormat_RGBA8Unorm; return true;
    case WGPUTextureFormat_BC2RGBAUnormSrgb: *bc = BcFormat::BC2, *host = WGPUTextureFormat_RGBA8UnormSrgb; return true;
    case WGPUTextureFormat_BC3RGBAUnorm: *bc = BcFormat::BC3, *host = WGPUTextureFormat_RGBA8Unorm; return true;
    case WGPUTextureFormat_BC3RGBAUnormSrgb: *bc = BcFormat::BC3, *host = WGPUTextureFormat_RGBA8UnormSrgb; return true;
    // One- and two-channel formats keep their channels (a quarter or half of
    // RGBA8's memory; sampling gives the same (r, 0, 0, 1) and (r, g, 0, 1)).
    case WGPUTextureFormat_BC4RUnorm: *bc = BcFormat::BC4, *host = WGPUTextureFormat_R8Unorm; return true;
    case WGPUTextureFormat_BC4RSnorm: *bc = BcFormat::BC4Snorm, *host = WGPUTextureFormat_R8Snorm; return true;
    case WGPUTextureFormat_BC5RGUnorm: *bc = BcFormat::BC5, *host = WGPUTextureFormat_RG8Unorm; return true;
    case WGPUTextureFormat_BC5RGSnorm: *bc = BcFormat::BC5Snorm, *host = WGPUTextureFormat_RG8Snorm; return true;
    case WGPUTextureFormat_BC7RGBAUnorm: *bc = BcFormat::BC7, *host = WGPUTextureFormat_RGBA8Unorm; return true;
    case WGPUTextureFormat_BC7RGBAUnormSrgb: *bc = BcFormat::BC7, *host = WGPUTextureFormat_RGBA8UnormSrgb; return true;
    default: return false;
    }
}

// Bytes per texel of the host texture bc_cpu_format() picks.
uint32_t bc_host_texel_bytes(lgp::gpu::texture::BcFormat f) {
    using lgp::gpu::texture::BcFormat;
    return f == BcFormat::BC4 || f == BcFormat::BC4Snorm ? 1 : f == BcFormat::BC5 || f == BcFormat::BC5Snorm ? 2 : 4;
}

// BCn blocks (rows of ceil(width / 4) blocks, `stride` bytes apart) -> tightly
// packed texels of a width x height region, bc_host_texel_bytes() each.
// pool: helpers for large levels (LGP_WEBGPU_DECODE_THREADS), or nullptr.
void decode_bc_rows(lgp::gpu::texture::BcFormat format, const uint8_t* blocks, uint32_t stride, uint32_t width,
                    uint32_t height, std::vector<uint8_t>* out, lgp::gpu::texture::DecodePool* pool) {
    const size_t block = lgp::gpu::texture::bc_block_bytes(format);
    const uint32_t bx = (width + 3) / 4, by = (height + 3) / 4;
    std::vector<uint8_t> packed;
    const uint8_t* src = blocks;
    if (stride != bx * block) {
        packed.resize(size_t(bx) * by * block);
        for (uint32_t y = 0; y < by; ++y)
            std::memcpy(packed.data() + size_t(y) * bx * block, blocks + size_t(y) * stride, size_t(bx) * block);
        src = packed.data();
    }
    out->resize(size_t(width) * height * 4);
    lgp::gpu::texture::decode_bc_image(format, src, width, height, out->data(), pool);
    const uint32_t n = bc_host_texel_bytes(format);
    if (n < 4) {
        const size_t texels = size_t(width) * height;
        uint8_t* p = out->data();
        for (size_t i = 0; i < texels; ++i)
            for (uint32_t c = 0; c < n; ++c)
                p[i * n + c] = p[i * 4 + c];
        out->resize(texels * n);
    }
}

// ASTC blocks (rows of block_x blocks, `stride` bytes apart) -> tightly
// packed RGBA8 texels of a width x height region. Returns the error blocks.
size_t decode_astc_rows(const nvn::FormatInfo& info, const uint8_t* blocks, uint32_t stride, uint32_t width,
                        uint32_t height, std::vector<uint8_t>* out) {
    const uint32_t bx = (width + info.block_width - 1) / info.block_width;
    const uint32_t by = (height + info.block_height - 1) / info.block_height;
    std::vector<uint8_t> packed;
    const uint8_t* src = blocks;
    if (stride != bx * 16) {
        packed.resize(size_t(bx) * by * 16);
        for (uint32_t y = 0; y < by; ++y)
            std::memcpy(packed.data() + size_t(y) * bx * 16, blocks + size_t(y) * stride, size_t(bx) * 16);
        src = packed.data();
    }
    out->resize(size_t(width) * height * 4);
    return lgp::gpu::texture::decode_astc_image(src, width, height, info.block_width, info.block_height, info.srgb,
                                                out->data());
}

// 16-bit packed colour (NVN component order, R in the low bits) -> RGBA8.
// Formats without alpha read as opaque.
void widen_16bit(uint32_t nvn_format, const std::vector<uint8_t>& in, std::vector<uint8_t>* out) {
    using nvn::Format;
    const size_t n = in.size() / 2;
    out->resize(n * 4);
    auto expand = [](uint32_t v, int bits) { return uint8_t((v * 255u + ((1u << bits) - 1) / 2) / ((1u << bits) - 1)); };
    for (size_t i = 0; i < n; ++i) {
        const uint32_t v = uint32_t(in[2 * i]) | uint32_t(in[2 * i + 1]) << 8;
        uint8_t* o = out->data() + 4 * i;
        switch (static_cast<Format>(nvn_format)) {
        case Format::RGBA4:
            o[0] = expand(v & 15, 4), o[1] = expand(v >> 4 & 15, 4), o[2] = expand(v >> 8 & 15, 4), o[3] = expand(v >> 12, 4);
            break;
        case Format::RGB5:
        case Format::RGB5A1:
            o[0] = expand(v & 31, 5), o[1] = expand(v >> 5 & 31, 5), o[2] = expand(v >> 10 & 31, 5);
            o[3] = nvn_format == uint32_t(Format::RGB5A1) ? uint8_t((v >> 15) * 255) : 255;
            break;
        case Format::RGB565:
            o[0] = expand(v & 31, 5), o[1] = expand(v >> 5 & 63, 6), o[2] = expand(v >> 11, 5), o[3] = 255;
            break;
        default:  // BGR565: B in the low bits
            o[2] = expand(v & 31, 5), o[1] = expand(v >> 5 & 63, 6), o[0] = expand(v >> 11, 5), o[3] = 255;
            break;
        }
    }
}

bool is_widened_16bit(uint32_t f) {
    using nvn::Format;
    return f == uint32_t(Format::RGBA4) || f == uint32_t(Format::RGB5) || f == uint32_t(Format::RGB5A1) ||
           f == uint32_t(Format::RGB565) || f == uint32_t(Format::BGR565);
}

// 16-bit normalized formats (NVN R16 6, R16SN 7, RG16 18, RG16SN 19, RGBA16 42,
// RGBA16SN 43): WebGPU core has none of them. They live in 16-bit float
// textures of the same texel size (about 11 bits of precision, renderable and
// blendable), and texels coming from the guest are converted.
bool is_norm16(uint32_t f) { return f == 6 || f == 7 || f == 18 || f == 19 || f == 42 || f == 43; }

uint16_t float_to_half(float v) {
    uint32_t x;
    std::memcpy(&x, &v, 4);
    const uint32_t sign = (x >> 16) & 0x8000;
    const int32_t exp = int32_t((x >> 23) & 0xFF) - 127 + 15;
    uint32_t mant = x & 0x7FFFFF;
    if (exp <= 0) {  // subnormal or zero
        if (exp < -10)
            return uint16_t(sign);
        mant |= 0x800000;
        const uint32_t shift = uint32_t(14 - exp);
        uint32_t h = mant >> shift;
        if ((mant >> (shift - 1)) & 1)  // round half up
            ++h;
        return uint16_t(sign | h);
    }
    if (exp >= 31)
        return uint16_t(sign | 0x7C00);
    uint32_t h = sign | uint32_t(exp) << 10 | mant >> 13;
    if (mant & 0x1000)  // round half up (carries into the exponent correctly)
        ++h;
    return uint16_t(h);
}

// In place: n bytes of norm16 components -> half floats.
void norm16_to_half(uint32_t f, uint8_t* p, size_t n) {
    const bool snorm = f == 7 || f == 19 || f == 43;
    for (size_t i = 0; i + 2 <= n; i += 2) {
        const uint16_t raw = uint16_t(p[i] | p[i + 1] << 8);
        const float v = snorm ? std::max(float(int16_t(raw)) / 32767.0f, -1.0f) : float(raw) / 65535.0f;
        const uint16_t h = float_to_half(v);
        p[i] = uint8_t(h);
        p[i + 1] = uint8_t(h >> 8);
    }
}

// Cheap content fingerprint (64-bit words; the tail bytes count too).
// Four independent lanes: the multiply chains overlap (about twice the
// speed of one lane; texture checks hash megabytes per frame). Only
// compared within a run.
uint64_t fingerprint(const uint8_t* p, size_t n) {
    uint64_t a = 0xCBF29CE484222325ull ^ n, b = 0x9E3779B97F4A7C15ull, c = 0xC2B2AE3D27D4EB4Full,
             d = 0x165667B19E3779F9ull;
    size_t i = 0;
    for (; i + 32 <= n; i += 32) {
        uint64_t w[4];
        std::memcpy(w, p + i, 32);
        a = (a ^ w[0]) * 0x100000001B3ull;
        b = (b ^ w[1]) * 0x100000001B3ull;
        c = (c ^ w[2]) * 0x100000001B3ull;
        d = (d ^ w[3]) * 0x100000001B3ull;
        a ^= a >> 29;
        b ^= b >> 29;
        c ^= c >> 29;
        d ^= d >> 29;
    }
    uint64_t h = a ^ (b * 3) ^ (c * 5) ^ (d * 7);
    for (; i < n; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

struct Region {
    uint32_t x = 0, y = 0, width = 0, height = 0;  // framebuffer space, row 0 = first row in memory
    bool full = true;
};

}  // namespace

WGPUTextureFormat texture_format(uint32_t nvn_format) {
    switch (static_cast<Format>(nvn_format)) {
    case Format::R8: return WGPUTextureFormat_R8Unorm;
    case Format::RG8: return WGPUTextureFormat_RG8Unorm;
    case Format::RGBA8: return WGPUTextureFormat_RGBA8Unorm;
    case Format::RGBA16F: return WGPUTextureFormat_RGBA16Float;
    case Format::RGBA32F: return WGPUTextureFormat_RGBA32Float;
    case Format::RGBX8_SRGB:  // X: alpha is undefined, storing it is harmless
    case Format::RGBA8_SRGB: return WGPUTextureFormat_RGBA8UnormSrgb;
    case Format::RGB10A2: return WGPUTextureFormat_RGB10A2Unorm;
    case Format::R11G11B10F: return WGPUTextureFormat_RG11B10Ufloat;
    case Format::BGRA8:
    case Format::BGRX8: return WGPUTextureFormat_BGRA8Unorm;
    case Format::BGRA8_SRGB:
    case Format::BGRX8_SRGB: return WGPUTextureFormat_BGRA8UnormSrgb;
    // 16-bit packed colours: WebGPU has none of them; uploads widen to RGBA8.
    case Format::RGBA4:
    case Format::RGB5:
    case Format::RGB5A1:
    case Format::RGB565:
    case Format::BGR565: return WGPUTextureFormat_RGBA8Unorm;
    // BCn (needs the texture-compression-bc feature, checked at creation).
    case Format::RGB_DXT1:
    case Format::RGBA_DXT1: return WGPUTextureFormat_BC1RGBAUnorm;
    case Format::RGB_DXT1_SRGB:
    case Format::RGBA_DXT1_SRGB: return WGPUTextureFormat_BC1RGBAUnormSrgb;
    case Format::RGBA_DXT3: return WGPUTextureFormat_BC2RGBAUnorm;
    case Format::RGBA_DXT3_SRGB: return WGPUTextureFormat_BC2RGBAUnormSrgb;
    case Format::RGBA_DXT5: return WGPUTextureFormat_BC3RGBAUnorm;
    case Format::RGBA_DXT5_SRGB: return WGPUTextureFormat_BC3RGBAUnormSrgb;
    case Format::RGTC1_UNORM: return WGPUTextureFormat_BC4RUnorm;
    case Format::RGTC1_SNORM: return WGPUTextureFormat_BC4RSnorm;
    case Format::RGTC2_UNORM: return WGPUTextureFormat_BC5RGUnorm;
    case Format::RGTC2_SNORM: return WGPUTextureFormat_BC5RGSnorm;
    case Format::BPTC_UNORM: return WGPUTextureFormat_BC7RGBAUnorm;
    case Format::BPTC_UNORM_SRGB: return WGPUTextureFormat_BC7RGBAUnormSrgb;
    case Format::BPTC_SFLOAT: return WGPUTextureFormat_BC6HRGBFloat;
    case Format::BPTC_UFLOAT: return WGPUTextureFormat_BC6HRGBUfloat;
    case Format::Stencil8: return WGPUTextureFormat_Stencil8;
    case Format::Depth16: return WGPUTextureFormat_Depth16Unorm;
    case Format::Depth24: return WGPUTextureFormat_Depth24Plus;
    case Format::Depth32F: return WGPUTextureFormat_Depth32Float;
    case Format::Depth24Stencil8: return WGPUTextureFormat_Depth24PlusStencil8;
    // depth32float-stencil8 is an optional WebGPU feature; 24-bit depth
    // loses precision but keeps the stencil. Revisit if the game uses it.
    case Format::Depth32FStencil8: return WGPUTextureFormat_Depth24PlusStencil8;
    case Format::None: break;
    default: break;
    }
    // Formats without an enum name above (public NVN order, sizes from the
    // driver's table in nvn::format_info). Integer formats stay unsupported:
    // the translated shaders sample and write floats.
    switch (nvn_format) {
    case 2: return WGPUTextureFormat_R8Snorm;
    case 5: return WGPUTextureFormat_R16Float;
    case 10: return WGPUTextureFormat_R32Float;
    case 14: return WGPUTextureFormat_RG8Snorm;
    case 17: return WGPUTextureFormat_RG16Float;
    case 22: return WGPUTextureFormat_RG32Float;
    case 38: return WGPUTextureFormat_RGBA8Snorm;
    case 6:  // R16 / R16SN as float (is_norm16)
    case 7: return WGPUTextureFormat_R16Float;
    case 18:  // RG16 / RG16SN
    case 19: return WGPUTextureFormat_RG16Float;
    case 42:  // RGBA16 / RGBA16SN
    case 43: return WGPUTextureFormat_RGBA16Float;
    default: break;
    }
    // ASTC (needs texture-compression-astc, checked at creation; otherwise
    // decoded on the CPU).
    if (is_nvn_astc(nvn_format)) {
        const uint32_t i = nvn_format - kNvnAstcFirst;
        return kAstcFormats[i / 14][i % 14];
    }
    return WGPUTextureFormat_Undefined;
}

// Draws: vertex formats, topologies, the pipeline a draw needs.
namespace {

constexpr uint64_t kCbufBytes = 65536;  // array<vec4<u32>, 4096> in the generated WGSL
constexpr size_t kMaxCachedBindGroups = 4096;  // texture bind groups kept across lists
constexpr uint32_t kMaxVertexBuffers = 8;  // WebGPU's guaranteed maxVertexBuffers (the device may allow more)

// Shader or pipeline creation slower than this is logged (stutter on the
// first use of an effect; docs/knowledge/webgpu-backend-draw-kosten.md).
constexpr uint64_t kSlowCreateUs = 2000;
// Pipeline cache: time per present for creating cached programs ahead.
constexpr uint64_t kWarmBudgetUs = 3000;

uint64_t micros_since(std::chrono::steady_clock::time_point t0) {
    return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0)
                        .count());
}

uint64_t nanos_since(std::chrono::steady_clock::time_point t0) {
    return uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
}

// GPU memory the backend holds and feeds (process-wide, all backends):
// buffers and textures created and destroyed, bytes passed to queue writes
// (the browser copies each into its own staging memory until the GPU ran it).
struct GpuCounters {
    std::atomic<uint64_t> buffers_created{0}, buffers_destroyed{0};
    std::atomic<uint64_t> buffer_bytes_created{0}, buffer_bytes_destroyed{0};
    std::atomic<uint64_t> textures_created{0}, textures_destroyed{0};
    std::atomic<uint64_t> texture_bytes_created{0}, texture_bytes_destroyed{0};
    std::atomic<uint64_t> queue_writes{0}, queue_write_bytes{0};
    // High-water marks since the last take_window_peaks(): live buffer and
    // texture bytes (as counted here) and commands in one list.
    std::atomic<uint64_t> buffer_bytes_peak{0}, texture_bytes_peak{0}, list_commands_peak{0};
};
GpuCounters g_counters;

// LGP_WEBGPU_UPLOAD_FLUSH_MB=N (native only; 64 on Apple TV, otherwise off, 0 turns it off):
// wgpu-native stages every queue write in a buffer of its own that lives
// until the submission carrying it has run. A scene that uploads its
// textures in one list stages all of them at once (Apple TV, 2026-10-29: 445
// MiB in one list at the scene build, IOAccelerator 516 MiB, peak 2061 MiB
// against a jetsam limit of ≈ 2098; docs/analysis/42-praesent-stillstand.md).
// After N MiB of writes since the last list submit the writes so far are
// submitted on their own and waited for, which frees their staging. The
// writes still run before the list's commands, as before.
struct UploadFlush {
    uint64_t budget = 0;  // bytes, 0 = off
    WGPUDevice device = nullptr;
    std::atomic<uint64_t> pending{0};
    std::atomic<uint64_t> flushes{0};
};
UploadFlush g_upload_flush;

void note_queue_write(WGPUQueue queue, size_t size) {
#if defined(__EMSCRIPTEN__)
    (void)queue;
    (void)size;
#else
    UploadFlush& f = g_upload_flush;
    if (f.budget == 0 || f.device == nullptr || f.pending.fetch_add(size, std::memory_order_relaxed) + size < f.budget)
        return;
    const auto t0 = std::chrono::steady_clock::now();
    const uint64_t bytes = f.pending.exchange(0, std::memory_order_relaxed);
    WGPUCommandEncoder e = wgpuDeviceCreateCommandEncoder(f.device, nullptr);
    WGPUCommandBuffer c = wgpuCommandEncoderFinish(e, nullptr);
    wgpuQueueSubmit(queue, 1, &c);
    wgpuCommandBufferRelease(c);
    wgpuCommandEncoderRelease(e);
    wgpuDevicePoll(f.device, true, nullptr);
    const uint64_t n = f.flushes.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 32 || n % 256 == 0)
        rt::log(rt::LogLevel::Info, "nvn webgpu: upload flush %llu: %.1f MiB of queue writes submitted and waited for "
                                    "in %.1f ms (LGP_WEBGPU_UPLOAD_FLUSH_MB)",
                (unsigned long long)n, double(bytes) / 1048576.0,
                double(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0)
                           .count()) / 1000.0);
#endif
}

void raise_peak(std::atomic<uint64_t>& peak, uint64_t value) {
    uint64_t seen = peak.load(std::memory_order_relaxed);
    while (value > seen && !peak.compare_exchange_weak(seen, value, std::memory_order_relaxed)) {
    }
}
// Destroyed is read first: an object created and destroyed by another
// thread between the two loads then only raises created, never destroyed
// past it (no wrap-around to a huge value).
uint64_t live_bytes(const std::atomic<uint64_t>& created, const std::atomic<uint64_t>& destroyed) {
    const uint64_t gone = destroyed.load(std::memory_order_acquire);
    const uint64_t made = created.load(std::memory_order_acquire);
    return made > gone ? made - gone : 0;
}

WGPUBuffer gpu_create_buffer(WGPUDevice device, const WGPUBufferDescriptor* desc) {
    g_counters.buffers_created.fetch_add(1, std::memory_order_relaxed);
    g_counters.buffer_bytes_created.fetch_add(desc->size, std::memory_order_relaxed);
    raise_peak(g_counters.buffer_bytes_peak,
               live_bytes(g_counters.buffer_bytes_created, g_counters.buffer_bytes_destroyed));
    return wgpuDeviceCreateBuffer(device, desc);
}
// Destroyed, not only released: in the browser a released buffer keeps its
// memory until garbage collection (Safari: hundreds of MB per second).
// Submitted work that uses it stays valid.
void gpu_destroy_buffer(WGPUBuffer b) {
    if (b == nullptr) return;
    g_counters.buffers_destroyed.fetch_add(1, std::memory_order_relaxed);
    g_counters.buffer_bytes_destroyed.fetch_add(wgpuBufferGetSize(b), std::memory_order_relaxed);
    wgpuBufferDestroy(b);
    wgpuBufferRelease(b);
}
// LGP_WEBGPU_BUFFER_POOL=1: buffers a list needs only while it runs (storage
// ranges, read-backs, slot tables, blit uniforms, copy arenas) go back here
// once the GPU ran the list and are taken again for the same usage and size,
// instead of being created and destroyed per list (gpu_allocator and
// vkAllocateMemory in profiles). Callers fill every byte they use; the
// least recently returned buffers are destroyed past cap_bytes.
struct BufferPool {
    using Key = std::pair<uint64_t, uint64_t>;  // (usage, size)
    struct Entry {
        Key key;
        WGPUBuffer buffer;
    };
    std::mutex mutex;
    std::list<Entry> order;  // oldest first
    std::multimap<Key, std::list<Entry>::iterator> index;
    uint64_t free_bytes = 0;
    uint64_t cap_bytes = 64ull << 20;
    bool closed = false;  // the backend is gone: returned buffers are destroyed
    std::atomic<uint64_t> reused{0}, created{0};

    WGPUBuffer take(WGPUDevice device, const WGPUBufferDescriptor* desc) {
        {
            std::lock_guard lock(mutex);
            if (auto it = index.find({uint64_t(desc->usage), desc->size}); it != index.end()) {
                WGPUBuffer b = it->second->buffer;
                order.erase(it->second);
                index.erase(it);
                free_bytes -= desc->size;
                reused.fetch_add(1, std::memory_order_relaxed);
                return b;
            }
        }
        created.fetch_add(1, std::memory_order_relaxed);
        return gpu_create_buffer(device, desc);
    }
    // After the GPU is done with `b` (and it is unmapped).
    void give(WGPUBuffer b) {
        if (b == nullptr) return;
        const Key key{uint64_t(wgpuBufferGetUsage(b)), wgpuBufferGetSize(b)};
        std::vector<WGPUBuffer> evicted;
        {
            std::lock_guard lock(mutex);
            if (closed || key.second > cap_bytes) {
                evicted.push_back(b);
            } else {
                index.emplace(key, order.insert(order.end(), Entry{key, b}));
                free_bytes += key.second;
                while (free_bytes > cap_bytes) {
                    const Entry& old = order.front();
                    for (auto [lo, hi] = index.equal_range(old.key); lo != hi; ++lo)
                        if (lo->second == order.begin()) {
                            index.erase(lo);
                            break;
                        }
                    free_bytes -= old.key.second;
                    evicted.push_back(old.buffer);
                    order.pop_front();
                }
            }
        }
        for (WGPUBuffer e : evicted) gpu_destroy_buffer(e);
    }
    // Destroys the free buffers (a low-memory warning); returns their bytes.
    uint64_t drain() {
        std::vector<WGPUBuffer> all;
        uint64_t bytes = 0;
        {
            std::lock_guard lock(mutex);
            for (const Entry& e : order) all.push_back(e.buffer);
            order.clear();
            index.clear();
            bytes = free_bytes;
            free_bytes = 0;
        }
        for (WGPUBuffer b : all) gpu_destroy_buffer(b);
        return bytes;
    }
    void close() {
        std::vector<WGPUBuffer> all;
        {
            std::lock_guard lock(mutex);
            closed = true;
            for (const Entry& e : order) all.push_back(e.buffer);
            order.clear();
            index.clear();
            free_bytes = 0;
        }
        for (WGPUBuffer b : all) gpu_destroy_buffer(b);
    }
};

// Bytes of 16 texels of a host format (an estimate of what the driver
// allocates: no padding, depth 24 as 4 bytes).
uint64_t bytes_per_16_texels(WGPUTextureFormat f) {
    switch (f) {
    case WGPUTextureFormat_R8Unorm: case WGPUTextureFormat_R8Snorm: case WGPUTextureFormat_R8Uint:
    case WGPUTextureFormat_R8Sint: case WGPUTextureFormat_Stencil8:
        return 16;
    case WGPUTextureFormat_R16Uint: case WGPUTextureFormat_R16Sint: case WGPUTextureFormat_R16Float:
    case WGPUTextureFormat_RG8Unorm: case WGPUTextureFormat_RG8Snorm: case WGPUTextureFormat_RG8Uint:
    case WGPUTextureFormat_RG8Sint: case WGPUTextureFormat_Depth16Unorm:
        return 32;
    case WGPUTextureFormat_RG32Float: case WGPUTextureFormat_RG32Uint: case WGPUTextureFormat_RG32Sint:
    case WGPUTextureFormat_RGBA16Uint: case WGPUTextureFormat_RGBA16Sint: case WGPUTextureFormat_RGBA16Float:
    case WGPUTextureFormat_Depth32FloatStencil8:
        return 128;
    case WGPUTextureFormat_RGBA32Float: case WGPUTextureFormat_RGBA32Uint: case WGPUTextureFormat_RGBA32Sint:
        return 256;
    case WGPUTextureFormat_BC1RGBAUnorm: case WGPUTextureFormat_BC1RGBAUnormSrgb: case WGPUTextureFormat_BC4RUnorm:
    case WGPUTextureFormat_BC4RSnorm:
        return 8;
    default:
        // Other BC and ASTC 4x4: 16 bytes per block; larger ASTC blocks less
        // (counted as 4x4). Everything else 4 bytes per texel.
        return is_compressed(f) ? 16 : 64;
    }
}
uint64_t texture_bytes(const WGPUTextureDescriptor& td) {
    const uint64_t w = td.size.width, h = td.size.height, d = std::max(td.size.depthOrArrayLayers, 1u);
    const bool is3d = td.dimension == WGPUTextureDimension_3D;
    const uint64_t per16 = bytes_per_16_texels(td.format);
    const uint64_t levels = std::max(td.mipLevelCount, 1u);
    uint64_t texels = 0;
    for (uint64_t l = 0; l < levels; ++l)
        texels += std::max<uint64_t>(w >> l, 1) * std::max<uint64_t>(h >> l, 1) *
                  (is3d ? std::max<uint64_t>(d >> l, 1) : d);
    return texels * per16 / 16 * std::max(td.sampleCount, 1u);
}
uint64_t texture_bytes(WGPUTexture t) {
    WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
    td.size = {wgpuTextureGetWidth(t), wgpuTextureGetHeight(t), wgpuTextureGetDepthOrArrayLayers(t)};
    td.dimension = wgpuTextureGetDimension(t);
    td.format = wgpuTextureGetFormat(t);
    td.mipLevelCount = wgpuTextureGetMipLevelCount(t);
    td.sampleCount = wgpuTextureGetSampleCount(t);
    return texture_bytes(td);
}
WGPUTexture gpu_create_texture(WGPUDevice device, const WGPUTextureDescriptor* desc) {
    g_counters.textures_created.fetch_add(1, std::memory_order_relaxed);
    WGPUTexture t = wgpuDeviceCreateTexture(device, desc);
    if (t != nullptr) {
        g_counters.texture_bytes_created.fetch_add(texture_bytes(t), std::memory_order_relaxed);
        raise_peak(g_counters.texture_bytes_peak,
                   live_bytes(g_counters.texture_bytes_created, g_counters.texture_bytes_destroyed));
    }
    return t;
}
void gpu_destroy_texture(WGPUTexture t) {
    if (t == nullptr) return;
    g_counters.textures_destroyed.fetch_add(1, std::memory_order_relaxed);
    g_counters.texture_bytes_destroyed.fetch_add(texture_bytes(t), std::memory_order_relaxed);
    wgpuTextureDestroy(t);
    wgpuTextureRelease(t);
}
void queue_write_buffer(WGPUQueue queue, WGPUBuffer buffer, uint64_t offset, const void* data, size_t size) {
    g_counters.queue_writes.fetch_add(1, std::memory_order_relaxed);
    g_counters.queue_write_bytes.fetch_add(size, std::memory_order_relaxed);
    wgpuQueueWriteBuffer(queue, buffer, offset, data, size);
    note_queue_write(queue, size);
}
void queue_write_texture(WGPUQueue queue, const WGPUTexelCopyTextureInfo* dst, const void* data, size_t size,
                         const WGPUTexelCopyBufferLayout* layout, const WGPUExtent3D* extent) {
    g_counters.queue_writes.fetch_add(1, std::memory_order_relaxed);
    g_counters.queue_write_bytes.fetch_add(size, std::memory_order_relaxed);
    wgpuQueueWriteTexture(queue, dst, data, size, layout, extent);
    note_queue_write(queue, size);
}

WGPUVertexFormat vertex_format(nvn::VertexFormat f) {
    switch (f) {
    case nvn::VertexFormat::Float32: return WGPUVertexFormat_Float32;
    case nvn::VertexFormat::Float32x2: return WGPUVertexFormat_Float32x2;
    case nvn::VertexFormat::Float32x3: return WGPUVertexFormat_Float32x3;
    case nvn::VertexFormat::Float32x4: return WGPUVertexFormat_Float32x4;
    case nvn::VertexFormat::Unorm8x4: return WGPUVertexFormat_Unorm8x4;
    case nvn::VertexFormat::Unorm8: return WGPUVertexFormat_Unorm8;
    case nvn::VertexFormat::Unorm8x2: return WGPUVertexFormat_Unorm8x2;
    case nvn::VertexFormat::Snorm8: return WGPUVertexFormat_Snorm8;
    case nvn::VertexFormat::Snorm8x2: return WGPUVertexFormat_Snorm8x2;
    case nvn::VertexFormat::Snorm8x4: return WGPUVertexFormat_Snorm8x4;
    case nvn::VertexFormat::Unorm8x4Bgra: return WGPUVertexFormat_Unorm8x4BGRA;
    case nvn::VertexFormat::Float16: return WGPUVertexFormat_Float16;
    case nvn::VertexFormat::Float16x2: return WGPUVertexFormat_Float16x2;
    case nvn::VertexFormat::Float16x4: return WGPUVertexFormat_Float16x4;
    case nvn::VertexFormat::Unorm16: return WGPUVertexFormat_Unorm16;
    case nvn::VertexFormat::Unorm16x2: return WGPUVertexFormat_Unorm16x2;
    case nvn::VertexFormat::Unorm16x4: return WGPUVertexFormat_Unorm16x4;
    case nvn::VertexFormat::Snorm16: return WGPUVertexFormat_Snorm16;
    case nvn::VertexFormat::Snorm16x2: return WGPUVertexFormat_Snorm16x2;
    case nvn::VertexFormat::Snorm16x4: return WGPUVertexFormat_Snorm16x4;
    case nvn::VertexFormat::Unorm10_10_10_2: return WGPUVertexFormat_Unorm10_10_10_2;
    case nvn::VertexFormat::Uint8: return WGPUVertexFormat_Uint8;
    case nvn::VertexFormat::Uint8x2: return WGPUVertexFormat_Uint8x2;
    case nvn::VertexFormat::Uint8x4: return WGPUVertexFormat_Uint8x4;
    case nvn::VertexFormat::Sint8: return WGPUVertexFormat_Sint8;
    case nvn::VertexFormat::Sint8x2: return WGPUVertexFormat_Sint8x2;
    case nvn::VertexFormat::Sint8x4: return WGPUVertexFormat_Sint8x4;
    case nvn::VertexFormat::Uint16: return WGPUVertexFormat_Uint16;
    case nvn::VertexFormat::Uint16x2: return WGPUVertexFormat_Uint16x2;
    case nvn::VertexFormat::Uint16x4: return WGPUVertexFormat_Uint16x4;
    case nvn::VertexFormat::Sint16: return WGPUVertexFormat_Sint16;
    case nvn::VertexFormat::Sint16x2: return WGPUVertexFormat_Sint16x2;
    case nvn::VertexFormat::Sint16x4: return WGPUVertexFormat_Sint16x4;
    case nvn::VertexFormat::Uint32: return WGPUVertexFormat_Uint32;
    case nvn::VertexFormat::Uint32x2: return WGPUVertexFormat_Uint32x2;
    case nvn::VertexFormat::Uint32x3: return WGPUVertexFormat_Uint32x3;
    case nvn::VertexFormat::Uint32x4: return WGPUVertexFormat_Uint32x4;
    case nvn::VertexFormat::Sint32: return WGPUVertexFormat_Sint32;
    case nvn::VertexFormat::Sint32x2: return WGPUVertexFormat_Sint32x2;
    case nvn::VertexFormat::Sint32x3: return WGPUVertexFormat_Sint32x3;
    case nvn::VertexFormat::Sint32x4: return WGPUVertexFormat_Sint32x4;
    // Fetched as integers, converted in the vertex shader (wgsl::retype_vertex_inputs).
    case nvn::VertexFormat::UScaled8: return WGPUVertexFormat_Uint8;
    case nvn::VertexFormat::UScaled8x2: return WGPUVertexFormat_Uint8x2;
    case nvn::VertexFormat::UScaled8x4: return WGPUVertexFormat_Uint8x4;
    case nvn::VertexFormat::SScaled8: return WGPUVertexFormat_Sint8;
    case nvn::VertexFormat::SScaled8x2: return WGPUVertexFormat_Sint8x2;
    case nvn::VertexFormat::SScaled8x4: return WGPUVertexFormat_Sint8x4;
    case nvn::VertexFormat::UScaled16: return WGPUVertexFormat_Uint16;
    case nvn::VertexFormat::UScaled16x2: return WGPUVertexFormat_Uint16x2;
    case nvn::VertexFormat::UScaled16x4: return WGPUVertexFormat_Uint16x4;
    case nvn::VertexFormat::SScaled16: return WGPUVertexFormat_Sint16;
    case nvn::VertexFormat::SScaled16x2: return WGPUVertexFormat_Sint16x2;
    case nvn::VertexFormat::SScaled16x4: return WGPUVertexFormat_Sint16x4;
    case nvn::VertexFormat::UScaled32: return WGPUVertexFormat_Uint32;
    case nvn::VertexFormat::UScaled32x2: return WGPUVertexFormat_Uint32x2;
    case nvn::VertexFormat::UScaled32x3: return WGPUVertexFormat_Uint32x3;
    case nvn::VertexFormat::UScaled32x4: return WGPUVertexFormat_Uint32x4;
    case nvn::VertexFormat::SScaled32: return WGPUVertexFormat_Sint32;
    case nvn::VertexFormat::SScaled32x2: return WGPUVertexFormat_Sint32x2;
    case nvn::VertexFormat::SScaled32x3: return WGPUVertexFormat_Sint32x3;
    case nvn::VertexFormat::SScaled32x4: return WGPUVertexFormat_Sint32x4;
    case nvn::VertexFormat::Uint10_10_10_2:
    case nvn::VertexFormat::Sint10_10_10_2:
    case nvn::VertexFormat::Snorm10_10_10_2:
    case nvn::VertexFormat::UScaled10_10_10_2:
    case nvn::VertexFormat::SScaled10_10_10_2: return WGPUVertexFormat_Uint32;
    }
    return WGPUVertexFormat_Float32x4;
}

// Bytes one attribute of the format reads.
uint32_t vertex_format_size(nvn::VertexFormat f) {
    switch (f) {
    case nvn::VertexFormat::Unorm8:
    case nvn::VertexFormat::Snorm8: return 1;
    case nvn::VertexFormat::Unorm8x2:
    case nvn::VertexFormat::Snorm8x2:
    case nvn::VertexFormat::Float16:
    case nvn::VertexFormat::Unorm16:
    case nvn::VertexFormat::Snorm16: return 2;
    case nvn::VertexFormat::Float32:
    case nvn::VertexFormat::Unorm8x4:
    case nvn::VertexFormat::Snorm8x4:
    case nvn::VertexFormat::Unorm8x4Bgra:
    case nvn::VertexFormat::Float16x2:
    case nvn::VertexFormat::Unorm16x2:
    case nvn::VertexFormat::Snorm16x2:
    case nvn::VertexFormat::Unorm10_10_10_2: return 4;
    case nvn::VertexFormat::Float32x2:
    case nvn::VertexFormat::Float16x4:
    case nvn::VertexFormat::Unorm16x4:
    case nvn::VertexFormat::Snorm16x4: return 8;
    case nvn::VertexFormat::Float32x3: return 12;
    case nvn::VertexFormat::Float32x4: return 16;
    default: break;
    }
    // Integer, scaled and packed formats.
    using F = nvn::VertexFormat;
    switch (f) {
    case F::Uint8: case F::Sint8: case F::UScaled8: case F::SScaled8: return 1;
    case F::Uint8x2: case F::Sint8x2: case F::UScaled8x2: case F::SScaled8x2:
    case F::Uint16: case F::Sint16: case F::UScaled16: case F::SScaled16: return 2;
    case F::Uint8x4: case F::Sint8x4: case F::UScaled8x4: case F::SScaled8x4:
    case F::Uint16x2: case F::Sint16x2: case F::UScaled16x2: case F::SScaled16x2:
    case F::Uint32: case F::Sint32: case F::UScaled32: case F::SScaled32:
    case F::Uint10_10_10_2: case F::Sint10_10_10_2: case F::Snorm10_10_10_2: case F::UScaled10_10_10_2:
    case F::SScaled10_10_10_2: return 4;
    case F::Uint16x4: case F::Sint16x4: case F::UScaled16x4: case F::SScaled16x4:
    case F::Uint32x2: case F::Sint32x2: case F::UScaled32x2: case F::SScaled32x2: return 8;
    case F::Uint32x3: case F::Sint32x3: case F::UScaled32x3: case F::SScaled32x3: return 12;
    default: return 16;
    }
}

// Instancing of a vertex stream: divisor 0 steps per vertex, 1 per instance
// (WebGPU stepMode "instance"). WebGPU has no divisors above 1: such streams
// are bound with stride 0 (every instance reads one element) and the draw is
// split into runs of instances that read the same element of every stream
// (instanced_runs in record_list), the buffer offset moving per run.
uint32_t stream_divisor(const nvn::CmdSetVertexLayout& l, size_t stream) {
    return stream < l.divisors.size() ? l.divisors[stream] : 0;
}

// Bytes of one element of each stream that attributes read (0 = unread).
std::vector<uint32_t> stream_reads(const nvn::CmdSetVertexLayout& l) {
    std::vector<uint32_t> last(l.strides.size(), 0);
    for (const nvn::VertexAttrib& a : l.attribs)
        if (a.stream < last.size())
            last[a.stream] = std::max(last[a.stream], a.offset + vertex_format_size(a.format));
    return last;
}

// More streams than the device has vertex buffers (Chrome: 8, LGP draws
// with 9, mostly per-instance ones): some streams with the same step (all
// per vertex or all divisor 1) share one WebGPU buffer, their elements
// interleaved (copied together at record time). The first stream of the
// group owns the slot; each member's element starts at base[i] of a merged
// element of `stride` bytes. Streams with divisors above 1 are rebound per
// run of instances and never merged. Inactive if the layout fits or cannot
// be merged.
struct VertexMerge {
    std::vector<uint32_t> streams;  // ascending
    std::vector<uint32_t> base;
    std::vector<uint32_t> last;  // bytes read of each member's element
    uint32_t stride = 0;
    // The merged buffer's arrayStride: `stride`, or 0 if every member has
    // stride 0 (one merged element, read by every vertex or instance).
    uint32_t array_stride = 0;
    uint32_t divisor = 0;  // of every member: 0 or 1
    bool active() const { return !streams.empty(); }
    int member(uint32_t stream) const {
        for (size_t i = 0; i < streams.size(); ++i)
            if (streams[i] == stream) return int(i);
        return -1;
    }
};

// Vertex buffers a layout needs unmerged: the streams attributes read, plus
// the zero buffer for unfed inputs.
uint32_t vertex_buffers_needed(const nvn::CmdSetVertexLayout& l, bool zero_slot) {
    uint32_t used = zero_slot ? 1 : 0;
    for (uint32_t r : stream_reads(l)) used += r != 0 ? 1 : 0;
    return used;
}

// `zero_slot`: the pipeline needs the extra zero buffer for unfed inputs;
// streams in `exclude` (bit per stream) are never merged: their elements
// are on the GPU (a dispatch's output) and the merge copies on the CPU.
VertexMerge vertex_merge(const nvn::CmdSetVertexLayout& l, bool zero_slot, uint32_t max_buffers,
                         uint32_t exclude = 0) {
    const std::vector<uint32_t> last = stream_reads(l);
    uint32_t used = zero_slot ? 1 : 0;
    std::vector<uint32_t> candidates[2];  // per vertex, per instance (divisor 1)
    for (uint32_t i = 0; i < last.size(); ++i) {
        if (last[i] == 0)
            continue;
        ++used;
        if (const uint32_t d = stream_divisor(l, i); d <= 1 && (i >= 32 || !(exclude >> i & 1)))
            candidates[d].push_back(i);
    }
    if (used <= max_buffers)
        return {};
    const size_t members = used - max_buffers + 1;
    // Per-instance streams first: an instance has fewer elements to copy
    // than a draw has vertices.
    for (uint32_t d : {1u, 0u}) {
        std::vector<uint32_t>& c = candidates[d];
        if (c.size() < members)
            continue;
        // The smallest elements: least to copy per element.
        std::stable_sort(c.begin(), c.end(), [&](uint32_t a, uint32_t b) { return last[a] < last[b]; });
        c.resize(members);
        std::sort(c.begin(), c.end());
        VertexMerge m;
        m.divisor = d;
        for (uint32_t i : c) {
            m.streams.push_back(i);
            m.base.push_back(m.stride);
            m.last.push_back(last[i]);
            m.stride += (last[i] + 3) & ~3u;  // attribute offsets stay 4-byte aligned
            if (l.strides[i] != 0)
                m.array_stride = 1;
        }
        m.array_stride = m.array_stride != 0 ? m.stride : 0;
        if (m.stride <= 2048)  // maxVertexBufferArrayStride
            return m;
    }
    return {};
}

// Guest ranges the dispatches of a list so far may write: the compute
// stage's storage bindings at each dispatch (what record_list binds as
// writable). The pipeline pre-pass in execute() and record_list both track
// it from the commands, so they choose the same streams to merge. A direct
// dispatch with a zero group count writes nothing.
struct DispatchWrites {
    std::vector<std::pair<uint64_t, uint64_t>> ranges;  // address, size
    void dispatch(const nvn::CmdDispatchCompute& c,
                  const std::map<std::pair<int, uint32_t>, nvn::CmdBindStorageBuffer>& storage) {
        if (c.indirect_address == 0 && (c.groups[0] == 0 || c.groups[1] == 0 || c.groups[2] == 0))
            return;
        for (const auto& [key, b] : storage)
            if (key.first == int(nvn::ShaderStage::Compute) && b.size > 0)
                ranges.emplace_back(b.address, b.size);
    }
    // Bit i: stream i is bound inside a written range.
    uint32_t streams(const std::map<uint32_t, nvn::CmdBindVertexBuffer>& vertex_buffers) const {
        uint32_t mask = 0;
        if (ranges.empty())
            return 0;
        for (const auto& [stream, v] : vertex_buffers)
            for (const auto& [address, size] : ranges)
                if (stream < 32 && v.size > 0 && v.address >= address && v.address - address < size)
                    mask |= 1u << stream;
        return mask;
    }
};

// WebGPU vertex buffer slot of each NVN stream (-1 = none). WebGPU counts
// every slot up to the last one against maxVertexBuffers (8 guaranteed),
// while NVN has 16 streams and games bind stream states they do not read.
// So only streams an attribute reads get a slot, numbered densely; merged
// streams share their group's slot; *used = the slots' count (the zero
// buffer for unfed inputs takes slot *used).
std::vector<int> vertex_slots(const nvn::CmdSetVertexLayout& l, uint32_t* used, const VertexMerge& merge = {}) {
    std::vector<int> slot(l.strides.size(), -1);
    for (const nvn::VertexAttrib& a : l.attribs)
        if (a.stream < slot.size())
            slot[a.stream] = 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < slot.size(); ++i) {
        if (slot[i] != 0)
            continue;
        const int m = merge.member(i);
        slot[i] = m > 0 ? slot[merge.streams[0]] : int(n++);
    }
    *used = n;
    return slot;
}

WGPUPrimitiveTopology topology(nvn::Topology t) {
    switch (t) {
    case nvn::Topology::Points: return WGPUPrimitiveTopology_PointList;
    case nvn::Topology::Lines: return WGPUPrimitiveTopology_LineList;
    case nvn::Topology::LineStrip: return WGPUPrimitiveTopology_LineStrip;
    case nvn::Topology::Triangles: return WGPUPrimitiveTopology_TriangleList;
    case nvn::Topology::TriangleStrip: return WGPUPrimitiveTopology_TriangleStrip;
    }
    return WGPUPrimitiveTopology_TriangleList;
}

WGPUCompareFunction compare(nvn::CompareFunc f) {
    switch (f) {
    case nvn::CompareFunc::Never: return WGPUCompareFunction_Never;
    case nvn::CompareFunc::Less: return WGPUCompareFunction_Less;
    case nvn::CompareFunc::Equal: return WGPUCompareFunction_Equal;
    case nvn::CompareFunc::LessEqual: return WGPUCompareFunction_LessEqual;
    case nvn::CompareFunc::Greater: return WGPUCompareFunction_Greater;
    case nvn::CompareFunc::NotEqual: return WGPUCompareFunction_NotEqual;
    case nvn::CompareFunc::GreaterEqual: return WGPUCompareFunction_GreaterEqual;
    case nvn::CompareFunc::Always: return WGPUCompareFunction_Always;
    }
    return WGPUCompareFunction_Always;
}

WGPUStencilOperation stencil_op(nvn::StencilOp op) {
    switch (op) {
    case nvn::StencilOp::Keep: return WGPUStencilOperation_Keep;
    case nvn::StencilOp::Zero: return WGPUStencilOperation_Zero;
    case nvn::StencilOp::Replace: return WGPUStencilOperation_Replace;
    case nvn::StencilOp::IncrementClamp: return WGPUStencilOperation_IncrementClamp;
    case nvn::StencilOp::DecrementClamp: return WGPUStencilOperation_DecrementClamp;
    case nvn::StencilOp::Invert: return WGPUStencilOperation_Invert;
    case nvn::StencilOp::IncrementWrap: return WGPUStencilOperation_IncrementWrap;
    case nvn::StencilOp::DecrementWrap: return WGPUStencilOperation_DecrementWrap;
    }
    return WGPUStencilOperation_Keep;
}

WGPUBlendFactor blend_factor(nvn::BlendFactor f) {
    switch (f) {
    case nvn::BlendFactor::Zero: return WGPUBlendFactor_Zero;
    case nvn::BlendFactor::One: return WGPUBlendFactor_One;
    case nvn::BlendFactor::SrcColor: return WGPUBlendFactor_Src;
    case nvn::BlendFactor::OneMinusSrcColor: return WGPUBlendFactor_OneMinusSrc;
    case nvn::BlendFactor::SrcAlpha: return WGPUBlendFactor_SrcAlpha;
    case nvn::BlendFactor::OneMinusSrcAlpha: return WGPUBlendFactor_OneMinusSrcAlpha;
    case nvn::BlendFactor::DstColor: return WGPUBlendFactor_Dst;
    case nvn::BlendFactor::OneMinusDstColor: return WGPUBlendFactor_OneMinusDst;
    case nvn::BlendFactor::DstAlpha: return WGPUBlendFactor_DstAlpha;
    case nvn::BlendFactor::OneMinusDstAlpha: return WGPUBlendFactor_OneMinusDstAlpha;
    case nvn::BlendFactor::SrcAlphaSaturated: return WGPUBlendFactor_SrcAlphaSaturated;
    case nvn::BlendFactor::Constant: return WGPUBlendFactor_Constant;
    case nvn::BlendFactor::OneMinusConstant: return WGPUBlendFactor_OneMinusConstant;
    }
    return WGPUBlendFactor_One;
}

WGPUBlendOperation blend_op(nvn::BlendOp op) {
    switch (op) {
    case nvn::BlendOp::Add: return WGPUBlendOperation_Add;
    case nvn::BlendOp::Subtract: return WGPUBlendOperation_Subtract;
    case nvn::BlendOp::ReverseSubtract: return WGPUBlendOperation_ReverseSubtract;
    case nvn::BlendOp::Min: return WGPUBlendOperation_Min;
    case nvn::BlendOp::Max: return WGPUBlendOperation_Max;
    }
    return WGPUBlendOperation_Add;
}

WGPUAddressMode address_mode(nvn::AddressMode m) {
    switch (m) {
    case nvn::AddressMode::Repeat: return WGPUAddressMode_Repeat;
    case nvn::AddressMode::MirrorRepeat: return WGPUAddressMode_MirrorRepeat;
    case nvn::AddressMode::ClampToEdge: return WGPUAddressMode_ClampToEdge;
    // WebGPU has no border; compare lookups get it in the shader (border_bits).
    case nvn::AddressMode::ClampToBorder: return WGPUAddressMode_ClampToEdge;
    }
    return WGPUAddressMode_Repeat;
}

bool compare_passes(nvn::CompareFunc f, float ref, float texel) {
    switch (f) {
    case nvn::CompareFunc::Never: return false;
    case nvn::CompareFunc::Less: return ref < texel;
    case nvn::CompareFunc::Equal: return ref == texel;
    case nvn::CompareFunc::LessEqual: return ref <= texel;
    case nvn::CompareFunc::Greater: return ref > texel;
    case nvn::CompareFunc::NotEqual: return ref != texel;
    case nvn::CompareFunc::GreaterEqual: return ref >= texel;
    case nvn::CompareFunc::Always: return true;
    }
    return true;
}

// Swizzle-word bits for a depth-compare binding (wgsl::Options::sampler_border):
// the border axes and the result outside them. The shader returns a constant,
// so the comparison with the border depth (red, clamped to [0, 1]) is taken
// for a reference inside (0, 1): the usual border depth 1 makes LESS/LEQUAL
// lookups outside the map lit. A border depth strictly between 0 and 1 is
// approximated with reference 0.5.
uint32_t border_bits(const nvn::SamplerDesc& d) {
    uint32_t bits = 0;
    if (d.wrap[0] == nvn::AddressMode::ClampToBorder) bits |= wgsl::kBorderU;
    if (d.wrap[1] == nvn::AddressMode::ClampToBorder) bits |= wgsl::kBorderV;
    if (bits == 0) return 0;
    const float depth = std::clamp(d.border[0], 0.0f, 1.0f);
    if (compare_passes(d.compare_func, 0.5f, depth)) bits |= wgsl::kBorderPass;
    return bits;
}

// Debug overrides from LGP_WEBGPU_DEBUG (comma-separated): nocull, nodepth,
// noblend, nomask, noscissor, noviewport switch the guest's state off to find
// which state hides the output; solid replaces the fragment shader with a
// constant colour (coverage only); trace logs the state of the first draws.
// computeoverlap counts reads of guest ranges compute dispatches write
// (Stats::diagnostics, docs/knowledge/nvn-hle.md).
struct DebugOverrides {
    bool nocull = false, nodepth = false, noblend = false, nomask = false;
    bool noscissor = false, noviewport = false, trace = false, solid = false;
    bool cpuastc = false;  // decode ASTC on the CPU even if the device has ASTC
    bool cpubc = false;    // decode BCn on the CPU even if the device has BC
    bool noborder = false;  // colour lookups clamp to the edge instead of the border colour
    bool noshadow = false;  // every depth-compare lookup passes (1.0): is an artefact a shadow?
    bool cbufzero = false;  // constant buffer windows read zeros past the guest's bytes (64 KiB per upload)
    bool computeoverlap = false;  // count reads of ranges compute dispatches wrote
    // Async pipelines: results for lists are dropped as if their callback never
    // came, so the wait's timeout creates them synchronously (tests).
    bool dropasync = false;
    // Compute first, diagnosis: keep the read-back in the main encoder, so
    // no map is registered before the main submit (cfreadbackmain);
    // validation, internal and out-of-memory error scopes around every list,
    // logged with the list number (errorscope).
    bool cfreadbackmain = false, errorscope = false;
    // GPU execution time of every compute and render pass via timestamp
    // queries (device feature timestamp-query, requested by the renderer
    // only with this option): Stats::gpu_time_*.
    bool gputime = false;
    // Every new draw pipeline counted by what sets it apart from the closest
    // pipeline of the same program (Stats::diagnostics "pipeline variant: ...").
    bool pipelinevariants = false;
    // Draws into targets of at least 1280x720 land in the top-left quarter:
    // viewport and scissor scaled by 0.5, clears stay full. A wrong picture
    // that bounds what rendering at half resolution could save
    // (docs/analysis/37-render-skalierung-entwurf.md, section 4).
    bool halffill = false;
    // Draws that would render at half resolution (WebGpuBackend::set_halfres_effects)
    // are left out instead: does the effect change the picture at all? Wrong picture.
    bool halfskip = false;
    // Render and compute passes named after what they write ("draw 1280x720
    // c12 d5 present", "cs <hash> 40x23x1"), so Metal System Trace and AGI
    // split the GPU time per target; the renderer also sets wgpu's Debug
    // instance flag, without which Vulkan drops the names.
    bool passlabels = false;
    // Pass diagnosis ("target set: ...", "store: ..." in Stats::pass_reasons),
    // as WebGpuBackend::set_pass_diagnosis (passdiag).
    bool passdiag = false;
    // Log every program's first draw with both hashes ("program N first
    // drawn ..."): which programs a scene brings (watch_programs).
    bool firstdraw = false;
    // LGP_WEBGPU_SOLID_PROGRAMS=hash,hash (hex prefixes): solid only for these
    // graphics programs, to see what their fragment work costs (wrong image).
    std::vector<std::string> solid_programs;
    bool solid_for(uint64_t hash) const {
        if (solid) return true;
        char hex[17];
        std::snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)hash);
        for (const std::string& p : solid_programs)
            if (std::string_view(hex).substr(0, p.size()) == p) return true;
        return false;
    }
};

// LGP_WEBGPU_DUMP_DIR (debug, private): created on first use, so a fresh
// path on a device works without a mkdir by hand; nullptr when unset.
const char* dump_dir() {
    static const char* const dir = [] {
        const char* d = std::getenv("LGP_WEBGPU_DUMP_DIR");
        if (d == nullptr || *d == 0) return static_cast<const char*>(nullptr);
        std::error_code ec;
        std::filesystem::create_directories(d, ec);
        if (ec)
            rt::log(rt::LogLevel::Warn, "nvn webgpu: cannot create LGP_WEBGPU_DUMP_DIR %s: %s", d,
                    ec.message().c_str());
        return d;
    }();
    return dir;
}

const DebugOverrides& debug_overrides() {
    static const DebugOverrides d = [] {
        DebugOverrides o;
        const char* env = std::getenv("LGP_WEBGPU_DEBUG");
        const std::string v = env != nullptr ? env : "";
        auto has = [&](const char* k) { return v.find(k) != std::string::npos; };
        o.nocull = has("nocull");
        o.nodepth = has("nodepth");
        o.noblend = has("noblend");
        o.nomask = has("nomask");
        o.noscissor = has("noscissor");
        o.noviewport = has("noviewport");
        o.trace = has("trace");
        o.solid = has("solid");  // fragment shader writes magenta: shows coverage
        o.cpuastc = has("cpuastc");
        o.cpubc = has("cpubc");
        o.noborder = has("noborder");
        o.noshadow = has("noshadow");
        o.cbufzero = has("cbufzero");
        o.computeoverlap = has("computeoverlap");
        o.dropasync = has("dropasync");
        o.cfreadbackmain = has("cfreadbackmain");
        o.errorscope = has("errorscope");
        o.gputime = has("gputime");
        o.pipelinevariants = has("pipelinevariants");
        o.halffill = has("halffill");
        o.halfskip = has("halfskip");
        o.passlabels = has("passlabels");
        o.passdiag = has("passdiag");
        o.firstdraw = has("firstdraw");
        if (const char* list = std::getenv("LGP_WEBGPU_SOLID_PROGRAMS"); list != nullptr && *list != '\0') {
            o.solid_programs = parse_hash_prefixes(list);
            std::string text;
            for (const std::string& h : o.solid_programs) text += (text.empty() ? "" : ",") + h;
            lgp::rt::log(lgp::rt::LogLevel::Warn, "webgpu: LGP_WEBGPU_SOLID_PROGRAMS: %s", text.c_str());
        }
        if (!v.empty())
            lgp::rt::log(lgp::rt::LogLevel::Warn, "webgpu: debug overrides active: %s", v.c_str());
        return o;
    }();
    return d;
}

// Swizzle-word bits for a colour binding (wgsl::Options::sampler_border):
// the border axes and the border colour, exact for r = g = b and a each 0 or
// 1 (black, white, transparent); other colours are rounded to them and
// logged once, as is a border on the W axis (3D textures).
uint32_t border_color_bits(const nvn::SamplerDesc& d) {
    uint32_t bits = 0;
    if (d.wrap[0] == nvn::AddressMode::ClampToBorder) bits |= wgsl::kBorderU;
    if (d.wrap[1] == nvn::AddressMode::ClampToBorder) bits |= wgsl::kBorderV;
    const bool w = d.wrap[2] == nvn::AddressMode::ClampToBorder;
    if (bits == 0 && !w) return 0;
    const float* c = d.border;
    if (c[0] >= 0.5f) bits |= wgsl::kBorderPass;
    if (c[3] >= 0.5f) bits |= wgsl::kBorderAlpha;
    const auto exact = [](float v) { return v == 0.0f || v == 1.0f; };
    const bool approximated = w || !(c[0] == c[1] && c[1] == c[2] && exact(c[0]) && exact(c[3]));
    // Each border seen (up to 16) once: which colours the game uses.
    static std::vector<std::array<float, 5>> logged;  // renderer thread only
    const std::array<float, 5> key = {c[0], c[1], c[2], c[3], float(bits | (w ? 1u : 0u))};
    if (logged.size() < 16 && std::find(logged.begin(), logged.end(), key) == logged.end()) {
        logged.push_back(key);
        rt::log(approximated ? rt::LogLevel::Warn : rt::LogLevel::Info,
                "nvn webgpu: colour lookup border (%g, %g, %g, %g) on%s%s%s %s", double(c[0]), double(c[1]),
                double(c[2]), double(c[3]), (bits & wgsl::kBorderU) ? " U" : "", (bits & wgsl::kBorderV) ? " V" : "",
                w ? " W" : "", approximated ? "approximated" : "emulated");
    }
    return debug_overrides().noborder ? 0 : bits;
}

// Pipeline-relevant fixed-function state. Only 1-byte members, so the bytes
// can go into the pipeline key as they are.
struct FixedState {
    uint8_t blend_enable = 0;
    uint8_t masks[nvn::kMaxColorTargets] = {0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF};
    nvn::BlendEquation equations[nvn::kMaxColorTargets];
    nvn::CmdSetDepthStencil depth_stencil;
    uint8_t stencil_read_mask = 0xFF, stencil_write_mask = 0xFF;
    nvn::CmdSetRaster raster;
    bool alpha_to_coverage = false;
    bool depth_clamp = false;                // unclippedDepth (CmdSetDepthClamp)
    uint8_t sample_mask = 0xFF;              // low bits of CmdSetSampleMask (WebGPU: at most 4 samples)
    uint8_t depth_bias[3 * sizeof(float)] = {};  // CmdSetDepthBias factor, units, clamp
    nvn::CompareFunc alpha_func = nvn::CompareFunc::Always;  // CmdSetAlphaTest (lgp_alpha_func)
    uint8_t alpha_ref[sizeof(float)] = {};                    // CmdSetAlphaRef (lgp_alpha_ref)

    nvn::CmdSetDepthBias bias() const {
        nvn::CmdSetDepthBias b;
        std::memcpy(&b.factor, depth_bias, 4);
        std::memcpy(&b.units, depth_bias + 4, 4);
        std::memcpy(&b.clamp, depth_bias + 8, 4);
        return b;
    }
};
static_assert(alignof(FixedState) == 1, "FixedState must not contain padding");

// State a command list builds up for draws (both passes of execute() track
// it the same way). Like the clear state, it starts from the defaults in
// every list.
struct DrawState {
    std::vector<nvn::TextureId> colors;
    nvn::TextureId depth_stencil = 0;
    nvn::ProgramId program = 0;
    nvn::ProgramId compute_program = 0;
    nvn::CmdSetVertexLayout layout;
    FixedState fixed;
    bool rasterizer_discard = false;  // draws are dropped (CmdSetRasterizerDiscard)

    void apply(const nvn::Command& command) {
        if (auto* c = std::get_if<nvn::CmdSetRenderTargets>(&command)) {
            colors = c->colors;
            depth_stencil = c->depth_stencil;
        } else if (auto* c = std::get_if<nvn::CmdBindProgram>(&command)) {
            (c->compute ? compute_program : program) = c->program;
        } else if (auto* c = std::get_if<nvn::CmdSetVertexLayout>(&command)) {
            layout = *c;
        } else if (auto* c = std::get_if<nvn::CmdSetBlendEquation>(&command)) {
            if (c->target < nvn::kMaxColorTargets) fixed.equations[c->target] = c->equation;
        } else if (auto* c = std::get_if<nvn::CmdSetBlendEnables>(&command)) {
            fixed.blend_enable = c->enable_mask;
        } else if (auto* c = std::get_if<nvn::CmdSetChannelMasks>(&command)) {
            std::memcpy(fixed.masks, c->masks, sizeof(fixed.masks));
        } else if (auto* c = std::get_if<nvn::CmdSetDepthStencil>(&command)) {
            fixed.depth_stencil = *c;
        } else if (auto* c = std::get_if<nvn::CmdSetRaster>(&command)) {
            fixed.raster = *c;
        } else if (auto* c = std::get_if<nvn::CmdSetMultisample>(&command)) {
            fixed.alpha_to_coverage = c->alpha_to_coverage;
        } else if (auto* c = std::get_if<nvn::CmdSetDepthBias>(&command)) {
            std::memcpy(fixed.depth_bias, &c->factor, 4);
            std::memcpy(fixed.depth_bias + 4, &c->units, 4);
            std::memcpy(fixed.depth_bias + 8, &c->clamp, 4);
        } else if (auto* c = std::get_if<nvn::CmdSetDepthClamp>(&command)) {
            fixed.depth_clamp = c->enable;
        } else if (auto* c = std::get_if<nvn::CmdSetAlphaTest>(&command)) {
            fixed.alpha_func = c->func;
        } else if (auto* c = std::get_if<nvn::CmdSetAlphaRef>(&command)) {
            std::memcpy(fixed.alpha_ref, &c->ref, 4);
        } else if (auto* c = std::get_if<nvn::CmdSetSampleMask>(&command)) {
            fixed.sample_mask = uint8_t(c->mask);
        } else if (auto* c = std::get_if<nvn::CmdSetRasterizerDiscard>(&command)) {
            rasterizer_discard = c->enable;
        } else if (auto* c = std::get_if<nvn::CmdSetStencilValues>(&command)) {
            // WebGPU has one pair of masks for both faces: the front face's.
            if (c->faces & 1) {
                if (c->kind == nvn::CmdSetStencilValues::ValueMask) fixed.stencil_read_mask = uint8_t(c->value);
                if (c->kind == nvn::CmdSetStencilValues::WriteMask) fixed.stencil_write_mask = uint8_t(c->value);
            }
        }
    }
};

nvn::Topology draw_topology(const nvn::Command& command) {
    if (auto* c = std::get_if<nvn::CmdDraw>(&command)) return c->topology;
    return std::get<nvn::CmdDrawIndexed>(command).topology;
}
bool is_draw(const nvn::Command& command) {
    return std::holds_alternative<nvn::CmdDraw>(command) || std::holds_alternative<nvn::CmdDrawIndexed>(command);
}
// WebGPU pipelines for strips name the index format of indexed draws
// (primitive restart index); 8-bit indices are widened to 16 bits.
WGPUIndexFormat strip_index_format(const nvn::Command& command) {
    auto* c = std::get_if<nvn::CmdDrawIndexed>(&command);
    if (c == nullptr || (c->topology != nvn::Topology::TriangleStrip && c->topology != nvn::Topology::LineStrip))
        return WGPUIndexFormat_Undefined;
    return c->type == nvn::IndexType::U32 ? WGPUIndexFormat_Uint32 : WGPUIndexFormat_Uint16;
}

struct PipelineRequest {
    std::string key;
    nvn::ProgramId program = 0;
    nvn::CmdSetVertexLayout layout;
    nvn::Topology topology = nvn::Topology::Triangles;
    std::vector<WGPUTextureFormat> colors;
    WGPUTextureFormat depth = WGPUTextureFormat_Undefined;
    uint32_t samples = 1;
    FixedState fixed;
    bool flip_y = false;  // lower-left window origin: negate clip y (lgp_flip_y), swap the front face
    bool depth_minus_w = false;  // NVN depth mode NEAR_IS_MINUS_W: clip z from [-w, w] to [0, w] (lgp_depth_minus_w)
    // The fragment shader reads gl_FragCoord.xy and the target is scaled
    // (State::render_scale): lgp_target_scale = s. In the recipe as bit 1
    // of the flip byte, so recipes without it stay valid.
    bool target_scaled = false;
    WGPUIndexFormat strip_index = WGPUIndexFormat_Undefined;
    // Texture bindings sampled as unfilterable-float (bit 32 * stage + k,
    // State::unfilterable_mask): pipeline layout Program::layout_variants.
    uint64_t unfilterable = 0;
    // Streams that must not share a buffer (VertexMerge): bit i = stream i
    // reads a dispatch's output (DispatchWrites). Set only when the layout
    // needs merging.
    uint32_t merge_exclude = 0;
};

// LGP_WEBGPU_DEBUG=pipelinevariants: the parts of two draw pipeline requests
// of one program that differ, as short names joined by '+' ("" = equal).
std::string pipeline_difference(const PipelineRequest& a, const PipelineRequest& b) {
    std::string out;
    auto add = [&out](bool differs, const char* what) {
        if (!differs) return;
        if (!out.empty()) out += '+';
        out += what;
    };
    // FixedState has no padding (static_assert above): its members compare
    // as the bytes the pipeline key holds.
    auto bytes_differ = [](const auto& x, const auto& y) { return std::memcmp(&x, &y, sizeof(x)) != 0; };
    add(a.topology != b.topology || a.strip_index != b.strip_index, "topology");
    add(a.colors != b.colors || a.depth != b.depth || a.samples != b.samples, "targets");
    // The vertex layout as the key holds it: attributes, strides and each
    // stream's step mode (vertex, instance, run of instances).
    auto layout_differs = [](const nvn::CmdSetVertexLayout& x, const nvn::CmdSetVertexLayout& y) {
        if (x.attribs.size() != y.attribs.size() || x.strides != y.strides) return true;
        for (size_t i = 0; i < x.attribs.size(); ++i) {
            const nvn::VertexAttrib &p = x.attribs[i], &q = y.attribs[i];
            if (p.location != q.location || p.format != q.format || p.stream != q.stream || p.offset != q.offset)
                return true;
        }
        auto step = [](uint32_t d) { return d == 0 ? 0 : d == 1 ? 1 : 2; };
        for (size_t i = 0; i < x.strides.size(); ++i)
            if (step(stream_divisor(x, i)) != step(stream_divisor(y, i))) return true;
        return false;
    };
    add(layout_differs(a.layout, b.layout) || a.merge_exclude != b.merge_exclude, "vertex layout");
    add(a.flip_y != b.flip_y || a.depth_minus_w != b.depth_minus_w, "window/depth mode");
    add(a.unfilterable != b.unfilterable, "unfilterable textures");
    const FixedState &x = a.fixed, &y = b.fixed;
    add(x.blend_enable != y.blend_enable || bytes_differ(x.equations, y.equations), "blend");
    add(bytes_differ(x.masks, y.masks), "write masks");
    add(bytes_differ(x.depth_stencil, y.depth_stencil) || x.stencil_read_mask != y.stencil_read_mask ||
            x.stencil_write_mask != y.stencil_write_mask,
        "depth/stencil");
    add(bytes_differ(x.raster, y.raster), "cull/front face");
    add(bytes_differ(x.depth_bias, y.depth_bias), "depth bias values");
    add(x.alpha_to_coverage != y.alpha_to_coverage || x.sample_mask != y.sample_mask, "multisample");
    add(x.depth_clamp != y.depth_clamp, "depth clamp");
    add(x.alpha_func != y.alpha_func, "alpha func");
    add(bytes_differ(x.alpha_ref, y.alpha_ref), "alpha ref");
    return out;
}

using BoundTextures = std::map<std::pair<int, uint32_t>, nvn::CmdBindTexture>;

#if LGP_DRIVER_PIPELINE_CACHE
// Pipelines created with the driver's cache, in all backends of the process:
// save_driver_cache writes the cache back once this changed (counting the
// .lgppipes records missed pipelines whose recipe was already in the file).
std::atomic<uint64_t> g_driver_cache_pipelines{0};
#endif

// Chains the driver's pipeline cache to a pipeline descriptor, holding a
// reference to it.
struct DriverCacheLink {
#if LGP_DRIVER_PIPELINE_CACHE
    DriverCacheLink() = default;
    DriverCacheLink(const DriverCacheLink&) = delete;
    DriverCacheLink& operator=(const DriverCacheLink&) = delete;
    ~DriverCacheLink() {
        if (extras.cache != nullptr) wgpuPipelineCacheRelease(extras.cache);
    }
    WGPUPipelineCacheExtras extras = {{nullptr, static_cast<WGPUSType>(WGPUSType_PipelineCacheExtras)}, nullptr};
    void attach(WGPUChainedStruct*& next_in_chain, WGPUPipelineCache cache) {
        if (cache == nullptr || extras.cache != nullptr)
            return;
        wgpuPipelineCacheAddRef(cache);
        g_driver_cache_pipelines.fetch_add(1, std::memory_order_relaxed);
        extras.cache = cache;
        extras.chain.next = next_in_chain;
        next_in_chain = &extras.chain;
    }
#else
    template <class Chain>
    void attach(Chain&, void*) {}
#endif
};
#if LGP_DRIVER_PIPELINE_CACHE
using DriverCache = WGPUPipelineCache;
#else
using DriverCache = void*;
#endif

// A render pipeline descriptor with everything it points to, holding its own
// references to the layout and shader modules: it can be created later or on
// another thread (pipeline prebuilds), after the program is gone. Not moved
// once filled (the descriptor points into it).
struct PipelineBuild {
    PipelineBuild() = default;
    PipelineBuild(const PipelineBuild&) = delete;
    PipelineBuild& operator=(const PipelineBuild&) = delete;
    ~PipelineBuild() {
        if (layout != nullptr) wgpuPipelineLayoutRelease(layout);
        if (vs != nullptr) wgpuShaderModuleRelease(vs);
        if (fs != nullptr) wgpuShaderModuleRelease(fs);
    }
    std::vector<std::vector<WGPUVertexAttribute>> attrs;
    std::vector<WGPUVertexAttribute> zero_attrs;
    std::vector<WGPUVertexBufferLayout> buffers;
    std::vector<WGPUColorTargetState> targets;
    std::vector<WGPUBlendState> blends;
    WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
    WGPUDepthStencilState ds = WGPU_DEPTH_STENCIL_STATE_INIT;
    WGPUConstantEntry constants[2] = {WGPU_CONSTANT_ENTRY_INIT, WGPU_CONSTANT_ENTRY_INIT};        // vertex
    WGPUConstantEntry alpha_constants[3] = {WGPU_CONSTANT_ENTRY_INIT, WGPU_CONSTANT_ENTRY_INIT,
                                            WGPU_CONSTANT_ENTRY_INIT};  // fragment
    WGPURenderPipelineDescriptor desc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    WGPUPipelineLayout layout = nullptr;
    WGPUShaderModule vs = nullptr, fs = nullptr;
    DriverCacheLink driver_cache;
    // For the slow-pipeline log (log_draw_pipeline), as ComputeBuild.
    uint64_t hash = 0;
    size_t wgsl_bytes = 0;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
};

// What a compute pipeline is created from, off the GPU thread (warm-up):
// holds its own references.
struct ComputeBuild {
    ComputeBuild(WGPUPipelineLayout l, WGPUShaderModule m) : layout(l), cs(m) {
        wgpuPipelineLayoutAddRef(layout);
        wgpuShaderModuleAddRef(cs);
        desc.label = pw::string_view("nvn compute");
        desc.layout = layout;
        desc.compute.module = cs;
        desc.compute.entryPoint = pw::string_view("main");
    }
    ComputeBuild(const ComputeBuild&) = delete;
    ComputeBuild& operator=(const ComputeBuild&) = delete;
    ~ComputeBuild() {
        wgpuPipelineLayoutRelease(layout);
        wgpuShaderModuleRelease(cs);
    }
    WGPUPipelineLayout layout;
    // For the slow-pipeline log (log_compute_pipeline): which program, how
    // much WGSL, when creation was asked for.
    uint64_t hash = 0;
    size_t wgsl_bytes = 0;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    WGPUShaderModule cs;
    WGPUComputePipelineDescriptor desc = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
    DriverCacheLink driver_cache;
};

ComputeBuild* compute_build_for(WGPUPipelineLayout layout, WGPUShaderModule cs, uint64_t hash, size_t wgsl_bytes,
                                DriverCache driver_cache) {
    auto* b = new ComputeBuild(layout, cs);
    b->hash = hash;
    b->wgsl_bytes = wgsl_bytes;
    b->driver_cache.attach(b->desc.nextInChain, driver_cache);
    return b;
}

// Compute pipelines are what makes a cold start slow (Android: four of them
// took 108 s; PC Chromium: six took 22 s): name the slow ones by program
// hash and WGSL size, so runs on different devices can be compared. `how`:
// where it was created. The time runs from the request (async: includes
// waiting for other compilations).
// `created`: when this thread started the creation (the background worker:
// the queue wait before it is logged on its own); default: the request.
void log_compute_pipeline(const ComputeBuild& b, const char* how,
                          std::chrono::steady_clock::time_point created = {}) {
    const auto now = std::chrono::steady_clock::now();
    const auto from = created == std::chrono::steady_clock::time_point{} ? b.started : created;
    const uint64_t us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(now - from).count());
    if (us < 2000)
        return;
    if (from != b.started) {
        const double queued = std::chrono::duration<double, std::milli>(from - b.started).count();
        rt::log(rt::LogLevel::Info,
                "nvn webgpu: compute pipeline %016llx (WGSL %zu bytes) ready after %.1f ms (%s, %.1f ms queued before)",
                (unsigned long long)b.hash, b.wgsl_bytes, us / 1000.0, how, queued);
    } else {
        rt::log(rt::LogLevel::Info, "nvn webgpu: compute pipeline %016llx (WGSL %zu bytes) ready after %.1f ms (%s)",
                (unsigned long long)b.hash, b.wgsl_bytes, us / 1000.0, how);
    }
}

// Draw pipelines the same way: a program has one per render state it is
// drawn with, so a run logs a program's hash once per slow variant
// (wgsl_stats.py adds them up per program).
void log_draw_pipeline(const PipelineBuild& b, const char* how, std::chrono::steady_clock::time_point created = {}) {
    const auto now = std::chrono::steady_clock::now();
    const auto from = created == std::chrono::steady_clock::time_point{} ? b.started : created;
    const uint64_t us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(now - from).count());
    if (us < 2000)
        return;
    rt::log(rt::LogLevel::Info, "nvn webgpu: draw pipeline %016llx (WGSL %zu bytes) ready after %.1f ms (%s)",
            (unsigned long long)b.hash, b.wgsl_bytes, us / 1000.0, how);
}

}  // namespace

struct WebGpuBackend::Program {
    WGPUShaderModule vs = nullptr, fs = nullptr;
    // Graphics: constant buffers vertex/fragment (groups 0/1), textures
    // vertex/fragment (groups 2/3). Compute: constant buffers (0), textures
    // (1), global memory (2).
    WGPUBindGroupLayout groups[4] = {nullptr, nullptr, nullptr, nullptr};
    WGPUPipelineLayout layout = nullptr;
    // Index = nvn::ShaderStage (vertex, fragment, compute).
    std::vector<uint32_t> banks[3];
    std::vector<uint32_t> texture_slots[3];  // bound slot of texture binding k
    std::vector<uint32_t> texture_dims[3];   // its dimension (2 or 3), ProgramDesc
    std::vector<bool> texture_arrays[3];     // binding k is a 2D array (ProgramDesc)
    std::vector<bool> texture_depth[3];      // binding k is a depth texture + comparison sampler (ProgramDesc)
    bool compute = false;
    bool global_memory = false;
    bool gmem_probe = false;  // group 2 has the computeprobe counters (ProgramDesc::compute_gmem_probe)
    bool texture_swizzle = false;  // texture groups end with the swizzle uniform (ProgramDesc)
    // Groups 0/1 take their constant buffers with dynamic offsets into the
    // arena (State::cbuf_arena); off when the program has more banks than
    // the device allows dynamic uniform buffers.
    bool dynamic_cbufs = false;
    bool flip_override = false;    // the vertex WGSL declares "override lgp_flip_y"
    bool depth_override = false;   // ... and "override lgp_depth_minus_w"
    bool alpha_override = false;   // the fragment WGSL declares "override lgp_alpha_func" (and lgp_alpha_ref)
    bool reads_screen_xy = false;  // the fragment shader reads fpos.x/.y (half-resolution effects)
    int8_t watched = -1;           // State::watched_programs lists it: -1 not known yet
    bool drawn = false;            // drawn at least once (LGP_WEBGPU_DEBUG=firstdraw)
    bool target_scale_override = false;  // ... "override lgp_target_scale" (wgsl::Options::render_scale)
    // No fragment stage (ProgramDesc::fragment_wgsl empty): fs is an empty
    // stub for passes with color targets, which it does not write.
    bool vertex_only = false;
    std::vector<uint32_t> vertex_inputs;  // @location(N) aN parameters of the vertex WGSL
    // Vertex WGSL as translated (inputs vec4<f32>) and its variants with
    // integer inputs for integer vertex formats (key: per location u/i).
    std::string vertex_wgsl;
    std::map<std::string, WGPUShaderModule> vs_variants;
    // Pipeline layouts whose texture groups declare some bindings
    // unfilterable-float with a non-filtering sampler (key:
    // PipelineRequest::unfilterable); groups 0/1 are the program's.
    struct LayoutVariant {
        WGPUPipelineLayout layout = nullptr;
        WGPUBindGroupLayout textures[2] = {nullptr, nullptr};
    };
    std::map<uint64_t, LayoutVariant> layout_variants;
    bool stage_global_memory[2] = {false, false};  // vertex/fragment load global memory (texture group)
    WGPUShaderModule cs = nullptr;
    WGPUComputePipeline compute_pipeline = nullptr;
    // Storage slots and uniform blocks the compute WGSL reads directly
    // (wgsl::Options::storage_desc_offset; block static_blocks[j] in constant
    // window j), the WGSL if there are any, and its pipelines with some of
    // them reading another table entry (wgsl::move_static_slots, key: the
    // moves), for dispatches that bind one range in two slots or a uniform
    // block inside a storage slot.
    std::vector<uint32_t> static_slots, static_blocks;
    std::string compute_wgsl;
    std::map<std::map<uint32_t, uint32_t>, WGPUComputePipeline> alias_pipelines;
    // Compute, from the pipeline cache: the pipeline is created in the
    // background (or at the first dispatch, claim_compute_prebuild).
    bool compute_deferred = false;
    bool valid = false;
    // Graphics: hash of the WGSL and the binding layout (pipeline cache), and
    // a number no other program of this backend had (stale prebuilds).
    uint64_t hash = 0;
    uint64_t serial = 0;
    size_t wgsl_bytes = 0;  // all stages' WGSL (log of slow programs and pipelines)
    uint64_t guest_hash = 0;  // ProgramDesc::guest_hash (watch_programs, firstdraw)
};

struct WebGpuBackend::State {
    // GPU state a command list leaves behind. NVN does not reset state
    // between command buffers: bindings and settings recorded in one stay in
    // effect for the next one submitted to the queue (LGP binds the textures
    // of its final composite pass in an earlier command buffer).
    struct Carried {
        DrawState st;
        bool scissor_set = false;
        nvn::Rect scissor;
        bool viewport_set = false;
        float viewport[4] = {0, 0, 0, 0};
        float depth_range[2] = {0.0f, 1.0f};
        std::map<uint32_t, nvn::CmdBindVertexBuffer> vertex_buffers;
        std::map<std::pair<int, uint32_t>, nvn::CmdBindUniformBuffer> uniform_buffers;
        std::map<std::pair<int, uint32_t>, nvn::CmdBindTexture> bound_textures;
        std::map<std::pair<int, uint32_t>, nvn::CmdBindStorageBuffer> storage_bindings;
        WGPUColor blend_color = {0, 0, 0, 0};
        uint32_t stencil_ref = 0;
    };
    Carried carried;
    // The textures shown by the last presents (LGP_WEBGPU_DEBUG=passlabels):
    // the guest rotates two or three window textures, and a list renders the
    // next one before its present.
    std::array<std::atomic<nvn::TextureId>, 4> presented{};
    std::atomic<uint32_t> presents_seen{0};
    bool was_presented(nvn::TextureId id) const {
        for (const auto& p : presented)
            if (id != 0 && p.load(std::memory_order_relaxed) == id) return true;
        return false;
    }
    struct Texture {
        nvn::TextureDesc desc;
        WGPUTexture texture = nullptr;
        WGPUTextureFormat format = WGPUTextureFormat_Undefined;
        uint32_t sample_count = 1;
        WGPUTextureView attachment = nullptr;  // level 0, layer 0
        WGPUTextureView raw = nullptr;         // same, linear variant (present); colour only
        WGPUTextureView sampled = nullptr;     // all levels, for shaders; colour, single-sampled only
        // Contents from the game's memory pool (desc.storage, block-linear):
        // uploaded when first sampled, re-checked now and then; never for
        // textures written otherwise (render targets, copy destinations,
        // nvnTextureWriteTexels), whose pool storage the HLE does not update.
        bool gpu_written = false;
        bool uploaded = false;
        uint64_t fingerprint = 0;
        uint64_t checked_at = 0;  // presents count of the last check
        // Pitch-linear: the bytes of the last upload (checked every frame,
        // memcmp instead of a fingerprint).
        std::vector<uint8_t> linear_shadow;
        WGPUTextureView sampled_array = nullptr;  // all levels and layers as 2D array (created when needed)
        // Depth formats: the depth aspect for shadow lookups (texture_depth_2d), all levels; and as 2D array.
        WGPUTextureView depth_sampled = nullptr;
        WGPUTextureView depth_array = nullptr;
        bool is_3d() const { return desc.target == uint32_t(nvn::TextureTarget::Target3D); }
        // ASTC without device support: RGBA8(_SRGB) texture, contents decoded on the CPU.
        bool astc_cpu = false;
        // BCn without device support (mobile GPUs): RGBA8 texture, decoded on the CPU.
        bool bc_cpu = false;
        lgp::gpu::texture::BcFormat bc_format = lgp::gpu::texture::BcFormat::BC1;
        bool cpu_decoded() const { return astc_cpu || bc_cpu; }
        // Colour texture sampled with depth compare (compare_view): a Depth16Unorm
        // copy of the red channel of level 0, from the memory pool.
        WGPUTexture compare_texture = nullptr;
        WGPUTextureView compare_view = nullptr;
        // The GPU texture as create_texture() worked it out; with lazy
        // textures it is created on the first lookup (State::find_texture).
        struct Plan {
            WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
            WGPUTextureFormat raw_format = WGPUTextureFormat_Undefined;
            bool target_3d = false, compressed = false, depth_stencil = false;
        } plan;
        bool lazy = false;       // planned, not created yet
        uint64_t last_used = 0;  // State::frame of the last lookup, 0 = never
        uint64_t compare_fingerprint = 0;
        uint64_t compare_checked_at = ~uint64_t{0};
        // Render scale (State::render_scale): a render target created at
        // s times its guest size. Rectangles from the guest (viewports,
        // scissors, clears, copies, present crops) are in guest texels and
        // are mapped with host_x()/host_y().
        bool scaled = false;
        uint32_t width(uint32_t level = 0) const {
            return std::max((scaled ? plan.td.size.width : desc.width) >> level, 1u);
        }
        uint32_t height(uint32_t level = 0) const {
            return std::max((scaled ? plan.td.size.height : desc.height) >> level, 1u);
        }
        // A guest texel edge of `level` in host texels (rounded).
        int64_t host_x(int64_t x, uint32_t level = 0) const {
            if (!scaled) return x;
            const double gw = std::max(desc.width >> level, 1u);
            return int64_t(std::llround(double(x) * double(width(level)) / gw));
        }
        int64_t host_y(int64_t y, uint32_t level = 0) const {
            if (!scaled) return y;
            const double gh = std::max(desc.height >> level, 1u);
            return int64_t(std::llround(double(y) * double(height(level)) / gh));
        }
    };

    // The depth aspect of a depth texture for comparison sampling (2D or
    // 2D array); nullptr if the texture has no depth.
    WGPUTextureView depth_view(Texture& t, bool array) {
        if (t.texture == nullptr || !has_depth(t.format) || t.sample_count != 1)
            return nullptr;
        WGPUTextureView& v = array ? t.depth_array : t.depth_sampled;
        if (v == nullptr) {
            WGPUTextureViewDescriptor vd = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
            // Undefined = the format of the depth aspect (Depth24Plus for
            // Depth24PlusStencil8); wgpu rejects the combined format here.
            vd.format = WGPUTextureFormat_Undefined;
            vd.dimension = array ? WGPUTextureViewDimension_2DArray : WGPUTextureViewDimension_2D;
            vd.mipLevelCount = wgpuTextureGetMipLevelCount(t.texture);
            vd.arrayLayerCount = array ? wgpuTextureGetDepthOrArrayLayers(t.texture) : 1;
            vd.aspect = WGPUTextureAspect_DepthOnly;
            v = wgpuTextureCreateView(t.texture, &vd);
        }
        return v;
    }

    // Depth compare on an 8-bit colour texture (LGP binds a small RGBA8
    // texture to a shadow lookup): the hardware is assumed to compare against
    // the red channel (UNVERIFIED). WebGPU compares only depth textures, so a
    // Depth16Unorm copy of the red channel of level 0 stands in (2D, contents
    // from the memory pool, re-read like uploads). nullptr if not possible
    // (other formats, arrays, textures the GPU wrote).
    WGPUTextureView compare_view(Texture& t, const nvn::GuestMemoryView* memory, uint64_t presents, bool array) {
        const uint32_t fmt = t.desc.format;
        const bool red8 = fmt == uint32_t(nvn::Format::RGBA8) || fmt == uint32_t(nvn::Format::R8);
        if (!red8 || array || t.gpu_written || t.desc.storage == 0 || t.desc.linear_stride != 0 ||
            t.desc.target != uint32_t(nvn::TextureTarget::Target2D) || t.sample_count != 1 || memory == nullptr)
            return nullptr;
        if (t.compare_view != nullptr && presents < t.compare_checked_at + 60)
            return t.compare_view;
        t.compare_checked_at = presents;
        const nvn::TextureLayout layout =
            nvn::texture_layout(fmt, t.desc.target, t.desc.width, t.desc.height, 1, 1, t.desc.gob_height_log2);
        if (layout.levels.empty())
            return t.compare_view;
        std::vector<uint8_t> raw(size_t(layout.levels[0].size));
        if (raw.empty() || !memory->read(t.desc.storage + layout.levels[0].offset, raw.data(), raw.size()))
            return t.compare_view;
        const uint64_t fp = fingerprint(raw.data(), raw.size());
        if (t.compare_view != nullptr && fp == t.compare_fingerprint)
            return t.compare_view;
        const nvn::LevelLayout& lv = layout.levels[0];
        std::vector<uint8_t> linear;
        if (!nvn::deswizzle_level(lv, raw.data(), raw.size(), &linear))
            return t.compare_view;
        const uint32_t bpp = fmt == uint32_t(nvn::Format::R8) ? 1 : 4;
        std::vector<uint16_t> depth(size_t(lv.width) * lv.height);
        for (uint32_t y = 0; y < lv.height; ++y)
            for (uint32_t x = 0; x < lv.width; ++x) {
                const size_t at = size_t(y) * lv.row_bytes + size_t(x) * bpp;
                depth[size_t(y) * lv.width + x] = at < linear.size() ? uint16_t(linear[at] * 257u) : 0;
            }
        if (t.compare_texture == nullptr) {
            WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
            td.label = pw::string_view("nvn compare stand-in");
            td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
            td.size = {lv.width, lv.height, 1};
            td.format = WGPUTextureFormat_Depth16Unorm;  // copy destination allowed (Depth32Float is not)
            t.compare_texture = gpu_create_texture(device, &td);
            t.compare_view = wgpuTextureCreateView(t.compare_texture, nullptr);
        }
        WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
        dst.texture = t.compare_texture;
        WGPUTexelCopyBufferLayout bl = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
        bl.bytesPerRow = lv.width * 2;
        bl.rowsPerImage = lv.height;
        const WGPUExtent3D size = {lv.width, lv.height, 1};
        queue_write_texture(queue, &dst, depth.data(), depth.size() * 2, &bl, &size);
        t.compare_fingerprint = fp;
        return t.compare_view;
    }

    // The 2D-array view of a texture, for shaders that sample it as array.
    WGPUTextureView array_view(Texture& t) {
        if (t.sampled_array == nullptr && t.texture != nullptr) {
            WGPUTextureViewDescriptor vd = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
            vd.format = t.format;
            vd.dimension = WGPUTextureViewDimension_2DArray;
            vd.mipLevelCount = wgpuTextureGetMipLevelCount(t.texture);
            vd.arrayLayerCount = wgpuTextureGetDepthOrArrayLayers(t.texture);
            t.sampled_array = wgpuTextureCreateView(t.texture, &vd);
        }
        return t.sampled_array != nullptr ? t.sampled_array : dummy_array_view;
    }

    // Uniform buffers with texture swizzle codes (wgsl::Options::texture_swizzle),
    // one per distinct set of codes; written once, kept for the device's life.
    std::map<std::vector<uint32_t>, WGPUBuffer> swizzle_buffers;
    // Guest bytes of the block-linear texture being checked (upload_from_pool).
    std::vector<uint8_t> upload_scratch;
    // Helpers for decoding large BCn levels on the CPU (LGP_WEBGPU_DECODE_THREADS=N,
    // default 0 = on the GPU thread alone, 3 on tvOS); only made when the CPU decodes BCn.
    std::unique_ptr<lgp::gpu::texture::DecodePool> decode_pool;
    uint32_t decode_threads = 0;
    WGPUBuffer swizzle_buffer(const std::vector<uint32_t>& codes) {
        auto [it, inserted] = swizzle_buffers.emplace(codes, nullptr);
        if (inserted) {
            WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
            bd.label = pw::string_view("nvn texture swizzles");
            bd.size = codes.size() * 4;
            bd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
            it->second = gpu_create_buffer(device, &bd);
            queue_write_buffer(queue, it->second, 0, codes.data(), bd.size);
        }
        return it->second;
    }

    // Reads the texture's storage from guest memory, deswizzles every level
    // and layer and writes it into the GPU texture (queue order: before the
    // command buffer that samples it is submitted).
    // True if new contents were written.
    bool upload_from_pool(Texture& t, const nvn::GuestMemoryView* memory, uint64_t presents,
                          WebGpuBackend::Stats* delta = nullptr) {
        if (t.gpu_written || t.texture == nullptr || t.desc.storage == 0 || memory == nullptr || t.sample_count != 1)
            return false;
        // Depth/stencil contents are not uploaded (WebGPU writes only one
        // aspect at a time, and the packed layouts differ from the host's).
        if (has_depth(t.format) || has_stencil(t.format))
            return false;
        // Linear textures are CPU-written caches (glyphs): checked every frame.
        if (t.uploaded && presents < t.checked_at + (t.desc.linear_stride != 0 ? 1 : 60))
            return false;
        t.checked_at = presents;
        // Host time of the check and upload (deswizzle, ASTC decode, write).
        struct Timer {
            WebGpuBackend::Stats* delta;
            std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
            ~Timer() {
                if (delta != nullptr) delta->texture_upload_us += micros_since(t0);
            }
        } timer{delta};
        if (t.desc.linear_stride != 0) {
            const auto t_linear = std::chrono::steady_clock::now();
            const bool written = upload_linear(t, memory, delta);
            if (delta != nullptr) {
                delta->texture_linear_us += micros_since(t_linear);
                ++delta->texture_linear_checks;
            }
            return written;
        }
        const nvn::TextureLayout layout =
            nvn::texture_layout(t.desc.format, t.desc.target, t.desc.width, t.desc.height, t.desc.depth, t.desc.levels,
                                t.desc.gob_height_log2);
        const auto t_read = std::chrono::steady_clock::now();
        // Reused: a fresh buffer of megabytes costs page faults every check.
        std::vector<uint8_t>& raw = upload_scratch;
        raw.resize(size_t(layout.total_size));
        if (raw.empty() || !memory->read(t.desc.storage, raw.data(), raw.size()))
            return false;
        const uint64_t fp = fingerprint(raw.data(), raw.size());
        if (delta != nullptr) {
            delta->texture_read_us += micros_since(t_read);
            delta->texture_read_bytes += raw.size();
            ++delta->texture_checks;
        }
        if (t.uploaded && fp == t.fingerprint)
            return false;
        t.fingerprint = fp;
        t.uploaded = true;
        const auto info = nvn::format_info(t.desc.format);
        if (!info)
            return false;
        const bool widen = is_widened_16bit(t.desc.format);
        const uint32_t layers = std::min<uint32_t>(layout.layers, std::max(t.desc.depth, 1u));
        std::vector<uint8_t> linear, wide;
        for (uint32_t layer = 0; layer < layout.layers && layer < std::max(layers, 1u); ++layer) {
            for (uint32_t l = 0; l < layout.levels.size() && l < std::max(t.desc.levels, 1u); ++l) {
                const nvn::LevelLayout& lv = layout.levels[l];
                const uint64_t at = layer * layout.layer_size + lv.offset;
                const auto t_convert = std::chrono::steady_clock::now();
                if (!nvn::deswizzle_level(lv, raw.data() + at, raw.size() - at, &linear))
                    return false;
                if (is_norm16(t.desc.format))
                    norm16_to_half(t.desc.format, linear.data(), linear.size());
                const std::vector<uint8_t>* data = &linear;
                uint32_t row = lv.row_bytes;
                uint32_t rows = lv.blocks_y;
                WGPUExtent3D size = {lv.blocks_x * info->block_width, lv.blocks_y * info->block_height,
                                     t.is_3d() ? lv.depth : 1};
                if (widen) {
                    widen_16bit(t.desc.format, linear, &wide);
                    data = &wide;
                    row = lv.blocks_x * 4;
                } else if (t.astc_cpu) {
                    // ASTC is 2D only in NVN; the RGBA8 texture has the level's
                    // exact size (no block rounding).
                    const size_t errors = decode_astc_rows(*info, linear.data(), lv.row_bytes, lv.width,
                                                           lv.height, &wide);
                    if (delta != nullptr) {
                        ++delta->astc_cpu_decoded;
                        delta->astc_error_blocks += errors;
                    }
                    data = &wide;
                    row = lv.width * 4;
                    rows = lv.height;
                    size = {lv.width, lv.height, 1};
                } else if (t.bc_cpu) {
                    // Layers of 2D arrays and cube maps come one by one; BCn
                    // has no 3D textures in WebGPU (sliced 3D: per slice).
                    const uint32_t slices = t.is_3d() ? std::max(lv.depth, 1u) : 1u;
                    const size_t slice_bytes = size_t(lv.row_bytes) * lv.blocks_y;
                    wide.clear();
                    std::vector<uint8_t> slice;
                    for (uint32_t z = 0; z < slices && slice_bytes * (z + 1) <= linear.size(); ++z) {
                        decode_bc_rows(t.bc_format, linear.data() + slice_bytes * z, lv.row_bytes, lv.width,
                                       lv.height, &slice, decode_pool.get());
                        wide.insert(wide.end(), slice.begin(), slice.end());
                    }
                    if (delta != nullptr)
                        ++delta->bc_cpu_decoded;
                    data = &wide;
                    row = lv.width * bc_host_texel_bytes(t.bc_format);
                    rows = lv.height;
                    size = {lv.width, lv.height, slices};
                }
                if (delta != nullptr)
                    delta->texture_convert_us += micros_since(t_convert);
                WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
                dst.texture = t.texture;
                dst.mipLevel = l;
                dst.origin = {0, 0, layer};
                WGPUTexelCopyBufferLayout src = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
                src.bytesPerRow = row;
                src.rowsPerImage = rows;
                const auto t_write = std::chrono::steady_clock::now();
                queue_write_texture(queue, &dst, data->data(), data->size(), &src, &size);
                if (delta != nullptr) {
                    delta->texture_write_us += micros_since(t_write);
                    delta->texture_write_bytes += data->size();
                    ++delta->texture_writes;
                }
            }
        }
        return true;
    }

    // Pitch-linear pool texture (TextureDesc::linear_stride), level 0.
    bool upload_linear(Texture& t, const nvn::GuestMemoryView* memory, WebGpuBackend::Stats* delta) {
        const auto info = nvn::format_info(t.desc.format);
        if (!info)
            return false;
        const uint32_t bx = (t.desc.width + info->block_width - 1) / info->block_width;
        const uint32_t by = (t.desc.height + info->block_height - 1) / info->block_height;
        const uint32_t row = bx * info->bytes_per_pixel, stride = t.desc.linear_stride;
        if (bx == 0 || by == 0 || stride < row)
            return false;
        const size_t bytes = size_t(stride) * (by - 1) + row;
        if (delta != nullptr)
            delta->texture_linear_bytes += bytes;
        // Unchanged (the usual case): compared in guest memory, no copy.
        if (t.uploaded && t.linear_shadow.size() == bytes && memory->equal(t.desc.storage, t.linear_shadow.data(), bytes))
            return false;
        t.linear_shadow.resize(bytes);
        if (!memory->read(t.desc.storage, t.linear_shadow.data(), bytes)) {
            t.linear_shadow.clear();
            return false;
        }
        const std::vector<uint8_t>& raw = t.linear_shadow;
        t.uploaded = true;
        std::vector<uint8_t> packed(size_t(row) * by), wide;
        for (uint32_t y = 0; y < by; ++y)
            std::memcpy(packed.data() + size_t(y) * row, raw.data() + size_t(y) * stride, row);
        // Debug (LGP_WEBGPU_DUMP_DIR, private): one-byte linear textures as PGM.
        if (const char* dir = dump_dir(); dir != nullptr && info->bytes_per_pixel == 1 &&
                                                                 info->block_width == 1) {
            const std::string path = std::string(dir) + "/linear" + std::to_string(uintptr_t(&t) & 0xFFFFFF) + ".pgm";
            if (FILE* f = std::fopen(path.c_str(), "wb")) {
                std::fprintf(f, "P5\n%u %u\n255\n", bx, by);
                std::fwrite(packed.data(), 1, packed.size(), f);
                std::fclose(f);
            }
        }
        const std::vector<uint8_t>* data = &packed;
        uint32_t bytes_per_row = row;
        if (is_norm16(t.desc.format))
            norm16_to_half(t.desc.format, packed.data(), packed.size());
        if (is_widened_16bit(t.desc.format)) {
            widen_16bit(t.desc.format, packed, &wide);
            data = &wide;
            bytes_per_row = bx * 4;
        }
        uint32_t rows = by;
        WGPUExtent3D size = {bx * info->block_width, by * info->block_height, 1};
        if (t.astc_cpu) {
            const size_t errors = decode_astc_rows(*info, packed.data(), row, t.desc.width, t.desc.height, &wide);
            if (delta != nullptr) {
                ++delta->astc_cpu_decoded;
                delta->astc_error_blocks += errors;
            }
            data = &wide;
            bytes_per_row = t.desc.width * 4;
            rows = t.desc.height;
            size = {t.desc.width, t.desc.height, 1};
        } else if (t.bc_cpu) {
            decode_bc_rows(t.bc_format, packed.data(), row, t.desc.width, t.desc.height, &wide, decode_pool.get());
            if (delta != nullptr)
                ++delta->bc_cpu_decoded;
            data = &wide;
            bytes_per_row = t.desc.width * bc_host_texel_bytes(t.bc_format);
            rows = t.desc.height;
            size = {t.desc.width, t.desc.height, 1};
        }
        WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
        dst.texture = t.texture;
        WGPUTexelCopyBufferLayout src = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
        src.bytesPerRow = bytes_per_row;
        src.rowsPerImage = rows;
        queue_write_texture(queue, &dst, data->data(), data->size(), &src, &size);
        return true;
    }

    WGPUDevice device = nullptr;
    WGPUQueue queue = nullptr;
    bool depth_clip_control = false;  // device has WGPUFeatureName_DepthClipControl (unclippedDepth)
    bool rg11b10_renderable = false;  // device has WGPUFeatureName_RG11B10UfloatRenderable
    uint32_t max_vertex_buffers = kMaxVertexBuffers;  // device limit (the renderer requests the adapter's)
    // Diagnostics: the depth target last bound together with each colour target.
    std::unordered_map<nvn::TextureId, nvn::TextureId> depth_partner;
    std::atomic<uint64_t> depth_partner_count{0};  // Stats::depth_partners
    // A destroyed texture leaves depth_partner, as a colour target and as the
    // depth target noted with one (texture ids are never reused).
    void forget_depth_partner(nvn::TextureId id) {
        depth_partner.erase(id);
        for (auto it = depth_partner.begin(); it != depth_partner.end();)
            it = it->second == id ? depth_partner.erase(it) : std::next(it);
        depth_partner_count.store(depth_partner.size(), std::memory_order_relaxed);
    }
    std::unordered_map<nvn::TextureId, Texture> textures;
    // Textures get their GPU side on first use instead of in create_texture()
    // (LGP_WEBGPU_LAZY_TEXTURES; P7.3, phones run out of GPU memory).
    bool lazy_textures = false;
    uint64_t frame = 1;                 // presents + 1 (Texture::last_used)
    uint64_t texture_report_every = 0;  // LGP_WEBGPU_TEXTURE_REPORT=N: report every N presents
    uint64_t texture_budget = 0;        // LGP_WEBGPU_TEXTURE_BUDGET_MB, bytes; 0 = no limit
    uint64_t texture_live = 0;          // bytes of the textures with a GPU side (Texture::plan)
    uint64_t textures_evicted = 0, texture_bytes_evicted = 0;

    // Blits (CmdCopyTextureToTexture with scaling or format conversion).
    WGPUShaderModule blit_shader = nullptr;
    WGPUBindGroupLayout blit_layout = nullptr;
    WGPUPipelineLayout blit_pipeline_layout = nullptr;
    WGPUSampler blit_samplers[2] = {nullptr, nullptr};  // nearest, linear
    std::map<uint32_t, WGPURenderPipeline> blit_pipelines;  // by destination format

    // additive: adds the source onto the destination (half-resolution draws).
    WGPURenderPipeline blit_pipeline(WGPUTextureFormat format, bool additive = false) {
        if (blit_shader == nullptr) {
            blit_shader = pw::create_wgsl_module(device, "nvn blit", kBlitWgsl);
            WGPUBindGroupLayoutEntry e[3] = {WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT, WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT,
                                             WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT};
            for (uint32_t i = 0; i < 3; ++i) {
                e[i].binding = i;
                e[i].visibility = WGPUShaderStage_Fragment;
            }
            e[0].texture.sampleType = WGPUTextureSampleType_Float;
            e[0].texture.viewDimension = WGPUTextureViewDimension_2D;
            e[1].sampler.type = WGPUSamplerBindingType_Filtering;
            e[2].buffer.type = WGPUBufferBindingType_Uniform;
            e[2].buffer.minBindingSize = sizeof(BlitUniform);
            WGPUBindGroupLayoutDescriptor ld = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
            ld.entryCount = 3;
            ld.entries = e;
            blit_layout = wgpuDeviceCreateBindGroupLayout(device, &ld);
            WGPUPipelineLayoutDescriptor pl = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
            pl.bindGroupLayoutCount = 1;
            pl.bindGroupLayouts = &blit_layout;
            blit_pipeline_layout = wgpuDeviceCreatePipelineLayout(device, &pl);
            for (int linear = 0; linear < 2; ++linear) {
                WGPUSamplerDescriptor sd = WGPU_SAMPLER_DESCRIPTOR_INIT;
                sd.magFilter = sd.minFilter = linear ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
                blit_samplers[linear] = wgpuDeviceCreateSampler(device, &sd);
            }
        }
        const uint32_t key = uint32_t(format) | (additive ? 0x80000000u : 0u);
        auto it = blit_pipelines.find(key);
        if (it != blit_pipelines.end())
            return it->second;
        WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
        target.format = format;
        WGPUBlendState add = WGPU_BLEND_STATE_INIT;
        add.color.operation = add.alpha.operation = WGPUBlendOperation_Add;
        add.color.srcFactor = add.color.dstFactor = WGPUBlendFactor_One;
        add.alpha.srcFactor = add.alpha.dstFactor = WGPUBlendFactor_One;
        if (additive)
            target.blend = &add;
        WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
        fragment.module = blit_shader;
        fragment.entryPoint = pw::string_view("fs");
        fragment.targetCount = 1;
        fragment.targets = &target;
        WGPURenderPipelineDescriptor desc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
        desc.label = pw::string_view("nvn blit");
        desc.layout = blit_pipeline_layout;
        desc.vertex.module = blit_shader;
        desc.vertex.entryPoint = pw::string_view("vs");
        desc.fragment = &fragment;
        WGPURenderPipeline p = wgpuDeviceCreateRenderPipeline(device, &desc);
        blit_pipelines[key] = p;
        return p;
    }

    WGPUShaderModule clear_shader = nullptr;
    WGPUBindGroupLayout clear_layout = nullptr;
    WGPUPipelineLayout clear_pipeline_layout = nullptr;
    WGPUBuffer clear_uniforms = nullptr;
    uint64_t clear_uniforms_size = 0;
    WGPUBindGroup clear_bind_group = nullptr;
    // (format, sample count, colour mask) -> pipeline
    std::map<std::tuple<uint32_t, uint32_t, uint32_t>, WGPURenderPipeline> color_pipelines;
    // (format, sample count, depth write, stencil write mask) -> pipeline
    std::map<std::tuple<uint32_t, uint32_t, bool, uint32_t>, WGPURenderPipeline> depth_pipelines;

    std::unordered_map<nvn::ProgramId, std::unique_ptr<Program>> programs;
    struct DrawPipeline {
        nvn::ProgramId program = 0;
        WGPURenderPipeline pipeline = nullptr;  // nullptr: rejected by validation
    };
    std::unordered_map<std::string, DrawPipeline> draw_pipelines;
    // LGP_WEBGPU_DEBUG=pipelinevariants: the requests created so far, by program.
    std::unordered_map<nvn::ProgramId, std::vector<PipelineRequest>> variant_requests;
    void note_pipeline_variant(const PipelineRequest& r, WebGpuBackend::Stats& delta) {
        std::vector<PipelineRequest>& seen = variant_requests[r.program];
        std::string best;
        size_t best_parts = SIZE_MAX;
        for (const PipelineRequest& o : seen) {
            const std::string d = pipeline_difference(r, o);
            const size_t parts = d.empty() ? 0 : size_t(std::count(d.begin(), d.end(), '+')) + 1;
            if (parts < best_parts) {
                best_parts = parts;
                best = d;
            }
        }
        ++delta.diagnostics[seen.empty() ? std::string("pipeline variant: first of its program")
                                          : "pipeline variant: " + (best.empty() ? std::string("same state") : best)];
        seen.push_back(r);
    }
    uint64_t next_program_serial = 1;
    std::unordered_map<uint64_t, nvn::ProgramId> program_by_serial;  // Program::serial -> id

    // Pipeline cache (WebGpuBackend::set_pipeline_cache). It holds the
    // programs of earlier runs (WGSL and binding layout, by Program::hash;
    // compute ones too, whose pipeline is part of the program) and their
    // draw pipelines (recipes).
    //
    // Warm-up: after each present, a few cached programs are created ahead
    // (warm_step, a small time budget), and their pipelines are created in
    // the background. When the game then creates a program with the same
    // hash, it takes the warm one with its pipelines (adopt_warm) instead of
    // compiling the shaders itself. Programs the cache did not have (or that
    // are not warm yet) get their cached pipelines built in the background as
    // soon as they exist (schedule_prebuilds).
    //
    // A warm compute program's pipeline (the expensive part with a cold
    // driver cache) is created the same way; until it is there the program
    // is compute_deferred, and a dispatch that needs it first takes it back
    // or waits (claim_compute_prebuild).
    //
    // Background creation: on the desktop worker threads
    // (prebuild_worker_count; wgpu-native objects may be used from any
    // thread; wgpu-native has no create*PipelineAsync), in the browser
    // createRenderPipelineAsync/createComputePipelineAsync, whose callbacks
    // arrive on the GPU thread. Finished ones wait in `done`
    // until execute() takes them (harvest_prebuilds). A job belongs to a
    // program serial (a warm program keeps its serial when adopted); its tag
    // is the serial's bytes followed by the recipe.
    //
    // The worker uses no error scope: wgpu-native's scopes are one stack per
    // device, not per thread, so its scopes would interleave with the GPU
    // thread's. A recipe comes from a pipeline that passed validation with
    // the same WGSL on the same adapter (the file's fingerprint), so its
    // descriptor is valid again; if not, the error lands in the GPU thread's
    // open scope (that batch is dropped) or is reported as uncaptured.
    struct Prebuilds {
        // A draw pipeline (build), or a warm compute program's pipeline
        // (compute; its recipe is empty).
        struct Job {
            std::string recipe;
            uint64_t serial = 0;
            std::unique_ptr<PipelineBuild> build;
            std::unique_ptr<ComputeBuild> compute;
            bool on_use = false;  // async pipelines: for a list or program now, not ahead (not counted as prebuilt)
            // The worker takes the lowest first: the record's position in
            // the cache file plus one, so pipelines come in the order the game
            // first needed them, not program by program (0: needed now, ahead
            // of every cached record).
            uint64_t order = 0;
        };
        struct Done {
            std::string recipe;
            uint64_t serial = 0;
            WGPURenderPipeline pipeline = nullptr;  // nullptr: creation failed
            WGPUComputePipeline compute = nullptr;
            bool is_compute = false;
            bool on_use = false;
        };
        static std::string tag(uint64_t serial, std::string_view recipe) {
            std::string t(reinterpret_cast<const char*>(&serial), sizeof(serial));
            return t.append(recipe);
        }
        WGPUDevice device = nullptr;
        bool async = false;  // createRenderPipelineAsync instead of the worker
        std::mutex mutex;
        std::condition_variable cv;
        std::deque<Job> queue;                    // for the worker
        std::unordered_multiset<std::string> running;  // tags of the jobs the workers run now
        std::vector<Done> done;
        std::unordered_set<std::string> pending;  // tags queued, running or in flight, not yet harvested
        uint64_t busy_us = 0;                     // worker time
        size_t outstanding = 0;                   // jobs not yet in done, warm programs not yet checked (wait_prebuilds)
        size_t scheduled = 0;                     // everything ever counted in outstanding (prebuild_progress)
        bool stop = false;
        bool held = false;  // workers take no new job (hold_prebuilds)
        // Async: told each finished job's tag (GPU thread, not after stop).
        std::function<void(const std::string&)> on_finish;

        // In `done`, not harvested yet (mutex held).
        bool is_done(const std::string& t) const {
            for (const Done& d : done)
                if (tag(d.serial, d.recipe) == t)
                    return true;
            return false;
        }

        // Async callbacks.
        void finish(Job&& job, WGPURenderPipeline pipeline, WGPUComputePipeline compute = nullptr) {
            std::string finished = tag(job.serial, job.recipe);
            bool stopped;
            {
                std::lock_guard lock(mutex);
                --outstanding;
                stopped = stop;
                if (stop) {
                    if (pipeline != nullptr) wgpuRenderPipelineRelease(pipeline);
                    if (compute != nullptr) wgpuComputePipelineRelease(compute);
                } else {
                    done.push_back(
                        {std::move(job.recipe), job.serial, pipeline, compute, job.compute != nullptr, job.on_use});
                }
            }
            cv.notify_all();
            if (!stopped && on_finish)
                on_finish(finished);
        }
    };
    std::shared_ptr<PipelineCacheFile> pipeline_cache;
    // The driver's pipeline cache next to it (LGP_DRIVER_PIPELINE_CACHE),
    // written back when the pipeline cache has grown, at most every
    // kDriverCacheSaveInterval and at the end.
    DriverCache driver_cache = nullptr;
    std::string driver_cache_path;
    uint64_t driver_cache_pipelines = 0;  // g_driver_cache_pipelines at the last save
    uint64_t driver_cache_opened_at = 0;  // ... when the cache was opened
    // Periodic saves run here, off the guest thread that executed the list.
    std::mutex driver_cache_mutex;
    std::thread driver_cache_saver;
    bool driver_cache_saving = false;  // under driver_cache_mutex
    void write_driver_cache(uint64_t pipelines);
    std::chrono::steady_clock::time_point driver_cache_saved;
    int64_t driver_cache_loaded = -1;  // Stats::driver_cache_loaded
    static constexpr auto kDriverCacheSaveInterval = std::chrono::seconds(30);
    void open_driver_cache(const std::string& path, const std::string& seed);
    void save_driver_cache(bool final);
    std::shared_ptr<Prebuilds> prebuilds;
    // Tests on the desktop (LGP_WEBGPU_PREBUILD_POP=late|fail): wgpu-native
    // settles schedule_prebuilds' error scope inside the call, the browser
    // later. With `late` it settles as in the browser: once a list waits
    // (create_pipelines_async), or at wait_prebuilds and teardown; `fail`
    // also reports the scope as invalid. GPU thread.
    int prebuild_pop_test = 0;  // 0 off, 1 late, 2 late and invalid
    std::vector<std::function<void()>> late_prebuild_pops;
    void settle_late_prebuild_pops() {
        std::vector<std::function<void()>> pops;
        pops.swap(late_prebuild_pops);
        for (auto& settle : pops) settle();
    }
    std::vector<std::thread> prebuild_workers;

    // Async pipelines (browser default, LGP_WEBGPU_ASYNC_PIPELINES): pipelines
    // are created with create*PipelineAsync, with or without a pipeline
    // cache. A compute program starts its pipeline at create_program and
    // does not wait for it; a list whose draws or dispatches need pipelines
    // that are not there yet creates the missing ones side by side and is
    // recorded once all of them (and those already under way) are done.
    // Without it the browser creates them one after the other in the GPU
    // process and the guest thread waits for each (seconds per compute
    // pipeline with a cold shader cache). Prebuilds is then always there
    // (async).
    bool async_pipelines = false;
    // What the execute() in progress waits for (GPU thread).
    struct PipelineWait {
        std::unordered_set<std::string> tags;  // prebuild tags not finished yet
        size_t scopes = 0;                     // error scopes not popped yet
        bool armed = false;                    // set once everything is started
        std::function<void()> then;
        uint64_t serial = 0;                   // for the timeout
        Stats* delta = nullptr;
        // Browser: the pending timeout (emscripten_set_timeout keeps the
        // runtime alive until it fires, which delayed exit() by up to
        // pipeline_wait_ms) and its argument; cleared once the wait ends.
        long timeout_id = 0;
        void* timeout_arg = nullptr;
    };
    struct PipelineWaitTimeout {
        State* s;
        std::shared_ptr<Prebuilds> pb;
        uint64_t serial;
    };
    // Ends a wait's pending timeout (browser; no-op elsewhere).
    void clear_pipeline_wait_timeout(PipelineWait& w);
    std::unique_ptr<PipelineWait> pipeline_wait;
    uint64_t next_pipeline_wait = 1;
    // A wait not done after this long (a callback that never comes) creates
    // what is left synchronously. LGP_WEBGPU_PIPELINE_WAIT_MS overrides.
    uint32_t pipeline_wait_ms = 20000;
    // Async pipelines without waiting (LGP_WEBGPU_PIPELINE_NOWAIT=1): a list
    // whose draw pipelines are still compiling is recorded at once and those
    // draws are skipped until their pipeline is there (an effect is missing
    // for a few frames instead of the game stalling). Compute pipelines are
    // still waited for: their results go back to guest memory.
    bool pipeline_nowait = false;
    // Without async pipelines (desktop, phones): the pipelines a list misses
    // are created on this many threads side by side, the list waiting for
    // all of them (LGP_WEBGPU_PIPELINE_THREADS=N, 1..16; 0 or 1: one after
    // another on the GPU thread). 4, measured cold on an M1 (Metal, draw
    // pipelines 31.0 -> 13.5 s) and a PC (Vulkan, RTX 5070 Ti, 8.1 -> 3.6 s),
    // images unchanged; Android 1 until measured on a phone.
#if defined(__ANDROID__)
    unsigned pipeline_threads = 1;
#else
    unsigned pipeline_threads = 4;
#endif
    // Draw pipeline keys compiling without a wait, with what the pipeline
    // cache records once they are there (program hash, recipe).
    std::unordered_map<std::string, std::pair<uint64_t, std::string>> nowait_pending;
    // Draw pipeline keys a list skipped draws for while they compiled.
    std::unordered_set<std::string> compiling_keys;
    void expire_pipeline_wait();
    // Adds a prebuild in flight to pipeline_wait; false if it is not in flight.
    bool await_prebuild(const std::string& tag);
    // Runs pipeline_wait's continuation once nothing is outstanding.
    void check_pipeline_wait();
    // execute(), async pipelines: starts the missing draw and compute
    // pipelines side by side and calls `record` once they and those already
    // under way (pipeline_wait) are done; invalid ones are marked so their
    // draws and dispatches are skipped.
    // pipeline_nowait: the preparation of these draw pipelines failed; their
    // draws are skipped (a pipeline that arrives anyway is released).
    void reject_nowait(const std::vector<std::pair<std::string, nvn::ProgramId>>& keys);
    // False once the backend is torn down (GPU thread), for callbacks that
    // nothing waits for (the browser runs them later).
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
    void create_pipelines_async(std::vector<PipelineRequest> missing, const std::vector<nvn::ProgramId>& missing_compute,
                                const std::vector<nvn::ProgramId>& deferred_compute,
                                std::vector<std::string> awaited_keys, Stats& delta, std::function<void()> record);

    // Programs created ahead from the cache, by serial; not in `programs`
    // until a create_program with the same hash adopts them.
    struct Warm {
        std::unique_ptr<Program> program;
        std::unordered_map<std::string, WGPURenderPipeline> pipelines;  // by recipe
        bool validated = false;
    };
    std::unordered_map<uint64_t, Warm> warm;
    std::unordered_map<uint64_t, uint64_t> warm_by_hash;  // Program::hash -> serial
    std::vector<uint64_t> warm_todo;                      // hashes of cached programs, in file order
    size_t warm_next = 0;
    // Warm every cached program at once when the cache is set, instead of a
    // few per frame: with async pipelines (browser) creating a program is
    // cheap on the GPU thread and its pipelines compile side by side in the
    // background, so all of them start long before the game needs them
    // (a cold Metal compile of one compute pipeline took 8.4 s in Chrome on
    // the Mac even with Chrome's shader cache warm). LGP_WEBGPU_WARM_ALL=0/1;
    // default: async_pipelines, always on Android.
    bool warm_all = false;

    // Creates a graphics program's shader modules and layouts (inside the
    // caller's error scope).
    void build_graphics_program(const nvn::ProgramDesc& desc, uint64_t hash, Program& p);
    // ... a compute program's module, layouts and (with `pipeline`) its pipeline.
    void build_compute_program(const nvn::ProgramDesc& desc, uint64_t hash, Program& p, bool pipeline = true);
    // Starts creating a validated compute program's deferred pipeline in the background.
    void schedule_compute_prebuild(Program& prog, bool on_use = false);
    // Before a dispatch with a deferred compute pipeline: true if it is
    // there now (takes a queued job back, waits for a running one); false:
    // the caller creates it.
    bool claim_compute_prebuild(Program& prog, Stats& delta);
    // Releases what a program holds (not its draw pipelines).
    void release_program(Program& p);
    // p's compute pipeline with the targets in `moved` reading other table
    // entries (Program::alias_pipelines, wgsl::move_static_slots), created on
    // first use, synchronously.
    WGPUComputePipeline alias_pipeline(Program& p, const std::map<uint32_t, uint32_t>& moved);
    // Creates cached programs ahead for up to `budget_us` (GPU thread).
    void warm_step(uint64_t budget_us, Stats& delta);
    // The warm program for `hash`, now program `id` with its pipelines; nullptr if none is ready.
    std::unique_ptr<Program> adopt_warm(uint64_t hash, nvn::ProgramId id, Stats& delta);
    // Starts creating the cached pipelines of a validated program: program
    // `id`, or a warm one (id 0).
    void schedule_prebuilds(Program& prog, nvn::ProgramId id);
    // Hands jobs to the worker or to createRenderPipelineAsync (GPU thread,
    // not inside an error scope callback of wgpu-native); they already count
    // in Prebuilds::outstanding.
    static void submit_prebuilds(const std::shared_ptr<Prebuilds>& pb, std::vector<Prebuilds::Job> jobs);
    // Moves finished prebuilds to their program; stale ones are released.
    void harvest_prebuilds(Stats& delta);
    // Before creating r's pipeline itself: true if a prebuild had it ready
    // (now in draw_pipelines). Takes a queued job back, waits for a running one.
    bool claim_prebuild(const PipelineRequest& r, Stats& delta);
    // The program goes: its queued prebuilds are dropped.
    void cancel_prebuilds(uint64_t serial);
    // Rebuilds the request behind a recipe for program `id`.
    bool parse_recipe(std::string_view recipe, nvn::ProgramId id, PipelineRequest* out) const;

    // Unused global memory slots of a dispatch: one per slot (writable
    // bindings must not alias).
    WGPUBuffer dummy_storage_buffers[wgsl::kGlobalSlots] = {};
    WGPUBuffer dummy_storage(uint32_t slot) {
        WGPUBuffer& b = dummy_storage_buffers[slot];
        if (b == nullptr) {
            WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
            bd.label = pw::string_view("nvn unbound storage");
            bd.size = 16;
            bd.usage = WGPUBufferUsage_Storage;
            b = gpu_create_buffer(device, &bd);
        }
        return b;
    }

    // Zero vertex data for shader inputs without an attribute.
    WGPUBuffer zero_vertex = nullptr;
    WGPUBuffer zero_vertex_buffer() {
        if (zero_vertex == nullptr) {
            WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
            bd.label = pw::string_view("nvn zero vertex");
            bd.size = 16;
            bd.usage = WGPUBufferUsage_Vertex;
            zero_vertex = gpu_create_buffer(device, &bd);  // zero-initialized
        }
        return zero_vertex;
    }

    // Unbound texture slots sample this (1x1, transparent black).
    WGPUTexture dummy_texture = nullptr;
    WGPUTextureView dummy_view = nullptr;
    // Unbound depth bindings (or a colour texture where the shader wants depth): 1x1 depth 0.
    WGPUTexture dummy_depth_texture = nullptr;
    WGPUTextureView dummy_depth_view = nullptr;
    WGPUTextureView dummy_depth_array_view = nullptr;
    bool float32_filterable = false;  // device feature: 32-bit float textures with filtering samplers
    bool timestamps = false;  // LGP_WEBGPU_DEBUG=gputime and the device has timestamp-query
    // Their results (Stats::gpu_time_*). Shared with the read-back callbacks,
    // which may still run while the device goes away after the backend.
    struct GpuTimes {
        std::mutex mutex;
        uint64_t passes = 0, untimed = 0;
        // wgpu-native resolves raw GPU ticks (unless the instance asks for
        // normalization); Dawn and browsers resolve ns.
        double ns_per_tick = 1.0;
        Stats::GpuTime compute, render;
        std::map<uint64_t, Stats::GpuTime> programs;
        std::map<std::string, Stats::GpuTime> targets;
        std::map<uint64_t, Stats::GpuTime> draw_programs;  // passes = draws
    };
    std::shared_ptr<GpuTimes> gpu_times = std::make_shared<GpuTimes>();
    std::shared_ptr<BufferPool> buffer_pool;  // LGP_WEBGPU_BUFFER_POOL=1
    // A buffer for one list: from the pool when there is one.
    WGPUBuffer list_buffer(const WGPUBufferDescriptor* desc) {
        return buffer_pool ? buffer_pool->take(device, desc) : gpu_create_buffer(device, desc);
    }
    WGPUTexture dummy_texture_3d = nullptr;  // for texture_3d bindings without a 3D texture
    WGPUTextureView dummy_view_3d = nullptr;
    WGPUTextureView dummy_array_view = nullptr;  // the same 1x1 texture as a 2D array
    // Distinct sampler descriptions, kept for the device's life. Nothing
    // bounds them but the game's own set; one that changes its LOD clamps or
    // border colour all the time would pile them up (drivers limit sampler
    // objects, e.g. Vulkan's maxSamplerAllocationCount, often 4000).
    // Stats::samplers; each new high from 64 on, doubling, is logged.
    std::vector<std::pair<nvn::SamplerDesc, WGPUSampler>> samplers;
    std::atomic<uint64_t> sampler_count{0};
    size_t sampler_report_at = 64;

    // Upload arenas for constant buffers and for vertex/index data, kept for
    // the device's life and refilled by every command list. Uploads are
    // collected in a host copy of each chunk and written with one queue write
    // per chunk before the list is submitted (wgpu-native stages every write
    // in a new buffer, the browser copies every write out of Wasm memory).
    // Reuse needs no fence: queue writes and submits run in queue order, so
    // a write for the next list lands after the GPU has run the previous one.
    struct UploadArena {
        UploadArena(const char* l, WGPUBufferUsage u, uint64_t w) : label(l), usage(u), window(w) {}
        const char* label = "";
        WGPUBufferUsage usage = WGPUBufferUsage_None;
        // Bytes a binding may read from an upload's offset on (constant
        // buffers: the whole 64 KiB bank), kept free at each chunk's end.
        uint64_t window = 0;
        struct Chunk {
            WGPUBuffer buffer = nullptr;
            uint64_t size = 0;
            std::vector<uint8_t> host;  // contents to write
            uint64_t fill = 0;          // bytes of host written by this list
        };
        std::vector<Chunk> chunks;
        size_t current = 0;
        uint64_t used = 0;
        // Uploads of this list: (guest address, bytes, kind) -> where they went.
        std::map<std::tuple<uint64_t, uint64_t, uint32_t>, std::pair<WGPUBuffer, uint64_t>> uploaded;

        void begin_list() {
            current = 0;
            used = 0;
            uploaded.clear();
        }
        // Copies `bytes` bytes into the arena (256-byte aligned, room for the
        // window behind the offset); returns the chunk's buffer and the offset.
        std::pair<WGPUBuffer, uint64_t> put(WGPUDevice device, const void* data, uint64_t bytes) {
            const uint64_t slot = (std::max<uint64_t>(bytes, 4) + 255) & ~uint64_t{255};
            const uint64_t need = std::max(slot, window);
            while (current < chunks.size() && used + need > chunks[current].size) {
                ++current;
                used = 0;
            }
            if (current == chunks.size()) {
                Chunk c;
                c.size = std::max<uint64_t>(need, kChunkBytes);
                WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
                bd.label = pw::string_view(label);
                bd.size = c.size;
                bd.usage = usage | WGPUBufferUsage_CopyDst;
                c.buffer = gpu_create_buffer(device, &bd);
                c.host.assign(size_t(c.size), 0);
                chunks.push_back(std::move(c));
            }
            Chunk& c = chunks[current];
            const uint64_t offset = used;
            std::memcpy(c.host.data() + offset, data, size_t(bytes));
            std::memset(c.host.data() + offset + bytes, 0, size_t(slot - bytes));
            used += slot;
            c.fill = used;
            return {c.buffer, offset};
        }
        // Before the list's submit: the queue writes of what put() collected.
        void flush(WGPUQueue queue) {
            for (Chunk& c : chunks) {
                if (c.fill > 0) queue_write_buffer(queue, c.buffer, 0, c.host.data(), c.fill);
                c.fill = 0;
            }
        }
        void release() {
            for (Chunk& c : chunks) gpu_destroy_buffer(c.buffer);
            chunks.clear();
        }
        static constexpr uint64_t kChunkBytes = 4ull << 20;
    };
    UploadArena cbuf_arena{"nvn constant buffers", WGPUBufferUsage_Uniform, kCbufBytes};
    WGPUBuffer zero_cbuf = nullptr;  // banks a list never bound (64 KiB of zeros)
    UploadArena geometry_arena{"nvn vertices and indices", WGPUBufferUsage_Vertex | WGPUBufferUsage_Index, 0};
    // Vertex and index data across lists: one GPU buffer per guest range
    // (address, bytes, kind) with the guest bytes it was last written from.
    // A list whose range still holds those bytes (compared in guest memory)
    // binds the buffer as it is; otherwise it is written again. Most of a
    // frame's geometry is the same as in the frame before (Mac, Safari,
    // Alabastia: 16.3 MiB of 18.2 MiB uploaded per frame were geometry).
    // A range used earlier in the same list with other bytes (a buffer copy
    // changed guest memory in between) goes to the arena instead: the queue
    // write would land before the whole list. LGP_WEBGPU_GEOMETRY_CACHE=0
    // turns it off; the cache keeps at most geometry_cache_limit bytes.
    struct GeometryEntry {
        WGPUBuffer buffer = nullptr;
        uint64_t slot = 0;           // buffer size
        std::vector<uint8_t> guest;  // guest bytes the buffer was written from
        uint64_t fp = 0;             // ... or only their fingerprint (geometry_fingerprint)
        uint64_t list = 0;           // lists_recorded of the last list that used it
    };
    std::map<std::tuple<uint64_t, uint64_t, uint32_t>, GeometryEntry> geometry_cache;
    uint64_t geometry_cache_bytes = 0;  // GPU bytes plus guest copies
    uint64_t geometry_cache_limit = 256ull << 20;
    uint64_t geometry_cache_full_list = 0;  // list whose entries alone fill the cache
    bool geometry_cache_on = true;
    // LGP_WEBGPU_GEOMETRY_FP=0/1 (default on phones, tablets and the Apple TV): entries keep a 64-bit
    // fingerprint of the guest bytes instead of a copy (half the cache's
    // memory, all of its CPU part; hashing instead of memcmp per check). A
    // fingerprint collision would keep stale geometry, as for textures.
    bool geometry_fingerprint = false;
    std::vector<uint8_t> geometry_bytes;  // the guest bytes of a write (fingerprint mode)
    // platform::low_memory_warnings() at the last list: a new warning drops
    // the geometry cache and the free buffers of the pool (both refill).
    uint64_t low_memory_seen = 0;
    void drop_for_low_memory(uint64_t warnings) {
        low_memory_seen = warnings;
        const uint64_t geometry = geometry_cache_bytes;
        const size_t entries = geometry_cache.size();
        for (auto& [key, e] : geometry_cache) gpu_destroy_buffer(e.buffer);
        geometry_cache.clear();
        geometry_cache_bytes = 0;
        const uint64_t pooled = buffer_pool ? buffer_pool->drain() : 0;
        platform::ProcessMemory m;
        const bool known = platform::process_memory(&m);
        rt::log(rt::LogLevel::Info,
                "nvn webgpu: low memory warning %llu: dropped geometry cache (%zu entries, %llu MiB) and %llu MiB "
                "of pooled buffers; footprint %llu MiB, available %llu MiB (GPU memory goes back once the "
                "lists in flight are done)",
                (unsigned long long)warnings, entries, (unsigned long long)(geometry >> 20),
                (unsigned long long)(pooled >> 20), (unsigned long long)(known ? m.footprint >> 20 : 0),
                (unsigned long long)(known ? m.available >> 20 : 0));
    }
    // Drops the least recently used entries until the cache is below 3/4 of
    // its limit (not those of list `keep`, which are bound in it).
    void trim_geometry_cache(uint64_t keep) {
        std::vector<std::pair<uint64_t, std::tuple<uint64_t, uint64_t, uint32_t>>> by_age;
        by_age.reserve(geometry_cache.size());
        for (const auto& [key, e] : geometry_cache)
            if (e.list != keep) by_age.emplace_back(e.list, key);
        std::sort(by_age.begin(), by_age.end());
        for (const auto& [list, key] : by_age) {
            if (geometry_cache_bytes <= geometry_cache_limit / 4 * 3)
                break;
            auto it = geometry_cache.find(key);
            geometry_cache_bytes -= it->second.slot + it->second.guest.size();  // guest empty with fingerprints
            gpu_destroy_buffer(it->second.buffer);
            geometry_cache.erase(it);
        }
    }

    // LGP_WEBGPU_DEBUG=computeoverlap: guest ranges compute dispatches wrote.
    // A list's storage buffers go back to guest memory after its submit; the
    // map callbacks (any thread) set fp and done.
    struct WrittenBack {
        uint64_t address = 0, size = 0;
        uint64_t list = 0;  // lists_recorded of the writing list
        uint64_t fp = 0;    // fingerprint of the bytes written back
        bool done = false;
        uint32_t age = 0;  // lists recorded since the writeback arrived
    };
    // Backpressure at present: frames submitted and not yet done on the GPU.
    // Without it the browser accepts every submit and queue write and keeps
    // their data until the GPU catches up, so a GPU slower than the guest
    // grows memory without bound. present() returns once at most
    // max_frames_in_flight - 1 earlier frames are still running
    // (LGP_WEBGPU_FRAMES_IN_FLIGHT; default 0 = no limit: Safari ran
    // slower with it, docs/knowledge/webgpu-backend-desktop-und-browser.md).
    uint32_t max_frames_in_flight = 0;
    std::atomic<uint32_t> frames_in_flight{0};
    std::mutex frame_mutex;
    std::vector<std::pair<uint32_t, std::function<void()>>> frame_waiters;  // (at most in flight, continuation)
    std::atomic<uint64_t> frames_in_flight_max{0}, frame_waits{0}, frame_wait_us{0};
    std::atomic<uint64_t> list_submits{0}, list_submit_ns{0}, warm_steps{0}, warm_step_ns{0};
    void frame_submitted() {
        const uint32_t n = frames_in_flight.fetch_add(1, std::memory_order_acq_rel) + 1;
        uint64_t m = frames_in_flight_max.load(std::memory_order_relaxed);
        while (n > m && !frames_in_flight_max.compare_exchange_weak(m, n, std::memory_order_relaxed)) {}
        WGPUQueueWorkDoneCallbackInfo cb = WGPU_QUEUE_WORK_DONE_CALLBACK_INFO_INIT;
        cb.mode = WGPUCallbackMode_AllowSpontaneous;
        cb.userdata1 = this;
        cb.callback = [](WGPUQueueWorkDoneStatus, WGPUStringView, void* u, void*) {
            static_cast<State*>(u)->frame_done();
        };
        wgpuQueueOnSubmittedWorkDone(queue, cb);
    }
    void frame_done() {
        std::vector<std::function<void()>> ready;
        {
            std::lock_guard lock(frame_mutex);
            const uint32_t n = frames_in_flight.fetch_sub(1, std::memory_order_acq_rel) - 1;
            for (size_t i = 0; i < frame_waiters.size();)
                if (n <= frame_waiters[i].first) {
                    ready.push_back(std::move(frame_waiters[i].second));
                    frame_waiters.erase(frame_waiters.begin() + ptrdiff_t(i));
                } else {
                    ++i;
                }
        }
        for (auto& fn : ready) fn();
    }
    // Runs fn once at most `limit` frames are in flight: natively after
    // polling the device, in the browser from the work-done callback (the
    // main thread keeps running its event loop meanwhile).
    void when_frames_at_most(uint32_t limit, std::function<void()> fn) {
        if (frames_in_flight.load(std::memory_order_acquire) > limit) {
            frame_waits.fetch_add(1, std::memory_order_relaxed);
            const auto t0 = std::chrono::steady_clock::now();
#if defined(__EMSCRIPTEN__)
            std::unique_lock lock(frame_mutex);
            if (frames_in_flight.load(std::memory_order_acquire) > limit) {
                frame_waiters.emplace_back(limit, [this, t0, fn = std::move(fn)] {
                    frame_wait_us.fetch_add(micros_since(t0), std::memory_order_relaxed);
                    fn();
                });
                return;
            }
#else
            PollWatch watch("waiting for frames in flight");
            while (frames_in_flight.load(std::memory_order_acquire) > limit) {
                wgpuDevicePoll(device, true, nullptr);
                watch.check();
            }
#endif
            frame_wait_us.fetch_add(micros_since(t0), std::memory_order_relaxed);
        }
        fn();
    }
    std::mutex diag_mutex;
    std::vector<WrittenBack> written_back;

    // Compute results on their way back to guest memory. A list's call
    // returns right after its submit (the guest runs on meanwhile); every
    // later call that reads guest memory or lets the guest look at results
    // (execute, present, read_texture, finish) first waits for them.
    // Only with LGP_WEBGPU_ASYNC_WRITEBACK=1; by default the list's call
    // waits itself (asynchronous gained little in real play, Safari ran slower).
    bool async_writeback = false;
    // wgpu-native: waits for a write-back block on the submission that
    // carries its read-back copies instead of the latest submission, which
    // with compute first is the list's draws (docs/analysis/39-rueckschrieb-
    // warten-strukturell.md). LGP_WEBGPU_POLL_SUBMIT_INDEX=1, off until
    // measured. No effect in the browser (no blocking poll there).
    bool poll_submit_index = false;
    // A list's dispatches that depend on nothing earlier in the list are
    // submitted ahead of its draws, with their read-back, so a synchronous
    // write-back waits for the compute work only
    // (docs/analysis/20-route1-30fps-zeitaufteilung.md, measurement H).
    // LGP_WEBGPU_COMPUTE_FIRST=0 records them in order as before.
    bool compute_first = true;
    // A vertex stream that only partly overlaps the output of a dispatch
    // earlier in its list (dispatch_output needs the stream inside it) reads
    // guest memory, which gets the results only after the list: the draw
    // sees the old bytes. With LGP_WEBGPU_PATCH_DISPATCH_OUTPUT=1 such a
    // stream is copied into a buffer of its own and the overlapping parts
    // are copied over on the GPU from the dispatch's storage buffer.
    bool patch_dispatch_output = false;
    // A dispatch submitted ahead of the list leaves the open draw pass open
    // (it records into another encoder); LGP_WEBGPU_KEEP_PASS_ON_DISPATCH=0/1,
    // default on Android (measured there, P7.3), elsewhere off.
    bool keep_pass_on_dispatch = false;
    // Diagnosis: draws into the chosen targets land in their top-left part,
    // viewport and scissor scaled by fill_scale, clears stay full. A wrong
    // picture that bounds what rendering those targets smaller could save
    // (docs/analysis/41-fuellflaeche-hauptziel.md). LGP_WEBGPU_FILL_SCALE=0.75
    // with LGP_WEBGPU_FILL_TARGETS=1280x720[,WxH...] (unset: every target of
    // at least 1280x720, like halffill); off by default. The pipelines stay
    // the same, so the pipeline cache stays on.
    float fill_scale = 1.0f;
    std::vector<std::pair<uint32_t, uint32_t>> fill_targets;
    bool fills(uint32_t w, uint32_t h) const {
        if (fill_scale >= 1.0f)
            return false;
        if (fill_targets.empty())
            return uint64_t(w) * h >= 1280u * 720u;
        for (const auto& [tw, th] : fill_targets)
            if (tw == w && th == h) {
                ++fill_hits;
                return true;
            }
        return false;
    }
    // Draws that met one of fill_targets; none by frame 600 is reported
    // (a wrong size, e.g. with a widescreen mod, would otherwise go unnoticed).
    mutable std::atomic<uint64_t> fill_hits{0};  // fills() runs on the async backend's thread too
    // Textures scale_target replaced while a list was recorded; record_list
    // destroys them after its submit.
    std::vector<WGPUTexture> retired_textures;
    // Render scale (LGP_WEBGPU_RENDER_SCALE=s, 0.25..2, default 1 = off;
    // docs/analysis/37-render-skalierung-entwurf.md): every 2D texture the
    // game binds as a render target before anything else wrote it is
    // created at s times its size (both sides at least render_scale_min
    // guest texels; smaller ones, such as 1x1 averages, keep theirs).
    // Shaders see guest sizes and texel coordinates (lgp_render_scale in the
    // translated WGSL, wgsl::Options::render_scale).
    float render_scale = 1.0f;
    uint32_t render_scale_min = 16;
    // Why a texture cannot become a scaled one (nullptr = it can).
    const char* scale_refusal(const Texture& t, bool contents_ok = false) const {
        if (t.scaled)
            return nullptr;
        if (t.gpu_written && !contents_ok)
            return "already written";
        if (t.uploaded && !contents_ok)
            return "uploaded";
        if (t.plan.td.format == WGPUTextureFormat_Undefined || t.plan.compressed)
            return "format";
        // Scaled targets go through blits (contents, uploads, copies between
        // sizes): float-filterable formats only.
        if (!blit_filterable(t.plan.td.format))
            return "integer or 32-bit float format";
        if (t.plan.target_3d || t.plan.td.size.depthOrArrayLayers != 1)
            return "3D or layers";
        if (t.desc.width < render_scale_min || t.desc.height < render_scale_min)
            return "below LGP_WEBGPU_RENDER_SCALE_MIN";
        if ((t.plan.td.usage & WGPUTextureUsage_RenderAttachment) == 0)
            return "not renderable";
        return nullptr;
    }
    // Turns a render target into a scaled one before its first use; true if
    // it is scaled now. Only textures that hold nothing yet: not uploaded,
    // not GPU-written. With `written_ok` a written or uploaded one too; its
    // contents survive only if the caller copies them over (record_list's
    // scale_with_contents).
    bool scale_target(Texture& t, bool written_ok = false) {
        if (render_scale == 1.0f || t.scaled)
            return t.scaled;
        if (scale_refusal(t, written_ok) != nullptr)
            return false;
        const uint32_t w = std::max<uint32_t>(uint32_t(std::lround(double(t.desc.width) * render_scale)), 1u);
        const uint32_t h = std::max<uint32_t>(uint32_t(std::lround(double(t.desc.height) * render_scale)), 1u);
        // WebGPU's default limit (maxTextureDimension2D).
        if (w > 8192 || h > 8192)
            return false;
        if (!t.lazy) {
            clear_bind_group_cache();  // cached groups may hold its views
            // The list being recorded may already use the old texture: it is
            // destroyed after the submit (retired_textures), its views go now.
            for (WGPUTexture* old : {&t.texture, &t.compare_texture})
                if (*old != nullptr) {
                    if (old == &t.texture) texture_live -= std::min(texture_live, texture_bytes(t.plan.td));
                    retired_textures.push_back(*old);
                    *old = nullptr;
                }
            release_gpu_texture(t);
        }
        t.plan.td.size.width = w;
        t.plan.td.size.height = h;
        // Levels as many as the scaled size allows.
        uint32_t levels = 1;
        while ((std::max(w, h) >> levels) != 0) ++levels;
        t.plan.td.mipLevelCount = std::min(t.plan.td.mipLevelCount, levels);
        t.scaled = true;
        t.gpu_written = true;  // never uploaded from the pool (guest-sized texels)
        materialize(t);
        ++render_scaled_targets;
        if (render_scaled_targets <= 32)
            rt::log(rt::LogLevel::Info, "nvn webgpu: render target %ux%u at %ux%u (render scale)",
                    t.desc.width, t.desc.height, w, h);
        return true;
    }
    uint64_t render_scaled_targets = 0;
    // A whole-texture clear waits for the next draw pass on that texture and
    // becomes its clear load op instead of a pass of its own;
    // LGP_WEBGPU_FOLD_CLEARS=0/1, default on Android (measured there, P7.3),
    // elsewhere off.
    bool fold_clears = false;
    // Render targets set again unchanged leave the open draw pass open;
    // LGP_WEBGPU_KEEP_PASS_SAME_TARGETS=0/1, default as fold_clears.
    bool keep_pass_same_targets = false;
    // Attachment view sets seen in render passes (Stats::attachment_sets_new),
    // forgotten past kMaxAttachmentSets.
    std::unordered_set<uint64_t> attachment_sets;
    static constexpr size_t kMaxAttachmentSets = 4096;
    void note_attachments(const WGPURenderPassDescriptor& pd, Stats& delta) {
        uint64_t h = 1469598103934665603ull;
        auto mix = [&h](const void* p) {
            h ^= uint64_t(reinterpret_cast<uintptr_t>(p));
            h *= 1099511628211ull;
        };
        for (size_t i = 0; i < pd.colorAttachmentCount; ++i)
            mix(pd.colorAttachments[i].view);
        mix(pd.depthStencilAttachment != nullptr ? pd.depthStencilAttachment->view : nullptr);
        if (attachment_sets.size() >= kMaxAttachmentSets)
            attachment_sets.clear();
        if (attachment_sets.insert(h).second)
            ++delta.attachment_sets_new;
    }
    std::atomic<uint32_t> writebacks_pending{0};
    // Lists submitted so far (fence marks); write-backs pending by the
    // number of their list.
    std::atomic<uint64_t> lists_submitted{0};
    std::mutex writeback_mutex;
    std::multiset<uint64_t> pending_lists;
    // poll_submit_index: submission of each pending list's read-back.
    std::map<uint64_t, uint64_t> pending_submission;
    // Browser: (before, continuation) run once no write-back of a list
    // before `before` is pending.
    std::vector<std::pair<uint64_t, std::function<void()>>> after_writeback;
    std::atomic<uint64_t> writebacks{0}, writeback_waits{0}, writeback_wait_us{0};
    std::atomic<uint64_t> finish_calls{0}, finish_pending{0};
    bool pending_before(uint64_t before) {
        std::lock_guard lock(writeback_mutex);
        return !pending_lists.empty() && *pending_lists.begin() < before;
    }
    // The latest read-back submission of the lists before `before` still
    // pending, when all of them have one (poll_submit_index).
    bool pending_submission_before(uint64_t before, uint64_t* index) {
        std::lock_guard lock(writeback_mutex);
        bool any = false;
        for (auto it = pending_lists.begin(); it != pending_lists.end() && *it < before; ++it) {
            auto sub = pending_submission.find(*it);
            if (sub == pending_submission.end())
                return false;
            *index = any ? std::max(*index, sub->second) : sub->second;
            any = true;
        }
        return any;
    }
    // Runs fn once no write-back of a list before `before` is pending:
    // natively after polling the device (on this thread), in the browser
    // from the map callback that completes them (fn then finishes the call).
    void after_writebacks(std::function<void()> fn, uint64_t before = UINT64_MAX) {
        if (writebacks_pending.load(std::memory_order_acquire) != 0 && pending_before(before)) {
            ++writeback_waits;
            const auto t0 = std::chrono::steady_clock::now();
#if defined(__EMSCRIPTEN__)
            std::unique_lock lock(writeback_mutex);
            if (!pending_lists.empty() && *pending_lists.begin() < before) {
                // Time until the map callback runs it (the guest thread
                // waits that long inside its renderer call).
                after_writeback.emplace_back(before, [this, t0, fn = std::move(fn)] {
                    writeback_wait_us += micros_since(t0);
                    fn();
                });
                return;
            }
#else
            PollWatch watch("waiting for compute write-back");
            while (pending_before(before)) {
#if defined(LGP_WEBGPU_WGPU_NATIVE)
                uint64_t index = 0;
                if (poll_submit_index && pending_submission_before(before, &index)) {
                    const WGPUSubmissionIndex i = index;
                    wgpuDevicePoll(device, true, &i);
                    watch.check();
                    continue;
                }
#endif
                wgpuDevicePoll(device, true, nullptr);
                watch.check();
            }
            writeback_wait_us += micros_since(t0);
#endif
        }
        fn();
    }
    void writeback_started(uint64_t list, const uint64_t* submission = nullptr) {
        std::lock_guard lock(writeback_mutex);
        pending_lists.insert(list);
        if (submission != nullptr)
            pending_submission[list] = *submission;
        ++writebacks_pending;
    }
    // The write-back of `list` is in guest memory: runs what no longer waits.
    void writeback_finished(uint64_t list) {
        for (bool first = true;; first = false) {
            std::function<void()> fn;
            {
                std::lock_guard lock(writeback_mutex);
                if (first) {
                    pending_lists.erase(pending_lists.find(list));
                    pending_submission.erase(list);
                    --writebacks_pending;
                }
                const uint64_t oldest = pending_lists.empty() ? UINT64_MAX : *pending_lists.begin();
                auto it = std::find_if(after_writeback.begin(), after_writeback.end(),
                                       [&](const auto& w) { return oldest >= w.first; });
                if (it == after_writeback.end())
                    return;
                fn = std::move(it->second);
                after_writeback.erase(it);
            }
            fn();
        }
    }
    uint64_t lists_recorded = 0;
    uint32_t diag_logged = 0;  // overlap lines logged so far (capped)
    // Device limit for constant buffers bound with dynamic offsets (vertex +
    // fragment banks of one program, Program::dynamic_cbufs).
    uint32_t max_dynamic_uniform_buffers = 8;

    // Bind groups kept across lists. Constant buffers: (layout, arena chunk
    // per bank) with dynamic offsets. Textures: (layout, the entries' views,
    // samplers and buffers). Cleared when a texture, program or layout goes.
    std::map<std::pair<WGPUBindGroupLayout, std::vector<WGPUBuffer>>, WGPUBindGroup> cbuf_groups;
    std::map<std::pair<WGPUBindGroupLayout, std::vector<uintptr_t>>, WGPUBindGroup> texture_groups;
    void clear_bind_group_cache() {
        for (auto& [key, g] : cbuf_groups) wgpuBindGroupRelease(g);
        for (auto& [key, g] : texture_groups) wgpuBindGroupRelease(g);
        cbuf_groups.clear();
        texture_groups.clear();
    }

    // dispatch_streams: DispatchWrites::streams of the bound vertex buffers.
    PipelineRequest pipeline_request(const DrawState& st, nvn::Topology topo, const nvn::CommandList& list,
                                     WGPUIndexFormat strip_index, const BoundTextures& bound,
                                     uint32_t dispatch_streams = 0) const;
    WGPURenderPipeline create_draw_pipeline(const PipelineRequest& r);
    // The descriptor create_draw_pipeline() creates; nullptr if the draw cannot have a pipeline.
    std::unique_ptr<PipelineBuild> prepare_draw_pipeline(const PipelineRequest& r, Program& prog);
    // The vertex shader reads inputs no attribute of the layout feeds (they
    // read zero from an extra buffer, State::zero_vertex_buffer).
    static bool unfed_inputs(const Program& prog, const nvn::CmdSetVertexLayout& layout) {
        for (uint32_t loc : prog.vertex_inputs) {
            bool fed = false;
            for (const nvn::VertexAttrib& a : layout.attribs)
                fed |= a.location == loc && a.stream < layout.strides.size();
            if (!fed)
                return true;
        }
        return false;
    }

    // A bound texture a filtering float binding cannot take: the depth
    // aspect of a depth texture (sampled without compare) and 32-bit float
    // formats without float32-filterable. Both work as unfilterable-float.
    bool needs_unfilterable(const Texture& t) const {
        if (t.texture == nullptr && !t.lazy) return false;
        if (has_depth(t.format)) return t.sample_count == 1;
        return !float32_filterable && (t.sampled != nullptr || (t.lazy && t.sample_count == 1)) &&
               (t.format == WGPUTextureFormat_R32Float || t.format == WGPUTextureFormat_RG32Float ||
                t.format == WGPUTextureFormat_RGBA32Float);
    }
    // Bit 32 * stage + k: graphics texture binding k (2D, no compare) holds
    // such a texture.
    uint64_t unfilterable_mask(const Program& prog, const BoundTextures& bound) const;
    const Program::LayoutVariant* layout_variant(Program& prog, uint64_t mask);

    WGPUSampler sampler(const nvn::SamplerDesc& desc_in) {
        // The LOD bias goes to the shader (texture_group), not into the sampler.
        nvn::SamplerDesc d = desc_in;
        d.lod_bias = 0.0f;
        for (const auto& [desc, s] : samplers)
            if (desc == d) return s;
        WGPUSamplerDescriptor sd = WGPU_SAMPLER_DESCRIPTOR_INIT;
        sd.label = pw::string_view("nvn sampler");
        sd.addressModeU = address_mode(d.wrap[0]);
        sd.addressModeV = address_mode(d.wrap[1]);
        sd.addressModeW = address_mode(d.wrap[2]);
        sd.magFilter = d.mag == nvn::Filter::Linear ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
        sd.minFilter = d.min == nvn::Filter::Linear ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
        sd.mipmapFilter = d.mip == nvn::Filter::Linear ? WGPUMipmapFilterMode_Linear : WGPUMipmapFilterMode_Nearest;
        // Without mipmapping NVN samples level 0 only.
        sd.lodMinClamp = d.mipmapped ? std::max(d.min_lod, 0.0f) : 0.0f;
        sd.lodMaxClamp = d.mipmapped ? std::max(d.max_lod, sd.lodMinClamp) : 0.0f;
        // Anisotropy needs all-linear filtering in WebGPU.
        const bool all_linear = sd.magFilter == WGPUFilterMode_Linear && sd.minFilter == WGPUFilterMode_Linear &&
                                sd.mipmapFilter == WGPUMipmapFilterMode_Linear;
        sd.maxAnisotropy = all_linear ? d.max_anisotropy : 1;
        if (d.compare)
            sd.compare = compare(d.compare_func);
        WGPUSampler s = wgpuDeviceCreateSampler(device, &sd);
        samplers.emplace_back(d, s);
        sampler_count.store(samplers.size(), std::memory_order_relaxed);
        if (samplers.size() >= sampler_report_at) {
            rt::log(sampler_report_at >= 1024 ? rt::LogLevel::Warn : rt::LogLevel::Info,
                    "nvn webgpu: %zu samplers alive (new high; one per distinct sampler description, never released)",
                    samplers.size());
            sampler_report_at *= 2;
        }
        return s;
    }

    // Depth/stencil stand-ins for draws whose depth texture is smaller or
    // larger than the colour target: NVN renders to the common area, WebGPU
    // wants equal sizes. (format, width, height, samples) -> texture and view.
    // The stand-in's contents are separate from the game's depth texture.
    std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>, std::pair<WGPUTexture, WGPUTextureView>> proxy_depth;
    // Half-resolution effects (WebGpuBackend::set_halfres_effects,
    // LGP_WEBGPU_HALFRES_PROGRAMS): the programs whose additive draws render
    // at half resolution, as hex prefixes of their hashes; only while on.
    std::vector<std::string> halfres_programs;
    bool halfres_on = false;
    // WebGpuBackend::watch_programs: hex prefixes of program hashes or guest
    // code hashes (ProgramDesc::guest_hash) whose draws take_watched_drawn()
    // reports.
    std::vector<std::string> watched_programs;
    bool watched_for(uint64_t hash, uint64_t guest_hash) const {
        char hex[17], guest[17];
        std::snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)hash);
        std::snprintf(guest, sizeof(guest), "%016llx", (unsigned long long)guest_hash);
        for (const std::string& p : watched_programs)
            if (std::string_view(hex).substr(0, p.size()) == p ||
                (guest_hash != 0 && std::string_view(guest).substr(0, p.size()) == p))
                return true;
        return false;
    }
    bool halfres_for(uint64_t hash) const {
        if (!halfres_on)
            return false;
        char hex[17];
        std::snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)hash);
        for (const std::string& p : halfres_programs)
            if (std::string_view(hex).substr(0, p.size()) == p) return true;
        return false;
    }
    // Stand-ins of half the width and height for half-resolution draws
    // (halfres_for), by format and size of the real target:
    // entries of `textures` with ids from kHalfResTextureBase down.
    std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>, nvn::TextureId> halfres_targets;
    nvn::TextureId halfres_target(const Texture& main) {
        const auto key =
            std::make_tuple(uint32_t(main.format), main.desc.width, main.desc.height, main.width(), main.height());
        if (auto it = halfres_targets.find(key); it != halfres_targets.end())
            return it->second;
        const nvn::TextureId id = kHalfResTextureBase - nvn::TextureId(halfres_targets.size());
        Texture t;
        t.desc = main.desc;
        t.desc.width = (main.desc.width + 1) / 2;
        t.desc.height = (main.desc.height + 1) / 2;
        t.desc.depth = t.desc.levels = 1;
        t.desc.storage = 0;
        t.format = main.format;
        t.plan = main.plan;
        // A scaled real target (State::render_scale) gets a stand-in of
        // half its host size, mapped like the real one.
        t.scaled = main.scaled;
        t.plan.td.size = {(main.width() + 1) / 2, (main.height() + 1) / 2, 1};
        t.plan.td.mipLevelCount = 1;
        t.gpu_written = true;  // never uploaded from guest memory
        materialize(t);
        textures[id] = t;
        halfres_targets[key] = id;
        rt::log(rt::LogLevel::Info, "nvn webgpu: half-resolution target %ux%u for %ux%u (half-resolution effects)",
                t.width(), t.height(), main.width(), main.height());
        return id;
    }

    WGPUTextureView proxy_depth_view(const Texture& depth, uint32_t width, uint32_t height) {
        const auto key = std::make_tuple(uint32_t(depth.format), width, height, depth.sample_count);
        if (auto it = proxy_depth.find(key); it != proxy_depth.end())
            return it->second.second;
        WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
        td.label = pw::string_view("nvn depth stand-in");
        td.usage = WGPUTextureUsage_RenderAttachment;
        td.size = {width, height, 1};
        td.format = depth.format;
        td.sampleCount = depth.sample_count;
        WGPUTexture tex = gpu_create_texture(device, &td);
        WGPUTextureView view = wgpuTextureCreateView(tex, nullptr);
        proxy_depth[key] = {tex, view};
        rt::log(rt::LogLevel::Warn, "nvn webgpu: depth target %ux%u used with a %ux%u colour target; drawing with "
                "a separate %ux%u depth buffer", depth.desc.width, depth.desc.height, width, height, width, height);
        return view;
    }

    // Creates the GPU texture and its views from t.plan (create_texture()).
    void materialize(Texture& t) {
        t.lazy = false;
        WGPUTextureDescriptor td = t.plan.td;
        td.label = pw::string_view("nvn texture");
        const WGPUTextureFormat raw_format = t.plan.raw_format;
        if (raw_format != t.format) {
            td.viewFormatCount = 1;
            td.viewFormats = &raw_format;
        }
        t.texture = gpu_create_texture(device, &td);
        if (t.texture != nullptr)
            texture_live += texture_bytes(t.plan.td);
        if (t.plan.target_3d) {
            WGPUTextureViewDescriptor vd3 = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
            vd3.format = t.format;
            vd3.dimension = WGPUTextureViewDimension_3D;
            vd3.mipLevelCount = td.mipLevelCount;
            vd3.arrayLayerCount = 1;
            t.sampled = wgpuTextureCreateView(t.texture, &vd3);
            return;
        }
        WGPUTextureViewDescriptor vd = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
        vd.format = t.format;
        vd.dimension = WGPUTextureViewDimension_2D;
        vd.baseMipLevel = 0;
        vd.mipLevelCount = 1;
        vd.baseArrayLayer = 0;
        vd.arrayLayerCount = 1;
        if (t.plan.compressed) {
            vd.mipLevelCount = td.mipLevelCount;
            t.sampled = wgpuTextureCreateView(t.texture, &vd);
            return;
        }
        t.attachment = wgpuTextureCreateView(t.texture, &vd);
        if (!t.plan.depth_stencil && t.sample_count == 1) {
            if (td.mipLevelCount > 1) {
                vd.mipLevelCount = td.mipLevelCount;
                t.sampled = wgpuTextureCreateView(t.texture, &vd);
                vd.mipLevelCount = 1;
            } else {
                t.sampled = t.attachment;
            }
            if (raw_format != t.format) {
                vd.format = raw_format;
                t.raw = wgpuTextureCreateView(t.texture, &vd);
            } else {
                t.raw = t.attachment;
            }
        }
    }

    // LGP_WEBGPU_TEXTURE_REPORT: where the texture memory goes (P7.3). One
    // line of totals and the largest groups (host format, CPU decoding, size).
    void texture_report() const {
        struct Group {
            uint64_t n = 0, bytes = 0, never = 0, recent = 0, written = 0, lazy = 0;
        };
        const uint64_t recent_frames = std::max<uint64_t>(texture_report_every, 1);
        std::map<std::tuple<uint32_t, int, uint32_t>, Group> groups;
        Group all;
        uint64_t bc_bytes = 0, bc_native = 0, astc_bytes = 0, rt_bytes = 0, alias_bytes = 0;
        std::unordered_map<uint64_t, int> storages;
        for (const auto& [id, t] : textures)
            if (t.plan.td.format != WGPUTextureFormat_Undefined && t.desc.storage != 0)
                ++storages[t.desc.storage];
        for (const auto& [id, t] : textures) {
            if (t.plan.td.format == WGPUTextureFormat_Undefined)
                continue;
            const uint64_t b = texture_bytes(t.plan.td);
            const int kind = t.bc_cpu ? 1 : t.astc_cpu ? 2 : 0;
            const uint32_t side = std::max(t.plan.td.size.width, t.plan.td.size.height);
            uint32_t bucket = 1;
            while (bucket < side) bucket <<= 1;
            for (Group* g : {&groups[{uint32_t(t.plan.td.format), kind, bucket}], &all}) {
                ++g->n;
                g->bytes += b;
                g->never += t.last_used == 0 ? b : 0;
                g->recent += t.last_used != 0 && t.last_used + recent_frames >= frame ? b : 0;
                g->written += t.gpu_written ? b : 0;
                g->lazy += t.lazy ? 1 : 0;
            }
            if (t.bc_cpu) {
                bc_bytes += b;
                const bool half = t.bc_format == lgp::gpu::texture::BcFormat::BC1 ||
                                  t.bc_format == lgp::gpu::texture::BcFormat::BC4 ||
                                  t.bc_format == lgp::gpu::texture::BcFormat::BC4Snorm;
                bc_native += b * (half ? 8 : 16) / bytes_per_16_texels(t.plan.td.format);
            }
            astc_bytes += t.astc_cpu ? b : 0;
            rt_bytes += t.gpu_written ? b : 0;
            alias_bytes += t.desc.storage != 0 && storages[t.desc.storage] > 1 ? b : 0;
        }
        auto mib = [](uint64_t b) { return double(b) / (1 << 20); };
        rt::log(rt::LogLevel::Info,
                "nvn webgpu: texture report frame %llu: %llu textures %.1f MiB (lazy %llu), never used %.1f MiB, "
                "used in the last %llu frames %.1f MiB, GPU-written %.1f MiB, BC decoded %.1f MiB (as BC ~%.1f MiB), "
                "ASTC decoded %.1f MiB, storage shared with another texture %.1f MiB; with a GPU side %.1f MiB, "
                "evicted so far %llu (%.1f MiB)",
                (unsigned long long)frame, (unsigned long long)all.n, mib(all.bytes), (unsigned long long)all.lazy,
                mib(all.never), (unsigned long long)recent_frames, mib(all.recent), mib(rt_bytes), mib(bc_bytes),
                mib(bc_native), mib(astc_bytes), mib(alias_bytes), mib(texture_live),
                (unsigned long long)textures_evicted, mib(texture_bytes_evicted));
        std::vector<std::pair<std::tuple<uint32_t, int, uint32_t>, Group>> sorted(groups.begin(), groups.end());
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second.bytes > b.second.bytes; });
        static constexpr const char* kKinds[] = {"gpu", "bc-cpu", "astc-cpu"};
        for (size_t i = 0; i < sorted.size() && i < 16; ++i) {
            const auto& [key, g] = sorted[i];
            rt::log(rt::LogLevel::Info,
                    "nvn webgpu: texture report   format 0x%x %s <=%u px: %llu textures %.1f MiB, never used %.1f, "
                    "recent %.1f, GPU-written %.1f MiB, lazy %llu",
                    std::get<0>(key), kKinds[std::get<1>(key)], std::get<2>(key), (unsigned long long)g.n,
                    mib(g.bytes), mib(g.never), mib(g.recent), mib(g.written), (unsigned long long)g.lazy);
        }
    }

    // LGP_WEBGPU_TEXTURE_REPORT, second line: what the backend keeps in CPU
    // memory besides the GPU objects (host copies for change detection and
    // uploads, shader text). Allocations inside wgpu/naga and the driver are
    // not counted.
    void cpu_report() const {
        auto mib = [](uint64_t b) { return double(b) / (1 << 20); };
        uint64_t geometry = 0, linear = 0, linear_n = 0, wgsl = 0, keys = 0, arena = 0;
        for (const auto& [key, e] : geometry_cache) geometry += e.guest.capacity();
        for (const auto& [id, t] : textures)
            if (t.linear_shadow.capacity() != 0) {
                linear += t.linear_shadow.capacity();
                ++linear_n;
            }
        for (const auto& [id, p] : programs)
            if (p) wgsl += p->vertex_wgsl.capacity() + p->compute_wgsl.capacity();
        for (const auto& [id, w] : warm)
            if (w.program) wgsl += w.program->vertex_wgsl.capacity() + w.program->compute_wgsl.capacity();
        for (const auto& [key, d] : draw_pipelines) keys += key.capacity();
        for (const UploadArena* a : {&cbuf_arena, &geometry_arena})
            for (const auto& c : a->chunks) arena += c.host.capacity();
        PipelineCacheFile::Held held;
        if (pipeline_cache) held = pipeline_cache->held();
        rt::log(rt::LogLevel::Info,
                "nvn webgpu: cpu copies frame %llu: geometry cache guest bytes %.1f MiB (%zu entries), upload arenas "
                "%.1f MiB, pitch-linear shadows %.1f MiB (%llu), upload scratch %.1f MiB, program WGSL %.1f MiB "
                "(%zu programs, %zu warm), draw pipeline keys %.1f MiB (%zu), pipeline cache programs %.1f MiB, "
                "recipes %.1f MiB, known %.1f MiB, pending %.1f MiB, bind groups cached %zu, samplers %zu, "
                "depth partners %zu",
                (unsigned long long)frame, mib(geometry), geometry_cache.size(), mib(arena), mib(linear),
                (unsigned long long)linear_n, mib(upload_scratch.capacity()), mib(wgsl), programs.size(), warm.size(),
                mib(keys), draw_pipelines.size(), mib(held.programs), mib(held.recipes), mib(held.known),
                mib(held.pending), cbuf_groups.size() + texture_groups.size(), samplers.size(), depth_partner.size());
    }

    // Every use of a texture looks it up here: stamps it as used this frame
    // and creates a lazy texture's GPU side.
    std::unordered_map<nvn::TextureId, Texture>::iterator find_texture(nvn::TextureId id) {
        auto it = textures.find(id);
        if (it != textures.end()) {
            it->second.last_used = frame;
            if (it->second.lazy)
                materialize(it->second);
        }
        return it;
    }

    // The GPU objects of a texture (not its description).
    void release_gpu_texture(Texture& t) {
        if (t.sampled_array != nullptr) wgpuTextureViewRelease(t.sampled_array);
        if (t.depth_sampled != nullptr) wgpuTextureViewRelease(t.depth_sampled);
        if (t.depth_array != nullptr) wgpuTextureViewRelease(t.depth_array);
        if (t.compare_view != nullptr) wgpuTextureViewRelease(t.compare_view);
        if (t.compare_texture != nullptr) {
            gpu_destroy_texture(t.compare_texture);
        }
        if (t.sampled != nullptr && t.sampled != t.attachment) wgpuTextureViewRelease(t.sampled);
        if (t.raw != nullptr && t.raw != t.attachment) wgpuTextureViewRelease(t.raw);
        if (t.attachment != nullptr) wgpuTextureViewRelease(t.attachment);
        if (t.texture != nullptr) {
            texture_live -= std::min(texture_live, texture_bytes(t.plan.td));
            gpu_destroy_texture(t.texture);
        }
        t.sampled_array = t.depth_sampled = t.depth_array = t.compare_view = nullptr;
        t.sampled = t.raw = t.attachment = nullptr;
        t.compare_texture = t.texture = nullptr;
    }

    void release_texture(Texture& t) {
        clear_bind_group_cache();  // cached groups may hold its views
        release_gpu_texture(t);
        t = {};
    }

    // LGP_WEBGPU_TEXTURE_BUDGET_MB (P7.3): above the budget, textures whose
    // contents come from the memory pool and that were not used in the last
    // frames lose their GPU side, least recently used first; the next use
    // creates and uploads them again. Textures the GPU wrote stay.
    void evict_textures() {
        if (texture_budget == 0 || texture_live <= texture_budget)
            return;
        std::vector<std::pair<uint64_t, Texture*>> candidates;
        for (auto& [id, t] : textures)
            if (t.texture != nullptr && !t.gpu_written && t.desc.storage != 0 && t.last_used + 2 < frame)
                candidates.push_back({t.last_used, &t});
        if (candidates.empty())
            return;
        std::sort(candidates.begin(), candidates.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        clear_bind_group_cache();
        const uint64_t target = texture_budget / 10 * 9;
        for (auto& [used, t] : candidates) {
            if (texture_live <= target)
                break;
            const uint64_t before = texture_live;
            release_gpu_texture(*t);
            t->lazy = true;
            t->uploaded = false;
            t->fingerprint = 0;
            t->checked_at = 0;
            std::vector<uint8_t>().swap(t->linear_shadow);  // clear() would keep the capacity
            t->compare_fingerprint = 0;
            t->compare_checked_at = ~uint64_t{0};
            ++textures_evicted;
            texture_bytes_evicted += before - texture_live;
        }
    }

    void ensure_uniforms(uint64_t slots) {
        const uint64_t size = std::max<uint64_t>(slots, 16) * kUniformStride;
        if (clear_uniforms != nullptr && clear_uniforms_size >= size)
            return;
        if (clear_bind_group != nullptr) wgpuBindGroupRelease(clear_bind_group);
        if (clear_uniforms != nullptr) wgpuBufferRelease(clear_uniforms);
        WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
        desc.label = pw::string_view("nvn clear params");
        desc.size = size;
        desc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
        clear_uniforms = gpu_create_buffer(device, &desc);
        clear_uniforms_size = size;
        WGPUBindGroupEntry entry = WGPU_BIND_GROUP_ENTRY_INIT;
        entry.binding = 0;
        entry.buffer = clear_uniforms;
        entry.size = sizeof(ClearUniform);
        WGPUBindGroupDescriptor bg = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        bg.layout = clear_layout;
        bg.entryCount = 1;
        bg.entries = &entry;
        clear_bind_group = wgpuDeviceCreateBindGroup(device, &bg);
    }

    WGPURenderPipeline color_pipeline(WGPUTextureFormat format, uint32_t samples, uint32_t mask) {
        const auto key = std::make_tuple(uint32_t(format), samples, mask);
        auto it = color_pipelines.find(key);
        if (it != color_pipelines.end())
            return it->second;
        WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
        target.format = format;
        target.writeMask = 0;
        if (mask & 1) target.writeMask |= WGPUColorWriteMask_Red;
        if (mask & 2) target.writeMask |= WGPUColorWriteMask_Green;
        if (mask & 4) target.writeMask |= WGPUColorWriteMask_Blue;
        if (mask & 8) target.writeMask |= WGPUColorWriteMask_Alpha;
        WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
        fragment.module = clear_shader;
        fragment.entryPoint = pw::string_view("fs");
        fragment.targetCount = 1;
        fragment.targets = &target;
        WGPURenderPipelineDescriptor desc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
        desc.label = pw::string_view("nvn clear color");
        desc.layout = clear_pipeline_layout;
        desc.vertex.module = clear_shader;
        desc.vertex.entryPoint = pw::string_view("vs");
        desc.multisample.count = samples;
        desc.fragment = &fragment;
        WGPURenderPipeline p = wgpuDeviceCreateRenderPipeline(device, &desc);
        color_pipelines[key] = p;
        return p;
    }

    WGPURenderPipeline depth_pipeline(WGPUTextureFormat format, uint32_t samples, bool depth_write,
                                      uint32_t stencil_mask) {
        const auto key = std::make_tuple(uint32_t(format), samples, depth_write, stencil_mask);
        auto it = depth_pipelines.find(key);
        if (it != depth_pipelines.end())
            return it->second;
        WGPUDepthStencilState ds = WGPU_DEPTH_STENCIL_STATE_INIT;
        ds.format = format;
        if (has_depth(format)) {
            ds.depthWriteEnabled = depth_write ? WGPUOptionalBool_True : WGPUOptionalBool_False;
            ds.depthCompare = WGPUCompareFunction_Always;
        }
        if (has_stencil(format)) {
            // Formats without stencil must keep the default (inactive) stencil state.
            WGPUStencilFaceState face = WGPU_STENCIL_FACE_STATE_INIT;
            face.compare = WGPUCompareFunction_Always;
            face.failOp = WGPUStencilOperation_Keep;
            face.depthFailOp = WGPUStencilOperation_Keep;
            face.passOp = WGPUStencilOperation_Replace;
            ds.stencilFront = face;
            ds.stencilBack = face;
            ds.stencilReadMask = 0xFF;
            ds.stencilWriteMask = stencil_mask;
        }
        WGPURenderPipelineDescriptor desc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
        desc.label = pw::string_view("nvn clear depth/stencil");
        desc.layout = clear_pipeline_layout;
        desc.vertex.module = clear_shader;
        desc.vertex.entryPoint = pw::string_view("vs");
        desc.multisample.count = samples;
        desc.depthStencil = &ds;
        WGPURenderPipeline p = wgpuDeviceCreateRenderPipeline(device, &desc);
        depth_pipelines[key] = p;
        return p;
    }
};

PipelineRequest WebGpuBackend::State::pipeline_request(const DrawState& st, nvn::Topology topo,
                                                      const nvn::CommandList& list, WGPUIndexFormat strip_index,
                                                      const BoundTextures& bound, uint32_t dispatch_streams) const {
    const bool flip_y = list.origin_lower_left;
    PipelineRequest r;
    auto p = programs.find(st.program);
    if (p == programs.end() || !p->second->valid || p->second->compute)
        return r;  // empty key: draw is skipped
    r.program = st.program;
    r.layout = st.layout;
    r.topology = topo;
    r.fixed = st.fixed;
    // The reference only matters while the alpha test is on (fewer pipelines).
    if (r.fixed.alpha_func == nvn::CompareFunc::Always)
        std::memset(r.fixed.alpha_ref, 0, sizeof(r.fixed.alpha_ref));
    if (const DebugOverrides& d = debug_overrides(); true) {
        if (d.nocull) r.fixed.raster.cull = nvn::CullMode::None;
        if (d.nodepth) r.fixed.depth_stencil.depth_test = false;
        if (d.nodepth) r.fixed.depth_stencil.stencil_test = false;
        if (d.noblend) r.fixed.blend_enable = 0;
        if (d.nomask) std::memset(r.fixed.masks, 0xF, sizeof(r.fixed.masks));
    }
    r.flip_y = flip_y;
    r.depth_minus_w = list.depth_minus_w;
    r.strip_index = strip_index;
    if (p->second->target_scale_override) {
        // The framebuffer's first target, as the draw picks it.
        const Texture* first = nullptr;
        for (nvn::TextureId id : st.colors)
            if (auto t = textures.find(id); id != 0 && t != textures.end()) {
                first = &t->second;
                break;
            }
        if (first == nullptr)
            if (auto t = textures.find(st.depth_stencil); st.depth_stencil != 0 && t != textures.end())
                first = &t->second;
        r.target_scaled = first != nullptr && first->scaled;
    }
    r.unfilterable = unfilterable_mask(*p->second, bound);
    if (dispatch_streams != 0 &&
        vertex_buffers_needed(st.layout, unfed_inputs(*p->second, st.layout)) > max_vertex_buffers)
        r.merge_exclude = dispatch_streams;
    // The key holds the fields' bytes (no text formatting: it is built for
    // every draw).
    std::string key;
    key.reserve(64 + 4 * st.colors.size() + 16 * st.layout.attribs.size() + 4 * st.layout.strides.size() +
                sizeof(r.fixed));
    auto put = [&key](const auto& v) { key.append(reinterpret_cast<const char*>(&v), sizeof(v)); };
    put(st.program);
    put(uint8_t(topo));
    put(uint32_t(strip_index));
    put(uint8_t((flip_y ? 1 : 0) | (r.target_scaled ? 2 : 0)));
    put(uint8_t(list.depth_minus_w));
    put(r.unfilterable);
    put(r.merge_exclude);
    put(uint32_t(st.colors.size()));
    // A colour target of another host size than the first is left out of
    // the pass (render scale, record_list), so of the pipeline too.
    const Texture* first_color = nullptr;
    for (nvn::TextureId id : st.colors) {
        auto t = textures.find(id);
        WGPUTextureFormat f = WGPUTextureFormat_Undefined;
        const bool usable = id != 0 && t != textures.end() && (t->second.texture || t->second.lazy);
        if (usable && first_color == nullptr)
            first_color = &t->second;
        if (usable && !has_depth(t->second.format) && !has_stencil(t->second.format) &&
            t->second.width() == first_color->width() && t->second.height() == first_color->height()) {
            f = t->second.format;
            r.samples = t->second.sample_count;
        }
        r.colors.push_back(f);
        put(uint32_t(f));
    }
    if (auto t = textures.find(st.depth_stencil);
        st.depth_stencil != 0 && t != textures.end() && (t->second.texture || t->second.lazy)) {
        r.depth = t->second.format;
        r.samples = t->second.sample_count;
    }
    put(uint32_t(r.depth));
    put(r.samples);
    put(uint32_t(st.layout.attribs.size()));
    for (const nvn::VertexAttrib& a : st.layout.attribs) {
        put(a.location);
        put(uint8_t(a.format));
        put(a.stream);
        put(a.offset);
    }
    put(uint32_t(st.layout.strides.size()));
    for (size_t i = 0; i < st.layout.strides.size(); ++i) {
        // Step mode: 0 = vertex, 1 = instance, 2 = run of instances (stride 0).
        const uint32_t d = stream_divisor(st.layout, i);
        put(st.layout.strides[i]);
        put(uint8_t(d == 0 ? 0 : d == 1 ? 1 : 2));
    }
    put(r.fixed);
    r.key = std::move(key);
    return r;
}

WGPURenderPipeline WebGpuBackend::State::create_draw_pipeline(const PipelineRequest& r) {
    std::unique_ptr<PipelineBuild> b = prepare_draw_pipeline(r, *programs.at(r.program));
    if (b == nullptr)
        return nullptr;
    WGPURenderPipeline p = wgpuDeviceCreateRenderPipeline(device, &b->desc);
    log_draw_pipeline(*b, "on use");
    return p;
}

std::unique_ptr<PipelineBuild> WebGpuBackend::State::prepare_draw_pipeline(const PipelineRequest& r, Program& prog) {
    auto build = std::make_unique<PipelineBuild>();
    build->hash = prog.hash;
    build->wgsl_bytes = prog.wgsl_bytes;
    std::vector<std::vector<WGPUVertexAttribute>>& attrs = build->attrs;
    attrs.resize(r.layout.strides.size());
    // Streams sharing a buffer (VertexMerge): their attributes move into the
    // group's first stream, behind the members before them.
    const VertexMerge merge = vertex_merge(r.layout, unfed_inputs(prog, r.layout), max_vertex_buffers, r.merge_exclude);
    for (const nvn::VertexAttrib& a : r.layout.attribs) {
        if (a.stream >= attrs.size())
            continue;
        WGPUVertexAttribute va = WGPU_VERTEX_ATTRIBUTE_INIT;
        va.format = vertex_format(a.format);
        va.offset = a.offset;
        va.shaderLocation = a.location;
        const int m = merge.member(a.stream);
        if (m >= 0) {
            va.offset += merge.base[size_t(m)];
            attrs[merge.streams[0]].push_back(va);
        } else {
            attrs[a.stream].push_back(va);
        }
    }
    if (merge.active())
        if (static std::atomic<bool> logged{false}; !logged.exchange(true))
            rt::log(rt::LogLevel::Info,
                    "nvn webgpu: %zu vertex streams (per %s) share one buffer (device allows %u vertex buffers)",
                    merge.streams.size(), merge.divisor == 0 ? "vertex" : "instance", max_vertex_buffers);
    // Shader inputs no attribute feeds (disabled or unsupported formats) read zero from one extra 16-byte buffer with stride 0
    // (State::zero_vertex_slot); WebGPU rejects a pipeline without them.
    std::vector<WGPUVertexAttribute>& zero_attrs = build->zero_attrs;
    for (uint32_t loc : prog.vertex_inputs) {
        bool fed = false;
        for (const nvn::VertexAttrib& a : r.layout.attribs)
            fed |= a.location == loc && a.stream < attrs.size();
        if (!fed) {
            WGPUVertexAttribute va = WGPU_VERTEX_ATTRIBUTE_INIT;
            va.format = WGPUVertexFormat_Float32x4;
            va.offset = 0;
            va.shaderLocation = loc;
            zero_attrs.push_back(va);
        }
    }
    // One buffer per stream an attribute reads (vertex_slots), in slot order.
    std::vector<WGPUVertexBufferLayout>& buffers = build->buffers;
    for (size_t i = 0; i < attrs.size(); ++i) {
        if (attrs[i].empty())
            continue;
        WGPUVertexBufferLayout b = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
        const uint32_t divisor = stream_divisor(r.layout, i);
        b.stepMode = divisor == 0 ? WGPUVertexStepMode_Vertex : WGPUVertexStepMode_Instance;
        b.arrayStride = divisor > 1 ? 0 : r.layout.strides[i];
        if (merge.active() && merge.streams[0] == i)
            b.arrayStride = merge.array_stride;
        b.attributeCount = attrs[i].size();
        b.attributes = attrs[i].data();
        buffers.push_back(b);
    }
    if (buffers.size() + (zero_attrs.empty() ? 0 : 1) > max_vertex_buffers) {
        // WebGPU would reject the pipeline, and the streams could not be
        // merged (too few streams with divisor 0 or 1): its draws are skipped.
        if (static std::atomic<int> warned{0}; warned.fetch_add(1) < 8) {
            std::string streams;
            for (size_t i = 0; i < r.layout.strides.size(); ++i)
                streams += " " + std::to_string(r.layout.strides[i]) + "/" +
                           std::to_string(stream_divisor(r.layout, i));
            rt::log(rt::LogLevel::Warn,
                    "nvn webgpu: a draw reads %zu vertex streams%s, the device allows %u vertex buffers and they "
                    "cannot be merged (stride/divisor:%s)",
                    buffers.size(), zero_attrs.empty() ? "" : " (+1 for unfed inputs)", max_vertex_buffers,
                    streams.c_str());
        }
        return nullptr;
    }
    if (!zero_attrs.empty()) {
        WGPUVertexBufferLayout b = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
        b.stepMode = WGPUVertexStepMode_Vertex;
        b.arrayStride = 0;
        b.attributeCount = zero_attrs.size();
        b.attributes = zero_attrs.data();
        buffers.push_back(b);
    }
    std::vector<WGPUColorTargetState>& targets = build->targets;
    std::vector<WGPUBlendState>& blends = build->blends;
    blends.resize(r.colors.size());
    // A vertex-only program writes no color (the stub fragment shader has no
    // outputs, which WebGPU allows for targets with an empty write mask).
    // A vertex-only program has no fragment work to take away: the list leaves
    // it as it is (solid for it would only add color writes).
    const bool solid = debug_overrides().solid || (!prog.vertex_only && debug_overrides().solid_for(prog.hash));
    const bool write_colors = !prog.vertex_only || solid;
    bool any_color = false;
    for (size_t i = 0; i < r.colors.size(); ++i) {
        const WGPUTextureFormat f = r.colors[i];
        WGPUColorTargetState t = WGPU_COLOR_TARGET_STATE_INIT;
        t.format = f;
        t.writeMask = WGPUColorWriteMask_None;
        any_color |= f != WGPUTextureFormat_Undefined;
        if (f != WGPUTextureFormat_Undefined && i < nvn::kMaxColorTargets && write_colors) {
            const uint8_t m = r.fixed.masks[i];
            if (m & 1) t.writeMask |= WGPUColorWriteMask_Red;
            if (m & 2) t.writeMask |= WGPUColorWriteMask_Green;
            if (m & 4) t.writeMask |= WGPUColorWriteMask_Blue;
            if (m & 8) t.writeMask |= WGPUColorWriteMask_Alpha;
            if (r.fixed.blend_enable & (1u << i)) {
                const nvn::BlendEquation& e = r.fixed.equations[i];
                WGPUBlendState& b = blends[i];
                b.color.operation = blend_op(e.color_op);
                b.color.srcFactor = blend_factor(e.src_color);
                b.color.dstFactor = blend_factor(e.dst_color);
                b.alpha.operation = blend_op(e.alpha_op);
                b.alpha.srcFactor = blend_factor(e.src_alpha);
                b.alpha.dstFactor = blend_factor(e.dst_alpha);
                // WebGPU requires factor One for min/max.
                if (e.color_op == nvn::BlendOp::Min || e.color_op == nvn::BlendOp::Max)
                    b.color.srcFactor = b.color.dstFactor = WGPUBlendFactor_One;
                if (e.alpha_op == nvn::BlendOp::Min || e.alpha_op == nvn::BlendOp::Max)
                    b.alpha.srcFactor = b.alpha.dstFactor = WGPUBlendFactor_One;
                t.blend = &b;
            }
        }
        targets.push_back(t);
    }
    WGPUFragmentState& fragment = build->fragment;
    fragment.module = prog.fs;
    WGPUShaderModule solid_fs = nullptr;
    if (solid) {
        std::string out = "struct O {", body;
        for (size_t i = 0; i < r.colors.size(); ++i)
            if (r.colors[i] != WGPUTextureFormat_Undefined) {
                out += " @location(" + std::to_string(i) + ") c" + std::to_string(i) + ": vec4f,";
                body += " o.c" + std::to_string(i) + " = vec4f(1.0, 0.0, 1.0, 1.0);";
            }
        const std::string wgsl = out + " }\n@fragment fn main() -> O { var o: O;" + body + " return o; }\n";
        if (!body.empty())
            solid_fs = pw::create_wgsl_module(device, "nvn debug solid", wgsl.c_str());
        if (solid_fs != nullptr)
            fragment.module = build->fs = solid_fs;  // the build's reference
    }
    fragment.entryPoint = pw::string_view("main");
    fragment.targetCount = targets.size();
    fragment.targets = targets.data();
    // Fixed-function alpha test (the translator's lgp_alpha_pass).
    WGPUConstantEntry* alpha_constants = build->alpha_constants;
    if (r.fixed.alpha_func != nvn::CompareFunc::Always && fragment.module == prog.fs) {
        if (prog.alpha_override) {
            float ref = 0.0f;
            std::memcpy(&ref, r.fixed.alpha_ref, 4);
            alpha_constants[0].key = pw::string_view("lgp_alpha_func");
            alpha_constants[0].value = double(uint32_t(r.fixed.alpha_func));
            alpha_constants[1].key = pw::string_view("lgp_alpha_ref");
            alpha_constants[1].value = double(ref);
            fragment.constantCount = 2;
            fragment.constants = alpha_constants;
        } else if (!prog.vertex_only) {
            if (static std::atomic<bool> warned{false}; !warned.exchange(true))
                rt::log(rt::LogLevel::Warn, "nvn webgpu: alpha test (func %u) on a fragment shader without "
                                            "lgp_alpha_func (no alpha output to target 0): not applied",
                        unsigned(r.fixed.alpha_func));
        }
    }
    // gl_FragCoord in guest pixels of a scaled target (wgsl::Options::render_scale).
    if (r.target_scaled && prog.target_scale_override && fragment.module == prog.fs) {
        WGPUConstantEntry& c = alpha_constants[fragment.constantCount];
        c.key = pw::string_view("lgp_target_scale");
        c.value = double(render_scale);
        fragment.constantCount += 1;
        fragment.constants = alpha_constants;
    }
    WGPUDepthStencilState& ds = build->ds;
    WGPURenderPipelineDescriptor& desc = build->desc;
    desc.label = pw::string_view("nvn draw");
    desc.layout = prog.layout;
    if (r.unfilterable != 0)
        if (const Program::LayoutVariant* v = layout_variant(prog, r.unfilterable))
            desc.layout = v->layout;
    desc.vertex.module = prog.vs;
    // Integer, scaled and packed vertex formats need integer shader inputs
    // (and a conversion): a variant of the vertex module with those locations
    // retyped (wgsl::retype_vertex_inputs). nvn::VertexInput and
    // wgsl::VertexInputType list the same kinds in the same order.
    static_assert(int(nvn::VertexInput::PackedSScaled) == int(wgsl::VertexInputType::PackedSScaled));
    std::map<uint32_t, wgsl::VertexInputType> int_inputs;
    std::string variant_key;
    for (const nvn::VertexAttrib& a : r.layout.attribs) {
        const nvn::VertexInput kind = nvn::vertex_format_input(a.format);
        if (kind == nvn::VertexInput::Float || a.stream >= r.layout.strides.size())
            continue;
        int_inputs[a.location] = static_cast<wgsl::VertexInputType>(kind);
        variant_key += std::to_string(a.location) + ":" + std::to_string(int(kind)) + ",";
    }
    if (!int_inputs.empty()) {
        auto v = prog.vs_variants.find(variant_key);
        if (v == prog.vs_variants.end()) {
            const std::string code = wgsl::retype_vertex_inputs(prog.vertex_wgsl, int_inputs);
            WGPUShaderModule m = nullptr;
            if (code.empty())
                rt::log(rt::LogLevel::Warn, "nvn webgpu: vertex WGSL not retypable for integer inputs %s",
                        variant_key.c_str());
            else
                m = pw::create_wgsl_module(device, "nvn vertex (integer inputs)", code.c_str());
            v = prog.vs_variants.emplace(variant_key, m).first;
        }
        if (v->second == nullptr)
            return nullptr;
        desc.vertex.module = v->second;
    }
    desc.vertex.entryPoint = pw::string_view("main");
    desc.vertex.bufferCount = buffers.size();
    desc.vertex.buffers = buffers.data();
    desc.primitive.topology = topology(r.topology);
    desc.primitive.stripIndexFormat = r.strip_index;
    desc.primitive.cullMode = r.fixed.raster.cull == nvn::CullMode::Front  ? WGPUCullMode_Front
                              : r.fixed.raster.cull == nvn::CullMode::Back ? WGPUCullMode_Back
                                                                           : WGPUCullMode_None;
    // Window rows are memory rows. With the lower-left origin the vertex
    // shader negates clip y, which mirrors every triangle in the framebuffer:
    // the front face swaps.
    const bool cw = (r.fixed.raster.front_face == nvn::FrontFace::Cw) != r.flip_y;
    desc.primitive.frontFace = cw ? WGPUFrontFace_CW : WGPUFrontFace_CCW;
    WGPUConstantEntry* constants = build->constants;
    size_t constant_count = 0;
    if (prog.flip_override) {
        constants[constant_count].key = pw::string_view("lgp_flip_y");
        constants[constant_count++].value = r.flip_y ? 1.0 : 0.0;
    }
    if (prog.depth_override) {
        constants[constant_count].key = pw::string_view("lgp_depth_minus_w");
        constants[constant_count++].value = r.depth_minus_w ? 1.0 : 0.0;
    } else if (r.depth_minus_w && !prog.vertex_wgsl.empty()) {
        if (static std::atomic<bool> warned{false}; !warned.exchange(true))
            rt::log(rt::LogLevel::Warn, "nvn webgpu: vertex shader without lgp_depth_minus_w: clip z stays in "
                                        "[-w, w], the near half of the depth range is clipped");
    }
    if (constant_count > 0) {
        desc.vertex.constantCount = constant_count;
        desc.vertex.constants = constants;
    }
    desc.multisample.count = r.samples;
    desc.multisample.alphaToCoverageEnabled = r.fixed.alpha_to_coverage && r.samples > 1;
    // Like OpenGL, the sample mask only applies to multisampled targets.
    if (r.samples > 1)
        desc.multisample.mask = 0xFFFFFF00u | r.fixed.sample_mask;
    if (r.fixed.depth_clamp) {
        // NVN depth clamp = no near/far clipping; WebGPU then clamps the
        // fragment depth to the viewport's depth range like the GPU does.
        if (depth_clip_control)
            desc.primitive.unclippedDepth = WGPU_TRUE;
        else if (static std::atomic<bool> warned{false}; !warned.exchange(true))
            rt::log(rt::LogLevel::Warn, "nvn webgpu: depth clamp needs the depth-clip-control feature, which the "
                                        "device lacks: primitives are clipped at near/far");
    }
    // Without color targets (depth-only pass) a vertex-only program needs
    // no fragment stage at all.
    if (!prog.vertex_only || any_color || solid_fs != nullptr)
        desc.fragment = &fragment;
    if (r.depth != WGPUTextureFormat_Undefined) {
        const nvn::CmdSetDepthStencil& d = r.fixed.depth_stencil;
        ds.format = r.depth;
        if (has_depth(r.depth)) {
            // As in OpenGL, a disabled depth test also disables depth writes.
            ds.depthWriteEnabled = d.depth_test && d.depth_write ? WGPUOptionalBool_True : WGPUOptionalBool_False;
            ds.depthCompare = d.depth_test ? compare(d.depth_func) : WGPUCompareFunction_Always;
            // Polygon offset. WebGPU allows a depth bias only for triangles;
            // the integer bias counts minimum resolvable differences like
            // OpenGL's units (fractions are rounded).
            const nvn::CmdSetDepthBias bias = r.fixed.bias();
            const bool triangles = r.topology == nvn::Topology::Triangles || r.topology == nvn::Topology::TriangleStrip;
            if (triangles && (r.fixed.raster.depth_bias & nvn::kDepthBiasTriangles)) {
                ds.depthBias = int32_t(std::lround(std::clamp(bias.units, -2.0e9f, 2.0e9f)));
                ds.depthBiasSlopeScale = bias.factor;
                ds.depthBiasClamp = bias.clamp;
            } else if (!triangles && (r.fixed.raster.depth_bias & (nvn::kDepthBiasPoints | nvn::kDepthBiasLines)) &&
                       (bias.units != 0.0f || bias.factor != 0.0f)) {
                static std::atomic<bool> warned{false};
                if (!warned.exchange(true))
                    rt::log(rt::LogLevel::Warn, "nvn webgpu: polygon offset for points/lines is not supported by "
                                                "WebGPU, ignored");
            }
        }
        if (has_stencil(r.depth) && d.stencil_test) {
            const nvn::StencilFace* faces[2] = {&d.front, &d.back};
            WGPUStencilFaceState* out[2] = {&ds.stencilFront, &ds.stencilBack};
            for (int f = 0; f < 2; ++f) {
                out[f]->compare = compare(faces[f]->func);
                out[f]->failOp = stencil_op(faces[f]->fail);
                out[f]->depthFailOp = stencil_op(faces[f]->depth_fail);
                out[f]->passOp = stencil_op(faces[f]->pass);
            }
            ds.stencilReadMask = r.fixed.stencil_read_mask;
            ds.stencilWriteMask = r.fixed.stencil_write_mask;
        }
        desc.depthStencil = &ds;
    }
    // The build's own references.
    build->layout = desc.layout;
    wgpuPipelineLayoutAddRef(build->layout);
    build->vs = desc.vertex.module;
    wgpuShaderModuleAddRef(build->vs);
    if (build->fs == nullptr) {
        build->fs = prog.fs;
        wgpuShaderModuleAddRef(build->fs);
    }
    build->driver_cache.attach(desc.nextInChain, driver_cache);
    return build;
}

bool WebGpuBackend::State::parse_recipe(std::string_view recipe, nvn::ProgramId id, PipelineRequest* out) const {
    size_t at = 0;
    bool ok = true;
    auto get = [&](auto& v) {
        if (at + sizeof(v) > recipe.size()) {
            ok = false;
            return;
        }
        std::memcpy(&v, recipe.data() + at, sizeof(v));
        at += sizeof(v);
    };
    PipelineRequest r;
    r.program = id;
    uint8_t topo = 0, flip = 0, minus_w = 0;
    uint32_t strip = 0, count = 0, depth = 0;
    get(topo);
    get(strip);
    get(flip);
    get(minus_w);
    get(r.unfilterable);
    get(r.merge_exclude);
    get(count);
    if (!ok || topo > uint8_t(nvn::Topology::TriangleStrip) || count > nvn::kMaxColorTargets)
        return false;
    r.topology = nvn::Topology(topo);
    r.strip_index = WGPUIndexFormat(strip);
    r.flip_y = (flip & 1) != 0;
    r.target_scaled = (flip & 2) != 0;
    r.depth_minus_w = minus_w != 0;
    for (uint32_t i = 0; i < count && ok; ++i) {
        uint32_t f = 0;
        get(f);
        r.colors.push_back(WGPUTextureFormat(f));
    }
    get(depth);
    r.depth = WGPUTextureFormat(depth);
    get(r.samples);
    get(count);
    if (!ok || count > 64)
        return false;
    for (uint32_t i = 0; i < count && ok; ++i) {
        nvn::VertexAttrib a;
        uint8_t format = 0;
        get(a.location);
        get(format);
        get(a.stream);
        get(a.offset);
        a.format = nvn::VertexFormat(format);
        r.layout.attribs.push_back(a);
    }
    get(count);
    if (!ok || count > 64)
        return false;
    for (uint32_t i = 0; i < count && ok; ++i) {
        uint32_t stride = 0;
        uint8_t step = 0;
        get(stride);
        get(step);
        r.layout.strides.push_back(stride);
        r.layout.divisors.push_back(step);  // 0, 1 or 2: what the pipeline sees of the divisor
    }
    get(r.fixed);
    if (!ok || at != recipe.size())
        return false;
    r.key.assign(reinterpret_cast<const char*>(&id), sizeof(id));
    r.key.append(recipe);
    *out = std::move(r);
    return true;
}

WGPUComputePipeline WebGpuBackend::State::alias_pipeline(Program& p, const std::map<uint32_t, uint32_t>& moved) {
    auto [it, fresh] = p.alias_pipelines.emplace(moved, nullptr);
    if (!fresh)
        return it->second;
    std::string key;
    for (const auto& [id, entry] : moved) key += " " + std::to_string(id) + "->" + std::to_string(entry);
    const std::string code = wgsl::move_static_slots(p.compute_wgsl, moved);
    if (code.empty()) {
        rt::log(rt::LogLevel::Warn, "nvn webgpu: compute program %016llx: cannot move%s", (unsigned long long)p.hash,
                key.c_str());
        return nullptr;
    }
    const auto t0 = std::chrono::steady_clock::now();
    WGPUShaderModule cs = pw::create_wgsl_module(device, "nvn compute (moved slots)", code.c_str());
    WGPUComputePipelineDescriptor cd = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
    cd.label = pw::string_view("nvn compute (moved slots)");
    cd.layout = p.layout;
    cd.compute.module = cs;
    cd.compute.entryPoint = pw::string_view("main");
    DriverCacheLink link;
    link.attach(cd.nextInChain, driver_cache);
    it->second = wgpuDeviceCreateComputePipeline(device, &cd);
    wgpuShaderModuleRelease(cs);
    rt::log(rt::LogLevel::Info, "nvn webgpu: compute pipeline %016llx with moved targets%s in %lld ms",
            (unsigned long long)p.hash, key.c_str(),
            (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
                .count());
    return it->second;
}

void WebGpuBackend::State::release_program(Program& p) {
    if (p.layout != nullptr) wgpuPipelineLayoutRelease(p.layout);
    for (auto& [mask, v] : p.layout_variants) {
        if (v.layout != nullptr) wgpuPipelineLayoutRelease(v.layout);
        for (WGPUBindGroupLayout g : v.textures)
            if (g != nullptr) wgpuBindGroupLayoutRelease(g);
    }
    for (WGPUBindGroupLayout g : p.groups)
        if (g != nullptr) wgpuBindGroupLayoutRelease(g);
    if (p.compute_pipeline != nullptr) wgpuComputePipelineRelease(p.compute_pipeline);
    for (auto& [key, pipeline] : p.alias_pipelines)
        if (pipeline != nullptr) wgpuComputePipelineRelease(pipeline);
    if (p.vs != nullptr) wgpuShaderModuleRelease(p.vs);
    for (auto& [key, m] : p.vs_variants)
        if (m != nullptr) wgpuShaderModuleRelease(m);
    if (p.fs != nullptr) wgpuShaderModuleRelease(p.fs);
    if (p.cs != nullptr) wgpuShaderModuleRelease(p.cs);
    p = Program{};
}

void WebGpuBackend::State::warm_step(uint64_t budget_us, Stats& delta) {
    if (!pipeline_cache || !prebuilds || warm_next >= warm_todo.size())
        return;
    struct Release {
        State& s;
        ~Release() {
            // All cached programs exist now (or were skipped): their texts
            // are not needed again in this run.
            if (s.warm_next < s.warm_todo.size())
                return;
            s.pipeline_cache->release_programs();
            std::vector<uint64_t>().swap(s.warm_todo);
            s.warm_next = 0;
        }
    } release{*this};
    const auto t0 = std::chrono::steady_clock::now();
    // At least one program per call, then as long as the budget lasts: the
    // budget is wall time, so a thread preempted before its first check (a
    // loaded CI runner or phone) would otherwise warm nothing in this present.
    for (bool first = true; warm_next < warm_todo.size() && (first || micros_since(t0) < budget_us);) {
        {
            // App in the background (hold_prebuilds): the rest after it is back.
            std::lock_guard lock(prebuilds->mutex);
            if (prebuilds->held)
                break;
        }
        const uint64_t hash = warm_todo[warm_next++];
        bool exists = warm_by_hash.count(hash) != 0;
        for (const auto& [id, prog] : programs) exists |= prog->hash == hash;
        nvn::ProgramDesc desc;
        std::string text;
        if (exists || !pipeline_cache->program(hash, &text) || !decode_program_desc(text, &desc) ||
            program_hash(desc) != hash)
            continue;
        first = false;
        const uint64_t serial = next_program_serial++;
        Warm& w = warm[serial];
        w.program = std::make_unique<Program>();
        w.program->serial = serial;
        warm_by_hash[hash] = serial;
        wgpuDevicePushErrorScope(device, WGPUErrorFilter_Validation);
        if (desc.is_compute())
            build_compute_program(desc, hash, *w.program, false);  // its pipeline in the background
        else
            build_graphics_program(desc, hash, *w.program);
        struct Pop {
            State* s;
            std::shared_ptr<Prebuilds> pb;
            uint64_t serial;
            bool in_pop = true, fired = false, ok = false;
            // The browser calls back later, on the GPU thread. The backend's
            // destructor sets `stop` before its last GPU-thread call, so a
            // callback that still sees it clear runs before the State goes.
            void settle() {
                bool stopped;
                {
                    std::lock_guard lock(pb->mutex);
                    stopped = pb->stop;
                }
                if (!stopped)
                    check();
                {
                    std::lock_guard lock(pb->mutex);
                    --pb->outstanding;  // the program's own check (wait_prebuilds)
                }
                pb->cv.notify_all();
            }
            void check() {
                auto it = s->warm.find(serial);
                if (it == s->warm.end())
                    return;
                if (!ok) {
                    rt::log(rt::LogLevel::Warn, "nvn webgpu: cached program not created");
                    s->warm_by_hash.erase(it->second.program->hash);
                    s->release_program(*it->second.program);
                    s->warm.erase(it);
                    return;
                }
                it->second.validated = true;
                it->second.program->valid = true;
                if (it->second.program->compute)
                    s->schedule_compute_prebuild(*it->second.program);
                else
                    s->schedule_prebuilds(*it->second.program, 0);
            }
        };
        {
            std::lock_guard lock(prebuilds->mutex);
            ++prebuilds->outstanding;
            ++prebuilds->scheduled;
        }
        auto* pop = new Pop{this, prebuilds, serial};
        WGPUPopErrorScopeCallbackInfo cb = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
        cb.mode = WGPUCallbackMode_AllowSpontaneous;
        cb.userdata1 = pop;
        cb.callback = [](WGPUPopErrorScopeStatus status, WGPUErrorType type, WGPUStringView, void* u1, void*) {
            auto* p = static_cast<Pop*>(u1);
            p->ok = status == WGPUPopErrorScopeStatus_Success && type == WGPUErrorType_NoError;
            if (p->in_pop) {  // wgpu-native, inside PopErrorScope: no WebGPU calls here
                p->fired = true;
                return;
            }
            p->settle();
            delete p;
        };
        wgpuDevicePopErrorScope(device, cb);
        pop->in_pop = false;
        if (pop->fired) {
            pop->settle();
            delete pop;
        }
        ++delta.programs_warmed;
    }
}

std::unique_ptr<WebGpuBackend::Program> WebGpuBackend::State::adopt_warm(uint64_t hash, nvn::ProgramId id,
                                                                         Stats& delta) {
    auto wh = warm_by_hash.find(hash);
    if (wh == warm_by_hash.end())
        return nullptr;
    auto it = warm.find(wh->second);
    warm_by_hash.erase(wh);
    if (it == warm.end())
        return nullptr;
    if (!it->second.validated) {
        // Not checked yet (browser): the program is created the usual way and
        // the warm one is dropped; its pending check finds nothing to do.
        for (auto& [recipe, pipeline] : it->second.pipelines) wgpuRenderPipelineRelease(pipeline);
        release_program(*it->second.program);
        warm.erase(it);
        return nullptr;
    }
    Warm& w = it->second;
    std::unique_ptr<Program> prog = std::move(w.program);
    program_by_serial[prog->serial] = id;
    for (auto& [recipe, pipeline] : w.pipelines) {
        std::string key(reinterpret_cast<const char*>(&id), sizeof(id));
        key.append(recipe);
        draw_pipelines[key] = {id, pipeline};
        ++delta.pipelines_prebuilt;
    }
    warm.erase(it);
    ++delta.programs_adopted;
    return prog;
}

void WebGpuBackend::State::schedule_prebuilds(Program& prog, nvn::ProgramId id) {
    if (!pipeline_cache || !prebuilds || !prog.valid || prog.compute)
        return;
    auto it = pipeline_cache->recipes().find(prog.hash);
    if (it == pipeline_cache->recipes().end())
        return;
    const Warm* w = id == 0 ? &warm.at(prog.serial) : nullptr;
    std::vector<Prebuilds::Job> jobs;
    // Preparing may create shader module variants and layouts: if one of
    // them is invalid, none of these jobs is used.
    wgpuDevicePushErrorScope(device, WGPUErrorFilter_Validation);
    const std::vector<uint32_t>& positions = pipeline_cache->recipe_positions().at(prog.hash);
    for (size_t i = 0; i < it->second.size(); ++i) {
        const std::string& recipe = it->second[i];
        PipelineRequest r;
        if (!parse_recipe(recipe, id, &r))
            continue;
        // A recipe from a seed file (another device) may name a target this
        // device cannot render to; the desktop worker has no error scope.
        if (!rg11b10_renderable &&
            std::find(r.colors.begin(), r.colors.end(), WGPUTextureFormat_RG11B10Ufloat) != r.colors.end())
            continue;
        if (w != nullptr ? w->pipelines.count(recipe) != 0 : draw_pipelines.count(r.key) != 0)
            continue;
        {
            std::lock_guard lock(prebuilds->mutex);
            if (prebuilds->pending.count(Prebuilds::tag(prog.serial, recipe)))
                continue;
        }
        std::unique_ptr<PipelineBuild> build = prepare_draw_pipeline(r, prog);
        if (build != nullptr)
            jobs.push_back({recipe, prog.serial, std::move(build), nullptr, false, uint64_t(positions[i]) + 1});
    }
    struct Pop {
        std::shared_ptr<Prebuilds> pb;
        std::vector<Prebuilds::Job> jobs;
        bool in_pop = true, fired = false, ok = false;
        // Hands the jobs on, or gives them up (they count as outstanding from now on).
        void settle() {
            bool stopped;
            {
                std::lock_guard lock(pb->mutex);
                stopped = pb->stop;
            }
            if (ok && !stopped) {
                submit_prebuilds(pb, std::move(jobs));
                return;
            }
            // Given up: finished without a pipeline, so their pending tags
            // go at the next harvest and a list waiting for them goes on.
            for (Prebuilds::Job& job : jobs) pb->finish(std::move(job), nullptr);
        }
    };
    {
        std::lock_guard lock(prebuilds->mutex);
        prebuilds->outstanding += jobs.size();
        prebuilds->scheduled += jobs.size();
        // Pending from now on, not only once the scope is popped (the browser
        // pops later): a list in between waits for them instead of creating
        // the same pipelines itself.
        for (const Prebuilds::Job& job : jobs) prebuilds->pending.insert(Prebuilds::tag(job.serial, job.recipe));
    }
    auto* pop = new Pop{prebuilds, std::move(jobs)};
    WGPUPopErrorScopeCallbackInfo cb = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
    cb.mode = WGPUCallbackMode_AllowSpontaneous;
    cb.userdata1 = pop;
    cb.callback = [](WGPUPopErrorScopeStatus status, WGPUErrorType type, WGPUStringView message, void* u1, void*) {
        auto* p = static_cast<Pop*>(u1);
        p->ok = status == WGPUPopErrorScopeStatus_Success && type == WGPUErrorType_NoError;
        if (!p->ok)
            rt::log(rt::LogLevel::Warn, "nvn webgpu: cached pipelines not prepared: %s", pw::to_string(message).c_str());
        if (p->in_pop) {  // wgpu-native, inside PopErrorScope: no WebGPU calls here
            p->fired = true;
            return;
        }
        p->settle();
        delete p;
    };
    wgpuDevicePopErrorScope(device, cb);
    pop->in_pop = false;
    if (pop->fired && prebuild_pop_test != 0) {  // tests: as in the browser, later
        if (prebuild_pop_test == 2)
            pop->ok = false;
        late_prebuild_pops.push_back([pop] {
            pop->settle();
            delete pop;
        });
        return;
    }
    if (pop->fired) {
        pop->settle();
        delete pop;
    }
}

void WebGpuBackend::State::submit_prebuilds(const std::shared_ptr<Prebuilds>& pb, std::vector<Prebuilds::Job> jobs) {
    if (jobs.empty())
        return;
    if (!pb->async) {
        {
            std::lock_guard lock(pb->mutex);
            for (Prebuilds::Job& job : jobs) {
                pb->pending.insert(Prebuilds::tag(job.serial, job.recipe));
                auto at = std::upper_bound(pb->queue.begin(), pb->queue.end(), job.order,
                                           [](uint64_t o, const Prebuilds::Job& q) { return o < q.order; });
                pb->queue.insert(at, std::move(job));
            }
        }
        pb->cv.notify_all();
        return;
    }
    struct Async {
        std::shared_ptr<Prebuilds> pb;
        Prebuilds::Job job;
    };
    for (Prebuilds::Job& job : jobs) {
        {
            std::lock_guard lock(pb->mutex);
            pb->pending.insert(Prebuilds::tag(job.serial, job.recipe));
        }
        auto* a = new Async{pb, std::move(job)};
        if (a->job.compute != nullptr) {
            const WGPUComputePipelineDescriptor* desc = &a->job.compute->desc;
            WGPUCreateComputePipelineAsyncCallbackInfo cb = WGPU_CREATE_COMPUTE_PIPELINE_ASYNC_CALLBACK_INFO_INIT;
            cb.mode = WGPUCallbackMode_AllowSpontaneous;
            cb.userdata1 = a;
            cb.callback = [](WGPUCreatePipelineAsyncStatus status, WGPUComputePipeline pipeline,
                             WGPUStringView message, void* u1, void*) {
                std::unique_ptr<Async> a(static_cast<Async*>(u1));
                if (a->job.on_use && debug_overrides().dropasync) {
                    if (pipeline != nullptr) wgpuComputePipelineRelease(pipeline);
                    return;
                }
                if (status != WGPUCreatePipelineAsyncStatus_Success) {
                    rt::log(rt::LogLevel::Warn, "nvn webgpu: compute pipeline rejected: %s",
                            pw::to_string(message).c_str());
                    if (pipeline != nullptr) wgpuComputePipelineRelease(pipeline);
                    pipeline = nullptr;
                } else {
                    log_compute_pipeline(*a->job.compute, "async");
                }
                a->pb->finish(std::move(a->job), nullptr, pipeline);  // job.compute marks it, goes with `a`
            };
#if defined(__EMSCRIPTEN__)
            wgpuDeviceCreateComputePipelineAsync(pb->device, desc, cb);
#else
            // As below: wgpu-native's createComputePipelineAsync, synchronously.
            wgpuDevicePushErrorScope(pb->device, WGPUErrorFilter_Validation);
            WGPUComputePipeline pipeline = wgpuDeviceCreateComputePipeline(pb->device, desc);
            bool valid = false;
            WGPUPopErrorScopeCallbackInfo pop = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
            pop.mode = WGPUCallbackMode_AllowSpontaneous;
            pop.userdata1 = &valid;
            pop.callback = [](WGPUPopErrorScopeStatus status, WGPUErrorType type, WGPUStringView, void* u1, void*) {
                *static_cast<bool*>(u1) = status == WGPUPopErrorScopeStatus_Success && type == WGPUErrorType_NoError;
            };
            wgpuDevicePopErrorScope(pb->device, pop);
            cb.callback(valid ? WGPUCreatePipelineAsyncStatus_Success : WGPUCreatePipelineAsyncStatus_ValidationError,
                        pipeline, pw::string_view(""), a, nullptr);
#endif
            continue;
        }
        const WGPURenderPipelineDescriptor* desc = &a->job.build->desc;
        WGPUCreateRenderPipelineAsyncCallbackInfo cb = WGPU_CREATE_RENDER_PIPELINE_ASYNC_CALLBACK_INFO_INIT;
        cb.mode = WGPUCallbackMode_AllowSpontaneous;
        cb.userdata1 = a;
        cb.callback = [](WGPUCreatePipelineAsyncStatus status, WGPURenderPipeline pipeline, WGPUStringView message,
                         void* u1, void*) {
            std::unique_ptr<Async> a(static_cast<Async*>(u1));
            if (a->job.on_use && debug_overrides().dropasync) {
                if (pipeline != nullptr) wgpuRenderPipelineRelease(pipeline);
                return;
            }
            if (status != WGPUCreatePipelineAsyncStatus_Success) {
                rt::log(rt::LogLevel::Warn, "nvn webgpu: draw pipeline rejected: %s", pw::to_string(message).c_str());
                if (pipeline != nullptr) wgpuRenderPipelineRelease(pipeline);
                pipeline = nullptr;
            } else {
                log_draw_pipeline(*a->job.build, "async");
            }
            a->job.build.reset();
            a->pb->finish(std::move(a->job), pipeline);
        };
#if defined(__EMSCRIPTEN__)
        wgpuDeviceCreateRenderPipelineAsync(pb->device, desc, cb);
#else
        // wgpu-native does not implement createRenderPipelineAsync (it
        // panics): the same path, synchronously, so tests cover it.
        wgpuDevicePushErrorScope(pb->device, WGPUErrorFilter_Validation);
        WGPURenderPipeline pipeline = wgpuDeviceCreateRenderPipeline(pb->device, desc);
        bool valid = false;
        WGPUPopErrorScopeCallbackInfo pop = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
        pop.mode = WGPUCallbackMode_AllowSpontaneous;
        pop.userdata1 = &valid;
        pop.callback = [](WGPUPopErrorScopeStatus status, WGPUErrorType type, WGPUStringView, void* u1, void*) {
            *static_cast<bool*>(u1) = status == WGPUPopErrorScopeStatus_Success && type == WGPUErrorType_NoError;
        };
        wgpuDevicePopErrorScope(pb->device, pop);
        cb.callback(valid ? WGPUCreatePipelineAsyncStatus_Success : WGPUCreatePipelineAsyncStatus_ValidationError,
                    pipeline, pw::string_view(""), a, nullptr);
#endif
    }
}

void WebGpuBackend::State::harvest_prebuilds(Stats& delta) {
    if (!prebuilds)
        return;
    std::vector<Prebuilds::Done> done;
    {
        std::lock_guard lock(prebuilds->mutex);
        done.swap(prebuilds->done);
        for (const Prebuilds::Done& d : done) prebuilds->pending.erase(Prebuilds::tag(d.serial, d.recipe));
        delta.prebuild_us += prebuilds->busy_us;
        prebuilds->busy_us = 0;
    }
    for (Prebuilds::Done& d : done) {
        if (d.is_compute) {
            // To the program (adopted or still warm) unless it has one by now.
            Program* prog = nullptr;
            if (auto p = program_by_serial.find(d.serial); p != program_by_serial.end())
                prog = programs.at(p->second).get();
            else if (auto w = warm.find(d.serial); w != warm.end())
                prog = w->second.program.get();
            if (d.compute == nullptr && d.on_use) {
                if (prog != nullptr && prog->compute_pipeline == nullptr && prog->compute_deferred) {
                    prog->compute_deferred = false;  // rejected: its dispatches are skipped
                    ++delta.invalid_pipelines;
                }
            } else if (d.compute == nullptr) {
                ++delta.prebuilds_failed;  // a dispatch creates it in an error scope and finds out
            } else if (prog != nullptr && prog->compute_pipeline == nullptr) {
                prog->compute_pipeline = d.compute;
                prog->compute_deferred = false;
                if (!d.on_use)
                    ++delta.compute_pipelines_prebuilt;
            } else {
                wgpuComputePipelineRelease(d.compute);
            }
            continue;
        }
        if (d.pipeline == nullptr && d.on_use) {  // rejected: its draws are skipped
            if (auto p = program_by_serial.find(d.serial); p != program_by_serial.end()) {
                std::string key(reinterpret_cast<const char*>(&p->second), sizeof(p->second));
                key.append(d.recipe);
                nowait_pending.erase(key);
                compiling_keys.erase(key);
                if (!draw_pipelines.count(key)) {
                    draw_pipelines[key] = {p->second, nullptr};
                    ++delta.invalid_pipelines;
                }
            }
            continue;
        }
        if (d.pipeline == nullptr) {
            ++delta.prebuilds_failed;
            continue;
        }
        if (auto p = program_by_serial.find(d.serial); p != program_by_serial.end()) {
            std::string key(reinterpret_cast<const char*>(&p->second), sizeof(p->second));
            key.append(d.recipe);
            compiling_keys.erase(key);
            auto nowait = nowait_pending.find(key);
            if (!draw_pipelines.count(key)) {
                draw_pipelines[key] = {p->second, d.pipeline};
                if (!d.on_use)
                    ++delta.pipelines_prebuilt;
                if (nowait != nowait_pending.end()) {  // created without a wait: to the cache now
                    if (pipeline_cache) pipeline_cache->add(nowait->second.first, nowait->second.second);
                    nowait_pending.erase(nowait);
                }
                continue;
            }
            if (nowait != nowait_pending.end()) nowait_pending.erase(nowait);  // marked invalid meanwhile
        } else if (auto w = warm.find(d.serial); w != warm.end() && !w->second.pipelines.count(d.recipe)) {
            w->second.pipelines[d.recipe] = d.pipeline;  // counted when the program is adopted
            continue;
        }
        wgpuRenderPipelineRelease(d.pipeline);  // program gone, or created meanwhile
    }
}

bool WebGpuBackend::State::claim_prebuild(const PipelineRequest& r, Stats& delta) {
    if (!prebuilds)
        return false;
    const uint64_t serial = programs.at(r.program)->serial;
    const std::string_view recipe = std::string_view(r.key).substr(sizeof(nvn::ProgramId));
    const std::string tag = Prebuilds::tag(serial, recipe);
    {
        std::unique_lock lock(prebuilds->mutex);
        if (!prebuilds->pending.count(tag))
            return false;
        // Browser: the async result is not there yet. With async pipelines
        // the list waits for it; otherwise the pipeline created now wins and
        // the late one is released.
        if (prebuilds->async) {
            if (async_pipelines && pipeline_wait) {
                if (!prebuilds->is_done(tag)) {
                    if (!pipeline_nowait) ++delta.prebuilds_waited;
                    pipeline_wait->tags.insert(tag);
                    return true;
                }
                // Finished since the last harvest: no callback will come for it.
                lock.unlock();
                harvest_prebuilds(delta);
                return draw_pipelines.count(r.key) != 0;
            }
            ++delta.prebuilds_late;
            return false;
        }
        for (auto q = prebuilds->queue.begin(); q != prebuilds->queue.end(); ++q) {
            if (q->serial == serial && q->recipe == recipe) {  // not started: create it with the others of this list
                prebuilds->pending.erase(tag);
                prebuilds->queue.erase(q);
                --prebuilds->outstanding;
                ++delta.prebuilds_late;
                return false;
            }
        }
        ++delta.prebuilds_waited;
        prebuilds->cv.wait(lock, [&] { return prebuilds->running.count(tag) == 0; });
    }
    harvest_prebuilds(delta);
    return draw_pipelines.count(r.key) != 0;
}

void WebGpuBackend::State::schedule_compute_prebuild(Program& prog, bool on_use) {
    if (!prebuilds || !prog.compute_deferred || prog.compute_pipeline != nullptr)
        return;
    std::vector<Prebuilds::Job> jobs(1);
    jobs[0].serial = prog.serial;
    jobs[0].on_use = on_use;
    if (!on_use && pipeline_cache)
        if (auto p = pipeline_cache->program_positions().find(prog.hash); p != pipeline_cache->program_positions().end())
            jobs[0].order = uint64_t(p->second) + 1;  // after on-use jobs (0)
    jobs[0].compute.reset(compute_build_for(prog.layout, prog.cs, prog.hash, prog.wgsl_bytes, driver_cache));
    {
        std::lock_guard lock(prebuilds->mutex);
        if (prebuilds->pending.count(Prebuilds::tag(prog.serial, {})))
            return;
        ++prebuilds->outstanding;
        ++prebuilds->scheduled;
    }
    submit_prebuilds(prebuilds, std::move(jobs));
}

bool WebGpuBackend::State::claim_compute_prebuild(Program& prog, Stats& delta) {
    if (!prebuilds)
        return false;
    const std::string tag = Prebuilds::tag(prog.serial, {});
    {
        std::unique_lock lock(prebuilds->mutex);
        if (!prebuilds->pending.count(tag))
            return false;
        if (prebuilds->async) {  // as claim_prebuild: waited for, or created now and the late one released
            if (async_pipelines && pipeline_wait) {
                if (!prebuilds->is_done(tag)) {
                    ++delta.compute_pipeline_waits;
                    pipeline_wait->tags.insert(tag);
                    return true;
                }
                lock.unlock();  // as claim_prebuild
                harvest_prebuilds(delta);
                return prog.compute_pipeline != nullptr;
            }
            ++delta.prebuilds_late;
            return false;
        }
        for (auto q = prebuilds->queue.begin(); q != prebuilds->queue.end(); ++q) {
            if (q->serial == prog.serial && q->compute != nullptr) {
                prebuilds->pending.erase(tag);
                prebuilds->queue.erase(q);
                --prebuilds->outstanding;
                ++delta.prebuilds_late;
                return false;
            }
        }
        ++delta.prebuilds_waited;
        prebuilds->cv.wait(lock, [&] { return prebuilds->running.count(tag) == 0; });
    }
    harvest_prebuilds(delta);
    return prog.compute_pipeline != nullptr;
}

bool WebGpuBackend::State::await_prebuild(const std::string& tag) {
    std::lock_guard lock(prebuilds->mutex);
    if (!prebuilds->pending.count(tag) || prebuilds->is_done(tag))
        return false;
    pipeline_wait->tags.insert(tag);
    return true;
}

void WebGpuBackend::State::clear_pipeline_wait_timeout(PipelineWait& w) {
#if defined(__EMSCRIPTEN__)
    if (w.timeout_id != 0) {
        emscripten_clear_timeout(w.timeout_id);
        delete static_cast<PipelineWaitTimeout*>(w.timeout_arg);
    }
#endif
    w.timeout_id = 0;
    w.timeout_arg = nullptr;
}

void WebGpuBackend::State::check_pipeline_wait() {
    if (!pipeline_wait || !pipeline_wait->armed || !pipeline_wait->tags.empty() || pipeline_wait->scopes != 0)
        return;
    std::function<void()> then = std::move(pipeline_wait->then);
    clear_pipeline_wait_timeout(*pipeline_wait);
    pipeline_wait.reset();
    then();
}

void WebGpuBackend::State::cancel_prebuilds(uint64_t serial) {
    if (!prebuilds)
        return;
    std::lock_guard lock(prebuilds->mutex);
    for (auto q = prebuilds->queue.begin(); q != prebuilds->queue.end();) {
        if (q->serial != serial) {
            ++q;
            continue;
        }
        prebuilds->pending.erase(Prebuilds::tag(q->serial, q->recipe));
        --prebuilds->outstanding;
        q = prebuilds->queue.erase(q);
    }
}

// Workers creating prebuilt pipelines (desktop). With a cold driver cache a
// pipeline can take seconds; several run side by side.
// LGP_WEBGPU_PREBUILD_THREADS=N overrides (1..16).
static unsigned prebuild_worker_count() {
    if (const char* n = std::getenv("LGP_WEBGPU_PREBUILD_THREADS"); n != nullptr) {
        const long v = std::strtol(n, nullptr, 10);
        if (v >= 1 && v <= 16)
            return unsigned(v);
    }
    return std::clamp(std::thread::hardware_concurrency() / 2, 1u, 4u);
}

static void prebuild_worker_main(std::shared_ptr<WebGpuBackend::State::Prebuilds> pb) {
    using Prebuilds = WebGpuBackend::State::Prebuilds;
    platform::set_current_thread_name("lgp-pipe");
    for (;;) {
        Prebuilds::Job job;
        {
            std::unique_lock lock(pb->mutex);
            pb->cv.wait(lock, [&] { return pb->stop || (!pb->held && !pb->queue.empty()); });
            if (pb->stop)
                return;
            job = std::move(pb->queue.front());
            pb->queue.pop_front();
            pb->running.insert(Prebuilds::tag(job.serial, job.recipe));
        }
        const auto t0 = std::chrono::steady_clock::now();
        WGPURenderPipeline pipeline = nullptr;
        WGPUComputePipeline compute = nullptr;
        const bool is_compute = job.compute != nullptr;
        if (is_compute) {
            compute = wgpuDeviceCreateComputePipeline(pb->device, &job.compute->desc);
            log_compute_pipeline(*job.compute, "worker", t0);
        } else {
            pipeline = wgpuDeviceCreateRenderPipeline(pb->device, &job.build->desc);
            log_draw_pipeline(*job.build, "worker", t0);
        }
        const uint64_t us = micros_since(t0);
        job.build.reset();
        job.compute.reset();
        {
            // One step for the claims: when the job stops running, it is done.
            std::lock_guard lock(pb->mutex);
            pb->running.erase(pb->running.find(Prebuilds::tag(job.serial, job.recipe)));
            pb->busy_us += us;
            --pb->outstanding;
            if (pb->stop) {
                if (pipeline != nullptr) wgpuRenderPipelineRelease(pipeline);
                if (compute != nullptr) wgpuComputePipelineRelease(compute);
            } else {
                pb->done.push_back({std::move(job.recipe), job.serial, pipeline, compute, is_compute});
            }
        }
        pb->cv.notify_all();
    }
}

WebGpuBackend::WebGpuBackend(pw::WebGpuRenderer& renderer) : renderer_(renderer), state_(std::make_unique<State>()) {
    renderer_.run_sync([this] {
        State& s = *state_;
        s.device = renderer_.device();
        s.queue = renderer_.queue();
        if (const char* v = std::getenv("LGP_WEBGPU_ASYNC_WRITEBACK"); v != nullptr && std::string(v) == "1")
            s.async_writeback = true;
        if (const char* v = std::getenv("LGP_WEBGPU_BUFFER_POOL"); v != nullptr && *v != '\0' && std::string(v) != "0") {
            s.buffer_pool = std::make_shared<BufferPool>();
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
            // Phones, iPad, Apple TV: at most 16 MiB free (not measured; the
            // GPU memory counts against the app there).
            s.buffer_pool->cap_bytes = uint64_t(16) << 20;
#endif
            if (const char* mb = std::getenv("LGP_WEBGPU_BUFFER_POOL_MB"); mb != nullptr && std::atoi(mb) > 0)
                s.buffer_pool->cap_bytes = uint64_t(std::atoi(mb)) << 20;
            rt::log(rt::LogLevel::Info, "nvn webgpu: buffer pool on, up to %llu MiB free (LGP_WEBGPU_BUFFER_POOL)",
                    (unsigned long long)(s.buffer_pool->cap_bytes >> 20));
        }
        if (const char* v = std::getenv("LGP_WEBGPU_COMPUTE_FIRST"); v != nullptr && *v != '\0')
            s.compute_first = std::string(v) != "0";
#if !defined(__EMSCRIPTEN__)
        {
#if defined(__APPLE__) && TARGET_OS_TV
            // Apple TV 4K, scene build, 2026-10-29 (A8): peak 2061/2063 MiB
            // without, 1811/1766 with 64 MiB (32: 1751, 128: 1814), the
            // building frame 0.54 instead of 0.47 s.
            double flush_mb = 64;
#else
            double flush_mb = 0;
#endif
            if (const char* v = std::getenv("LGP_WEBGPU_UPLOAD_FLUSH_MB"); v != nullptr && *v != '\0')
                flush_mb = std::max(std::atof(v), 0.0);
            if (flush_mb > 0) {
                g_upload_flush.device = s.device;
                g_upload_flush.budget = std::max<uint64_t>(uint64_t(flush_mb * 1048576.0), 1);  // tests: fractions
                rt::log(rt::LogLevel::Info, "nvn webgpu: queue writes submitted after every %.3f MiB within a list "
                                            "(LGP_WEBGPU_UPLOAD_FLUSH_MB)", flush_mb);
            }
        }
#endif
        if (const char* v = std::getenv("LGP_WEBGPU_POLL_SUBMIT_INDEX"); v != nullptr && *v != '\0')
            s.poll_submit_index = std::string(v) != "0";
        if (const char* v = std::getenv("LGP_WEBGPU_PATCH_DISPATCH_OUTPUT"); v != nullptr && *v != '\0')
            s.patch_dispatch_output = std::string(v) != "0";
        // BCn decoding on the CPU (hosts without texture-compression-bc):
        // LGP_WEBGPU_BC_DECODER=fast|reference, both write the same bytes
        // (gpu/texture/bc.hpp); fast by default (measured on the Apple TV).
        if (const char* v = std::getenv("LGP_WEBGPU_BC_DECODER"); v != nullptr && *v != '\0') {
            const std::string d = v;
            if (d == "fast" || d == "reference")
                lgp::gpu::texture::set_bc_decoder(d == "fast" ? lgp::gpu::texture::BcDecoder::Fast
                                                              : lgp::gpu::texture::BcDecoder::Reference);
            else
                rt::log(rt::LogLevel::Warn, "nvn webgpu: LGP_WEBGPU_BC_DECODER=%s unknown (fast, reference)", v);
            rt::log(rt::LogLevel::Info, "nvn webgpu: BCn CPU decoder %s (LGP_WEBGPU_BC_DECODER)",
                    lgp::gpu::texture::bc_decoder() == lgp::gpu::texture::BcDecoder::Fast ? "fast" : "reference");
        }
#if defined(__ANDROID__)
        // OnePlus 7 Pro, frames 2401-3600, host clock, ABAB: two pairs of 4 runs: 14.6 -> 15.7 fps
        // (+7.5 %, within the spread; B never slower in a single run), P7.3.
        s.keep_pass_on_dispatch = true;
#endif
        if (const char* v = std::getenv("LGP_WEBGPU_KEEP_PASS_ON_DISPATCH"); v != nullptr && *v != '\0')
            s.keep_pass_on_dispatch = std::string(v) != "0";
        if (s.keep_pass_on_dispatch)
            rt::log(rt::LogLevel::Info, "nvn webgpu: draw passes stay open across dispatches submitted ahead "
                                        "(LGP_WEBGPU_KEEP_PASS_ON_DISPATCH)");
        if (const char* v = std::getenv("LGP_WEBGPU_HALFRES_PROGRAMS"); v != nullptr && *v != '\0') {
            s.halfres_programs = parse_hash_prefixes(v);
            s.halfres_on = !s.halfres_programs.empty();
            std::string text;
            for (const std::string& h : s.halfres_programs) text += (text.empty() ? "" : ",") + h;
            rt::log(rt::LogLevel::Warn, "nvn webgpu: half-resolution effects for %s (LGP_WEBGPU_HALFRES_PROGRAMS)",
                    text.c_str());
        }
        if (const char* v = std::getenv("LGP_WEBGPU_FILL_SCALE"); v != nullptr && *v != '\0') {
            const float f = std::strtof(v, nullptr);
            if (f > 0.0f && f < 1.0f) s.fill_scale = f;
            std::string targets;
            bool targets_ok = true;
            if (const char* t = std::getenv("LGP_WEBGPU_FILL_TARGETS"); t != nullptr && *t != '\0') {
                targets = t;
                for (size_t at = 0; at <= targets.size();) {
                    size_t end = targets.find(',', at);
                    if (end == std::string::npos) end = targets.size();
                    const std::string entry = targets.substr(at, end - at);
                    unsigned w = 0, h = 0;
                    char x = 0, rest = 0;
                    if (std::sscanf(entry.c_str(), "%u%c%u%c", &w, &x, &h, &rest) == 3 && (x == 'x' || x == 'X') &&
                        w > 0 && h > 0)
                        s.fill_targets.emplace_back(w, h);
                    else
                        targets_ok = false;
                    at = end + 1;
                }
            }
            // A typo must not silently widen the switch to every large target.
            if (!targets_ok) {
                rt::log(rt::LogLevel::Warn,
                        "nvn webgpu: LGP_WEBGPU_FILL_TARGETS=%s not understood (WxH[,WxH...]), fill scale off",
                        targets.c_str());
                s.fill_scale = 1.0f;
                s.fill_targets.clear();
            } else if (s.fill_scale < 1.0f)
                rt::log(rt::LogLevel::Warn,
                        "nvn webgpu: draws fill %.2f of %s (LGP_WEBGPU_FILL_SCALE, diagnosis: wrong picture)",
                        s.fill_scale, s.fill_targets.empty() ? "targets of at least 1280x720" : targets.c_str());
            else
                rt::log(rt::LogLevel::Warn, "nvn webgpu: LGP_WEBGPU_FILL_SCALE=%s ignored (needs 0 < scale < 1)", v);
        }
        if (const char* v = std::getenv("LGP_WEBGPU_RENDER_SCALE"); v != nullptr && *v != '\0') {
            const float f = std::strtof(v, nullptr);
            if (f >= 0.25f && f <= 2.0f) {
                s.render_scale = f;
            } else {
                rt::log(rt::LogLevel::Warn, "nvn webgpu: LGP_WEBGPU_RENDER_SCALE=%s ignored (needs 0.25..2)", v);
            }
        }
        if (const char* v = std::getenv("LGP_WEBGPU_RENDER_SCALE_MIN"); v != nullptr && *v != '\0')
            s.render_scale_min = uint32_t(std::strtoul(v, nullptr, 10));
        nvn::set_render_scale_shaders(s.render_scale);
        if (s.render_scale != 1.0f)
            rt::log(rt::LogLevel::Warn,
                    "nvn webgpu: render targets at %.2f of their size, from %u texels per side "
                    "(LGP_WEBGPU_RENDER_SCALE, prototype)",
                    s.render_scale, s.render_scale_min);
#if defined(__ANDROID__)
        // OnePlus 7 Pro (Adreno 640), Arena, frames 1501-3000, host clock, ABAB x3, both together:
        // 8.87 -> 9.33 fps (+5.2 %, every B run above every A run), render passes 48.7 -> 38.0 per
        // frame; frame grid with the virtual clock unchanged (frame 1500). Neutral on the Shield
        // (Analysis 49). P7.3.
        s.fold_clears = true;
        s.keep_pass_same_targets = true;
#endif
        if (const char* v = std::getenv("LGP_WEBGPU_FOLD_CLEARS"); v != nullptr && *v != '\0')
            s.fold_clears = std::string(v) != "0";
        if (s.fold_clears)
            rt::log(rt::LogLevel::Info, "nvn webgpu: whole-texture clears become the next draw pass's load op "
                                        "(LGP_WEBGPU_FOLD_CLEARS)");
        if (const char* v = std::getenv("LGP_WEBGPU_KEEP_PASS_SAME_TARGETS"); v != nullptr && *v != '\0')
            s.keep_pass_same_targets = std::string(v) != "0";
        if (s.keep_pass_same_targets)
            rt::log(rt::LogLevel::Info, "nvn webgpu: render targets set again unchanged keep the draw pass open "
                                        "(LGP_WEBGPU_KEEP_PASS_SAME_TARGETS)");
        // Async pipelines: browser default; on the desktop (tests) the
        // browser's way with wgpu-native's synchronous stand-in.
#if defined(__EMSCRIPTEN__)
        s.async_pipelines = true;
#endif
        if (const char* v = std::getenv("LGP_WEBGPU_ASYNC_PIPELINES"); v != nullptr && *v != '\0')
            s.async_pipelines = std::string(v) != "0";
        rt::log(rt::LogLevel::Info, "nvn webgpu: async pipelines %s (LGP_WEBGPU_ASYNC_PIPELINES)",
                s.async_pipelines ? "on" : "off");
        rt::log(rt::LogLevel::Info, "nvn webgpu: async writeback %s (LGP_WEBGPU_ASYNC_WRITEBACK)",
                s.async_writeback ? "on" : "off");
        rt::log(rt::LogLevel::Info, "nvn webgpu: compute first %s (LGP_WEBGPU_COMPUTE_FIRST)",
                s.compute_first ? "on" : "off");
#if defined(LGP_WEBGPU_WGPU_NATIVE)
        if (s.poll_submit_index)
            rt::log(rt::LogLevel::Info,
                    "nvn webgpu: write-back waits poll the read-back's submission (LGP_WEBGPU_POLL_SUBMIT_INDEX)");
#endif
        if (const char* v = std::getenv("LGP_WEBGPU_GEOMETRY_CACHE"); v != nullptr && *v != '\0')
            s.geometry_cache_on = std::string(v) != "0";
        // LGP_WEBGPU_GEOMETRY_CACHE_MB: its limit (default 256; P7.3, phones).
#if defined(__APPLE__) && TARGET_OS_TV
        // tvOS: 64 MiB. Apple TV 4K, frame 8500: buffers 29 instead of 77 MiB,
        // footprint peak 1615 instead of 1730 MiB under the ≈ 2 GiB jetsam
        // limit (P7.4, Analyse 28).
        s.geometry_cache_limit = uint64_t(64) << 20;
#endif
        if (const char* v = std::getenv("LGP_WEBGPU_GEOMETRY_CACHE_MB"); v != nullptr && std::atoi(v) > 0)
            s.geometry_cache_limit = uint64_t(std::atoi(v)) << 20;
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
        // Mac, native, frame 8500, ABAB: footprint -75..-93 MB, same fps (P7.3).
        s.geometry_fingerprint = true;
#endif
        if (const char* v = std::getenv("LGP_WEBGPU_GEOMETRY_FP"); v != nullptr && *v != '\0')
            s.geometry_fingerprint = std::string(v) != "0";
        rt::log(rt::LogLevel::Info, "nvn webgpu: geometry cache with fingerprints %s (LGP_WEBGPU_GEOMETRY_FP)",
                s.geometry_fingerprint ? "on" : "off");
        s.warm_all = s.async_pipelines;
#if defined(__ANDROID__)
        // Phones: a cached compute pipeline took up to 57 s to create on the
        // Adreno 640; warmed at once on the worker threads the first 300
        // frames took 15.9 s instead of 130 s (P7.3).
        s.warm_all = true;
#endif
        // LGP_WEBGPU_LAZY_TEXTURES=0/1: a texture's GPU side only once something
        // uses it. Default on phones and tablets: on the OnePlus 7 Pro 2891 of
        // 2959 MiB of textures were never used by frame 300 (BCn decoded to
        // RGBA8), and the device ran out of GPU memory at frame ~5800 (P7.3).
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
        s.lazy_textures = true;
#endif
        if (const char* v = std::getenv("LGP_WEBGPU_LAZY_TEXTURES"); v != nullptr && *v != '\0')
            s.lazy_textures = std::string(v) != "0";
        if (const char* v = std::getenv("LGP_WEBGPU_TEXTURE_BUDGET_MB"); v != nullptr && std::atof(v) > 0)
            s.texture_budget = std::max<uint64_t>(uint64_t(std::atof(v) * (1 << 20)), 1);
        if (const char* v = std::getenv("LGP_WEBGPU_TEXTURE_REPORT"); v != nullptr && std::atoi(v) > 0)
            s.texture_report_every = uint64_t(std::atoi(v));
        if (const char* v = std::getenv("LGP_WEBGPU_WARM_ALL"); v != nullptr && *v != '\0')
            s.warm_all = std::string(v) != "0";
        if (const char* v = std::getenv("LGP_WEBGPU_PIPELINE_WAIT_MS"); v != nullptr && std::atoi(v) > 0)
            s.pipeline_wait_ms = uint32_t(std::atoi(v));
        if (const char* v = std::getenv("LGP_WEBGPU_PIPELINE_NOWAIT"); v != nullptr && *v != '\0')
            s.pipeline_nowait = std::string(v) != "0";
#if defined(__APPLE__) && TARGET_OS_TV
        // Apple TV 4K (A15), scene change, ABAB, host clock: uploads -32 %,
        // fps 24.6-24.8 instead of 23.4; quiet frames unchanged (P7.4, #215).
        s.decode_threads = 3;
#endif
        if (const char* v = std::getenv("LGP_WEBGPU_DECODE_THREADS"); v != nullptr && *v != '\0') {
            char* end = nullptr;
            const long n = std::strtol(v, &end, 10);
            if (end != v && *end == '\0' && n >= 0 && n <= 16)
                s.decode_threads = uint32_t(n);
            else  // not a number from 0 to 16: the default stays
                rt::log(rt::LogLevel::Warn, "nvn webgpu: LGP_WEBGPU_DECODE_THREADS=%s ignored (0-16), %u helpers",
                        v, s.decode_threads);
        }
        if (const char* v = std::getenv("LGP_WEBGPU_PIPELINE_THREADS"); v != nullptr && *v != '\0') {
            const long n = std::strtol(v, nullptr, 10);
            if (n >= 0 && n <= 16)
                s.pipeline_threads = unsigned(std::max(n, 1L));
        }
        if (!s.async_pipelines)
            rt::log(rt::LogLevel::Info, "nvn webgpu: pipeline threads %u (LGP_WEBGPU_PIPELINE_THREADS)",
                    s.pipeline_threads);
        if (s.async_pipelines)
            rt::log(rt::LogLevel::Info, "nvn webgpu: pipeline nowait %s (LGP_WEBGPU_PIPELINE_NOWAIT)",
                    s.pipeline_nowait ? "on" : "off");
        if (s.async_pipelines) {
            s.prebuilds = std::make_shared<State::Prebuilds>();
            s.prebuilds->device = s.device;
            s.prebuilds->async = true;
            s.prebuilds->on_finish = [&s](const std::string& tag) {
                if (s.pipeline_wait && s.pipeline_wait->tags.erase(tag))
                    s.check_pipeline_wait();
            };
        }
        s.depth_clip_control = wgpuDeviceHasFeature(s.device, WGPUFeatureName_DepthClipControl);
        s.rg11b10_renderable = wgpuDeviceHasFeature(s.device, WGPUFeatureName_RG11B10UfloatRenderable);
        {
            // Which optional features the adapter offers and which the device
            // got (requested), e.g. whether R11G11B10F renders natively or as
            // RGBA16Float.
            static constexpr std::pair<WGPUFeatureName, const char*> kFeatures[] = {
                {WGPUFeatureName_TextureCompressionBC, "texture-compression-bc"},
                {WGPUFeatureName_TextureCompressionASTC, "texture-compression-astc"},
                {WGPUFeatureName_TextureCompressionETC2, "texture-compression-etc2"},
                {WGPUFeatureName_RG11B10UfloatRenderable, "rg11b10ufloat-renderable"},
                {WGPUFeatureName_Float32Filterable, "float32-filterable"},
                {WGPUFeatureName_DepthClipControl, "depth-clip-control"},
                {WGPUFeatureName_TimestampQuery, "timestamp-query"},
            };
            const std::vector<WGPUFeatureName>& offered = renderer_.adapter_features();
            std::string adapter, device;
            for (const auto& [f, name] : kFeatures) {
                if (std::find(offered.begin(), offered.end(), f) != offered.end())
                    adapter += std::string(adapter.empty() ? "" : ", ") + name;
                if (wgpuDeviceHasFeature(s.device, f))
                    device += std::string(device.empty() ? "" : ", ") + name;
            }
            rt::log(rt::LogLevel::Info, "nvn webgpu: features: adapter %s; device %s",
                    adapter.empty() ? "-" : adapter.c_str(), device.empty() ? "-" : device.c_str());
        }
        WGPULimits limits = WGPU_LIMITS_INIT;
        if (wgpuDeviceGetLimits(s.device, &limits) == WGPUStatus_Success)
            s.max_vertex_buffers = std::max(limits.maxVertexBuffers, kMaxVertexBuffers);
        // LGP_WEBGPU_MAX_VERTEX_BUFFERS=N uses at most N (tests, and the
        // browser's 8 on the desktop).
        if (const char* n = std::getenv("LGP_WEBGPU_MAX_VERTEX_BUFFERS"); n != nullptr && std::atoi(n) >= 2)
            s.max_vertex_buffers = std::min(s.max_vertex_buffers, uint32_t(std::atoi(n)));
        if (const char* n = std::getenv("LGP_WEBGPU_FRAMES_IN_FLIGHT"); n != nullptr && *n != '\0')
            s.max_frames_in_flight = uint32_t(std::max(0, std::atoi(n)));
        s.clear_shader = pw::create_wgsl_module(s.device, "nvn clear", kClearWgsl);
        WGPUBindGroupLayoutEntry entry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        entry.binding = 0;
        entry.visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
        entry.buffer.type = WGPUBufferBindingType_Uniform;
        entry.buffer.hasDynamicOffset = true;
        entry.buffer.minBindingSize = sizeof(ClearUniform);
        WGPUBindGroupLayoutDescriptor layout = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
        layout.entryCount = 1;
        layout.entries = &entry;
        s.clear_layout = wgpuDeviceCreateBindGroupLayout(s.device, &layout);
        WGPUPipelineLayoutDescriptor pl = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
        pl.bindGroupLayoutCount = 1;
        pl.bindGroupLayouts = &s.clear_layout;
        s.clear_pipeline_layout = wgpuDeviceCreatePipelineLayout(s.device, &pl);
        s.ensure_uniforms(16);

        WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
        td.label = pw::string_view("nvn unbound texture");
        td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
        td.size = {1, 1, 1};
        td.format = WGPUTextureFormat_RGBA8Unorm;
        s.dummy_texture = gpu_create_texture(s.device, &td);
        s.dummy_view = wgpuTextureCreateView(s.dummy_texture, nullptr);
        WGPUTextureViewDescriptor avd = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
        avd.dimension = WGPUTextureViewDimension_2DArray;
        avd.arrayLayerCount = 1;
        avd.mipLevelCount = 1;
        s.dummy_array_view = wgpuTextureCreateView(s.dummy_texture, &avd);
        const uint8_t zero[4] = {0, 0, 0, 0};
        WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
        dst.texture = s.dummy_texture;
        WGPUTexelCopyBufferLayout layout_info = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
        layout_info.bytesPerRow = 4;
        layout_info.rowsPerImage = 1;
        const WGPUExtent3D one = {1, 1, 1};
        queue_write_texture(s.queue, &dst, zero, sizeof(zero), &layout_info, &one);
        {
            WGPUTextureDescriptor dd = WGPU_TEXTURE_DESCRIPTOR_INIT;
            dd.label = pw::string_view("nvn unbound depth texture");
            dd.usage = WGPUTextureUsage_TextureBinding;  // zero-initialized: depth 0
            dd.size = {1, 1, 1};
            dd.format = WGPUTextureFormat_Depth32Float;
            s.dummy_depth_texture = gpu_create_texture(s.device, &dd);
            s.dummy_depth_view = wgpuTextureCreateView(s.dummy_depth_texture, nullptr);
            WGPUTextureViewDescriptor dav = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
            dav.dimension = WGPUTextureViewDimension_2DArray;
            dav.arrayLayerCount = 1;
            dav.mipLevelCount = 1;
            s.dummy_depth_array_view = wgpuTextureCreateView(s.dummy_depth_texture, &dav);
        }
        s.float32_filterable = wgpuDeviceHasFeature(s.device, WGPUFeatureName_Float32Filterable);
        if (debug_overrides().gputime) {
            s.timestamps = wgpuDeviceHasFeature(s.device, WGPUFeatureName_TimestampQuery);
            if (!s.timestamps)
                rt::log(rt::LogLevel::Warn, "nvn webgpu: gputime: the device has no timestamp-query, nothing timed");
#if !defined(__EMSCRIPTEN__)
            if (s.timestamps && s.queue != nullptr) {
                const float period = wgpuQueueGetTimestampPeriod(s.queue);
                if (period > 0.0f)
                    s.gpu_times->ns_per_tick = period;
                rt::log(rt::LogLevel::Info, "nvn webgpu: gputime: %.3f ns per timestamp tick", double(period));
            }
#endif
        }
        {
            WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
            bd.label = pw::string_view("nvn unbound constant buffer");
            bd.size = kCbufBytes;
            bd.usage = WGPUBufferUsage_Uniform;  // zero-initialized
            s.zero_cbuf = gpu_create_buffer(s.device, &bd);
        }
        if (WGPULimits limits = WGPU_LIMITS_INIT; wgpuDeviceGetLimits(s.device, &limits) == WGPUStatus_Success &&
                                                  limits.maxDynamicUniformBuffersPerPipelineLayout != WGPU_LIMIT_U32_UNDEFINED)
            s.max_dynamic_uniform_buffers = limits.maxDynamicUniformBuffersPerPipelineLayout;
        td.dimension = WGPUTextureDimension_3D;
        s.dummy_texture_3d = gpu_create_texture(s.device, &td);
        s.dummy_view_3d = wgpuTextureCreateView(s.dummy_texture_3d, nullptr);
        dst.texture = s.dummy_texture_3d;
        queue_write_texture(s.queue, &dst, zero, sizeof(zero), &layout_info, &one);
    });
}

void WebGpuBackend::wait_prebuilds() {
    const std::shared_ptr<State::Prebuilds> pb = state_->prebuilds;
    if (!pb)
        return;
    if (state_->prebuild_pop_test != 0)
        renderer_.run_sync([this] { state_->settle_late_prebuild_pops(); });
    std::unique_lock lock(pb->mutex);
    pb->cv.wait(lock, [&] { return pb->outstanding == 0 || pb->stop; });
}

#if LGP_DRIVER_PIPELINE_CACHE
namespace {

// The .driver file: this header, then wgpu's data. wgpu-core checks only
// that the data fits the adapter and driver, not that it is intact, and some
// Android drivers crash on damaged caches; a torn or mixed file (two
// processes on one path) fails the checksum and the cache starts empty.
constexpr char kDriverCacheMagic[8] = {'L', 'G', 'P', 'D', 'R', 'V', '0', '1'};
constexpr size_t kDriverCacheHeader = sizeof(kDriverCacheMagic) + 16;  // + u64 size + u64 FNV-1a 64

std::string driver_cache_file(const std::vector<char>& data) {
    std::string out(kDriverCacheMagic, sizeof(kDriverCacheMagic));
    const uint64_t size = data.size();
    const uint64_t sum = fnv1a64(std::string_view(data.data(), data.size()));
    out.append(reinterpret_cast<const char*>(&size), 8);
    out.append(reinterpret_cast<const char*>(&sum), 8);
    out.append(data.data(), data.size());
    return out;
}

// The data inside a .driver file, or nothing when it is damaged.
std::vector<char> driver_cache_data(const std::vector<char>& file) {
    if (file.size() < kDriverCacheHeader || std::memcmp(file.data(), kDriverCacheMagic, sizeof(kDriverCacheMagic)) != 0)
        return {};
    uint64_t size, sum;
    std::memcpy(&size, file.data() + sizeof(kDriverCacheMagic), 8);
    std::memcpy(&sum, file.data() + sizeof(kDriverCacheMagic) + 8, 8);
    if (size != file.size() - kDriverCacheHeader)
        return {};
    std::vector<char> data(file.begin() + kDriverCacheHeader, file.end());
    if (fnv1a64(std::string_view(data.data(), data.size())) != sum)
        return {};
    return data;
}

}  // namespace
#endif

// The driver's pipeline cache from an earlier run at path, or an empty one
// (wgpu-core checks that the data fits this adapter and driver). Without a
// file at path, the one at seed (packaged with the app, made on the same kind
// of device) starts it; it is written back to path only. Not with
// LGP_WEBGPU_DRIVER_CACHE=0 or without the device feature.
void WebGpuBackend::State::open_driver_cache([[maybe_unused]] const std::string& path,
                                             [[maybe_unused]] const std::string& seed) {
#if LGP_DRIVER_PIPELINE_CACHE
    State& s = *this;
    if (const char* env = std::getenv("LGP_WEBGPU_DRIVER_CACHE"); env != nullptr && std::string(env) == "0")
        return;
    if (!wgpuDeviceHasFeature(s.device, static_cast<WGPUFeatureName>(WGPUNativeFeature_PipelineCache))) {
        rt::log(rt::LogLevel::Info, "nvn webgpu: no driver pipeline cache on this device");
        return;
    }
    // The data of a .driver file; nothing when it is missing or damaged.
    auto read = [](const std::string& file_path) {
        std::vector<char> file;
        if (FILE* f = std::fopen(file_path.c_str(), "rb")) {
            std::vector<char> chunk(65536);
            size_t n;
            while ((n = std::fread(chunk.data(), 1, chunk.size(), f)) > 0)
                file.insert(file.end(), chunk.data(), chunk.data() + n);
            std::fclose(f);
        }
        std::vector<char> data = driver_cache_data(file);
        if (!file.empty() && data.empty())
            rt::log(rt::LogLevel::Warn, "nvn webgpu: driver pipeline cache %s is damaged or from an older build; "
                    "ignored", file_path.c_str());
        return data;
    };
    std::vector<char> data = read(path);
    const bool from_seed = data.empty() && !seed.empty();
    if (from_seed)
        data = read(seed);
    WGPUPipelineCacheDescriptor desc{};
    desc.label = pw::string_view("lgp driver pipeline cache");
    desc.data = data.empty() ? nullptr : data.data();
    desc.dataSize = data.size();
    desc.fallback = true;  // data from another driver: start empty
    WGPUPipelineCache cache = wgpuDeviceCreatePipelineCache(s.device, &desc);
    if (cache == nullptr) {
        rt::log(rt::LogLevel::Warn, "nvn webgpu: driver pipeline cache not created");
        return;
    }
    // Once per backend: set_pipeline_cache refuses a second cache, so no
    // save of an earlier one can be running.
    std::lock_guard lock(s.driver_cache_mutex);
    assert(s.driver_cache == nullptr && !s.driver_cache_saver.joinable());
    s.driver_cache = cache;
    s.driver_cache_path = path;
    s.driver_cache_loaded = static_cast<int64_t>(data.size());
    s.driver_cache_saved = std::chrono::steady_clock::now();
    s.driver_cache_opened_at = s.driver_cache_pipelines = g_driver_cache_pipelines.load(std::memory_order_relaxed);
    if (from_seed)
        rt::log(rt::LogLevel::Info,
                "nvn webgpu: driver pipeline cache %s: %zu bytes offered from seed %s (dropped if made by another "
                "GPU or driver)",
                path.c_str(), data.size(), seed.c_str());
    else
        rt::log(rt::LogLevel::Info, "nvn webgpu: driver pipeline cache %s: %zu bytes from earlier runs", path.c_str(),
                data.size());
#endif
}

// Writes the driver's pipeline cache back when pipelines were created with it
// since the last save: at most every kDriverCacheSaveInterval on a thread of
// its own, and at the end (final) on the calling thread whenever any
// pipeline used the cache in this run (a pipeline counted while an earlier
// save copied the data would otherwise be missed). Through a temporary file:
// a run killed while writing keeps the old data.
void WebGpuBackend::State::save_driver_cache([[maybe_unused]] bool final) {
#if LGP_DRIVER_PIPELINE_CACHE
    State& s = *this;
    std::unique_lock lock(s.driver_cache_mutex);
    const uint64_t pipelines = g_driver_cache_pipelines.load(std::memory_order_relaxed);
    if (final) {
        // Joined whatever else holds, so no running std::thread is destroyed.
        std::thread saver = std::move(s.driver_cache_saver);
        lock.unlock();
        if (saver.joinable())
            saver.join();
        if (s.driver_cache != nullptr && s.pipeline_cache && pipelines != s.driver_cache_opened_at)
            s.write_driver_cache(pipelines);
        return;
    }
    if (s.driver_cache == nullptr || !s.pipeline_cache)
        return;
    const auto now = std::chrono::steady_clock::now();
    if (s.driver_cache_saving || pipelines == s.driver_cache_pipelines || now - s.driver_cache_saved < kDriverCacheSaveInterval)
        return;
    if (s.driver_cache_saver.joinable())
        s.driver_cache_saver.join();  // finished: driver_cache_saving is clear
    s.driver_cache_saving = true;
    s.driver_cache_saved = now;  // the next attempt after the interval, whatever this one does
    s.driver_cache_saver = std::thread([&s, pipelines] {
        platform::set_current_thread_name("lgp-pipe-save");
        s.write_driver_cache(pipelines);
        std::lock_guard done(s.driver_cache_mutex);
        s.driver_cache_saving = false;
    });
#endif
}

void WebGpuBackend::State::write_driver_cache([[maybe_unused]] uint64_t pipelines) {
#if LGP_DRIVER_PIPELINE_CACHE
    State& s = *this;
    const auto t0 = std::chrono::steady_clock::now();
    // Prebuild threads may add to the cache between asking for the size and
    // copying; GetData then copies nothing and reports the larger size.
    std::vector<char> data;
    for (int attempt = 0; attempt < 8; ++attempt) {
        data.resize(wgpuPipelineCacheGetData(s.driver_cache, nullptr, 0));
        if (data.empty())
            return;
        const size_t got = wgpuPipelineCacheGetData(s.driver_cache, data.data(), data.size());
        if (got <= data.size()) {
            data.resize(got);
            break;
        }
        data.clear();
    }
    if (data.empty()) {
        rt::log(rt::LogLevel::Warn, "nvn webgpu: driver pipeline cache kept growing while saving; next time");
        return;
    }
    const std::string bytes = driver_cache_file(data);
    // Own temporary name per process and thread: two processes on one cache
    // must not write into each other's file.
    const std::string tmp = s.driver_cache_path + ".tmp." +
                            std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()) ^
                                           uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()));
    FILE* f = std::fopen(tmp.c_str(), "wb");
    bool ok = f != nullptr && std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    if (f != nullptr) ok &= std::fclose(f) == 0;
    std::error_code ec;
    if (ok) std::filesystem::rename(tmp, s.driver_cache_path, ec);
    if (!ok || ec) {
        rt::log(rt::LogLevel::Warn, "nvn webgpu: cannot write driver pipeline cache %s", s.driver_cache_path.c_str());
        std::filesystem::remove(tmp, ec);
        return;  // tried again after the interval, and at the end
    }
    {
        std::lock_guard lock(s.driver_cache_mutex);
        s.driver_cache_pipelines = std::max(s.driver_cache_pipelines, pipelines);
    }
    rt::log(rt::LogLevel::Info, "nvn webgpu: driver pipeline cache saved: %zu bytes in %.1f ms", data.size(),
            micros_since(t0) / 1000.0);
#endif
}

size_t WebGpuBackend::warm_cached_programs(uint64_t budget_us) {
    State& s = *state_;
    Stats delta;
    size_t left = 0;
    renderer_.run_sync([&] {
        s.warm_step(budget_us, delta);
        left = s.warm_todo.size() - std::min(s.warm_next, s.warm_todo.size());
    });
    std::lock_guard lock(stats_mutex_);
    stats_.programs_warmed += delta.programs_warmed;
    return left;
}

WebGpuBackend::PrebuildProgress WebGpuBackend::prebuild_progress() const {
    PrebuildProgress p;
    const std::shared_ptr<State::Prebuilds> pb = state_->prebuilds;
    if (!pb)
        return p;
    std::lock_guard lock(pb->mutex);
    p.total = pb->scheduled;
    p.done = pb->scheduled - pb->outstanding;
    return p;
}

size_t WebGpuBackend::hold_prebuilds(bool hold) {
    std::lock_guard hold_lock(hold_mutex_);
    prebuilds_held_ = hold;
    const std::shared_ptr<State::Prebuilds> pb = state_->prebuilds;
    if (!pb)
        return 0;
    size_t running;
    {
        std::lock_guard lock(pb->mutex);
        pb->held = hold;
        running = pb->running.size();
    }
    pb->cv.notify_all();
    return running;
}

bool WebGpuBackend::set_pipeline_cache(const std::string& path, std::string* error, const std::string& seed) {
    auto fail = [error](const char* why) {
        if (error != nullptr) *error = why;
        return false;
    };
    State& s = *state_;
    if (s.pipeline_cache)
        return fail("the pipeline cache is already set");
    if (const DebugOverrides& d = debug_overrides(); d.nocull || d.nodepth || d.noblend || d.nomask || d.solid ||
                                                     !d.solid_programs.empty())
        return fail("LGP_WEBGPU_DEBUG changes the pipelines; no pipeline cache");
    if (wgsl::compare_const_default())
        return fail("LGP_WGSL_COMPARE_CONST changes the shaders; no pipeline cache");
    // Recipes are valid again on the same adapter, driver and WebGPU
    // implementation; other ones start over.
    // The recipe layout follows FixedState: its size is part of it.
    std::string fingerprint = "recipes " + std::to_string(kPipelineRecipeVersion) + "/" +
                              std::to_string(sizeof(FixedState)) + "; " + renderer_.adapter_name() +
                              "; vertex buffers " + std::to_string(s.max_vertex_buffers) + "; dynamic uniforms " +
                              std::to_string(s.max_dynamic_uniform_buffers) + "; depth clip " +
                              std::to_string(s.depth_clip_control) + "; float32 filterable " +
                              std::to_string(s.float32_filterable);
#if !defined(__EMSCRIPTEN__)
    fingerprint += "; wgpu-native " + std::to_string(wgpuGetVersion());
#endif
    auto cache = std::make_shared<PipelineCacheFile>();
    if (!cache->open(path, fingerprint, error))
        return false;
    size_t seeded = 0;
    if (!seed.empty()) {
        std::string why;
        seeded = cache->import_records(seed, &why);
        if (!why.empty())
            rt::log(rt::LogLevel::Warn, "nvn webgpu: pipeline cache seed ignored: %s", why.c_str());
        else
            rt::log(rt::LogLevel::Info, "nvn webgpu: pipeline cache seed %s: %zu new records", seed.c_str(), seeded);
    }
    // Async pipelines bring their (async) Prebuilds along.
    std::shared_ptr<State::Prebuilds> pb = s.prebuilds;
    if (!pb) {
        pb = std::make_shared<State::Prebuilds>();
        pb->device = s.device;
#if defined(__EMSCRIPTEN__)
        pb->async = true;  // no second thread may use the browser's WebGPU objects
#else
        // LGP_WEBGPU_PREBUILD=async: the browser's way on the desktop (tests).
        const char* mode = std::getenv("LGP_WEBGPU_PREBUILD");
        pb->async = mode != nullptr && std::string(mode) == "async";
#endif
    }
#if !defined(__EMSCRIPTEN__)
    if (const char* pop = std::getenv("LGP_WEBGPU_PREBUILD_POP"); pop != nullptr)
        s.prebuild_pop_test = std::string(pop) == "late" ? 1 : std::string(pop) == "fail" ? 2 : 0;
#endif
    Stats delta;
    uint64_t warm_us = 0;
    size_t cached_programs = 0;
    bool warm_stopped = false;
    renderer_.run_sync([&] {
        s.open_driver_cache(path + ".driver", seed.empty() ? std::string() : seed + ".driver");
        s.pipeline_cache = cache;
        {
            std::lock_guard hold(hold_mutex_);
            s.prebuilds = pb;
            std::lock_guard lock(pb->mutex);
            pb->held = prebuilds_held_;
        }
        s.warm_todo = cache->program_order();
        cached_programs = cache->program_count();
        if (!pb->async)
            for (unsigned i = 0; i < prebuild_worker_count(); ++i)
                s.prebuild_workers.emplace_back(prebuild_worker_main, pb);
        if (s.warm_all) {
            const auto t0 = std::chrono::steady_clock::now();
            s.warm_step(std::numeric_limits<uint64_t>::max(), delta);
            warm_us = micros_since(t0);
            warm_stopped = s.warm_next < s.warm_todo.size();
        }
    });
    rt::log(rt::LogLevel::Info, "nvn webgpu: pipeline cache %s: %zu programs and %zu draw pipelines from earlier runs",
            path.c_str(), cached_programs, cache->loaded() - cached_programs);
    if (s.warm_all)
        rt::log(rt::LogLevel::Info, "nvn webgpu: %" PRIu64 " cached programs warmed at once in %.1f ms "
                "(their pipelines in the background; LGP_WEBGPU_WARM_ALL)%s", delta.programs_warmed, warm_us / 1000.0,
                warm_stopped ? "; stopped, app in the background" : "");
    std::lock_guard lock(stats_mutex_);
    stats_.programs_warmed += delta.programs_warmed;
    stats_.pipeline_cache_records = cache->loaded() - cached_programs;
    stats_.pipeline_cache_seeded = seeded;
    stats_.pipeline_cache_programs = cached_programs;
    stats_.driver_cache_loaded = s.driver_cache_loaded;
    return true;
}

WebGpuBackend::~WebGpuBackend() {
    finish();  // write-back callbacks use the state and the guest memory view
    if (const auto& pb = state_->prebuilds) {
        {
            std::lock_guard lock(pb->mutex);
            pb->stop = true;
            pb->queue.clear();
        }
        pb->cv.notify_all();
    }
    for (std::thread& t : state_->prebuild_workers) t.join();
    state_->prebuild_workers.clear();
    // The work-done callbacks of presented frames point at the state.
    if (state_->frames_in_flight.load() != 0)
        renderer_.run([this](pw::WebGpuRenderer::Call& call) {
            state_->when_frames_at_most(0, [&call] { call.finish(); });
        });
    if (state_->pipeline_cache)
        state_->pipeline_cache->flush();
    state_->save_driver_cache(true);
    renderer_.run_sync([this] {
        State& s = *state_;
        s.settle_late_prebuild_pops();  // given up (stop)
        *s.alive = false;
#if LGP_DRIVER_PIPELINE_CACHE
        if (s.driver_cache != nullptr) {
            wgpuPipelineCacheRelease(s.driver_cache);
            s.driver_cache = nullptr;
        }
#endif
        if (s.prebuilds) {
            std::lock_guard lock(s.prebuilds->mutex);
            for (const State::Prebuilds::Done& d : s.prebuilds->done) {
                if (d.pipeline != nullptr) wgpuRenderPipelineRelease(d.pipeline);
                if (d.compute != nullptr) wgpuComputePipelineRelease(d.compute);
            }
            s.prebuilds->done.clear();
        }
        for (auto& [serial, w] : s.warm) {
            for (auto& [recipe, p] : w.pipelines) wgpuRenderPipelineRelease(p);
            s.release_program(*w.program);
        }
        s.warm.clear();
        for (auto& [id, t] : s.textures)
            s.release_texture(t);
        while (!s.programs.empty())
            destroy_program_locked(s.programs.begin()->first);
        s.clear_bind_group_cache();
        s.cbuf_arena.release();
        if (s.buffer_pool) s.buffer_pool->close();
        gpu_destroy_buffer(s.zero_cbuf);
        s.geometry_arena.release();
        for (auto& [key, e] : s.geometry_cache)
            gpu_destroy_buffer(e.buffer);
        s.geometry_cache.clear();
        for (auto& [key, p] : s.color_pipelines)
            wgpuRenderPipelineRelease(p);
        for (auto& [key, p] : s.blit_pipelines)
            wgpuRenderPipelineRelease(p);
        for (WGPUSampler smp : s.blit_samplers)
            if (smp != nullptr) wgpuSamplerRelease(smp);
        if (s.blit_pipeline_layout != nullptr) wgpuPipelineLayoutRelease(s.blit_pipeline_layout);
        if (s.blit_layout != nullptr) wgpuBindGroupLayoutRelease(s.blit_layout);
        if (s.blit_shader != nullptr) wgpuShaderModuleRelease(s.blit_shader);
        for (auto& [key, p] : s.depth_pipelines)
            wgpuRenderPipelineRelease(p);
        for (auto& [key, tv] : s.proxy_depth) {
            wgpuTextureViewRelease(tv.second);
            gpu_destroy_texture(tv.first);
        }
        if (s.clear_bind_group != nullptr) wgpuBindGroupRelease(s.clear_bind_group);
        gpu_destroy_buffer(s.clear_uniforms);
        if (s.clear_pipeline_layout != nullptr) wgpuPipelineLayoutRelease(s.clear_pipeline_layout);
        if (s.clear_layout != nullptr) wgpuBindGroupLayoutRelease(s.clear_layout);
        if (s.clear_shader != nullptr) wgpuShaderModuleRelease(s.clear_shader);
        for (auto& [desc, smp] : s.samplers) wgpuSamplerRelease(smp);
        gpu_destroy_buffer(s.zero_vertex);
        for (WGPUBuffer b : s.dummy_storage_buffers)
            gpu_destroy_buffer(b);
        for (auto& [codes, b] : s.swizzle_buffers) gpu_destroy_buffer(b);
        if (s.dummy_view != nullptr) wgpuTextureViewRelease(s.dummy_view);
        if (s.dummy_depth_view != nullptr) wgpuTextureViewRelease(s.dummy_depth_view);
        if (s.dummy_depth_array_view != nullptr) wgpuTextureViewRelease(s.dummy_depth_array_view);
        if (s.dummy_depth_texture != nullptr) {
            gpu_destroy_texture(s.dummy_depth_texture);
        }
        if (s.dummy_array_view != nullptr) wgpuTextureViewRelease(s.dummy_array_view);
        if (s.dummy_texture != nullptr) {
            gpu_destroy_texture(s.dummy_texture);
        }
        if (s.dummy_view_3d != nullptr) wgpuTextureViewRelease(s.dummy_view_3d);
        if (s.dummy_texture_3d != nullptr) {
            gpu_destroy_texture(s.dummy_texture_3d);
        }
    });
}

void WebGpuBackend::create_texture(nvn::TextureId id, const nvn::TextureDesc& desc) {
    renderer_.run_sync([&] {
        State& s = *state_;
        auto old = s.textures.find(id);
        if (old != s.textures.end())
            s.release_texture(old->second);
        State::Texture t;
        t.desc = desc;
        t.format = texture_format(desc.format);
        if (is_bc(t.format) &&
            (force_cpu_bc_ || debug_overrides().cpubc ||
             !wgpuDeviceHasFeature(s.device, WGPUFeatureName_TextureCompressionBC))) {
            WGPUTextureFormat host = WGPUTextureFormat_Undefined;
            t.bc_cpu = bc_cpu_format(t.format, &t.bc_format, &host);
            t.format = host;  // BC6H: Undefined (no decoder)
            if (t.bc_cpu && s.decode_threads > 0 && !s.decode_pool) {
                s.decode_pool = std::make_unique<lgp::gpu::texture::DecodePool>(s.decode_threads);
                rt::log(rt::LogLevel::Info, "nvn webgpu: BCn decoding on the GPU thread and %u helper(s) "
                        "(LGP_WEBGPU_DECODE_THREADS)", s.decode_threads);
            }
            static std::atomic<bool> logged{false};
            if (!logged.exchange(true))
                rt::log(rt::LogLevel::Info, "nvn webgpu: BCn textures are decoded on the CPU (no "
                        "texture-compression-bc)");
        }
        if (is_astc(t.format) &&
            (force_cpu_astc_ || debug_overrides().cpuastc ||
             !wgpuDeviceHasFeature(s.device, WGPUFeatureName_TextureCompressionASTC))) {
            const auto info = nvn::format_info(desc.format);
            t.format = info && info->srgb ? WGPUTextureFormat_RGBA8UnormSrgb : WGPUTextureFormat_RGBA8Unorm;
            t.astc_cpu = true;
        }
        if (is_nvn_astc(desc.format) || t.is_3d()) {
            std::lock_guard lock(stats_mutex_);
            stats_.astc_textures += is_nvn_astc(desc.format) ? 1 : 0;
            stats_.textures_3d += t.is_3d() ? 1 : 0;
        }
        // R11G11B10F is mostly an HDR render target; without the renderable
        // feature a wider float format keeps it renderable (uploads of packed
        // texels would be wrong then, not seen so far).
        if (t.format == WGPUTextureFormat_RG11B10Ufloat &&
            !wgpuDeviceHasFeature(s.device, WGPUFeatureName_RG11B10UfloatRenderable))
            t.format = WGPUTextureFormat_RGBA16Float;
        const bool compressed = is_compressed(t.format);
        const bool target_2d = desc.target == uint32_t(nvn::TextureTarget::Target2D) ||
                               desc.target == uint32_t(nvn::TextureTarget::Target2DArray) ||
                               desc.target == uint32_t(nvn::TextureTarget::TargetRectangle) ||
                               desc.target == uint32_t(nvn::TextureTarget::Target2DMultisample);
        // 3D: sampled and copied, not rendered to (WebGPU's limit is 2048 per side).
        const bool target_3d = t.is_3d() && !compressed && !has_depth(t.format) && !has_stencil(t.format) &&
                               desc.samples <= 1 &&
                               desc.width <= 2048 && desc.height <= 2048 && desc.depth <= 2048;
        if (t.format == WGPUTextureFormat_Undefined || !(target_2d || target_3d) || desc.width == 0 ||
            desc.height == 0 || desc.width > 16384 || desc.height > 16384) {
            rt::log(rt::LogLevel::Warn, "nvn webgpu: texture #%u (format %u, target %u, %ux%u) not supported", id,
                    desc.format, desc.target, desc.width, desc.height);
            s.textures[id] = t;  // known, but no GPU texture: clears and presents skip it
            return;
        }
        // WebGPU knows 1 and 4 samples; NVN's 2 and 8 round to 4 (UNVERIFIED
        // whether LGP renders multisampled at all).
        t.sample_count = desc.samples > 1 ? 4 : 1;
        const bool depth_stencil = has_depth(t.format) || has_stencil(t.format);
        WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
        td.label = pw::string_view("nvn texture");
        td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
        // 8-bit snorm formats are not color-renderable in core WebGPU.
        const bool snorm8 = t.format == WGPUTextureFormat_R8Snorm || t.format == WGPUTextureFormat_RG8Snorm ||
                            t.format == WGPUTextureFormat_RGBA8Snorm;
        if (!compressed && !target_3d && !snorm8)
            td.usage |= WGPUTextureUsage_RenderAttachment;
        if (t.sample_count == 1)
            td.usage |= WGPUTextureUsage_CopySrc;  // read back, texture-to-texture copies
        td.dimension = target_3d ? WGPUTextureDimension_3D : WGPUTextureDimension_2D;
        // Compressed textures are whole blocks in WebGPU (BCn: 4x4, ASTC: its footprint).
        const auto finfo = nvn::format_info(desc.format);
        const uint32_t bw = compressed && finfo ? finfo->block_width : 1, bh = compressed && finfo ? finfo->block_height : 1;
        const uint32_t tw = (desc.width + bw - 1) / bw * bw;
        const uint32_t th = (desc.height + bh - 1) / bh * bh;
        td.size = {tw, th, t.sample_count == 1 ? std::max(desc.depth, 1u) : 1u};
        td.format = t.format;
        td.mipLevelCount = t.sample_count == 1 ? std::max(desc.levels, 1u) : 1u;
        td.sampleCount = t.sample_count;
        t.plan.td = td;
        t.plan.td.label = {};
        t.plan.raw_format = pw::linear_variant(t.format);
        t.plan.target_3d = target_3d;
        t.plan.compressed = compressed;
        t.plan.depth_stencil = depth_stencil;
        if (s.lazy_textures)
            t.lazy = true;
        else
            s.materialize(t);
        s.textures[id] = t;
    });
}

void WebGpuBackend::set_halfres_effects(bool on, const std::string& programs) {
    renderer_.run_sync([&] {
        State& s = *state_;
        if (!programs.empty())
            s.halfres_programs = parse_hash_prefixes(programs);
        s.halfres_on = on && !s.halfres_programs.empty();
    });
}

void WebGpuBackend::watch_programs(const std::string& programs) {
    renderer_.run_sync([&] {
        State& s = *state_;
        s.watched_programs = parse_hash_prefixes(programs);
        for (auto& [id, p] : s.programs)
            p->watched = -1;
    });
}

bool WebGpuBackend::take_watched_drawn() {
    return watched_drawn_.exchange(false, std::memory_order_relaxed);
}

void WebGpuBackend::set_render_scale(float scale) {
    if (const char* v = std::getenv("LGP_WEBGPU_RENDER_SCALE"); v != nullptr && *v != '\0')
        return;
    if (!(scale >= 0.25f && scale <= 2.0f)) {
        rt::log(rt::LogLevel::Warn, "nvn webgpu: render scale %.2f ignored (needs 0.25..2)", double(scale));
        return;
    }
    bool late = false;
    uint32_t min = 0;
    renderer_.run_sync([&] {
        State& s = *state_;
        late = !s.programs.empty();
        min = s.render_scale_min;
        if (!late)
            s.render_scale = scale;
    });
    if (late) {
        rt::log(rt::LogLevel::Warn, "nvn webgpu: render scale %.2f ignored (programs exist already)", double(scale));
        return;
    }
    nvn::set_render_scale_shaders(scale);
    if (scale != 1.0f)
        rt::log(rt::LogLevel::Info, "nvn webgpu: render targets at %.2f of their size, from %u texels per side",
                double(scale), min);
}

void WebGpuBackend::set_texture_budget(uint64_t bytes) {
    renderer_.run_sync([&] { state_->texture_budget = bytes; });
}

void WebGpuBackend::destroy_texture(nvn::TextureId id) {
    renderer_.run_sync([&] {
        State& s = *state_;
        auto it = s.textures.find(id);
        if (it == s.textures.end())
            return;
        s.release_texture(it->second);
        s.textures.erase(it);
        s.forget_depth_partner(id);
    });
}

namespace {

WGPUBindGroupLayout cbuf_layout(WGPUDevice device, const std::vector<uint32_t>& banks, WGPUShaderStage visibility,
                                bool dynamic = false) {
    std::vector<WGPUBindGroupLayoutEntry> entries;
    for (uint32_t bank : banks) {
        WGPUBindGroupLayoutEntry e = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        e.binding = bank;
        e.visibility = visibility;
        e.buffer.type = WGPUBufferBindingType_Uniform;
        e.buffer.hasDynamicOffset = dynamic;
        e.buffer.minBindingSize = kCbufBytes;
        entries.push_back(e);
    }
    WGPUBindGroupLayoutDescriptor ld = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    ld.entryCount = entries.size();
    ld.entries = entries.data();
    return wgpuDeviceCreateBindGroupLayout(device, &ld);
}

static_assert(uint32_t(nvn::Swizzle::Zero) == wgsl::kSwizzleZero && uint32_t(nvn::Swizzle::A) == wgsl::kSwizzleA,
              "texture swizzle codes go to the shader unchanged");

// Bytes of the texture swizzle uniform for `count` texture bindings.
// The swizzle words and one vec4 of scaled-binding bits (wgsl::Options::render_scale).
uint64_t swizzle_uniform_size(size_t count) { return 16 * ((count + 3) / 4 + 1); }

// arrays: per binding a 2D array; dims: per binding 2 or 3
// (ProgramDesc::*_texture_dims); missing = 2D.
WGPUBindGroupLayout texture_layout(WGPUDevice device, size_t count, WGPUShaderStage visibility, bool swizzle,
                                   bool global_memory = false, const std::vector<bool>& arrays = {},
                                   const std::vector<uint32_t>& dims = {}, const std::vector<bool>& depth = {},
                                   const std::vector<bool>& unfilterable = {}) {
    std::vector<WGPUBindGroupLayoutEntry> entries;
    for (size_t k = 0; k < count; ++k) {
        WGPUBindGroupLayoutEntry e = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        e.binding = uint32_t(2 * k);
        e.visibility = visibility;
        const bool is_depth = k < depth.size() && depth[k];
        const bool is_unfilterable = !is_depth && k < unfilterable.size() && unfilterable[k];
        e.texture.sampleType = is_depth          ? WGPUTextureSampleType_Depth
                               : is_unfilterable ? WGPUTextureSampleType_UnfilterableFloat
                                                 : WGPUTextureSampleType_Float;
        e.texture.viewDimension =
            k < dims.size() && dims[k] == 3 ? WGPUTextureViewDimension_3D
            : k < arrays.size() && arrays[k] ? WGPUTextureViewDimension_2DArray
                                             : WGPUTextureViewDimension_2D;
        entries.push_back(e);
        WGPUBindGroupLayoutEntry smp = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        smp.binding = uint32_t(2 * k + 1);
        smp.visibility = visibility;
        smp.sampler.type = is_depth          ? WGPUSamplerBindingType_Comparison
                           : is_unfilterable ? WGPUSamplerBindingType_NonFiltering
                                             : WGPUSamplerBindingType_Filtering;
        entries.push_back(smp);
    }
    if (swizzle && count > 0) {
        WGPUBindGroupLayoutEntry e = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        e.binding = uint32_t(2 * count);
        e.visibility = visibility;
        e.buffer.type = WGPUBufferBindingType_Uniform;
        e.buffer.minBindingSize = swizzle_uniform_size(count);
        entries.push_back(e);
    }
    // Read-only global memory of a graphics stage (wgsl::Options::graphics_global_memory).
    for (uint32_t k = 0; global_memory && k <= wgsl::kGlobalSlots; ++k) {
        WGPUBindGroupLayoutEntry e = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        e.binding = wgsl::kGraphicsGlobalBindingBase + k;
        e.visibility = visibility;
        e.buffer.type = k < wgsl::kGlobalSlots ? WGPUBufferBindingType_ReadOnlyStorage : WGPUBufferBindingType_Uniform;
        e.buffer.minBindingSize = k < wgsl::kGlobalSlots ? 4 : wgsl::kGlobalSlots * 16;
        entries.push_back(e);
    }
    WGPUBindGroupLayoutDescriptor ld = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    ld.entryCount = entries.size();
    ld.entries = entries.data();
    return wgpuDeviceCreateBindGroupLayout(device, &ld);
}

// Global memory of a compute shader (wgsl.h): storage buffer slots and the
// slot table; empty if the shader has none.
WGPUBindGroupLayout global_memory_layout(WGPUDevice device, bool used, bool probe) {
    std::vector<WGPUBindGroupLayoutEntry> entries;
    for (uint32_t k = 0; used && k < wgsl::kConstantSlotBinding + wgsl::kConstantSlots; ++k) {
        if (k == wgsl::kGlobalSlots + 1 && !probe)
            continue;
        WGPUBindGroupLayoutEntry e = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        e.binding = k;
        e.visibility = WGPUShaderStage_Compute;
        const bool uniform = k == wgsl::kGlobalSlots || k >= wgsl::kConstantSlotBinding;
        e.buffer.type = uniform ? WGPUBufferBindingType_Uniform : WGPUBufferBindingType_Storage;
        e.buffer.minBindingSize = k < wgsl::kGlobalSlots        ? 4
                                  : k == wgsl::kGlobalSlots     ? (wgsl::kGlobalSlots + wgsl::kConstantSlots) * 16
                                  : k == wgsl::kGlobalSlots + 1 ? wgsl::kGmemProbeWords * 4
                                                                : kCbufBytes;
        entries.push_back(e);
    }
    WGPUBindGroupLayoutDescriptor ld = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    ld.entryCount = entries.size();
    ld.entries = entries.data();
    return wgpuDeviceCreateBindGroupLayout(device, &ld);
}

}  // namespace

uint64_t WebGpuBackend::State::unfilterable_mask(const Program& prog, const BoundTextures& bound) const {
    uint64_t mask = 0;
    for (int stage = 0; stage < 2 && !prog.compute; ++stage) {
        const std::vector<uint32_t>& slots = prog.texture_slots[stage];
        for (size_t k = 0; k < slots.size() && k < 32; ++k) {
            const bool want_depth = k < prog.texture_depth[stage].size() && prog.texture_depth[stage][k];
            const bool want_3d = k < prog.texture_dims[stage].size() && prog.texture_dims[stage][k] == 3;
            if (want_depth || want_3d)
                continue;
            auto b = bound.find({stage, slots[k]});
            if (b == bound.end())
                continue;
            auto t = textures.find(b->second.texture);
            if (t != textures.end() && !t->second.is_3d() && needs_unfilterable(t->second))
                mask |= uint64_t(1) << (32 * stage + k);
        }
    }
    return mask;
}

const WebGpuBackend::Program::LayoutVariant* WebGpuBackend::State::layout_variant(Program& prog, uint64_t mask) {
    if (auto v = prog.layout_variants.find(mask); v != prog.layout_variants.end())
        return &v->second;
    Program::LayoutVariant v;
    const WGPUShaderStage vis[2] = {WGPUShaderStage_Vertex, WGPUShaderStage_Fragment};
    WGPUBindGroupLayout groups[4] = {prog.groups[0], prog.groups[1], prog.groups[2], prog.groups[3]};
    for (int stage = 0; stage < 2; ++stage) {
        const uint32_t bits = uint32_t(mask >> (32 * stage));
        if (bits == 0)
            continue;
        std::vector<bool> unfilterable(prog.texture_slots[stage].size());
        for (size_t k = 0; k < unfilterable.size() && k < 32; ++k) unfilterable[k] = (bits >> k) & 1;
        v.textures[stage] = texture_layout(device, prog.texture_slots[stage].size(), vis[stage], prog.texture_swizzle,
                                           prog.stage_global_memory[stage], prog.texture_arrays[stage],
                                           prog.texture_dims[stage], prog.texture_depth[stage], unfilterable);
        groups[2 + stage] = v.textures[stage];
    }
    WGPUPipelineLayoutDescriptor pl = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    pl.bindGroupLayoutCount = 4;
    pl.bindGroupLayouts = groups;
    v.layout = wgpuDeviceCreatePipelineLayout(device, &pl);
    return &prog.layout_variants.emplace(mask, v).first->second;
}

void WebGpuBackend::State::build_graphics_program(const nvn::ProgramDesc& desc, uint64_t hash, Program& p) {
    p.texture_swizzle = desc.texture_swizzle;
    p.stage_global_memory[0] = desc.vertex_global_memory;
    p.stage_global_memory[1] = desc.fragment_global_memory;
    p.texture_arrays[0] = desc.vertex_texture_arrays;
    p.texture_arrays[1] = desc.fragment_texture_arrays;
    p.texture_depth[0] = desc.vertex_texture_depth;
    p.texture_depth[1] = desc.fragment_texture_depth;
    p.banks[0] = desc.vertex_cbufs;
    p.banks[1] = desc.fragment_cbufs;
    p.texture_slots[0] = desc.vertex_textures;
    p.texture_slots[1] = desc.fragment_textures;
    p.texture_dims[0] = desc.vertex_texture_dims;
    p.texture_dims[1] = desc.fragment_texture_dims;
    p.hash = hash;
    p.wgsl_bytes = desc.vertex_wgsl.size() + desc.fragment_wgsl.size();
    p.vs = pw::create_wgsl_module(device, "nvn vertex", desc.vertex_wgsl.c_str());
    p.vertex_wgsl = desc.vertex_wgsl;
    p.flip_override = desc.vertex_wgsl.find("override lgp_flip_y") != std::string::npos;
    p.depth_override = desc.vertex_wgsl.find("override lgp_depth_minus_w") != std::string::npos;
    // Vertex inputs: "@location(N) aN:" in the entry point's parameters.
    for (size_t at = desc.vertex_wgsl.find("@location("); at != std::string::npos;
         at = desc.vertex_wgsl.find("@location(", at + 1)) {
        const size_t num = at + 10, close = desc.vertex_wgsl.find(')', num);
        if (close == std::string::npos)
            break;
        const std::string n = desc.vertex_wgsl.substr(num, close - num);
        if (desc.vertex_wgsl.compare(close, 3 + n.size(), ") a" + n) == 0)
            p.vertex_inputs.push_back(uint32_t(std::stoul(n)));
    }
    p.vertex_only = desc.fragment_wgsl.empty();
    if (!debug_overrides().solid && debug_overrides().solid_for(p.hash))
        rt::log(rt::LogLevel::Warn,
                p.vertex_only ? "nvn webgpu: program %016llx: vertex-only, no fragment work, not solid (LGP_WEBGPU_SOLID_PROGRAMS)"
                              : "nvn webgpu: program %016llx: solid (LGP_WEBGPU_SOLID_PROGRAMS)",
                (unsigned long long)p.hash);
    p.alpha_override = desc.fragment_wgsl.find("override lgp_alpha_func") != std::string::npos;
    // Whether the fragment shader reads its screen position (fpos.x/.y):
    // drawn into a smaller target (half-resolution draws), it would see
    // other coordinates.
    for (const char* c : {"fpos.x", "fpos.y"})
        p.reads_screen_xy = p.reads_screen_xy || desc.fragment_wgsl.find(c) != std::string::npos;
    p.target_scale_override = desc.fragment_wgsl.find("override lgp_target_scale") != std::string::npos;
    p.fs = pw::create_wgsl_module(device, "nvn fragment",
                                  p.vertex_only ? "@fragment fn main() {}\n" : desc.fragment_wgsl.c_str());
    const WGPUShaderStage vis[2] = {WGPUShaderStage_Vertex, WGPUShaderStage_Fragment};
    p.dynamic_cbufs = p.banks[0].size() + p.banks[1].size() <= max_dynamic_uniform_buffers;
    for (int stage = 0; stage < 2; ++stage) {
        p.groups[stage] = cbuf_layout(device, p.banks[stage], vis[stage], p.dynamic_cbufs);
        p.groups[2 + stage] = texture_layout(device, p.texture_slots[stage].size(), vis[stage], desc.texture_swizzle,
                                             p.stage_global_memory[stage], p.texture_arrays[stage],
                                             p.texture_dims[stage], p.texture_depth[stage]);
    }
    WGPUPipelineLayoutDescriptor pl = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    pl.bindGroupLayoutCount = 4;
    pl.bindGroupLayouts = p.groups;
    p.layout = wgpuDeviceCreatePipelineLayout(device, &pl);
}

void WebGpuBackend::State::build_compute_program(const nvn::ProgramDesc& desc, uint64_t hash, Program& p,
                                                 bool pipeline) {
    p.hash = hash;
    p.compute = true;
    p.global_memory = desc.compute_global_memory;
    p.gmem_probe = desc.compute_gmem_probe;
    p.banks[2] = desc.compute_cbufs;
    p.texture_slots[2] = desc.compute_textures;
    p.texture_dims[2] = desc.compute_texture_dims;
    p.wgsl_bytes = desc.compute_wgsl.size();
    for (uint32_t k = 0; k < wgsl::kGlobalSlots; ++k)
        if (desc.compute_wgsl.find("// static slot " + std::to_string(k) + " begin\n") != std::string::npos)
            p.static_slots.push_back(k);
    // Ascending, as wgsl::emit gives blocks their windows.
    for (uint32_t s = 0; s < wgsl::kUniformDescCount; ++s)
        if (desc.compute_wgsl.find("// static block " + std::to_string(s) + " begin\n") != std::string::npos)
            p.static_blocks.push_back(s);
    if (!p.static_slots.empty() || !p.static_blocks.empty())
        p.compute_wgsl = desc.compute_wgsl;
    p.cs = pw::create_wgsl_module(device, "nvn compute", desc.compute_wgsl.c_str());
    p.groups[0] = cbuf_layout(device, p.banks[2], WGPUShaderStage_Compute);
    p.texture_swizzle = desc.texture_swizzle;
    p.texture_arrays[2] = desc.compute_texture_arrays;
    p.texture_depth[2] = desc.compute_texture_depth;
    p.groups[1] = texture_layout(device, p.texture_slots[2].size(), WGPUShaderStage_Compute,
                                 p.texture_swizzle, false, p.texture_arrays[2], p.texture_dims[2],
                                 p.texture_depth[2]);
    p.groups[2] = global_memory_layout(device, p.global_memory, p.gmem_probe);
    WGPUPipelineLayoutDescriptor pl = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    pl.bindGroupLayoutCount = 3;
    pl.bindGroupLayouts = p.groups;
    p.layout = wgpuDeviceCreatePipelineLayout(device, &pl);
    if (!pipeline) {
        p.compute_deferred = true;
        return;
    }
    WGPUComputePipelineDescriptor cd = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
    cd.label = pw::string_view("nvn compute");
    cd.layout = p.layout;
    cd.compute.module = p.cs;
    cd.compute.entryPoint = pw::string_view("main");
    DriverCacheLink link;
    link.attach(cd.nextInChain, driver_cache);
    p.compute_pipeline = wgpuDeviceCreateComputePipeline(device, &cd);
}

void WebGpuBackend::create_program(nvn::ProgramId id, const nvn::ProgramDesc& desc) {
    bool valid = false;
    const auto t0 = std::chrono::steady_clock::now();
    const uint64_t hash = program_hash(desc);
    bool adopted = false;
    Stats delta;
    renderer_.run([&](pw::WebGpuRenderer::Call& call) {
        State& s = *state_;
        destroy_program_locked(id);
        // Created ahead from the pipeline cache: shaders and pipelines are there.
        s.harvest_prebuilds(delta);
        if (std::unique_ptr<Program> warm = s.adopt_warm(hash, id, delta)) {
            warm->guest_hash = desc.guest_hash;
            s.programs[id] = std::move(warm);
            valid = adopted = true;
            call.finish();
            return;
        }
        auto program = std::make_unique<Program>();
        Program& p = *program;
        p.guest_hash = desc.guest_hash;
        p.serial = s.next_program_serial++;
        s.program_by_serial[p.serial] = id;
        // Compile errors only surface through an error scope (or as
        // uncaptured errors); wgpu-native aborts on a submit that uses an
        // invalid object, so programs are validated here, once.
        wgpuDevicePushErrorScope(s.device, WGPUErrorFilter_Validation);
        if (desc.is_compute()) {
            // Debug: as below; named with the hash the slow-pipeline log
            // prints (log_compute_pipeline).
            if (const char* dir = dump_dir(); dir != nullptr) {
                char name[64];
                std::snprintf(name, sizeof(name), "/prog%u-%016llx.cs.wgsl", unsigned(id), (unsigned long long)hash);
                if (FILE* f = std::fopen((std::string(dir) + name).c_str(), "wb")) {
                    std::fwrite(desc.compute_wgsl.data(), 1, desc.compute_wgsl.size(), f);
                    std::fclose(f);
                }
            }
            // Async pipelines: the pipeline compiles while the game goes on;
            // the first dispatch waits for it if it is not done by then.
            s.build_compute_program(desc, hash, p, !s.async_pipelines);
            if (s.async_pipelines)
                s.schedule_compute_prebuild(p, true);
        } else {
            // Debug: LGP_WEBGPU_DUMP_DIR receives the WGSL of every program
            // (translated title shaders: keep the directory private).
            if (const char* dir = dump_dir(); dir != nullptr) {
                for (int stage = 0; stage < 2; ++stage) {
                    const std::string& w = stage ? desc.fragment_wgsl : desc.vertex_wgsl;
                    if (w.empty())
                        continue;  // no fragment stage
                    char name[64];
                    std::snprintf(name, sizeof(name), "/prog%u-%016llx.%s.wgsl", unsigned(id), (unsigned long long)hash,
                                  stage ? "fs" : "vs");
                    if (FILE* f = std::fopen((std::string(dir) + name).c_str(), "wb")) {
                        std::fwrite(w.data(), 1, w.size(), f);
                        std::fclose(f);
                    }
                }
            }
            s.build_graphics_program(desc, hash, p);
        }
        s.programs[id] = std::move(program);

        struct Pop {
            Program* program;
            bool* valid;
            pw::WebGpuRenderer::Call* call;
        };
        auto* pop = new Pop{s.programs[id].get(), &valid, &call};
        WGPUPopErrorScopeCallbackInfo cb = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
        cb.mode = WGPUCallbackMode_AllowSpontaneous;
        cb.userdata1 = pop;
        cb.callback = [](WGPUPopErrorScopeStatus status, WGPUErrorType type, WGPUStringView message, void* u1, void*) {
            auto* p = static_cast<Pop*>(u1);
            const bool ok = status == WGPUPopErrorScopeStatus_Success && type == WGPUErrorType_NoError;
            if (!ok)
                rt::log(rt::LogLevel::Warn, "nvn webgpu: program rejected: %s", pw::to_string(message).c_str());
            p->program->valid = ok;
            *p->valid = ok;
            p->call->finish();
            delete p;
        };
        wgpuDevicePopErrorScope(s.device, cb);
    });
    const uint64_t us = micros_since(t0);
    if (valid && state_->pipeline_cache) {
        // Its draw pipelines from earlier runs (recipes() does not change
        // after set_pipeline_cache); an adopted program has them already or
        // under way, this only adds what is missing.
        if (!desc.is_compute() && state_->pipeline_cache->recipes().count(hash))
            renderer_.run_sync([&] {
                if (auto it = state_->programs.find(id); it != state_->programs.end())
                    state_->schedule_prebuilds(*it->second, id);
            });
        if (!adopted)
            state_->pipeline_cache->add_program(hash, encode_program_desc(desc));
    }
    std::lock_guard lock(stats_mutex_);
    stats_.programs_adopted += delta.programs_adopted;
    stats_.pipelines_prebuilt += delta.pipelines_prebuilt;
    stats_.compute_pipelines_prebuilt += delta.compute_pipelines_prebuilt;
    stats_.prebuilds_failed += delta.prebuilds_failed;
    stats_.prebuild_us += delta.prebuild_us;
    stats_.invalid_pipelines += delta.invalid_pipelines;
    if (!valid)
        ++stats_.invalid_programs;
    ++stats_.programs_created;
    stats_.program_create_us += us;
    if (us >= kSlowCreateUs)
        rt::log(rt::LogLevel::Info, "nvn webgpu: %s program %u (%016llx, WGSL %zu bytes) created in %.1f ms "
                "(after present %llu)", desc.is_compute() ? "compute" : "graphics", unsigned(id),
                (unsigned long long)hash,
                desc.is_compute() ? desc.compute_wgsl.size() : desc.vertex_wgsl.size() + desc.fragment_wgsl.size(),
                us / 1000.0, (unsigned long long)stats_.presents);
}

void WebGpuBackend::destroy_program(nvn::ProgramId id) {
    renderer_.run_sync([&] { destroy_program_locked(id); });
}

void WebGpuBackend::destroy_program_locked(nvn::ProgramId id) {
    State& s = *state_;
    auto it = s.programs.find(id);
    if (it == s.programs.end())
        return;
    Program& p = *it->second;
    s.cancel_prebuilds(p.serial);
    s.program_by_serial.erase(p.serial);
    s.variant_requests.erase(id);
    s.clear_bind_group_cache();  // keyed by its layouts
    // Pipelines built from it go too.
    for (auto pit = s.draw_pipelines.begin(); pit != s.draw_pipelines.end();) {
        if (pit->second.program == id) {
            if (pit->second.pipeline != nullptr) wgpuRenderPipelineRelease(pit->second.pipeline);
            pit = s.draw_pipelines.erase(pit);
        } else {
            ++pit;
        }
    }
    s.release_program(p);
    s.programs.erase(it);
}

void WebGpuBackend::State::reject_nowait(const std::vector<std::pair<std::string, nvn::ProgramId>>& keys) {
    for (const auto& [key, program] : keys) {
        nowait_pending.erase(key);
        compiling_keys.erase(key);
        auto it = draw_pipelines.find(key);
        if (it == draw_pipelines.end()) {
            draw_pipelines[key] = {program, nullptr};
        } else if (it->second.pipeline != nullptr) {
            wgpuRenderPipelineRelease(it->second.pipeline);
            it->second.pipeline = nullptr;
        }
    }
}

void WebGpuBackend::State::create_pipelines_async(std::vector<PipelineRequest> missing,
                                                  const std::vector<nvn::ProgramId>& missing_compute,
                                                  const std::vector<nvn::ProgramId>& deferred_compute,
                                                  std::vector<std::string> awaited_keys, Stats& delta,
                                                  std::function<void()> record) {
    PipelineWait& w = *pipeline_wait;
    // Without a wait, only compute pipelines hold the list back: the draw
    // pipelines it found under way (prebuild tags carry a recipe, compute
    // ones none) compile on, their draws skipped meanwhile.
    if (pipeline_nowait) {
        for (auto t = w.tags.begin(); t != w.tags.end();)
            t = t->size() > sizeof(uint64_t) ? w.tags.erase(t) : std::next(t);
        for (std::string& key : awaited_keys) compiling_keys.insert(std::move(key));
        awaited_keys.clear();
    }
    if (missing.empty() && missing_compute.empty() && w.tags.empty()) {
        pipeline_wait.reset();
        record();
        return;
    }
    struct Batch {
        std::vector<std::string> keys;  // draw pipelines created or awaited here
        std::vector<std::pair<uint64_t, std::string>> records;  // per created key, for the pipeline cache
        std::vector<nvn::ProgramId> compute;
        size_t awaited = 0;  // keys[0, awaited): prebuilds under way, not created here
        bool scope_ok = true;
    };
    auto batch = std::make_shared<Batch>();
    batch->compute = deferred_compute;
    batch->keys = std::move(awaited_keys);
    batch->awaited = batch->keys.size();
    batch->records.resize(batch->keys.size());
    const bool draws = (!missing.empty() && !pipeline_nowait) || !batch->keys.empty();  // what the wait counts as
    const auto t0 = std::chrono::steady_clock::now();
    // Preparing may create shader module variants and layouts: if one of
    // them is invalid, all of this batch's draw pipelines are dropped (as
    // without async pipelines). The pipelines report their own errors.
    wgpuDevicePushErrorScope(device, WGPUErrorFilter_Validation);
    std::vector<Prebuilds::Job> jobs;
    std::vector<std::pair<std::string, nvn::ProgramId>> nowait_keys;  // pipeline_nowait: created, not waited for
    for (const PipelineRequest& r : missing) {
        Program& prog = *programs.at(r.program);
        std::unique_ptr<PipelineBuild> build = prepare_draw_pipeline(r, prog);
        if (build == nullptr) {  // as create_draw_pipeline
            draw_pipelines[r.key] = {r.program, nullptr};
            continue;
        }
        std::string recipe = r.key.substr(sizeof(nvn::ProgramId));
        if (pipeline_nowait) {
            nowait_pending[r.key] = {pipeline_cache ? prog.hash : 0, pipeline_cache ? recipe : std::string()};
            compiling_keys.insert(r.key);
            nowait_keys.emplace_back(r.key, r.program);
            jobs.push_back({std::move(recipe), prog.serial, std::move(build), nullptr, true});
            continue;
        }
        w.tags.insert(Prebuilds::tag(prog.serial, recipe));
        batch->keys.push_back(r.key);
        batch->records.emplace_back(pipeline_cache ? prog.hash : 0, pipeline_cache ? recipe : std::string());
        jobs.push_back({std::move(recipe), prog.serial, std::move(build), nullptr, true});
    }
    delta.draw_pipelines_created += missing.size();
    {
        std::lock_guard lock(prebuilds->mutex);
        prebuilds->outstanding += jobs.size();
        prebuilds->scheduled += jobs.size();
    }
    // Errors of the async creations do not reach this scope; the desktop's
    // stand-in uses scopes of its own inside it.
    submit_prebuilds(prebuilds, std::move(jobs));
    for (nvn::ProgramId id : missing_compute) {
        Program& p = *programs.at(id);
        schedule_compute_prebuild(p, true);
        await_prebuild(Prebuilds::tag(p.serial, {}));
    }
    if (pipeline_nowait) {
        // The preparation's scope settles the draw pipelines created without
        // a wait on its own; the list waits for compute pipelines only.
        struct NowaitPop {
            State* s;
            std::shared_ptr<bool> alive;  // State::alive: nothing waits for this scope
            std::vector<std::pair<std::string, nvn::ProgramId>> keys;
            bool in_pop = true, fired = false, ok = true;
        };
        auto* np = new NowaitPop{this, alive, std::move(nowait_keys)};
        WGPUPopErrorScopeCallbackInfo ncb = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
        ncb.mode = WGPUCallbackMode_AllowSpontaneous;
        ncb.userdata1 = np;
        ncb.callback = [](WGPUPopErrorScopeStatus status, WGPUErrorType type, WGPUStringView message, void* u1,
                          void*) {
            auto* p = static_cast<NowaitPop*>(u1);
            p->ok = status == WGPUPopErrorScopeStatus_Success && type == WGPUErrorType_NoError;
            if (!p->ok)
                rt::log(rt::LogLevel::Warn, "nvn webgpu: draw pipelines not prepared: %s",
                        pw::to_string(message).c_str());
            if (p->in_pop) {  // wgpu-native, inside PopErrorScope: no WebGPU calls here
                p->fired = true;
                return;
            }
            if (!p->ok && *p->alive) p->s->reject_nowait(p->keys);
            delete p;
        };
        wgpuDevicePopErrorScope(device, ncb);
        np->in_pop = false;
        if (np->fired) {
            if (!np->ok) reject_nowait(np->keys);
            delete np;
        }
        if (w.tags.empty()) {
            harvest_prebuilds(delta);  // what finished at once (wgpu-native's stand-in) is drawn now
            pipeline_wait.reset();
            record();
            return;
        }
    }
    struct Pop {
        State* s;
        std::shared_ptr<Batch> batch;
        bool in_pop = true, fired = false;
    };
    Pop* pop = nullptr;
    WGPUPopErrorScopeCallbackInfo cb = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
    if (!pipeline_nowait) {
        ++w.scopes;
        pop = new Pop{this, batch};
    }
    cb.mode = WGPUCallbackMode_AllowSpontaneous;
    cb.userdata1 = pop;
    cb.callback = [](WGPUPopErrorScopeStatus status, WGPUErrorType type, WGPUStringView message, void* u1, void*) {
        auto* p = static_cast<Pop*>(u1);
        p->batch->scope_ok = status == WGPUPopErrorScopeStatus_Success && type == WGPUErrorType_NoError;
        if (!p->batch->scope_ok)
            rt::log(rt::LogLevel::Warn, "nvn webgpu: draw pipelines not prepared: %s", pw::to_string(message).c_str());
        if (p->in_pop) {  // wgpu-native, inside PopErrorScope: no WebGPU calls here
            p->fired = true;
            return;
        }
        --p->s->pipeline_wait->scopes;
        p->s->check_pipeline_wait();
        delete p;
    };
    w.then = [this, batch, draws, t0, &delta, record = std::move(record)] {
        harvest_prebuilds(delta);
        std::vector<PipelineRequest> retry;
        for (size_t i = 0; i < batch->keys.size(); ++i) {
            const std::string& key = batch->keys[i];
            auto it = draw_pipelines.find(key);
            if (it == draw_pipelines.end()) {  // rejected
                nvn::ProgramId program;
                std::memcpy(&program, key.data(), sizeof(program));
                // A prebuild given up (its preparation's scope invalid, the
                // whole batch dropped) or failed: created now, as if none had
                // been under way; rejected then, it is marked invalid.
                PipelineRequest r;
                if (i < batch->awaited && programs.count(program) &&
                    parse_recipe(std::string_view(key).substr(sizeof(program)), program, &r)) {
                    retry.push_back(std::move(r));
                    continue;
                }
                draw_pipelines[key] = {program, nullptr};
                ++delta.invalid_pipelines;
            } else if (!batch->scope_ok && it->second.pipeline != nullptr) {
                wgpuRenderPipelineRelease(it->second.pipeline);
                it->second.pipeline = nullptr;
                ++delta.invalid_pipelines;
            } else if (it->second.pipeline != nullptr && pipeline_cache && !batch->records[i].second.empty()) {
                pipeline_cache->add(batch->records[i].first, batch->records[i].second);
            }
        }
        for (nvn::ProgramId id : batch->compute) {  // ahead from the cache, rejected
            auto it = programs.find(id);
            if (it == programs.end() || it->second->compute_pipeline != nullptr || !it->second->compute_deferred)
                continue;
            it->second->compute_deferred = false;  // its dispatches are skipped
            ++delta.invalid_pipelines;
        }
        const uint64_t us = micros_since(t0);
        if (draws)
            delta.draw_pipeline_create_us += us;
        else
            delta.compute_pipeline_wait_us += us;
        if (!retry.empty()) {
            pipeline_wait = std::make_unique<PipelineWait>();
            pipeline_wait->serial = next_pipeline_wait++;
            pipeline_wait->delta = &delta;
            create_pipelines_async(std::move(retry), {}, {}, {}, delta, record);
            return;
        }
        record();
    };
    if (pop != nullptr) {
        wgpuDevicePopErrorScope(device, cb);
        pop->in_pop = false;
        if (pop->fired) {
            --w.scopes;
            delete pop;
        }
    }
    w.armed = true;
    const uint64_t serial = w.serial;
    check_pipeline_wait();
    if (!pipeline_wait || pipeline_wait->serial != serial)
        return;
#if defined(__EMSCRIPTEN__)
    auto* timeout = new PipelineWaitTimeout{this, prebuilds, serial};
    pipeline_wait->timeout_arg = timeout;
    pipeline_wait->timeout_id = emscripten_set_timeout(
        [](void* u) {
            std::unique_ptr<PipelineWaitTimeout> t(static_cast<PipelineWaitTimeout*>(u));
            {
                std::lock_guard lock(t->pb->mutex);
                if (t->pb->stop)  // the backend is going (or gone)
                    return;
            }
            if (t->s->pipeline_wait && t->s->pipeline_wait->serial == t->serial) {
                t->s->pipeline_wait->timeout_id = 0;  // fired: nothing to clear
                t->s->pipeline_wait->timeout_arg = nullptr;
                t->s->expire_pipeline_wait();
            }
        },
        double(pipeline_wait_ms), timeout);
#else
    // Tests: the browser's late scope callbacks come while the list waits.
    settle_late_prebuild_pops();
    if (!pipeline_wait || pipeline_wait->serial != serial)
        return;
    // The desktop's stand-in finished everything already: what is left was
    // dropped (LGP_WEBGPU_DEBUG=dropasync).
    expire_pipeline_wait();
#endif
}

void WebGpuBackend::State::expire_pipeline_wait() {
    PipelineWait& w = *pipeline_wait;
    rt::log(rt::LogLevel::Warn,
            "nvn webgpu: %zu async pipelines not done after %u ms (callback lost?): created synchronously",
            w.tags.size(), unsigned(pipeline_wait_ms));
    struct Pop {
        State* s;
        std::vector<std::string> keys;
        std::vector<nvn::ProgramId> compute;
        bool in_pop = true, fired = false, ok = true;
        void settle() {
            if (!ok) {
                rt::log(rt::LogLevel::Warn, "nvn webgpu: pipelines created after the timeout rejected");
                for (const std::string& k : keys) {
                    auto it = s->draw_pipelines.find(k);
                    if (it != s->draw_pipelines.end() && it->second.pipeline != nullptr) {
                        wgpuRenderPipelineRelease(it->second.pipeline);
                        it->second.pipeline = nullptr;
                    }
                }
                for (nvn::ProgramId id : compute) {
                    auto it = s->programs.find(id);
                    if (it != s->programs.end() && it->second->compute_pipeline != nullptr) {
                        wgpuComputePipelineRelease(it->second->compute_pipeline);
                        it->second->compute_pipeline = nullptr;
                    }
                }
                s->pipeline_wait->delta->invalid_pipelines += keys.size() + compute.size();
            }
            --s->pipeline_wait->scopes;
            s->check_pipeline_wait();
        }
    };
    auto* pop = new Pop{this};
    wgpuDevicePushErrorScope(device, WGPUErrorFilter_Validation);
    for (const std::string& tag : w.tags) {
        uint64_t serial;
        std::memcpy(&serial, tag.data(), sizeof(serial));
        const std::string_view recipe = std::string_view(tag).substr(sizeof(serial));
        auto ps = program_by_serial.find(serial);
        if (ps == program_by_serial.end())
            continue;
        Program& prog = *programs.at(ps->second);
        if (recipe.empty()) {
            if (prog.compute_pipeline != nullptr)
                continue;
            std::unique_ptr<ComputeBuild> b(compute_build_for(prog.layout, prog.cs, prog.hash, prog.wgsl_bytes, driver_cache));
            prog.compute_pipeline = wgpuDeviceCreateComputePipeline(device, &b->desc);
            log_compute_pipeline(*b, "on use");
            prog.compute_deferred = false;
            pop->compute.push_back(ps->second);
            continue;
        }
        PipelineRequest r;
        if (!parse_recipe(recipe, ps->second, &r) || draw_pipelines.count(r.key))
            continue;
        draw_pipelines[r.key] = {ps->second, create_draw_pipeline(r)};
        pop->keys.push_back(r.key);
    }
    {
        std::lock_guard lock(prebuilds->mutex);
        for (const std::string& tag : w.tags) prebuilds->pending.erase(tag);  // a late result is released
    }
    w.tags.clear();
    ++w.scopes;
    WGPUPopErrorScopeCallbackInfo cb = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
    cb.mode = WGPUCallbackMode_AllowSpontaneous;
    cb.userdata1 = pop;
    cb.callback = [](WGPUPopErrorScopeStatus status, WGPUErrorType type, WGPUStringView, void* u1, void*) {
        auto* p = static_cast<Pop*>(u1);
        p->ok = status == WGPUPopErrorScopeStatus_Success && type == WGPUErrorType_NoError;
        if (p->in_pop) {  // wgpu-native, inside PopErrorScope: no WebGPU calls here
            p->fired = true;
            return;
        }
        p->settle();
        delete p;
    };
    wgpuDevicePopErrorScope(device, cb);
    pop->in_pop = false;
    if (pop->fired) {
        pop->settle();
        delete pop;
    }
}

void WebGpuBackend::execute(const nvn::CommandList& list) {
    Stats delta;
    // Compute results of earlier lists first: this list may read them.
    auto run = [&](pw::WebGpuRenderer::Call& call) {
        State& s = *state_;

        // Pass 1: which draw pipelines does this list need that do not exist
        // yet? They are created inside an error scope first, so an invalid
        // one never reaches a submit.
        std::vector<PipelineRequest> missing;
        std::vector<nvn::ProgramId> missing_compute;  // deferred compute pipelines (pipeline cache)
        // Async pipelines: what this list waits for besides `missing` (draw
        // pipelines and compute programs whose pipelines are under way).
        std::vector<nvn::ProgramId> deferred_compute;
        std::vector<std::string> awaited_keys;
        s.harvest_prebuilds(delta);
        if (s.async_pipelines) {
            s.pipeline_wait = std::make_unique<State::PipelineWait>();
            s.pipeline_wait->serial = s.next_pipeline_wait++;
            s.pipeline_wait->delta = &delta;
        }
        {
            DrawState st = s.carried.st;
            BoundTextures bound = s.carried.bound_textures;  // as record_list tracks them
            auto vertex_buffers = s.carried.vertex_buffers;
            auto storage_bindings = s.carried.storage_bindings;
            DispatchWrites dispatch_writes;
            for (const nvn::Command& command : list.commands) {
                st.apply(command);
                if (auto* c = std::get_if<nvn::CmdBindTexture>(&command))
                    bound[{int(c->stage), c->slot}] = *c;
                else if (auto* c = std::get_if<nvn::CmdBindVertexBuffer>(&command))
                    vertex_buffers[c->stream] = *c;
                else if (auto* c = std::get_if<nvn::CmdBindStorageBuffer>(&command))
                    storage_bindings[{int(c->stage), c->index}] = *c;
                if (auto* c = std::get_if<nvn::CmdDispatchCompute>(&command)) {
                    dispatch_writes.dispatch(*c, storage_bindings);
                    auto pit = s.programs.find(st.compute_program);
                    if (pit != s.programs.end() && pit->second->valid && pit->second->compute_deferred &&
                        pit->second->compute_pipeline == nullptr &&
                        std::find(deferred_compute.begin(), deferred_compute.end(), st.compute_program) ==
                            deferred_compute.end()) {
                        deferred_compute.push_back(st.compute_program);
                        if (!s.claim_compute_prebuild(*pit->second, delta))
                            missing_compute.push_back(st.compute_program);
                    }
                }
                if (is_draw(command) && !st.rasterizer_discard) {
                    PipelineRequest r = s.pipeline_request(st, draw_topology(command), list,
                                                           strip_index_format(command), bound,
                                                           dispatch_writes.streams(vertex_buffers));
                    if (!r.key.empty() && !s.draw_pipelines.count(r.key)) {
                        bool dup = std::find(awaited_keys.begin(), awaited_keys.end(), r.key) != awaited_keys.end();
                        for (const PipelineRequest& m : missing) dup |= m.key == r.key;
                        if (!dup && !s.claim_prebuild(r, delta)) {
                            if (debug_overrides().pipelinevariants)
                                s.note_pipeline_variant(r, delta);
                            missing.push_back(std::move(r));
                        }
                        else if (!dup && !s.draw_pipelines.count(r.key))
                            awaited_keys.push_back(r.key);  // async, under way
                    }
                }
            }
        }

        // record_list finishes the call: right after the submit, or once
        // compute results are back in guest memory.
        auto record = [this, &list, &delta, &call]() {
            record_list(list, delta, [&call] { call.finish(); },
                        [&call](uint64_t submission) { call.wait_for_submission(submission); });
        };
        if (s.async_pipelines) {
            s.create_pipelines_async(std::move(missing), missing_compute, deferred_compute, std::move(awaited_keys),
                                     delta, record);
            return;
        }
        if (missing.empty() && missing_compute.empty()) {
            record();
            return;
        }
        const auto t0 = std::chrono::steady_clock::now();
        wgpuDevicePushErrorScope(s.device, WGPUErrorFilter_Validation);
        std::vector<std::string> keys;
        std::vector<std::pair<uint64_t, std::string>> records;  // for the pipeline cache, once validated
        // Descriptors here (they may create shader module variants and
        // layouts, GPU-thread state), the pipelines then side by side
        // (pipeline_threads) or one after another. Errors of the helper
        // threads land in this scope too: wgpu-native keeps one scope stack
        // per device, and this thread holds the scope open until they joined.
        std::vector<std::unique_ptr<PipelineBuild>> draw_builds;
        std::vector<WGPURenderPipeline> draw_made(missing.size(), nullptr);
        for (const PipelineRequest& r : missing) draw_builds.push_back(s.prepare_draw_pipeline(r, *s.programs.at(r.program)));
        std::vector<std::unique_ptr<ComputeBuild>> compute_builds;
        std::vector<WGPUComputePipeline> compute_made(missing_compute.size(), nullptr);
        for (nvn::ProgramId id : missing_compute) {
            Program& p = *s.programs.at(id);
            compute_builds.emplace_back(compute_build_for(p.layout, p.cs, p.hash, p.wgsl_bytes, s.driver_cache));
        }
        const size_t jobs = draw_builds.size() + compute_builds.size();
        std::atomic<size_t> next_job{0};
        auto work = [&] {
            for (size_t j; (j = next_job.fetch_add(1)) < jobs;) {
                if (j < draw_builds.size()) {
                    if (draw_builds[j] != nullptr) {
                        const auto t0 = std::chrono::steady_clock::now();
                        draw_made[j] = wgpuDeviceCreateRenderPipeline(s.device, &draw_builds[j]->desc);
                        log_draw_pipeline(*draw_builds[j], "on use", t0);
                    }
                    continue;
                }
                const size_t c = j - draw_builds.size();
                compute_made[c] = wgpuDeviceCreateComputePipeline(s.device, &compute_builds[c]->desc);
                log_compute_pipeline(*compute_builds[c], "on use");
            }
        };
        std::vector<std::thread> helpers;
#if !defined(__EMSCRIPTEN__)  // no other thread may use the browser's WebGPU objects
        for (size_t t = 1; t < std::min<size_t>(s.pipeline_threads, jobs); ++t) helpers.emplace_back(work);
#endif
        work();
        for (std::thread& t : helpers) t.join();
        for (size_t i = 0; i < missing.size(); ++i) {
            const PipelineRequest& r = missing[i];
            s.draw_pipelines[r.key] = {r.program, draw_made[i]};
            keys.push_back(r.key);
            if (draw_made[i] != nullptr && s.pipeline_cache)
                records.emplace_back(s.programs.at(r.program)->hash, r.key.substr(sizeof(nvn::ProgramId)));
        }
        delta.draw_pipelines_created += missing.size();
        for (size_t i = 0; i < missing_compute.size(); ++i) {
            Program& p = *s.programs.at(missing_compute[i]);
            p.compute_pipeline = compute_made[i];
            p.compute_deferred = false;
        }
        // wgpu-native calls a spontaneous callback inside PopErrorScope with
        // its own locks held; recording there deadlocks in
        // CommandEncoderFinish. So a callback during the call only marks the
        // result, and the recording happens after PopErrorScope returned (the
        // browser calls back later, outside, and records itself).
        struct Pop {
            State* s;
            std::vector<std::string> keys;
            std::function<void()> record;
            Stats* delta;
            std::chrono::steady_clock::time_point t0;
            std::vector<std::pair<uint64_t, std::string>> records;
            std::vector<nvn::ProgramId> compute;
            bool in_pop = true;
            bool fired = false;
        };
        auto* pop = new Pop{&s, std::move(keys), record, &delta, t0, std::move(records), std::move(missing_compute)};
        WGPUPopErrorScopeCallbackInfo cb = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
        cb.mode = WGPUCallbackMode_AllowSpontaneous;
        cb.userdata1 = pop;
        cb.callback = [](WGPUPopErrorScopeStatus status, WGPUErrorType type, WGPUStringView message, void* u1, void*) {
            auto* p = static_cast<Pop*>(u1);
            // Timed up to the callback: in the browser it fires later, once the
            // GPU process has validated (and usually compiled) the pipelines,
            // and the guest thread waits for it until record() finishes the call.
            p->delta->draw_pipeline_create_us += micros_since(p->t0);
            if (status != WGPUPopErrorScopeStatus_Success || type != WGPUErrorType_NoError) {
                // One of them is invalid; without per-pipeline scopes we drop
                // all of this batch rather than risk an invalid submit.
                rt::log(rt::LogLevel::Warn, "nvn webgpu: draw pipeline rejected: %s", pw::to_string(message).c_str());
                for (const std::string& k : p->keys) {
                    auto it = p->s->draw_pipelines.find(k);
                    if (it->second.pipeline != nullptr) wgpuRenderPipelineRelease(it->second.pipeline);
                    it->second.pipeline = nullptr;
                }
                for (nvn::ProgramId id : p->compute) {
                    auto it = p->s->programs.find(id);
                    if (it == p->s->programs.end() || it->second->compute_pipeline == nullptr)
                        continue;
                    wgpuComputePipelineRelease(it->second->compute_pipeline);
                    it->second->compute_pipeline = nullptr;  // its dispatches are skipped
                }
                p->delta->invalid_pipelines += p->keys.size() + p->compute.size();
            } else {
                for (const auto& [hash, recipe] : p->records) p->s->pipeline_cache->add(hash, recipe);
            }
            if (p->in_pop) {
                p->fired = true;
                return;
            }
            p->record();
            delete p;
        };
        wgpuDevicePopErrorScope(s.device, cb);
        pop->in_pop = false;
        if (pop->fired) {
            pop->record();
            delete pop;
        }
    };
    renderer_.run([&](pw::WebGpuRenderer::Call& call) { state_->after_writebacks([&] { run(call); }); });
    if (state_->pipeline_cache) {
        state_->pipeline_cache->flush();  // outside the GPU thread (OPFS in the browser)
        state_->save_driver_cache(false);
    }
    std::lock_guard lock(stats_mutex_);
    stats_.clears += delta.clears;
    stats_.draws += delta.draws;
    stats_.discarded_draws += delta.discarded_draws;
    stats_.skipped_commands += delta.skipped_commands;
    stats_.render_passes += delta.render_passes;
    stats_.invalid_pipelines += delta.invalid_pipelines;
    stats_.copies += delta.copies;
    stats_.texture_copies += delta.texture_copies;
    stats_.blits += delta.blits;
    stats_.uploads += delta.uploads;
    stats_.astc_cpu_decoded += delta.astc_cpu_decoded;
    stats_.astc_error_blocks += delta.astc_error_blocks;
    stats_.bc_cpu_decoded += delta.bc_cpu_decoded;
    stats_.astc_bindings += delta.astc_bindings;
    stats_.bindings_3d += delta.bindings_3d;
    stats_.unbound_textures += delta.unbound_textures;
    stats_.dispatches += delta.dispatches;
    stats_.lists += delta.lists;
    stats_.list_commands += delta.list_commands;
    stats_.dispatch_lists += delta.dispatch_lists;
    stats_.draws_before_dispatch += delta.draws_before_dispatch;
    stats_.draws_between_dispatches += delta.draws_between_dispatches;
    stats_.draws_after_dispatch += delta.draws_after_dispatch;
    stats_.dispatches_first += delta.dispatches_first;
    stats_.dispatches_in_order += delta.dispatches_in_order;
    stats_.lists_compute_first += delta.lists_compute_first;
    stats_.lists_written_back_first += delta.lists_written_back_first;
    stats_.compute_vertex_streams += delta.compute_vertex_streams;
    stats_.compute_uniform_slots += delta.compute_uniform_slots;
    stats_.cbuf_uploads += delta.cbuf_uploads;
    stats_.cbuf_upload_bytes += delta.cbuf_upload_bytes;
    stats_.cbuf_reused += delta.cbuf_reused;
    stats_.geometry_uploads += delta.geometry_uploads;
    stats_.geometry_upload_bytes += delta.geometry_upload_bytes;
    stats_.geometry_reused += delta.geometry_reused;
    stats_.geometry_cached += delta.geometry_cached;
    stats_.bind_groups_created += delta.bind_groups_created;
    stats_.bind_groups_reused += delta.bind_groups_reused;
    stats_.texture_upload_us += delta.texture_upload_us;
    stats_.texture_read_us += delta.texture_read_us;
    stats_.texture_read_bytes += delta.texture_read_bytes;
    stats_.texture_write_us += delta.texture_write_us;
    stats_.texture_write_bytes += delta.texture_write_bytes;
    stats_.texture_writes += delta.texture_writes;
    stats_.texture_checks += delta.texture_checks;
    stats_.texture_convert_us += delta.texture_convert_us;
    stats_.texture_linear_us += delta.texture_linear_us;
    stats_.texture_linear_bytes += delta.texture_linear_bytes;
    stats_.texture_linear_checks += delta.texture_linear_checks;
    stats_.draw_pipelines_created += delta.draw_pipelines_created;
    stats_.draw_pipeline_create_us += delta.draw_pipeline_create_us;
    stats_.pipelines_prebuilt += delta.pipelines_prebuilt;
    stats_.compute_pipelines_prebuilt += delta.compute_pipelines_prebuilt;
    stats_.prebuilds_failed += delta.prebuilds_failed;
    stats_.prebuild_us += delta.prebuild_us;
    stats_.prebuilds_late += delta.prebuilds_late;
    stats_.draws_skipped_compiling += delta.draws_skipped_compiling;
    stats_.compute_pipeline_waits += delta.compute_pipeline_waits;
    stats_.compute_pipeline_wait_us += delta.compute_pipeline_wait_us;
    stats_.prebuilds_waited += delta.prebuilds_waited;
    if (delta.texture_upload_us >= kSlowCreateUs)
        rt::log(rt::LogLevel::Info, "nvn webgpu: textures from pools took %.1f ms (%llu uploads, %llu ASTC levels "
                "decoded on the CPU; %llu checks: read and fingerprint %.1f ms for %.1f MiB, deswizzle and "
                "convert %.1f ms, %llu writeTexture %.1f ms for %.1f MiB; %llu pitch-linear %.1f ms for %.1f MiB; "
                "after present %llu)", delta.texture_upload_us / 1000.0,
                (unsigned long long)delta.uploads, (unsigned long long)delta.astc_cpu_decoded,
                (unsigned long long)delta.texture_checks, delta.texture_read_us / 1000.0,
                delta.texture_read_bytes / 1048576.0, delta.texture_convert_us / 1000.0,
                (unsigned long long)delta.texture_writes, delta.texture_write_us / 1000.0,
                delta.texture_write_bytes / 1048576.0, (unsigned long long)delta.texture_linear_checks,
                delta.texture_linear_us / 1000.0, delta.texture_linear_bytes / 1048576.0,
                (unsigned long long)stats_.presents);
    if (delta.draw_pipeline_create_us >= kSlowCreateUs)
        rt::log(rt::LogLevel::Info, "nvn webgpu: %llu draw pipelines created in %.1f ms (after present %llu)",
                (unsigned long long)delta.draw_pipelines_created, delta.draw_pipeline_create_us / 1000.0,
                (unsigned long long)stats_.presents);
    if (delta.compute_pipeline_wait_us >= kSlowCreateUs)
        rt::log(rt::LogLevel::Info, "nvn webgpu: waited %.1f ms for compute pipelines (after present %llu)",
                delta.compute_pipeline_wait_us / 1000.0, (unsigned long long)stats_.presents);
    for (const auto& [reason, n] : delta.skip_reasons)
        stats_.skip_reasons[reason] += n;
    for (const auto& [name, n] : delta.diagnostics)
        stats_.diagnostics[name] += n;
    for (const auto& [why, n] : delta.pass_reasons)
        stats_.pass_reasons[why] += n;
    stats_.draw_passes += delta.draw_passes;
    stats_.attachment_sets_new += delta.attachment_sets_new;
}

void WebGpuBackend::record_list(const nvn::CommandList& list, Stats& delta, std::function<void()> done,
                                std::function<void(uint64_t)> wait_for) {
    State& s = *state_;
    const uint64_t presents_seen = [this] {
        std::lock_guard lock(stats_mutex_);
        return stats_.presents;
    }();
    size_t clears = 0;
    // Bytes per texel a buffer-to-texture copy into `id` uploads, 0 = not
    // supported. Compressed data (BC, ASTC; also ASTC decoded on the CPU,
    // whose host texture is RGBA8), widened 16-bit formats and textures whose
    // host format was substituted (R11G11B10F as RGBA16Float) are not copied
    // from buffers yet; every other format has the guest's bytes per texel
    // (as for pool uploads). find_texture() marks the target used and
    // materializes a lazy one here already; recording the copy would do both
    // anyway.
    auto copy_bpp = [&](nvn::TextureId id) -> uint32_t {
        auto it = s.find_texture(id);
        if (it == s.textures.end() || it->second.cpu_decoded() || is_widened_16bit(it->second.desc.format) ||
            it->second.format != texture_format(it->second.desc.format))
            return 0;
        const auto info = nvn::format_info(it->second.desc.format);
        return info && info->block_width == 1 && info->block_height == 1 ? info->bytes_per_pixel : 0;
    };
    uint64_t copy_bytes = 0;
    for (const nvn::Command& c : list.commands) {
        clears += std::holds_alternative<nvn::CmdClearColor>(c) || std::holds_alternative<nvn::CmdClearTexture>(c) ||
                  std::holds_alternative<nvn::CmdClearDepthStencil>(c);
        // The copy arena holds the rows at their real size (256-byte pitch);
        // a texture unknown now is sized at 16 bytes per texel.
        if (auto* cp = std::get_if<nvn::CmdCopyBufferToTexture>(&c))
            if (cp->width > 0 && cp->height > 0 && cp->width <= 16384 && cp->height <= 16384 && cp->depth > 0 &&
                cp->depth <= 2048) {
                const uint32_t bpp = s.find_texture(cp->texture) == s.textures.end() ? 16 : copy_bpp(cp->texture);
                if (bpp != 0)
                    copy_bytes += ((uint64_t(cp->width) * bpp + 255) & ~uint64_t{255}) * uint64_t(cp->height) *
                                  uint64_t(cp->depth);
            }
    }
    s.ensure_uniforms(clears);
    std::vector<ClearUniform> uniforms;
    uniforms.reserve(clears);
    // Per-list arena for texture copies (released after the submit; the GPU
    // keeps it alive). Constant buffers, vertices and indices go to the
    // State's arenas.
    auto make_arena = [&](uint64_t size, WGPUBufferUsage usage, const char* label) -> WGPUBuffer {
        if (size == 0) return nullptr;
        WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
        bd.label = pw::string_view(label);
        bd.size = size;
        bd.usage = usage | WGPUBufferUsage_CopyDst;
        return s.list_buffer(&bd);
    };
    WGPUBuffer copy_arena = make_arena(copy_bytes, WGPUBufferUsage_CopySrc, "nvn copies");
    uint64_t copy_used = 0;
    // Draws before, between and after the list's dispatches that write
    // guest memory back (a Writeback::Entry): whether its compute part could
    // be submitted (and written back) ahead of them. Counted as recorded, so
    // skipped dispatches and ones without a storage binding do not count.
    uint64_t draws_recorded = 0, writing_dispatches = 0, draws_at_first = 0, draws_at_last = 0;
    bool dispatch_binds_storage = false;  // the dispatch being recorded bound a storage buffer
    if (const uint64_t warnings = platform::low_memory_warnings(); warnings != s.low_memory_seen)
        s.drop_for_low_memory(warnings);  // before this list binds any entry
    s.cbuf_arena.begin_list();
    s.geometry_arena.begin_list();
    std::vector<uint8_t> scratch;
    // LGP_WEBGPU_DEBUG=computeoverlap (State::WrittenBack): guest ranges the
    // dispatches of this list bind writable, and reads of guest memory
    // checked against them and against what earlier lists wrote back.
    const bool diag = debug_overrides().computeoverlap;
    const uint64_t list_no = ++s.lists_recorded;
    std::vector<std::pair<uint64_t, uint64_t>> dispatched;
    if (diag) {
        std::lock_guard lock(s.diag_mutex);
        // Did the CPU (or a buffer copy) change a written-back range since?
        for (size_t i = 0; i < s.written_back.size();) {
            State::WrittenBack& w = s.written_back[i];
            bool drop = false;
            if (w.done) {
                scratch.assign(size_t(w.size), 0);
                if (memory_ != nullptr && memory_->read(w.address, scratch.data(), w.size) &&
                    fingerprint(scratch.data(), scratch.size()) != w.fp) {
                    ++delta.diagnostics["writeback: guest memory changed the compute output before a later list"];
                    drop = true;
                } else if (++w.age > 8) {
                    ++delta.diagnostics["writeback: compute output unchanged for 8 lists"];
                    drop = true;
                }
            }
            if (drop) {
                w = s.written_back.back();
                s.written_back.pop_back();
            } else {
                ++i;
            }
        }
    }
    // A read of guest memory [va, va+n) for `what` (constant buffer, vertex
    // buffer, texture ...): same list = the dispatch's result is not in
    // guest memory yet, so the reader sees the old bytes.
    auto check_read = [&](uint64_t va, uint64_t n, const char* what) {
        if (!diag || n == 0)
            return;
        const auto overlaps = [&](uint64_t a, uint64_t size) { return va < a + size && a < va + n; };
        for (const auto& [a, size] : dispatched)
            if (overlaps(a, size)) {
                ++delta.diagnostics[std::string("same list: ") + what + " reads a range an earlier dispatch writes (old bytes)"];
                if (s.diag_logged < 32) {
                    ++s.diag_logged;
                    rt::log(rt::LogLevel::Warn,
                            "nvn webgpu: computeoverlap: list %llu: %s 0x%llx+0x%llx overlaps dispatch output "
                            "0x%llx+0x%llx",
                            (unsigned long long)list_no, what, (unsigned long long)va, (unsigned long long)n,
                            (unsigned long long)a, (unsigned long long)size);
                }
                break;
            }
        std::lock_guard lock(s.diag_mutex);
        for (const State::WrittenBack& w : s.written_back)
            if (overlaps(w.address, w.size)) {
                ++delta.diagnostics[std::string(w.done ? "later list: " : "later list, writeback not arrived: ") +
                                    what + " reads compute output"];
                break;
            }
    };
    auto check_texture = [&](const State::Texture& t, const char* what) {
        if (!diag || t.gpu_written || t.desc.storage == 0)
            return;
        const uint64_t size = t.desc.linear_stride != 0
                                  ? uint64_t(t.desc.linear_stride) * t.desc.height
                                  : nvn::texture_storage_size(t.desc.format, t.desc.target, t.desc.width,
                                                              t.desc.height, t.desc.depth, t.desc.levels);
        check_read(t.desc.storage, size, what);
    };
    std::vector<WGPUBindGroup> bind_groups;
    std::vector<WGPUBuffer> cbuf_buffers;
    std::vector<uint32_t> cbuf_offsets;
    // What the open draw pass has set, so draws skip calls that change nothing
    // (each call crosses into JavaScript in the browser).
    struct PassState {
        WGPURenderPipeline pipeline = nullptr;
        WGPUBindGroup groups[4] = {nullptr, nullptr, nullptr, nullptr};
        std::vector<uint32_t> offsets[4];
        struct Binding {
            WGPUBuffer buffer = nullptr;
            uint64_t offset = 0, size = 0;
            bool operator==(const Binding& o) const {
                return buffer == o.buffer && offset == o.offset && size == o.size;
            }
        };
        Binding vertex[kMaxVertexBuffers];
        Binding index;
        WGPUIndexFormat index_format = WGPUIndexFormat_Undefined;
        bool viewport_set = false, scissor_set = false, blend_set = false, stencil_set = false;
        float viewport[6] = {};
        uint32_t scissor[4] = {};
        WGPUColor blend = {};
        uint32_t stencil = 0;

        void pipeline_(WGPURenderPassEncoder p, WGPURenderPipeline v) {
            if (v != pipeline) wgpuRenderPassEncoderSetPipeline(p, v);
            pipeline = v;
        }
        void bind_group(WGPURenderPassEncoder p, uint32_t i, WGPUBindGroup g, const std::vector<uint32_t>& off) {
            if (groups[i] == g && offsets[i] == off) return;
            wgpuRenderPassEncoderSetBindGroup(p, i, g, off.size(), off.empty() ? nullptr : off.data());
            groups[i] = g;
            offsets[i] = off;
        }
        void vertex_buffer(WGPURenderPassEncoder p, uint32_t slot, WGPUBuffer b, uint64_t offset, uint64_t size) {
            const Binding v{b, offset, size};
            if (slot < kMaxVertexBuffers && vertex[slot] == v) return;
            wgpuRenderPassEncoderSetVertexBuffer(p, slot, b, offset, size);
            if (slot < kMaxVertexBuffers) vertex[slot] = v;
        }
        void index_buffer(WGPURenderPassEncoder p, WGPUBuffer b, WGPUIndexFormat f, uint64_t offset, uint64_t size) {
            const Binding v{b, offset, size};
            if (index == v && index_format == f) return;
            wgpuRenderPassEncoderSetIndexBuffer(p, b, f, offset, size);
            index = v;
            index_format = f;
        }
        void viewport_(WGPURenderPassEncoder p, const float v[6]) {
            if (viewport_set && std::memcmp(v, viewport, sizeof(viewport)) == 0) return;
            wgpuRenderPassEncoderSetViewport(p, v[0], v[1], v[2], v[3], v[4], v[5]);
            std::memcpy(viewport, v, sizeof(viewport));
            viewport_set = true;
        }
        void scissor_(WGPURenderPassEncoder p, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
            const uint32_t v[4] = {x, y, w, h};
            if (scissor_set && std::memcmp(v, scissor, sizeof(scissor)) == 0) return;
            wgpuRenderPassEncoderSetScissorRect(p, x, y, w, h);
            std::memcpy(scissor, v, sizeof(scissor));
            scissor_set = true;
        }
        void blend_(WGPURenderPassEncoder p, const WGPUColor& c) {
            if (blend_set && c.r == blend.r && c.g == blend.g && c.b == blend.b && c.a == blend.a) return;
            wgpuRenderPassEncoderSetBlendConstant(p, &c);
            blend = c;
            blend_set = true;
        }
        void stencil_(WGPURenderPassEncoder p, uint32_t ref) {
            if (stencil_set && ref == stencil) return;
            wgpuRenderPassEncoderSetStencilReference(p, ref);
            stencil = ref;
            stencil_set = true;
        }
    } pass_state;
    // Arena buffer, offset and bound size of each vertex stream of the current
    // draw (prepare_draw).
    struct StreamRange {
        WGPUBuffer buffer = nullptr;
        uint64_t offset = 0, size = 0;
    };
    std::vector<StreamRange> stream_ranges;
    std::vector<int> vertex_slot_of;  // WebGPU slot of each stream (vertex_slots), set with stream_ranges

    const DebugOverrides& dbg = debug_overrides();
    if (dbg.errorscope) {
        wgpuDevicePushErrorScope(s.device, WGPUErrorFilter_OutOfMemory);
#if defined(__EMSCRIPTEN__)  // wgpu-native has no internal filter (panics)
        wgpuDevicePushErrorScope(s.device, WGPUErrorFilter_Internal);
#endif
        wgpuDevicePushErrorScope(s.device, WGPUErrorFilter_Validation);
    }
    // Compute first: the compute encoder is created before `encoder`. Created
    // after it, Safari on the iPad lost its GPU connection at the first
    // dispatches (InvalidStateError from createBindGroup/finish, read-backs
    // empty); created before, it ran stable (Analyse 20, iPad runs a/b).
    WGPUCommandEncoder compute_encoder = nullptr;
    bool compute_used = false;  // a dispatch went to `compute_encoder`
    if (s.compute_first &&
        std::any_of(list.commands.begin(), list.commands.end(),
                    [](const nvn::Command& c) { return std::holds_alternative<nvn::CmdDispatchCompute>(c); }))
        compute_encoder = wgpuDeviceCreateCommandEncoder(s.device, nullptr);
    WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(s.device, nullptr);
    // LGP_WEBGPU_DEBUG=gputime: a query pair around each pass, resolved and
    // read back after the list's submits (in submit_main).
    constexpr uint32_t kTimedPasses = 256;
    struct PassTimer {
        WGPUQuerySet set = nullptr;
        std::vector<uint64_t> tags;  // per timed pass: compute program hash, kRenderPass for render passes
        std::vector<std::string> targets;  // per timed pass: render passes "draw 1280x720 c12 d5", else empty
        std::vector<std::map<uint64_t, uint32_t>> draws;  // per timed pass: draws by program hash
        uint64_t untimed = 0;
        ~PassTimer() {
            if (set != nullptr) wgpuQuerySetRelease(set);
        }
    } timer;
    static constexpr uint64_t kRenderPass = ~uint64_t{0};
    // Fills `tw` and returns it for the pass descriptor, or nullptr.
    // `target` names a render pass's target (pass_text) for Stats::gpu_time_targets.
    auto time_pass = [&](uint64_t tag, WGPUPassTimestampWrites* tw,
                         const char* target = nullptr) -> const WGPUPassTimestampWrites* {
        if (!s.timestamps) return nullptr;
        if (timer.tags.size() == kTimedPasses) {
            ++timer.untimed;
            return nullptr;
        }
        if (timer.set == nullptr) {
            WGPUQuerySetDescriptor qd = WGPU_QUERY_SET_DESCRIPTOR_INIT;
            qd.label = pw::string_view("nvn gputime");
            qd.type = WGPUQueryType_Timestamp;
            qd.count = 2 * kTimedPasses;
            timer.set = wgpuDeviceCreateQuerySet(s.device, &qd);
        }
        *tw = WGPU_PASS_TIMESTAMP_WRITES_INIT;
        tw->querySet = timer.set;
        tw->beginningOfPassWriteIndex = uint32_t(2 * timer.tags.size());
        tw->endOfPassWriteIndex = uint32_t(2 * timer.tags.size() + 1);
        timer.tags.push_back(tag);
        timer.targets.emplace_back(target != nullptr ? target : "");
        timer.draws.emplace_back();
        return tw;
    };
    // A pass's name from its target, e.g. "draw 1280x720 c12 d5 present".
    // Valid until the next call.
    char pass_label_text[96];
    auto pass_text = [&](const char* what, const State::Texture* t, nvn::TextureId color,
                         nvn::TextureId depth) -> const char* {
        int n = std::snprintf(pass_label_text, sizeof(pass_label_text), "%s %ux%u", what,
                              t != nullptr ? uint32_t(t->desc.width) : 0, t != nullptr ? uint32_t(t->desc.height) : 0);
        if (color != 0) n += std::snprintf(pass_label_text + n, sizeof(pass_label_text) - n, " c%u", color);
        if (depth != 0) n += std::snprintf(pass_label_text + n, sizeof(pass_label_text) - n, " d%u", depth);
        if (s.was_presented(color)) std::snprintf(pass_label_text + n, sizeof(pass_label_text) - n, " present");
        return pass_label_text;
    };
    // LGP_WEBGPU_DEBUG=passlabels: the pass's label is pass_text; empty otherwise.
    const bool pass_labels = debug_overrides().passlabels;
    auto pass_label = [&](const char* what, const State::Texture* t, nvn::TextureId color,
                          nvn::TextureId depth) -> WGPUStringView {
        if (!pass_labels) return WGPUStringView{nullptr, WGPU_STRLEN};
        return pw::string_view(pass_text(what, t, color, depth));
    };
    // gputime: the target for time_pass, nullptr without timestamps.
    auto pass_target = [&](const char* what, const State::Texture* t, nvn::TextureId color,
                           nvn::TextureId depth) -> const char* {
        return s.timestamps ? pass_text(what, t, color, depth) : nullptr;
    };
    // State from the previous lists (State::Carried), saved back at the end.
    DrawState st = s.carried.st;
    // State::compute_first: dispatches recorded into their own encoder,
    // submitted before `encoder`. They may not read what earlier commands of
    // the list write on the GPU: textures this list renders, clears or
    // copies into so far (the targets carried in count as rendered).
    bool dispatch_in_order = false;  // a dispatch went to `encoder`
    std::set<nvn::TextureId> written_textures(st.colors.begin(), st.colors.end());
    written_textures.insert(st.depth_stencil);
    bool scissor_set = s.carried.scissor_set;
    nvn::Rect scissor = s.carried.scissor;
    bool viewport_set = s.carried.viewport_set;
    float viewport[4];
    std::memcpy(viewport, s.carried.viewport, sizeof(viewport));
    float depth_range[2] = {s.carried.depth_range[0], s.carried.depth_range[1]};
    std::map<uint32_t, nvn::CmdBindVertexBuffer> vertex_buffers = s.carried.vertex_buffers;
    std::map<std::pair<int, uint32_t>, nvn::CmdBindUniformBuffer> uniform_buffers = s.carried.uniform_buffers;
    std::map<std::pair<int, uint32_t>, nvn::CmdBindTexture> bound_textures = s.carried.bound_textures;
    WGPUColor blend_color = s.carried.blend_color;
    uint32_t stencil_ref = s.carried.stencil_ref;
    WGPURenderPassEncoder draw_pass = nullptr;
    // gputime: the open draw pass's index in timer.draws, -1 if untimed.
    int draw_pass_timed = -1;
    // The open pass has its depth/stencil attachment read-only (the bound
    // shaders sample that texture; prepare_draw).
    bool draw_pass_depth_read_only = false;
    // The targets the open pass was begun with (pass causes in Stats).
    std::vector<nvn::TextureId> draw_pass_colors;
    nvn::TextureId draw_pass_depth = 0;
    auto end_draw_pass = [&](const char* why) {
        if (draw_pass != nullptr) {
            wgpuRenderPassEncoderEnd(draw_pass);
            wgpuRenderPassEncoderRelease(draw_pass);
            draw_pass = nullptr;
            ++delta.pass_reasons[std::string("draw pass ended: ") + why];
        }
        draw_pass_depth_read_only = false;
        draw_pass_timed = -1;
    };
    // The list's draw passes and what lies between them, to count how often
    // a pass goes back to a target set an earlier pass of the list used, and
    // whether its draws could have joined that pass (Stats::pass_reasons,
    // "target set: ..."). Diagnosis only: nothing is reordered.
    struct PassRecord {
        std::vector<nvn::TextureId> targets;  // attachments, sorted; empty = an event
        std::set<nvn::TextureId> sampled;     // by the pass's draws; for an event: textures it touches
        bool barrier = false;                 // an event that orders everything (in-order dispatch)
        bool cleared = false;                 // the pass began with a folded clear
        bool depth_read_only = false;         // its depth attachment is read-only (sampled)
        // Whole textures written without reading them: a full clear (event)
        // or an attachment loaded with a folded clear (pass).
        std::set<nvn::TextureId> overwritten;
        std::vector<nvn::TextureId> stored_colors;  // colour attachments the pass stores
        nvn::TextureId stored_depth = 0;  // the pass stores this depth attachment (not read-only, no stand-in)
    };
    // Only with set_pass_diagnosis or LGP_WEBGPU_DEBUG=passdiag: without,
    // nothing is recorded and the comparisons at the end of the list do nothing.
    const bool diagnose = pass_diagnosis_.load(std::memory_order_relaxed) || debug_overrides().passdiag;
    std::vector<PassRecord> pass_records;
    auto note_event = [&](std::initializer_list<nvn::TextureId> touched, bool barrier,
                          std::initializer_list<nvn::TextureId> overwritten = {}) {
        if (!diagnose)
            return;
        PassRecord e;
        for (nvn::TextureId id : touched)
            if (id != 0)
                e.sampled.insert(id);
        for (nvn::TextureId id : overwritten)
            if (id != 0)
                e.overwritten.insert(id), e.sampled.insert(id);
        e.barrier = barrier;
        pass_records.push_back(std::move(e));
    };

    // Clear region of a target in framebuffer space.
    // Host texels of a scaled target (State::render_scale); `full` as in the guest.
    auto region_of = [&](const State::Texture& t) {
        const int64_t w = t.width(), h = t.height();
        Region r;
        r.width = uint32_t(w);
        r.height = uint32_t(h);
        if (!scissor_set || debug_overrides().noscissor)
            return r;
        const int64_t x0 = std::clamp<int64_t>(t.host_x(scissor.x), 0, w);
        const int64_t x1 = std::clamp<int64_t>(t.host_x(int64_t{scissor.x} + scissor.width), 0, w);
        int64_t y0 = std::clamp<int64_t>(t.host_y(scissor.y), 0, h);
        int64_t y1 = std::clamp<int64_t>(t.host_y(int64_t{scissor.y} + scissor.height), 0, h);
        r.x = uint32_t(x0);
        r.y = uint32_t(y0);
        r.width = uint32_t(std::max<int64_t>(x1 - x0, 0));
        r.height = uint32_t(std::max<int64_t>(y1 - y0, 0));
        r.full = r.x == 0 && r.y == 0 && r.width == uint32_t(w) && r.height == uint32_t(h);
        return r;
    };
    auto push_uniform = [&](const float rgba[4], float depth) {
        ClearUniform u{};
        std::memcpy(u.color, rgba, sizeof(u.color));
        u.depth = depth;
        uniforms.push_back(u);
        return uint32_t((uniforms.size() - 1) * kUniformStride);
    };
    // Guest bytes -> arena (kind: 0 = constant buffer, 1 = vertex data);
    // zero-filled where the guest has nothing mapped. Only the guest's bytes
    // are written (up to the next 256 bytes), and a range already uploaded
    // for this list is reused: all uploads read guest memory now, so the
    // same range has the same bytes (CmdCopyBuffer forgets them). A constant
    // buffer binding still covers the whole bank (64 KiB, the WGSL array):
    // past the guest's size it sees later uploads instead of zeros
    // (LGP_WEBGPU_DEBUG=cbufzero writes the zeros; docs/knowledge/
    // webgpu-backend-draw-kosten.md).
    // Geometry from the cache across lists (State::geometry_cache): the GPU
    // buffer holding guest range `key` (n guest bytes), written again only
    // if the bytes changed; `fill` turns the guest bytes into the buffer's
    // `slot` bytes. Unset (buffer nullptr): the caller uses the arena.
    auto cached_geometry = [&](const std::tuple<uint64_t, uint64_t, uint32_t>& key, uint64_t va, uint64_t n,
                               uint64_t slot, const std::function<void(const std::vector<uint8_t>&)>& fill)
        -> std::pair<WGPUBuffer, uint64_t> {
        if (!s.geometry_cache_on || memory_ == nullptr || n == 0)
            return {nullptr, 0};
        // Fingerprint mode: hash in place where the view allows it, else
        // read once (the bytes are needed anyway if they changed).
        const bool fp_mode = s.geometry_fingerprint;
        const uint8_t* direct = fp_mode ? memory_->view(va, n) : nullptr;
        bool have_bytes = false;
        uint64_t fp = 0;
        if (fp_mode) {
            if (direct == nullptr) {
                s.geometry_bytes.resize(size_t(n));
                if (!memory_->read(va, s.geometry_bytes.data(), n))
                    return {nullptr, 0};
                have_bytes = true;
            }
            fp = fingerprint(direct != nullptr ? direct : s.geometry_bytes.data(), size_t(n));
        }
        auto it = s.geometry_cache.find(key);
        if (it != s.geometry_cache.end()) {
            State::GeometryEntry& e = it->second;
            if (fp_mode ? e.guest.empty() && e.fp == fp : memory_->equal(va, e.guest.data(), n)) {
                e.list = list_no;
                ++delta.geometry_cached;
                return {e.buffer, 0};
            }
            if (e.list == list_no)
                return {nullptr, 0};  // earlier draws of this list read the old bytes
        } else {
            // A new entry stays within the limit: older lists' entries make
            // room, and once this list's own fill the cache the rest of it
            // uses the arena.
            const uint64_t need = slot + (fp_mode ? 0 : n);
            if (s.geometry_cache_bytes + need > s.geometry_cache_limit) {
                if (s.geometry_cache_full_list != list_no)
                    s.trim_geometry_cache(list_no);
                if (s.geometry_cache_bytes + need > s.geometry_cache_limit) {
                    s.geometry_cache_full_list = list_no;
                    return {nullptr, 0};
                }
            }
            it = s.geometry_cache.emplace(key, State::GeometryEntry{}).first;
        }
        State::GeometryEntry& e = it->second;
        s.geometry_cache_bytes -= e.guest.size();
        if (fp_mode) {
            std::vector<uint8_t>().swap(e.guest);
            e.fp = fp;
            if (!have_bytes) {
                s.geometry_bytes.assign(direct, direct + n);
                have_bytes = true;
            }
        } else {
            e.guest.resize(size_t(n));
            s.geometry_cache_bytes += e.guest.size();
        }
        if (!fp_mode && !memory_->read(va, e.guest.data(), n)) {
            s.geometry_cache_bytes -= e.slot + e.guest.size();
            gpu_destroy_buffer(e.buffer);
            s.geometry_cache.erase(it);
            return {nullptr, 0};
        }
        if (e.buffer == nullptr) {
            WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
            bd.label = pw::string_view("nvn cached geometry");
            bd.size = slot;
            bd.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_Index | WGPUBufferUsage_CopyDst;
            e.buffer = gpu_create_buffer(s.device, &bd);
            e.slot = slot;
            s.geometry_cache_bytes += slot;
        }
        fill(fp_mode ? s.geometry_bytes : e.guest);  // into scratch, slot bytes
        queue_write_buffer(s.queue, e.buffer, 0, scratch.data(), size_t(slot));
        e.list = list_no;
        ++delta.geometry_uploads;
        delta.geometry_upload_bytes += slot;
        return {e.buffer, 0};
    };
    auto upload = [&](State::UploadArena& arena, uint64_t va, uint64_t n, uint32_t kind) {
        check_read(va, n, kind == 0 ? "constant buffer" : "vertex buffer");
        const auto key = std::make_tuple(va, n, kind);
        if (auto it = arena.uploaded.find(key); it != arena.uploaded.end()) {
            ++(kind == 0 ? delta.cbuf_reused : delta.geometry_reused);
            return it->second;
        }
        if (kind == 1) {
            const uint64_t slot = (std::max<uint64_t>(n, 4) + 255) & ~uint64_t{255};
            const auto where = cached_geometry(key, va, n, slot, [&](const std::vector<uint8_t>& guest) {
                scratch.assign(size_t(slot), 0);
                std::memcpy(scratch.data(), guest.data(), guest.size());
            });
            if (where.first != nullptr) {
                arena.uploaded.emplace(key, where);
                return where;
            }
        }
        const bool zero_window = kind == 0 && debug_overrides().cbufzero;
        scratch.assign(size_t(zero_window ? kCbufBytes : (std::max<uint64_t>(n, 4) + 255) & ~uint64_t{255}), 0);
        if (n > 0 && (memory_ == nullptr || !memory_->read(va, scratch.data(), n)))
            rt::log(rt::LogLevel::Warn, "nvn webgpu: buffer 0x%llx+0x%llx not readable", (unsigned long long)va,
                    (unsigned long long)n);
        const auto where = arena.put(s.device, scratch.data(), scratch.size());
        if (kind == 0) {
            ++delta.cbuf_uploads;
            delta.cbuf_upload_bytes += scratch.size();
        } else {
            ++delta.geometry_uploads;
            delta.geometry_upload_bytes += scratch.size();
        }
        arena.uploaded.emplace(key, where);
        return where;
    };

    // Storage buffers bound per stage and index (compute, and global memory
    // loads of graphics stages), and the GPU buffers behind guest ranges for
    // this list (see the header).
    std::map<std::pair<int, uint32_t>, nvn::CmdBindStorageBuffer> storage_bindings = s.carried.storage_bindings;
    DispatchWrites dispatch_writes;  // as the pipeline pre-pass in execute() tracks it
    struct StorageBuffer {
        uint64_t address = 0, size = 0;
        WGPUBuffer buffer = nullptr;
        bool written = false;  // bound to a dispatch: read back after the list
        uint64_t write_order = 0;  // when it was last bound to a dispatch (later wins on overlaps)
        // State::compute_first: used by a command in `encoder` (a dispatch
        // after it cannot go first), and written there (its read-back
        // cannot go first either).
        bool main_use = false, main_write = false;
        // A later copy in the list changed part of the range in a way the GPU
        // buffer could not follow: draws read guest memory again.
        bool superseded = false;
        uint64_t initial_fp = 0;  // computeoverlap: fingerprint of the guest bytes uploaded
        // Asynchronous write-back: the guest bytes uploaded, to keep what
        // the guest writes into the range before the results arrive.
        std::vector<uint8_t> uploaded;
    };
    std::vector<StorageBuffer> storage;
    std::map<std::pair<uint64_t, uint64_t>, size_t> storage_index;
    uint64_t storage_writes = 0;  // StorageBuffer::write_order
    // Written storage buffers overlapping guest range [lo, hi), oldest write
    // first: copied in this order, the latest dispatch's bytes win.
    auto overlapping_outputs = [&](uint64_t lo, uint64_t hi, const StorageBuffer* except) {
        std::vector<StorageBuffer*> parts;
        for (StorageBuffer& b : storage)
            if (&b != except && b.written && !b.superseded && lo < b.address + b.size && b.address < hi)
                parts.push_back(&b);
        std::sort(parts.begin(), parts.end(),
                  [](const StorageBuffer* a, const StorageBuffer* b) { return a->write_order < b->write_order; });
        return parts;
    };
    std::vector<WGPUBuffer> temp_buffers;  // slot tables, blit uniforms
    // LGP_WEBGPU_DEBUG=computeprobe: per probed dispatch its counter readback
    // and what was bound (logged with the counters after the list ran).
    struct Probe {
        WGPUBuffer readback = nullptr;
        nvn::ProgramId program = 0;
        std::string bound;
    };
    std::vector<Probe> probes;
    constexpr uint64_t kMaxStorageBytes = 128ull << 20;  // maxStorageBufferBindingSize default
    auto storage_buffer = [&](uint64_t address, uint64_t size) -> StorageBuffer& {
        auto [it, inserted] = storage_index.emplace(std::make_pair(address, size), storage.size());
        if (!inserted)
            return storage[it->second];
        check_read(address, size, "storage buffer (other range)");
        StorageBuffer b;
        b.address = address;
        b.size = size;
        const uint64_t gpu_size = (std::max<uint64_t>(size, 4) + 3) & ~uint64_t{3};
        WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
        bd.label = pw::string_view("nvn storage buffer");
        bd.size = gpu_size;
        // Vertex: draws later in the list read a dispatch's output from here.
        bd.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_Vertex | WGPUBufferUsage_CopySrc |
                   WGPUBufferUsage_CopyDst;
        b.buffer = s.list_buffer(&bd);
        scratch.assign(size_t(gpu_size), 0);
        if (memory_ == nullptr || !memory_->read(address, scratch.data(), size))
            rt::log(rt::LogLevel::Warn, "nvn webgpu: storage buffer 0x%llx+0x%llx not readable",
                    (unsigned long long)address, (unsigned long long)size);
        queue_write_buffer(s.queue, b.buffer, 0, scratch.data(), scratch.size());
        if (diag)
            b.initial_fp = fingerprint(scratch.data(), size_t(size));
        if (s.async_writeback)
            b.uploaded.assign(scratch.begin(), scratch.begin() + ptrdiff_t(size));
        storage.push_back(std::move(b));
        return storage.back();
    };
    // State::patch_dispatch_output: a storage buffer about to be bound to a
    // dispatch gets the overlapping parts of output written after its own
    // last dispatch copied over in `enc` (a new one: all of it, its guest
    // bytes are from before the list; an earlier one: what another range's
    // dispatch wrote since). Ranges that are not 4-byte aligned keep the old
    // bytes.
    auto copy_dispatch_output = [&](StorageBuffer& to, WGPUCommandEncoder enc) {
        for (StorageBuffer* from_part : overlapping_outputs(to.address, to.address + to.size, &to)) {
            StorageBuffer& from = *from_part;
            if (from.write_order <= to.write_order)
                continue;  // older than what `to` holds
            const uint64_t lo = std::max(to.address, from.address);
            const uint64_t hi = std::min(to.address + to.size, from.address + from.size);
            if ((lo - from.address) % 4 != 0 || (lo - to.address) % 4 != 0 || (hi - lo) % 4 != 0) {
                ++delta.diagnostics["dispatch output patch: overlap not 4-byte aligned (old bytes)"];
                continue;
            }
            wgpuCommandEncoderCopyBufferToBuffer(enc, from.buffer, lo - from.address, to.buffer, lo - to.address,
                                                 hi - lo);
            if (enc == encoder)
                from.main_use = true;  // a later dispatch writing it cannot go first
            ++delta.diagnostics["dispatch output patch: storage range patched from dispatch output"];
        }
    };
    // The storage buffer of a dispatch earlier in this list that holds guest
    // range [va, va+n) whole, at a 4-byte aligned offset; nullptr if none.
    // Its result reaches guest memory only after the list, so a later read
    // of the range in this list must come from the GPU buffer.
    auto dispatch_output = [&](uint64_t va, uint64_t n) -> const StorageBuffer* {
        for (StorageBuffer& b : storage)
            if (b.written && !b.superseded && va >= b.address && va + n <= b.address + b.size &&
                (va - b.address) % 4 == 0) {
                b.main_use = true;  // draws read it
                return &b;
            }
        return nullptr;
    };
    // Bind group with the textures and samplers of one stage of a program.
    // unfilterable: bit k = binding k takes its texture as unfilterable-float
    // (PipelineRequest::unfilterable) with a non-filtering sampler.
    // excluded: a texture that must not be sampled (the depth attachment the
    // draw writes); its bindings get the stand-in.
    // Sampled ASTC/3D bindings of the draw being prepared: counted into delta
    // only once the draw is issued (prepare_draw or instanced_runs can still reject it).
    uint64_t pending_astc = 0, pending_3d = 0;
    auto texture_group = [&](const Program& prog, int stage, WGPUBindGroupLayout layout, uint32_t unfilterable = 0,
                             nvn::TextureId excluded = 0) {
        std::vector<WGPUBindGroupEntry> entries;
        const std::vector<uint32_t>& slots = prog.texture_slots[stage];
        // Swizzle words, then one vec4 of scaled-binding bits (wgsl::Options::render_scale).
        std::vector<uint32_t> swizzles((slots.size() + 3) / 4 * 4 + 4, wgsl::kSwizzleIdentity);
        std::fill(swizzles.end() - 4, swizzles.end(), 0u);
        const std::vector<uint32_t>& dims = prog.texture_dims[stage];
        for (size_t k = 0; k < slots.size(); ++k) {
            const bool want_3d = k < dims.size() && dims[k] == 3;
            const bool array = k < prog.texture_arrays[stage].size() && prog.texture_arrays[stage][k];
            const bool want_depth = k < prog.texture_depth[stage].size() && prog.texture_depth[stage][k];
            const bool as_unfilterable = k < 32 && ((unfilterable >> k) & 1);
            WGPUTextureView view = want_depth ? (array ? s.dummy_depth_array_view : s.dummy_depth_view)
                                   : want_3d  ? s.dummy_view_3d
                                   : array    ? s.dummy_array_view
                                              : s.dummy_view;
            nvn::SamplerDesc sampler;
            auto b = bound_textures.find({stage, slots[k]});
            if (b != bound_textures.end() && excluded != 0 && b->second.texture == excluded) {
                // NVN allows sampling the depth target while the draw writes it
                // (undefined results); WebGPU forbids the usage conflict.
                sampler = b->second.sampler;
                ++delta.unbound_textures;
                ++delta.skip_reasons["texture: depth target sampled while the draw writes it"];
            } else if (b != bound_textures.end() && want_depth) {
                // Shadow lookup: the depth aspect and a comparison sampler.
                sampler = b->second.sampler;
                swizzles[k] |= debug_overrides().noshadow ? wgsl::kBorderForce | wgsl::kBorderPass : border_bits(sampler);
                if (border_bits(sampler) != 0) {
                    static bool logged = false;  // renderer thread only
                    if (!logged) {
                        logged = true;
                        rt::log(rt::LogLevel::Info,
                                "nvn webgpu: depth compare with border addressing (border depth %g, compare %d)",
                                double(sampler.border[0]), int(sampler.compare_func));
                    }
                }
                auto t = s.find_texture(b->second.texture);
                WGPUTextureView dv = t != s.textures.end() ? s.depth_view(t->second, array) : nullptr;
                if (dv == nullptr && t != s.textures.end())
                    dv = s.compare_view(t->second, memory_, presents_seen, array);
                if (dv != nullptr) {
                    view = dv;
                    // The hardware clamps the reference for fixed-point depth (D16, D24,
                    // the RGBA8 stand-in); WebGPU may back Depth24Plus with float and not.
                    const uint32_t f = t->second.desc.format;
                    if (f != uint32_t(nvn::Format::Depth32F) && f != uint32_t(nvn::Format::Depth32FStencil8))
                        swizzles[k] |= wgsl::kCompareClampRef;
                    // Filters included: linear compare filtering is the hardware's 2x2 PCF.
                    static std::set<std::tuple<uint32_t, uint32_t, uint32_t, int, int>> seen;  // renderer thread only
                    if (seen.size() < 16 && seen.insert({f, t->second.desc.width, t->second.desc.height,
                                                         int(sampler.min), int(sampler.mag)})
                                                .second)
                        rt::log(rt::LogLevel::Info,
                                "nvn webgpu: depth compare on format %u, %ux%u, wrap %d/%d, compare %d, "
                                "filter min %s mag %s",
                                f, t->second.desc.width, t->second.desc.height, int(sampler.wrap[0]),
                                int(sampler.wrap[1]), int(sampler.compare_func),
                                sampler.min == nvn::Filter::Linear ? "linear" : "nearest",
                                sampler.mag == nvn::Filter::Linear ? "linear" : "nearest");
                } else {
                    ++delta.unbound_textures;
                    ++delta.skip_reasons[b->second.texture == 0 ? "texture: slot bound to no texture"
                                         : t == s.textures.end() ? "texture: unknown texture id"
                                                                 : "texture: depth compare on a texture without depth"];
                    if (t != s.textures.end()) {
                        static std::set<nvn::TextureId> logged;  // once per texture (renderer thread only)
                        if (logged.insert(b->second.texture).second) {
                            auto partner = s.depth_partner.find(b->second.texture);
                            rt::log(rt::LogLevel::Warn,
                                    "nvn webgpu: stage %d slot %u: depth compare on texture #%u (format %u, target "
                                    "%u, %ux%u, %u samples) without depth; draw targets: %zu colour, depth #%u; "
                                    "depth target last bound with it: #%d",
                                    stage, slots[k], b->second.texture, t->second.desc.format,
                                    t->second.desc.target, t->second.desc.width, t->second.desc.height,
                                    t->second.desc.samples, st.colors.size(), st.depth_stencil,
                                    partner == s.depth_partner.end() ? -1 : int(partner->second));
                        }
                    }
                }
            } else if (b != bound_textures.end()) {
                sampler = b->second.sampler;
                uint32_t code = 0;
                for (int c = 0; c < 4; ++c) code |= uint32_t(b->second.swizzle[c]) << (3 * c);
                // Programs translated with sampler_lod_bias read the bias from the high half.
                swizzles[k] = code | wgsl::lod_bias_bits(sampler.lod_bias);
                swizzles[k] |= border_color_bits(sampler);
                auto t = s.find_texture(b->second.texture);
                // The pipeline layout decided per bound texture (State::unfilterable_mask); a
                // texture needing unfilterable-float in a filtering binding (compute) stays out.
                const bool needs = t != s.textures.end() && s.needs_unfilterable(t->second);
                if (as_unfilterable && needs && has_depth(t->second.format)) {
                    // Depth sampled as colour: the depth aspect, depth in .r.
                    if (WGPUTextureView dv = s.depth_view(t->second, array)) view = dv;
                } else if (as_unfilterable && needs && t->second.sampled != nullptr) {
                    check_texture(t->second, "texture");
                    if (s.upload_from_pool(t->second, memory_, presents_seen, &delta))
                        ++delta.uploads;
                    view = array ? s.array_view(t->second) : t->second.sampled;
                    pending_astc += is_nvn_astc(t->second.desc.format) ? 1 : 0;
                    pending_3d += t->second.is_3d() ? 1 : 0;
                } else if (needs || as_unfilterable) {
                    // as_unfilterable without need cannot happen (same bindings as pass 1).
                    ++delta.unbound_textures;
                    ++delta.skip_reasons[has_depth(t->second.format)
                                             ? "texture: depth texture sampled without compare (compute)"
                                             : "texture: 32-bit float without float32-filterable (compute)"];
                } else if (t != s.textures.end() && t->second.sampled != nullptr && t->second.is_3d() == want_3d) {
                    check_texture(t->second, "texture");
                    if (s.upload_from_pool(t->second, memory_, presents_seen, &delta))
                        ++delta.uploads;
                    view = array ? s.array_view(t->second) : t->second.sampled;
                    pending_astc += is_nvn_astc(t->second.desc.format) ? 1 : 0;
                    pending_3d += t->second.is_3d() ? 1 : 0;
                } else {
                    ++delta.unbound_textures;
                    if (t != s.textures.end() && t->second.sampled == nullptr) {
                        static std::set<nvn::TextureId> logged;  // once per texture (renderer thread only)
                        if (logged.insert(b->second.texture).second)
                            rt::log(rt::LogLevel::Warn,
                                    "nvn webgpu: texture #%u (format %u, target %u, %ux%u, %u samples) cannot be sampled",
                                    b->second.texture, t->second.desc.format, t->second.desc.target,
                                    t->second.desc.width, t->second.desc.height, t->second.desc.samples);
                    }
                    ++delta.skip_reasons[b->second.texture == 0          ? "texture: slot bound to no texture"
                                         : t == s.textures.end()         ? "texture: unknown texture id"
                                         : has_depth(t->second.format)   ? "texture: depth texture sampled without compare"
                                         : t->second.sampled == nullptr  ? "texture: format or target not supported"
                                                                         : "texture: 2D/3D does not match the shader"];
                }
            } else {
                ++delta.unbound_textures;
                ++delta.skip_reasons["texture: shader slot " + std::to_string(slots[k]) + " never bound"];
                static std::atomic<bool> logged{false};
                if (!logged.exchange(true)) {
                    std::string bound;
                    for (const auto& [key, bt] : bound_textures)
                        bound += " " + std::to_string(key.first) + ":" + std::to_string(key.second) + "=#" +
                                 std::to_string(bt.texture);
                    rt::log(rt::LogLevel::Warn, "nvn webgpu: stage %d samples slot %u, bound (stage:slot=texture):%s",
                            stage, slots[k], bound.c_str());
                }
            }
            if (b != bound_textures.end() && s.render_scale != 1.0f && k < 128)
                if (auto t = s.textures.find(b->second.texture); t != s.textures.end() && t->second.scaled)
                    swizzles[swizzles.size() - 4 + k / 32] |= 1u << (k % 32);
            WGPUBindGroupEntry e = WGPU_BIND_GROUP_ENTRY_INIT;
            e.binding = uint32_t(2 * k);
            e.textureView = view;
            entries.push_back(e);
            WGPUBindGroupEntry se = WGPU_BIND_GROUP_ENTRY_INIT;
            se.binding = uint32_t(2 * k + 1);
            // WebGPU: comparison samplers only in comparison slots and vice versa. A
            // shadow lookup through a sampler without compare mode uses LESS_EQUAL
            // (UNVERIFIED what Maxwell does then).
            if (sampler.compare != want_depth) {
                if (want_depth)
                    sampler.compare_func = nvn::CompareFunc::LessEqual;
                sampler.compare = want_depth;
            }
            // Unfilterable-float bindings take non-filtering samplers only: NVN's
            // linear filtering of these textures becomes nearest (UNVERIFIED how
            // much it shows; depth sampled as colour usually reads exact texels).
            if (as_unfilterable) {
                sampler.mag = sampler.min = sampler.mip = nvn::Filter::Nearest;
                sampler.max_anisotropy = 1;
            }
            se.sampler = s.sampler(sampler);
            entries.push_back(se);
        }
        if (stage < 2 && prog.stage_global_memory[stage]) {
            // Read-only slots k from the stage's storage buffers, plus the slot table.
            uint32_t table[wgsl::kGlobalSlots * 4] = {};
            for (uint32_t k = 0; k < wgsl::kGlobalSlots; ++k) {
                WGPUBindGroupEntry e = WGPU_BIND_GROUP_ENTRY_INIT;
                e.binding = wgsl::kGraphicsGlobalBindingBase + k;
                auto b = storage_bindings.find({stage, k});
                if (b != storage_bindings.end() && b->second.size > 0 && b->second.size <= kMaxStorageBytes) {
                    StorageBuffer& sb = storage_buffer(b->second.address, b->second.size);
                    sb.main_use = true;
                    e.buffer = sb.buffer;
                    e.size = wgpuBufferGetSize(sb.buffer);
                    table[4 * k] = uint32_t(sb.address);
                    table[4 * k + 1] = uint32_t(sb.address >> 32);
                    table[4 * k + 2] = uint32_t(sb.size);
                } else {
                    e.buffer = s.dummy_storage(k);
                    e.size = 16;
                }
                entries.push_back(e);
            }
            WGPUBufferDescriptor td = WGPU_BUFFER_DESCRIPTOR_INIT;
            td.label = pw::string_view("nvn global memory slots");
            td.size = sizeof(table);
            td.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
            WGPUBuffer tab = s.list_buffer(&td);
            temp_buffers.push_back(tab);
            queue_write_buffer(s.queue, tab, 0, table, sizeof(table));
            WGPUBindGroupEntry e = WGPU_BIND_GROUP_ENTRY_INIT;
            e.binding = wgsl::kGraphicsGlobalBindingBase + wgsl::kGlobalSlots;
            e.buffer = tab;
            e.size = sizeof(table);
            entries.push_back(e);
        }
        if (prog.texture_swizzle && !slots.empty()) {
            WGPUBindGroupEntry e = WGPU_BIND_GROUP_ENTRY_INIT;
            e.binding = uint32_t(2 * slots.size());
            e.buffer = s.swizzle_buffer(swizzles);
            e.size = swizzle_uniform_size(slots.size());
            entries.push_back(e);
        }
        WGPUBindGroupDescriptor bd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        bd.layout = layout;
        bd.entryCount = entries.size();
        bd.entries = entries.data();
        // Kept across lists (State::texture_groups), unless the group holds
        // this list's global memory buffers.
        if (stage < 2 && prog.stage_global_memory[stage]) {
            WGPUBindGroup g = wgpuDeviceCreateBindGroup(s.device, &bd);
            ++delta.bind_groups_created;
            bind_groups.push_back(g);
            return g;
        }
        std::vector<uintptr_t> key;
        key.reserve(entries.size() * 5);
        for (const WGPUBindGroupEntry& e : entries) {
            key.push_back(uintptr_t(e.textureView));
            key.push_back(uintptr_t(e.sampler));
            key.push_back(uintptr_t(e.buffer));
            key.push_back(uintptr_t(e.offset));
            key.push_back(uintptr_t(e.size));
        }
        auto it = s.texture_groups.find({layout, key});
        if (it != s.texture_groups.end()) {
            ++delta.bind_groups_reused;
            return it->second;
        }
        if (s.texture_groups.size() >= kMaxCachedBindGroups) {
            // Released groups' handles may come back for new ones: forget what the pass has set.
            s.clear_bind_group_cache();
            pass_state = PassState();
        }
        WGPUBindGroup g = wgpuDeviceCreateBindGroup(s.device, &bd);
        ++delta.bind_groups_created;
        s.texture_groups.emplace(std::make_pair(layout, std::move(key)), g);
        return g;
    };

    // Clears a region of a colour texture (memory rows): a clear load op when
    // it covers everything, otherwise a scissored full-screen triangle.
    auto clear_region = [&](nvn::TextureId id, const State::Texture& t, const Region& r, const float rgba[4],
                            uint32_t mask) {
        if (r.width == 0 || r.height == 0 || mask == 0)
            return;
        WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
        color.view = t.attachment;
        color.storeOp = WGPUStoreOp_Store;
        const bool direct = r.full && mask == 0xF;
        color.loadOp = direct ? WGPULoadOp_Clear : WGPULoadOp_Load;
        color.clearValue = WGPUColor{rgba[0], rgba[1], rgba[2], rgba[3]};
        WGPURenderPassDescriptor pass_desc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
        pass_desc.colorAttachmentCount = 1;
        pass_desc.colorAttachments = &color;
        pass_desc.label = pass_label("clear", &t, id, 0);
        WGPUPassTimestampWrites pass_desc_time;
        pass_desc.timestampWrites = time_pass(kRenderPass, &pass_desc_time, pass_target("clear", &t, id, 0));
        s.note_attachments(pass_desc, delta);
        WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &pass_desc);
        if (!direct) {
            const uint32_t offset = push_uniform(rgba, 0.0f);
            wgpuRenderPassEncoderSetPipeline(pass, s.color_pipeline(t.format, t.sample_count, mask));
            wgpuRenderPassEncoderSetBindGroup(pass, 0, s.clear_bind_group, 1, &offset);
            wgpuRenderPassEncoderSetScissorRect(pass, r.x, r.y, r.width, r.height);
            wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
        }
        wgpuRenderPassEncoderEnd(pass);
        wgpuRenderPassEncoderRelease(pass);
        ++delta.render_passes;
        ++delta.pass_reasons[direct ? "own pass: colour clear" : "own pass: colour clear (part, drawn)"];
    };

    // Clears a region of a depth/stencil texture: clear load ops when they
    // cover everything, otherwise a scissored full-screen triangle.
    auto depth_clear_pass = [&](nvn::TextureId id, const State::Texture& t, const Region& r, bool depth_op,
                                uint32_t stencil_mask, float depth, uint32_t stencil) {
        const bool direct = r.full && (stencil_mask == 0 || stencil_mask == 0xFF);
        WGPURenderPassDepthStencilAttachment ds = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
        ds.view = t.attachment;
        if (has_depth(t.format)) {
            ds.depthLoadOp = direct && depth_op ? WGPULoadOp_Clear : WGPULoadOp_Load;
            ds.depthStoreOp = WGPUStoreOp_Store;
            ds.depthClearValue = depth;
        }
        if (has_stencil(t.format)) {
            ds.stencilLoadOp = direct && stencil_mask == 0xFF ? WGPULoadOp_Clear : WGPULoadOp_Load;
            ds.stencilStoreOp = WGPUStoreOp_Store;
            ds.stencilClearValue = stencil;
        }
        WGPURenderPassDescriptor pass_desc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
        pass_desc.depthStencilAttachment = &ds;
        pass_desc.label = pass_label("clear", &t, 0, id);
        WGPUPassTimestampWrites pass_desc_time;
        pass_desc.timestampWrites = time_pass(kRenderPass, &pass_desc_time, pass_target("clear", &t, 0, id));
        s.note_attachments(pass_desc, delta);
        WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &pass_desc);
        if (!direct) {
            const float none[4] = {0, 0, 0, 0};
            const uint32_t offset = push_uniform(none, depth);
            wgpuRenderPassEncoderSetPipeline(pass, s.depth_pipeline(t.format, t.sample_count, depth_op, stencil_mask));
            wgpuRenderPassEncoderSetBindGroup(pass, 0, s.clear_bind_group, 1, &offset);
            wgpuRenderPassEncoderSetStencilReference(pass, stencil);
            wgpuRenderPassEncoderSetScissorRect(pass, r.x, r.y, r.width, r.height);
            wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
        }
        wgpuRenderPassEncoderEnd(pass);
        wgpuRenderPassEncoderRelease(pass);
        ++delta.render_passes;
        ++delta.pass_reasons["own pass: depth/stencil clear"];
    };

    // State::fold_clears: whole-texture clears not yet encoded. The next
    // draw pass with the texture as attachment takes them as clear load ops;
    // anything else that uses the texture first (a draw sampling it, a copy,
    // a partial clear, an in-order dispatch, the end of the list) encodes
    // them as their own passes before. Dispatches submitted ahead never read
    // a texture the list clears (written_textures).
    struct PendingColorClear {
        float rgba[4] = {};
    };
    struct PendingDepthClear {
        bool depth = false, stencil = false;
        float depth_value = 1.0f;
        uint32_t stencil_value = 0;
    };
    std::map<nvn::TextureId, PendingColorClear> pending_color;
    // The real target of a half-resolution draw while it renders into its
    // stand-in (State::halfres_for), else nullptr.
    const State::Texture* halfres_main = nullptr;
    std::map<nvn::TextureId, PendingDepthClear> pending_depth;
    // Encodes the pending clears of `id`; no draw pass may be open.
    auto flush_clear = [&](nvn::TextureId id) {
        if (auto p = pending_color.find(id); p != pending_color.end()) {
            const PendingColorClear c = p->second;
            pending_color.erase(p);
            if (auto t = s.find_texture(id); t != s.textures.end() && t->second.attachment != nullptr) {
                Region r;
                r.width = t->second.width();
                r.height = t->second.height();
                clear_region(id, t->second, r, c.rgba, 0xF);
                note_event({}, false, {id});
            }
        }
        if (auto p = pending_depth.find(id); p != pending_depth.end()) {
            const PendingDepthClear c = p->second;
            pending_depth.erase(p);
            if (auto t = s.find_texture(id); t != s.textures.end() && t->second.attachment != nullptr) {
                Region r;
                r.width = t->second.width();
                r.height = t->second.height();
                depth_clear_pass(id, t->second, r, c.depth, c.stencil ? 0xFF : 0, c.depth_value, c.stencil_value);
                const bool whole = (c.depth || !has_depth(t->second.format)) && (c.stencil || !has_stencil(t->second.format));
                if (whole)
                    note_event({}, false, {id});
                else
                    note_event({id}, false);
            }
        }
    };
    auto flush_clears = [&](const char* why) {
        if (pending_color.empty() && pending_depth.empty())
            return;
        end_draw_pass(why);
        std::vector<nvn::TextureId> ids;
        for (const auto& entry : pending_color)
            ids.push_back(entry.first);
        for (const auto& entry : pending_depth)
            ids.push_back(entry.first);
        for (nvn::TextureId id : ids)
            flush_clear(id);
    };

    // Streams sharing one buffer (VertexMerge): their elements interleaved
    // from guest memory (as many as every member holds) into the geometry
    // arena, once per list and set of ranges, and bound at `slot`. Each
    // member's stream_ranges entry gets the size its own stride would need
    // for that many elements, so instanced_runs checks the draw against it.
    // An unbound member reads zeros. Returns the reason the draw is skipped,
    // nullptr if bound.
    std::map<std::vector<uint64_t>, std::pair<std::pair<WGPUBuffer, uint64_t>, uint64_t>> merged_uploads;
    auto bind_merged_streams = [&](const VertexMerge& merge, uint32_t slot) -> const char* {
        std::vector<uint64_t> key;
        uint64_t elements = UINT64_MAX;
        for (size_t m = 0; m < merge.streams.size(); ++m) {
            const uint32_t stride = st.layout.strides[merge.streams[m]];
            auto v = vertex_buffers.find(merge.streams[m]);
            if (v == vertex_buffers.end() || v->second.size == 0) {
                key.insert(key.end(), {0, 0, stride, merge.base[m], merge.last[m]});
                continue;
            }
            // Not foreseen by DispatchWrites (the pre-pass chose the streams).
            if (dispatch_output(v->second.address, v->second.size))
                return "draw: merged vertex stream written by a dispatch (device vertex buffer limit)";
            const uint64_t size = std::min<uint64_t>(v->second.size, 64ull << 20);
            if (size < merge.last[m])
                return "draw: merged vertex stream shorter than one element (device vertex buffer limit)";
            if (stride != 0)
                elements = std::min<uint64_t>(elements, (size - merge.last[m]) / stride + 1);
            key.insert(key.end(), {v->second.address, size, stride, merge.base[m], merge.last[m]});
        }
        if (elements == UINT64_MAX)
            elements = 1;  // all strides 0
        elements = std::min<uint64_t>(elements, (64ull << 20) / merge.stride);
        auto known = merged_uploads.find(key);
        if (known == merged_uploads.end()) {
            std::vector<uint8_t> merged(size_t(elements * merge.stride), 0), src;
            for (size_t m = 0; m < merge.streams.size(); ++m) {
                const uint32_t stride = st.layout.strides[merge.streams[m]];
                const uint64_t address = key[5 * m];
                if (key[5 * m + 1] == 0)
                    continue;  // unbound: zeros
                src.assign(size_t((elements - 1) * stride + merge.last[m]), 0);
                check_read(address, src.size(), "vertex buffer");
                if (memory_ == nullptr || !memory_->read(address, src.data(), src.size()))
                    rt::log(rt::LogLevel::Warn, "nvn webgpu: vertex buffer 0x%llx not readable",
                            (unsigned long long)address);
                for (uint64_t e = 0; e < elements; ++e)
                    std::memcpy(merged.data() + e * merge.stride + merge.base[m], src.data() + e * stride,
                                merge.last[m]);
            }
            const auto where = s.geometry_arena.put(s.device, merged.data(), merged.size());
            ++delta.geometry_uploads;
            delta.geometry_upload_bytes += merged.size();
            known = merged_uploads.emplace(key, std::make_pair(where, elements)).first;
        } else {
            ++delta.geometry_reused;
        }
        const auto [where, count] = known->second;
        for (size_t m = 0; m < merge.streams.size(); ++m) {
            const uint32_t stride = st.layout.strides[merge.streams[m]];
            stream_ranges[merge.streams[m]] = {where.first, where.second, (count - 1) * stride + merge.last[m]};
        }
        pass_state.vertex_buffer(draw_pass, slot, where.first, where.second, count * merge.stride);
        return nullptr;
    };

    // Everything a draw needs besides the draw call itself; false = skip.
    // State::patch_dispatch_output: per vertex stream of the draw being
    // prepared, a buffer with its guest bytes and the overlapping output of
    // dispatches earlier in the list copied over. Built once per range until
    // the next dispatch or buffer copy (patched_ranges, keyed with the
    // dispatch count).
    std::map<uint32_t, WGPUBuffer> patched_streams;
    std::map<std::tuple<uint64_t, uint64_t, uint64_t>, WGPUBuffer> patched_ranges;
    // Only streams the draw binds on their own (not merged ones).
    auto patch_vertex_streams = [&](const std::vector<int>& slots, const VertexMerge& merge) {
        for (const auto& [stream, v] : vertex_buffers) {
            const uint64_t size = std::min<uint64_t>(v.size, 64ull << 20);
            if (size == 0 || stream >= st.layout.strides.size() || stream >= slots.size() || slots[stream] < 0 ||
                merge.member(stream) >= 0 || dispatch_output(v.address, size))
                continue;  // unused, merged, or inside one output (bound directly)
            const uint64_t va = v.address;
            const std::vector<StorageBuffer*> parts = overlapping_outputs(va, va + size, nullptr);
            if (parts.empty())
                continue;
            const auto key = std::make_tuple(va, size, delta.dispatches);
            if (auto it = patched_ranges.find(key); it != patched_ranges.end()) {
                patched_streams[stream] = it->second;
                continue;
            }
            end_draw_pass("vertex stream patched with dispatch output");  // copies are recorded outside passes
            const uint64_t gpu_size = (size + 255) & ~uint64_t{255};  // the bound range binding uses
            WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
            bd.label = pw::string_view("nvn vertices with dispatch output");
            bd.size = gpu_size;
            bd.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
            WGPUBuffer buffer = gpu_create_buffer(s.device, &bd);
            temp_buffers.push_back(buffer);
            scratch.assign(size_t(gpu_size), 0);
            if (memory_ == nullptr || !memory_->read(va, scratch.data(), size))
                rt::log(rt::LogLevel::Warn, "nvn webgpu: vertex buffer 0x%llx+0x%llx not readable",
                        (unsigned long long)va, (unsigned long long)size);
            // Lands before the list's submits, so before the copies below.
            queue_write_buffer(s.queue, buffer, 0, scratch.data(), scratch.size());
            for (StorageBuffer* b : parts) {
                const uint64_t lo = std::max(va, b->address), hi = std::min(va + size, b->address + b->size);
                if ((lo - b->address) % 4 != 0 || (lo - va) % 4 != 0 || (hi - lo) % 4 != 0) {
                    ++delta.diagnostics["dispatch output patch: vertex overlap not 4-byte aligned (old bytes)"];
                    continue;
                }
                wgpuCommandEncoderCopyBufferToBuffer(encoder, b->buffer, lo - b->address, buffer, lo - va, hi - lo);
                b->main_use = true;  // a later dispatch writing it cannot go first
            }
            ++delta.diagnostics["dispatch output patch: vertex stream patched from dispatch output"];
            patched_ranges.emplace(key, buffer);
            patched_streams[stream] = buffer;
        }
    };
    auto prepare_draw = [&](nvn::Topology topo, WGPUIndexFormat strip_index) -> bool {
        pending_astc = pending_3d = 0;
        const PipelineRequest req =
            s.pipeline_request(st, topo, list, strip_index, bound_textures, dispatch_writes.streams(vertex_buffers));
        auto pit = req.key.empty() ? s.draw_pipelines.end() : s.draw_pipelines.find(req.key);
        if (pit == s.draw_pipelines.end() && s.compiling_keys.count(req.key)) {
            ++delta.draws_skipped_compiling;
            return false;
        }
        if (pit == s.draw_pipelines.end() || pit->second.pipeline == nullptr) {
            ++delta.skip_reasons[st.program == 0 ? "draw: no program (untranslated shaders)"
                                 : req.key.empty() ? "draw: no pipeline request (state incomplete)"
                                                   : "draw: pipeline missing or invalid"];
            return false;
        }
        const Program& prog = *s.programs.at(st.program);
        // Target size: the first bound target defines the framebuffer.
        const State::Texture* first = nullptr;
        for (nvn::TextureId id : st.colors)
            if (auto t = s.find_texture(id); id != 0 && t != s.textures.end() && t->second.attachment) {
                first = &t->second;
                break;
            }
        if (first == nullptr)
            if (auto t = s.find_texture(st.depth_stencil); t != s.textures.end() && t->second.attachment)
                first = &t->second;
        if (first == nullptr) {
            ++delta.skip_reasons["draw: no usable render target"];
            return false;
        }
        uint32_t used_slots = 0;
        const VertexMerge merge =
            vertex_merge(st.layout, State::unfed_inputs(prog, st.layout), s.max_vertex_buffers, req.merge_exclude);
        const std::vector<int> slots = vertex_slots(st.layout, &used_slots, merge);
        // Before the pass begins: patching records copies outside passes.
        patched_streams.clear();
        if (s.patch_dispatch_output)
            patch_vertex_streams(slots, merge);
        // Does the draw sample its own depth/stencil attachment? NVN allows
        // that; WebGPU only with the attachment read-only (no depth or
        // stencil writes in the whole pass). With writes, the binding gets
        // the stand-in instead. A proxy attachment (other size) is another
        // texture and does not conflict.
        nvn::TextureId sampled_target = 0;
        auto dt = s.find_texture(st.depth_stencil);
        const bool direct_depth = st.depth_stencil != 0 && dt != s.textures.end() && dt->second.attachment &&
                                  dt->second.width() == first->width() && dt->second.height() == first->height();
        if (direct_depth)
            for (int stage = 0; stage < 2 && sampled_target == 0; ++stage)
                for (uint32_t slot : prog.texture_slots[stage])
                    if (auto b = bound_textures.find({stage, slot});
                        b != bound_textures.end() && b->second.texture == st.depth_stencil) {
                        sampled_target = st.depth_stencil;
                        break;
                    }
        // Writes as WebGPU counts them for the pipeline (create_draw_pipeline).
        bool target_writes = false;
        if (st.depth_stencil != 0 && dt != s.textures.end()) {
            const nvn::CmdSetDepthStencil& d = req.fixed.depth_stencil;
            const auto stencil_writes = [](const nvn::StencilFace& f) {
                return f.fail != nvn::StencilOp::Keep || f.depth_fail != nvn::StencilOp::Keep ||
                       f.pass != nvn::StencilOp::Keep;
            };
            target_writes = (has_depth(dt->second.format) && d.depth_test && d.depth_write) ||
                            (has_stencil(dt->second.format) && d.stencil_test && req.fixed.stencil_write_mask != 0 &&
                             (stencil_writes(d.front) || stencil_writes(d.back)));
        }
        const bool want_read_only = sampled_target != 0 && !target_writes;
        // A texture the draw samples gets its pending clear first.
        if (!pending_color.empty() || !pending_depth.empty())
            for (int stage = 0; stage < 2; ++stage)
                for (uint32_t slot : prog.texture_slots[stage])
                    if (auto b = bound_textures.find({stage, slot});
                        b != bound_textures.end() &&
                        (pending_color.count(b->second.texture) || pending_depth.count(b->second.texture))) {
                        end_draw_pass("sampled texture has a clear pending");
                        flush_clear(b->second.texture);
                    }
        // A read-only pass ends before a draw that writes depth or stencil, a
        // writable one before a draw that needs it read-only.
        if (draw_pass != nullptr && ((draw_pass_depth_read_only && target_writes) ||
                                     (!draw_pass_depth_read_only && want_read_only)))
            end_draw_pass(want_read_only ? "depth becomes read-only (sampled)" : "depth becomes writable");
        if (draw_pass == nullptr) {
            bool pass_cleared = false;  // a folded clear (PassRecord)
            PassRecord rec;
            std::vector<WGPURenderPassColorAttachment> atts;
            for (nvn::TextureId id : st.colors) {
                WGPURenderPassColorAttachment a = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
                auto t = s.find_texture(id);
                if (id != 0 && t != s.textures.end() && t->second.attachment && !has_depth(t->second.format) &&
                    !has_stencil(t->second.format)) {
                    a.view = t->second.attachment;
                    t->second.gpu_written = true;
                    if (first != nullptr && (t->second.width() != first->width() || t->second.height() != first->height())) {
                        // WebGPU rejects attachments of different sizes: this one is left out.
                        static const std::string what = "render scale: colour target of another size left out of the pass";
                        ++delta.diagnostics[what];
                        a.view = nullptr;
                        atts.push_back(a);
                        continue;
                    }
                    if (diagnose)
                        rec.stored_colors.push_back(id);
                    a.loadOp = WGPULoadOp_Load;
                    a.storeOp = WGPUStoreOp_Store;
                    if (auto p = pending_color.find(id); p != pending_color.end()) {
                        a.loadOp = WGPULoadOp_Clear;
                        a.clearValue = WGPUColor{p->second.rgba[0], p->second.rgba[1], p->second.rgba[2],
                                                 p->second.rgba[3]};
                        pending_color.erase(p);
                        pass_cleared = true;
                        if (diagnose)
                            rec.overwritten.insert(id);
                        ++delta.pass_reasons["folded: colour clear (no own pass)"];
                    }
                }
                atts.push_back(a);
            }
            WGPURenderPassDepthStencilAttachment ds = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
            WGPURenderPassDescriptor pd = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
            pd.colorAttachmentCount = atts.size();
            pd.colorAttachments = atts.data();
            if (auto t = s.find_texture(st.depth_stencil);
                st.depth_stencil != 0 && t != s.textures.end() && t->second.attachment) {
                t->second.gpu_written = true;
                // Host sizes: a scaled colour target with an unscaled depth target takes the stand-in.
                const bool same_size = t->second.width() == first->width() && t->second.height() == first->height();
                if (t->second.scaled != first->scaled) {
                    static const std::string what = "render scale: depth and colour target scaled differently";
                    ++delta.diagnostics[what];
                    // The first pairs, with the reason the unscaled one stayed so.
                    static std::atomic<int> logged{0};
                    if (logged.fetch_add(1) < 16) {
                        const State::Texture& u = t->second.scaled ? *first : t->second;
                        const char* why = s.scale_refusal(u);
                        rt::log(rt::LogLevel::Info,
                                "nvn webgpu: render scale: colour %ux%u%s with depth %ux%u%s (unscaled: %s)",
                                first->desc.width, first->desc.height, first->scaled ? " scaled" : "",
                                t->second.desc.width, t->second.desc.height, t->second.scaled ? " scaled" : "",
                                why != nullptr ? why : "-");
                    }
                }
                ds.view = same_size ? t->second.attachment
                                    : s.proxy_depth_view(t->second, first->width(), first->height());
                // A read-only or proxy attachment cannot take the clear.
                auto pending = pending_depth.find(st.depth_stencil);
                if (pending != pending_depth.end() && (want_read_only || !same_size)) {
                    flush_clear(st.depth_stencil);
                    pending = pending_depth.end();
                }
                // Unused with Load, but the browser's IDL conversion
                // rejects the NaN default ("non-finite") anyway.
                ds.depthClearValue = 1.0f;
                if (want_read_only) {
                    // Load/store ops must stay undefined on read-only aspects.
                    ds.depthReadOnly = has_depth(t->second.format);
                    ds.stencilReadOnly = has_stencil(t->second.format);
                } else {
                    if (has_depth(t->second.format)) {
                        ds.depthLoadOp = WGPULoadOp_Load;
                        ds.depthStoreOp = WGPUStoreOp_Store;
                    }
                    if (has_stencil(t->second.format)) {
                        ds.stencilLoadOp = WGPULoadOp_Load;
                        ds.stencilStoreOp = WGPUStoreOp_Store;
                    }
                    if (same_size)
                        rec.stored_depth = st.depth_stencil;
                    if (pending != pending_depth.end()) {
                        if (diagnose && (pending->second.depth || !has_depth(t->second.format)) &&
                            (pending->second.stencil || !has_stencil(t->second.format)))
                            rec.overwritten.insert(st.depth_stencil);
                        if (pending->second.depth && has_depth(t->second.format)) {
                            ds.depthLoadOp = WGPULoadOp_Clear;
                            ds.depthClearValue = pending->second.depth_value;
                        }
                        if (pending->second.stencil && has_stencil(t->second.format)) {
                            ds.stencilLoadOp = WGPULoadOp_Clear;
                            ds.stencilClearValue = pending->second.stencil_value;
                        }
                        pending_depth.erase(pending);
                        pass_cleared = true;
                        ++delta.pass_reasons["folded: depth/stencil clear (no own pass)"];
                    }
                }
                pd.depthStencilAttachment = &ds;
                draw_pass_depth_read_only = want_read_only;
                rec.depth_read_only = want_read_only;
            }
            const char* target = nullptr;
            if (pass_labels || s.timestamps) {
                nvn::TextureId first_color = 0;
                for (nvn::TextureId id : st.colors)
                    if (id != 0 && s.find_texture(id) != s.textures.end()) {
                        first_color = id;
                        break;
                    }
                const nvn::TextureId depth = pd.depthStencilAttachment ? st.depth_stencil : 0;
                pd.label = pass_label("draw", first, first_color, depth);
                target = pass_target("draw", first, first_color, depth);
            }
            WGPUPassTimestampWrites pd_time;
            pd.timestampWrites = time_pass(kRenderPass, &pd_time, target);
            draw_pass_timed = pd.timestampWrites != nullptr ? int(timer.tags.size()) - 1 : -1;
            s.note_attachments(pd, delta);
            draw_pass = wgpuCommandEncoderBeginRenderPass(encoder, &pd);
            pass_state = PassState();
            ++delta.render_passes;
            ++delta.draw_passes;
            draw_pass_colors.assign(st.colors.begin(), st.colors.end());
            draw_pass_depth = st.depth_stencil;
            if (diagnose) {
                for (nvn::TextureId id : st.colors)
                    if (id != 0)
                        rec.targets.push_back(id);
                if (st.depth_stencil != 0)
                    rec.targets.push_back(st.depth_stencil);
                std::sort(rec.targets.begin(), rec.targets.end());
                rec.cleared = pass_cleared;
                pass_records.push_back(std::move(rec));
            }
        }
        // A half-resolution draw (State::halfres_for) keeps the
        // real target's framebuffer coordinates, halved; guest window
        // coordinates map to host texels (State::render_scale).
        const State::Texture* frame = halfres_main != nullptr ? halfres_main : first;
        const float sx = float(first->width()) / float(first->desc.width);
        const float sy = float(first->height()) / float(first->desc.height);
        const bool halffill = halfres_main == nullptr && debug_overrides().halffill &&
                              uint64_t(first->desc.width) * first->desc.height >= 1280u * 720u;
        const bool scaled = halfres_main == nullptr && !halffill && s.fills(first->desc.width, first->desc.height);
        const float fill = halffill ? 0.5f : scaled ? s.fill_scale : 1.0f;
        // Half-resolution: scaled by the stand-in's size over the real
        // target's, per axis (an odd size rounds the stand-in up, its last
        // texel is drawn too). Multiplied before dividing, so a full-size
        // viewport lands exactly on the stand-in's size (641/1281*1281 is
        // 640.99994 in float; D3D12 WARP in CI then left the last column empty).
        // Guest window coordinates of the real target to the stand-in's
        // host texels.
        auto fill_x = [&](float v) {
            return halfres_main != nullptr
                       ? float(double(v) * first->width() / frame->desc.width)
                       : v * sx * fill;
        };
        auto fill_y = [&](float v) {
            return halfres_main != nullptr
                       ? float(double(v) * first->height() / frame->desc.height)
                       : v * sy * fill;
        };
        if (viewport_set && !debug_overrides().noviewport) {
            // Window rows are memory rows; the lower-left origin flips clip y in
            // the vertex shader instead (PipelineRequest::flip_y).
            const float v[6] = {fill_x(viewport[0]), fill_y(viewport[1]), fill_x(viewport[2]), fill_y(viewport[3]),
                                depth_range[0], depth_range[1]};
            pass_state.viewport_(draw_pass, v);
        } else {
            const float v[6] = {0, 0, fill_x(float(frame->desc.width)), fill_y(float(frame->desc.height)),
                                depth_range[0], depth_range[1]};
            pass_state.viewport_(draw_pass, v);
        }
        if (diagnose)
            for (int stage = 0; stage < 2; ++stage)
                for (uint32_t slot : prog.texture_slots[stage])
                    if (auto b = bound_textures.find({stage, slot}); b != bound_textures.end())
                        pass_records.back().sampled.insert(b->second.texture);
        Region r = region_of(*frame);
        if (halffill) {
            r.x /= 2;
            r.y /= 2;
            r.width = (r.width + 1) / 2;
            r.height = (r.height + 1) / 2;
            static const std::string what = "halffill: draws into the top-left quarter";
            ++delta.diagnostics[what];
        }
        if (scaled) {
            const uint32_t x1 = uint32_t(std::ceil(float(r.x + r.width) * fill));
            const uint32_t y1 = uint32_t(std::ceil(float(r.y + r.height) * fill));
            r.x = uint32_t(float(r.x) * fill);
            r.y = uint32_t(float(r.y) * fill);
            // An empty scissor stays empty instead of growing to 1 px.
            r.width = r.width ? x1 - r.x : 0;
            r.height = r.height ? y1 - r.y : 0;
            static const std::string what = "fill scale: draws into a scaled top-left part";
            ++delta.diagnostics[what];
        }
        if (halfres_main != nullptr) {
            const uint32_t x1 = std::min<uint32_t>((r.x + r.width + 1) / 2, first->width());
            const uint32_t y1 = std::min<uint32_t>((r.y + r.height + 1) / 2, first->height());
            r.x = std::min<uint32_t>(r.x / 2, x1);
            r.y = std::min<uint32_t>(r.y / 2, y1);
            r.width = r.width ? x1 - r.x : 0;
            r.height = r.height ? y1 - r.y : 0;
        }
        if (static int traced = 0; debug_overrides().trace && traced < 12) {
            ++traced;
            const FixedState& f = st.fixed;
            const nvn::CmdSetDepthStencil& d = f.depth_stencil;
            char line[512];
            std::snprintf(line, sizeof(line),
                          "draw-trace prog %u target %ux%u vp%s %.0f,%.0f %.0fx%.0f depth %.2f..%.2f "
                          "scissor %u,%u %ux%u cull %d front %d depth test %d write %d func %d stencil %d "
                          "blend 0x%x mask0 0x%x eq0 %d/%d/%d topo %d",
                          unsigned(st.program), first->desc.width, first->desc.height, viewport_set ? "" : "(unset)",
                          viewport[0], viewport[1], viewport[2], viewport[3], depth_range[0], depth_range[1], r.x, r.y,
                          r.width, r.height, int(f.raster.cull), int(f.raster.front_face), d.depth_test, d.depth_write,
                          int(d.depth_func), d.stencil_test, f.blend_enable, f.masks[0], int(f.equations[0].src_color),
                          int(f.equations[0].dst_color), int(f.equations[0].color_op), int(topo));
            rt::log(rt::LogLevel::Warn, "%s", line);
            auto floats = [&](const char* what, uint64_t va, uint64_t size) {
                float v[16] = {};
                const uint64_t n = std::min<uint64_t>(size, sizeof(v));
                const bool ok = memory_ != nullptr && n > 0 && memory_->read(va, v, n);
                std::string text;
                for (uint64_t i = 0; i < n / 4; ++i) {
                    char b[32];
                    std::snprintf(b, sizeof(b), " %g", v[i]);
                    text += b;
                }
                rt::log(rt::LogLevel::Warn, "draw-trace   %s 0x%llx+0x%llx%s:%s", what, (unsigned long long)va,
                        (unsigned long long)size, ok ? "" : " (unreadable)", text.c_str());
            };
            for (const auto& [key, u] : uniform_buffers)
                rt::log(rt::LogLevel::Warn, "draw-trace   bound uniform stage %d index %u 0x%llx+0x%llx", key.first,
                        key.second, (unsigned long long)u.address, (unsigned long long)u.size);
            for (const nvn::VertexAttrib& a : st.layout.attribs)
                rt::log(rt::LogLevel::Warn, "draw-trace   attrib loc %u format %u stream %u offset %u", a.location,
                        unsigned(a.format), a.stream, a.offset);
            for (uint32_t stream = 0; stream < st.layout.strides.size(); ++stream)
                if (auto v = vertex_buffers.find(stream); v != vertex_buffers.end())
                    floats(("stream " + std::to_string(stream) + " stride " +
                            std::to_string(st.layout.strides[stream])).c_str(),
                           v->second.address, v->second.size);
            for (int stage = 0; stage < 2; ++stage)
                for (uint32_t bank : prog.banks[stage])
                    if (auto u = uniform_buffers.find({stage, bank}); u != uniform_buffers.end())
                        floats((std::string(stage ? "fs" : "vs") + " cbuf " + std::to_string(bank)).c_str(),
                               u->second.address, u->second.size);
                    else
                        rt::log(rt::LogLevel::Warn, "draw-trace   %s cbuf %u unbound", stage ? "fs" : "vs", bank);
        }
        pass_state.scissor_(draw_pass, r.x, r.y, r.width, r.height);
        pass_state.pipeline_(draw_pass, pit->second.pipeline);
        if (used_slots < s.max_vertex_buffers)  // inputs without an attribute (create_draw_pipeline)
            pass_state.vertex_buffer(draw_pass, used_slots, s.zero_vertex_buffer(), 0, 16);
        pass_state.blend_(draw_pass, blend_color);
        pass_state.stencil_(draw_pass, stencil_ref & 0xFF);
        for (int stage = 0; stage < 2; ++stage) {
            // Where each bank's bytes are: an arena chunk and offset, or the
            // zero buffer for banks the list never bound.
            cbuf_buffers.clear();
            cbuf_offsets.clear();
            for (uint32_t bank : prog.banks[stage]) {
                auto u = uniform_buffers.find({stage, bank});
                std::pair<WGPUBuffer, uint64_t> where{s.zero_cbuf, 0};
                if (bank == nvn::kDriverConstantBank && prog.stage_global_memory[stage]) {
                    // The driver bank carries the storage buffer descriptors the shader reads.
                    scratch.assign(size_t(kCbufBytes), 0);
                    uint64_t n = 0;
                    if (u != uniform_buffers.end() && memory_ != nullptr) {
                        n = std::min<uint64_t>(u->second.size, kCbufBytes);
                        check_read(u->second.address, n, "constant buffer");
                        (void)memory_->read(u->second.address, scratch.data(), n);
                    }
                    for (const auto& [key, b] : storage_bindings) {
                        const uint64_t off = nvn::storage_desc_offset(nvn::ShaderStage(stage)) +
                                             uint64_t{key.second} * nvn::kStorageBufferDescStride;
                        if (key.first != stage || off + 12 > kCbufBytes) continue;
                        const uint32_t desc[3] = {uint32_t(b.address), uint32_t(b.address >> 32), uint32_t(b.size)};
                        std::memcpy(scratch.data() + off, desc, sizeof(desc));
                        n = std::max(n, off + 12);
                    }
                    if (!debug_overrides().cbufzero)
                        scratch.resize(size_t((std::max<uint64_t>(n, 4) + 255) & ~uint64_t{255}));
                    where = s.cbuf_arena.put(s.device, scratch.data(), scratch.size());
                    ++delta.cbuf_uploads;
                    delta.cbuf_upload_bytes += scratch.size();
                } else if (u != uniform_buffers.end() && u->second.size > 0) {
                    where = upload(s.cbuf_arena, u->second.address, std::min<uint64_t>(u->second.size, kCbufBytes), 0);
                }
                cbuf_buffers.push_back(where.first);
                cbuf_offsets.push_back(uint32_t(where.second));
            }
            if (prog.dynamic_cbufs) {
                // One bind group per chunk combination, kept across lists; the
                // offsets go with SetBindGroup.
                auto [it, inserted] = s.cbuf_groups.try_emplace({prog.groups[stage], cbuf_buffers}, nullptr);
                if (inserted) {
                    std::vector<WGPUBindGroupEntry> entries;
                    for (size_t i = 0; i < cbuf_buffers.size(); ++i) {
                        WGPUBindGroupEntry e = WGPU_BIND_GROUP_ENTRY_INIT;
                        e.binding = prog.banks[stage][i];
                        e.buffer = cbuf_buffers[i];
                        e.size = kCbufBytes;
                        entries.push_back(e);
                    }
                    WGPUBindGroupDescriptor bd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
                    bd.layout = prog.groups[stage];
                    bd.entryCount = entries.size();
                    bd.entries = entries.data();
                    it->second = wgpuDeviceCreateBindGroup(s.device, &bd);
                    ++delta.bind_groups_created;
                } else {
                    ++delta.bind_groups_reused;
                }
                pass_state.bind_group(draw_pass, uint32_t(stage), it->second, cbuf_offsets);
            } else {
                std::vector<WGPUBindGroupEntry> entries;
                for (size_t i = 0; i < cbuf_buffers.size(); ++i) {
                    WGPUBindGroupEntry e = WGPU_BIND_GROUP_ENTRY_INIT;
                    e.binding = prog.banks[stage][i];
                    e.buffer = cbuf_buffers[i];
                    e.offset = cbuf_offsets[i];
                    e.size = kCbufBytes;
                    entries.push_back(e);
                }
                WGPUBindGroupDescriptor bd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
                bd.layout = prog.groups[stage];
                bd.entryCount = entries.size();
                bd.entries = entries.data();
                WGPUBindGroup g = wgpuDeviceCreateBindGroup(s.device, &bd);
                ++delta.bind_groups_created;
                bind_groups.push_back(g);
                pass_state.bind_group(draw_pass, uint32_t(stage), g, {});
            }
        }
        const Program::LayoutVariant* variant = nullptr;
        if (req.unfilterable != 0)
            if (auto v = prog.layout_variants.find(req.unfilterable); v != prog.layout_variants.end())
                variant = &v->second;
        for (int stage = 0; stage < 2; ++stage) {
            WGPUBindGroupLayout layout = prog.groups[2 + stage];
            const uint32_t bits = variant != nullptr ? uint32_t(req.unfilterable >> (32 * stage)) : 0;
            if (bits != 0 && variant->textures[stage] != nullptr)
                layout = variant->textures[stage];
            pass_state.bind_group(draw_pass, uint32_t(2 + stage),
                                  texture_group(prog, stage, layout, bits, draw_pass_depth_read_only ? 0 : sampled_target),
                                  {});
        }
        stream_ranges.assign(st.layout.strides.size(), StreamRange{});
        vertex_slot_of.assign(slots.begin(), slots.end());
        if (merge.active())
            if (const char* why = bind_merged_streams(merge, uint32_t(slots[merge.streams[0]]))) {
                ++delta.skip_reasons[why];
                return false;
            }
        for (uint32_t stream = 0; stream < st.layout.strides.size(); ++stream) {
            auto v = vertex_buffers.find(stream);
            if (v == vertex_buffers.end() || slots[stream] < 0 || merge.member(stream) >= 0)
                continue;
            const uint64_t size = std::min<uint64_t>(v->second.size, 64ull << 20);
            if (size == 0)
                continue;
            WGPUBuffer buffer = nullptr;
            uint64_t offset = 0, bound = (size + 255) & ~uint64_t{255};
            if (auto p = patched_streams.find(stream); p != patched_streams.end()) {
                // Guest bytes with a dispatch's output copied over (patch_vertex_streams).
                buffer = p->second;
                ++delta.compute_vertex_streams;
            } else if (const StorageBuffer* out = dispatch_output(v->second.address, size)) {
                // Vertices a dispatch of this list wrote (skinning, particles).
                buffer = out->buffer;
                offset = v->second.address - out->address;
                bound = std::min(bound, wgpuBufferGetSize(out->buffer) - offset);
                ++delta.compute_vertex_streams;
            } else {
                std::tie(buffer, offset) = upload(s.geometry_arena, v->second.address, size, 1);
            }
            stream_ranges[stream] = {buffer, offset, bound};  // what WebGPU checks reads against
            pass_state.vertex_buffer(draw_pass, uint32_t(slots[stream]), buffer, offset, bound);
        }
        return true;
    };
    // gputime: a draw issued into the open timed draw pass, by program.
    auto count_timed_draw = [&] {
        if (draw_pass_timed >= 0) ++timer.draws[size_t(draw_pass_timed)][s.programs.at(st.program)->hash];
    };

    // Half-resolution effects (State::halfres_for): a listed program's draw renders into a
    // stand-in of half the width and height (cleared to zero, the same
    // pipeline, depth through a stand-in of that size) and is then added onto
    // the real target with a bilinear blit. That is the draw at a lower
    // resolution only where adding later gives the same sum: one colour
    // target, blending with destination factor one and operation add for
    // colour and alpha, source factors that do not read the destination, no
    // depth or stencil test, one sample. Other draws of the program run as
    // usual (counted in Stats::diagnostics).
    nvn::TextureId halfres_id = 0;
    nvn::TextureId halfres_main_id = 0;  // the real target
    std::vector<nvn::TextureId> halfres_colors;  // st.colors of the real draw
    bool halfres_skip = false;  // debug_overrides().halfskip: leave this draw out
    // watch_programs: a draw of a listed program (the scene it belongs to is on screen).
    auto note_watched = [&] {
        if (st.program == 0)
            return;
        if (debug_overrides().firstdraw) {
            if (const auto wit = s.programs.find(st.program); wit != s.programs.end() && !wit->second->drawn) {
                wit->second->drawn = true;
                rt::log(rt::LogLevel::Info, "nvn webgpu: program %u first drawn after present %llu: hash %016llx guest %016llx",
                        unsigned(st.program), (unsigned long long)(s.frame - 1),
                        (unsigned long long)wit->second->hash, (unsigned long long)wit->second->guest_hash);
            }
        }
        if (s.watched_programs.empty())
            return;
        if (const auto wit = s.programs.find(st.program); wit != s.programs.end()) {
            Program& wp = *wit->second;
            if (wp.watched < 0)
                wp.watched = s.watched_for(wp.hash, wp.guest_hash) ? 1 : 0;
            if (wp.watched == 1)
                watched_drawn_.store(true, std::memory_order_relaxed);
        }
    };
    auto begin_halfres = [&]() -> bool {
        if (!s.halfres_on || st.program == 0)
            return false;
        const auto pit = s.programs.find(st.program);
        if (pit == s.programs.end() || !s.halfres_for(pit->second->hash))
            return false;
        if (pit->second->reads_screen_xy) {
            ++delta.diagnostics["half-res: shader reads the screen position"];
            return false;
        }
        size_t slot = 0, targets = 0;
        for (size_t i = 0; i < st.colors.size(); ++i)
            if (st.colors[i] != 0) {
                slot = i;
                ++targets;
            }
        const auto reads_dst = [](nvn::BlendFactor b) {
            return b == nvn::BlendFactor::DstColor || b == nvn::BlendFactor::OneMinusDstColor ||
                   b == nvn::BlendFactor::DstAlpha || b == nvn::BlendFactor::OneMinusDstAlpha ||
                   b == nvn::BlendFactor::SrcAlphaSaturated;
        };
        const FixedState& f = st.fixed;
        auto mt = targets == 1 ? s.find_texture(st.colors[slot]) : s.textures.end();
        const char* why = nullptr;
        if (targets != 1 || slot >= nvn::kMaxColorTargets)
            why = "half-res: not one colour target";
        else if (mt == s.textures.end() || mt->second.attachment == nullptr || mt->second.sample_count != 1 ||
                 has_depth(mt->second.format) || has_stencil(mt->second.format))
            why = "half-res: target not drawable";
        else if (!(f.blend_enable & (1u << slot)) || f.equations[slot].dst_color != nvn::BlendFactor::One ||
                 f.equations[slot].dst_alpha != nvn::BlendFactor::One ||
                 f.equations[slot].color_op != nvn::BlendOp::Add || f.equations[slot].alpha_op != nvn::BlendOp::Add ||
                 reads_dst(f.equations[slot].src_color) || reads_dst(f.equations[slot].src_alpha))
            why = "half-res: blending not additive";
        else if (f.depth_stencil.depth_test || f.depth_stencil.stencil_test || f.alpha_to_coverage)
            why = "half-res: depth, stencil or coverage test";
        if (why != nullptr) {
            ++delta.diagnostics[why];
            return false;
        }
        if (debug_overrides().halfskip) {
            halfres_skip = true;
            return false;
        }
        const nvn::TextureId main_id = st.colors[slot];
        const State::Texture* main = &mt->second;  // stays valid when textures grows (unordered_map)
        end_draw_pass("half-resolution draw");
        if (pending_color.count(main_id))
            flush_clear(main_id);  // the clear goes before what is added later
        halfres_id = s.halfres_target(*main);
        pending_color[halfres_id] = PendingColorClear{};
        halfres_colors = st.colors;
        st.colors.assign(halfres_colors.size(), 0);
        st.colors[slot] = halfres_id;
        halfres_main = main;
        halfres_main_id = main_id;
        return true;
    };
    auto end_halfres = [&] {
        const bool drew = draw_pass != nullptr;
        end_draw_pass("half-resolution draw done");
        st.colors = std::move(halfres_colors);
        halfres_colors.clear();
        const State::Texture* main = halfres_main;
        halfres_main = nullptr;
        pending_color.erase(halfres_id);  // skipped draw: the stand-in was never cleared
        auto ht = s.find_texture(halfres_id);
        if (!drew || ht == s.textures.end() || ht->second.sampled == nullptr)
            return;
        // The add pass writes the real target like any draw pass would.
        if (auto mt = s.find_texture(halfres_main_id); mt != s.textures.end())
            mt->second.gpu_written = true;
        WGPURenderPipeline pipeline = s.blit_pipeline(main->format, /*additive=*/true);
        BlitUniform u{};
        // Framebuffer [0, 1] of the real target -> the whole stand-in (the
        // draws were scaled to its size, fill_x/fill_y in record_list).
        u.src[2] = 1.0f;
        u.src[3] = 1.0f;
        WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
        bd.label = pw::string_view("nvn half-res params");
        bd.size = sizeof(BlitUniform);
        bd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
        WGPUBuffer ub = s.list_buffer(&bd);
        queue_write_buffer(s.queue, ub, 0, &u, sizeof(u));
        temp_buffers.push_back(ub);
        WGPUBindGroupEntry e[3] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
        e[0].binding = 0;
        e[0].textureView = ht->second.sampled;
        e[1].binding = 1;
        e[1].sampler = s.blit_samplers[1];  // linear
        e[2].binding = 2;
        e[2].buffer = ub;
        e[2].size = sizeof(BlitUniform);
        WGPUBindGroupDescriptor bgd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        bgd.layout = s.blit_layout;
        bgd.entryCount = 3;
        bgd.entries = e;
        WGPUBindGroup group = wgpuDeviceCreateBindGroup(s.device, &bgd);
        bind_groups.push_back(group);
        WGPURenderPassColorAttachment ca = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
        ca.view = main->attachment;
        ca.loadOp = WGPULoadOp_Load;
        ca.storeOp = WGPUStoreOp_Store;
        WGPURenderPassDescriptor rp = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
        rp.label = pass_labels ? pass_label("half-res add", main, 0, 0) : pw::string_view("nvn half-res add");
        rp.colorAttachmentCount = 1;
        rp.colorAttachments = &ca;
        WGPUPassTimestampWrites rp_time;
        rp.timestampWrites = time_pass(kRenderPass, &rp_time, pass_target("half-res add", main, 0, 0));
        s.note_attachments(rp, delta);
        WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &rp);
        wgpuRenderPassEncoderSetPipeline(pass, pipeline);
        wgpuRenderPassEncoderSetBindGroup(pass, 0, group, 0, nullptr);
        wgpuRenderPassEncoderSetViewport(pass, 0.0f, 0.0f, float(main->width()), float(main->height()), 0.0f, 1.0f);
        // Only where the draw could have written (its scissor).
        const Region r = region_of(*main);
        wgpuRenderPassEncoderSetScissorRect(pass, r.x, r.y, r.width, r.height);
        wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
        wgpuRenderPassEncoderEnd(pass);
        wgpuRenderPassEncoderRelease(pass);
        ++delta.render_passes;
        ++delta.pass_reasons["own pass: half-resolution add"];
        // Pass diagnosis: the add pass samples the stand-in and loads the real target.
        note_event({halfres_id, halfres_main_id}, false);
        ++delta.diagnostics["half-res: draws at half resolution"];
    };
    struct HalfResScope {
        std::function<void()> done;
        ~HalfResScope() {
            if (done) done();
        }
    };

    // Issues a prepared draw for instances [first_instance, first_instance +
    // instance_count) as WebGPU draws (issue(first, count)): one, or with
    // divisors above 1 one per run of g instances (g = gcd of those divisors)
    // that read the same element of every such stream, whose buffer is
    // rebound at that element (stride 0 in the pipeline). Streams that would
    // be read past their bound range are checked first (WebGPU would reject
    // the whole command buffer); vertices = first + count of a non-indexed
    // draw (0 = indexed: vertex streams are not checked). False = skipped.
    auto instanced_runs = [&](uint32_t first_instance, uint32_t instance_count, uint64_t vertices,
                              const std::function<void(uint32_t, uint32_t)>& issue) -> bool {
        const nvn::CmdSetVertexLayout& l = st.layout;
        std::vector<uint64_t> last(l.strides.size(), 0);  // bytes the attributes read from an element
        for (const nvn::VertexAttrib& a : l.attribs)
            if (a.stream < last.size())
                last[a.stream] = std::max<uint64_t>(last[a.stream], uint64_t(a.offset) + vertex_format_size(a.format));
        uint32_t run = 0;
        for (size_t i = 0; i < l.strides.size(); ++i) {
            if (last[i] == 0)
                continue;  // no attribute reads the stream
            const uint32_t d = stream_divisor(l, i);
            const uint64_t elements = d == 0 ? vertices : d == 1 ? uint64_t(first_instance) + instance_count
                                                               : uint64_t(first_instance) + 1;
            if (d > 1) {
                run = std::gcd(run, d);
            } else if (elements != 0 && (elements - 1) * l.strides[i] + last[i] > stream_ranges[i].size) {
                ++delta.skip_reasons[d == 0 ? "draw: vertex stream shorter than the draw"
                                            : "draw: instanced stream shorter than the instances"];
                return false;
            }
        }
        if (run == 0) {
            issue(first_instance, instance_count);
            return true;
        }
        if (instance_count / run > 65536) {
            ++delta.skip_reasons["draw: too many instance runs (vertex stream divisor)"];
            return false;
        }
        for (uint32_t done = 0; done < instance_count; done += run) {
            for (size_t i = 0; i < l.strides.size(); ++i) {
                const uint32_t d = stream_divisor(l, i);
                if (d <= 1 || last[i] == 0)
                    continue;
                const uint64_t at = (uint64_t(first_instance) + done / d) * l.strides[i];
                if (at + last[i] > stream_ranges[i].size || at % 4 != 0) {
                    ++delta.skip_reasons[at % 4 != 0 ? "draw: instanced stream element not 4-byte aligned"
                                                     : "draw: instanced stream shorter than the instances"];
                    return done != 0;
                }
                pass_state.vertex_buffer(draw_pass, uint32_t(vertex_slot_of[i]), stream_ranges[i].buffer,
                                         stream_ranges[i].offset + at, stream_ranges[i].size - at);
            }
            issue(first_instance + done, std::min(run, instance_count - done));
        }
        return true;
    };

    std::vector<WGPUTexture> temp_textures;  // copies within one subresource
    std::vector<WGPUTextureView> temp_views;  // blit sources/targets at other levels

    // Draws `from_view` (the uniform's source rectangle, 0..1) into the
    // destination rectangle of `to_view` in a pass of its own.
    auto blit = [&](WGPUTextureView from_view, const BlitUniform& u, State::Texture& dst, nvn::TextureId dst_id,
                    WGPUTextureView to_view, const int32_t rect[4], bool linear) {
        WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
        bd.label = pw::string_view("nvn blit params");
        bd.size = sizeof(BlitUniform);
        bd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
        WGPUBuffer ub = s.list_buffer(&bd);
        queue_write_buffer(s.queue, ub, 0, &u, sizeof(u));
        temp_buffers.push_back(ub);

        WGPURenderPipeline pipeline = s.blit_pipeline(dst.format);
        WGPUBindGroupEntry e[3] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
        e[0].binding = 0;
        e[0].textureView = from_view;
        e[1].binding = 1;
        e[1].sampler = s.blit_samplers[linear ? 1 : 0];
        e[2].binding = 2;
        e[2].buffer = ub;
        e[2].size = sizeof(BlitUniform);
        WGPUBindGroupDescriptor bgd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        bgd.layout = s.blit_layout;
        bgd.entryCount = 3;
        bgd.entries = e;
        WGPUBindGroup group = wgpuDeviceCreateBindGroup(s.device, &bgd);
        bind_groups.push_back(group);

        WGPURenderPassColorAttachment ca = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
        ca.view = to_view;
        ca.loadOp = WGPULoadOp_Load;
        ca.storeOp = WGPUStoreOp_Store;
        WGPURenderPassDescriptor rp = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
        rp.label = pass_labels ? pass_label("blit", &dst, dst_id, 0) : pw::string_view("nvn blit");
        rp.colorAttachmentCount = 1;
        rp.colorAttachments = &ca;
        WGPUPassTimestampWrites rp_time;
        rp.timestampWrites = time_pass(kRenderPass, &rp_time, pass_target("blit", &dst, dst_id, 0));
        s.note_attachments(rp, delta);
        WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &rp);
        wgpuRenderPassEncoderSetPipeline(pass, pipeline);
        wgpuRenderPassEncoderSetBindGroup(pass, 0, group, 0, nullptr);
        wgpuRenderPassEncoderSetViewport(pass, float(rect[0]), float(rect[1]), float(rect[2]), float(rect[3]),
                                         0.0f, 1.0f);
        wgpuRenderPassEncoderSetScissorRect(pass, uint32_t(rect[0]), uint32_t(rect[1]), uint32_t(rect[2]),
                                            uint32_t(rect[3]));
        wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
        wgpuRenderPassEncoderEnd(pass);
        wgpuRenderPassEncoderRelease(pass);
        ++delta.render_passes;
        ++delta.pass_reasons["own pass: blit"];
    };

    // nvnCommandBufferCopyTextureToTexture: a plain copy when the regions have
    // the same size and the formats are copy-compatible, otherwise a blit (a
    // draw sampling the source region) into the destination region.
    // nullptr = done, otherwise the reason it was skipped.
    auto copy_texture = [&](const nvn::CmdCopyTextureToTexture& guest) -> const char* {
        auto si = s.find_texture(guest.src.texture), di = s.find_texture(guest.dst.texture);
        if (si == s.textures.end() || di == s.textures.end() || si->second.texture == nullptr ||
            di->second.texture == nullptr)
            return "copy texture: unknown or unsupported texture";
        State::Texture& src = si->second;
        State::Texture& dst = di->second;
        if (src.sample_count != 1 || dst.sample_count != 1)
            return "copy texture: multisampled texture";
        // Region inside the level; z counts slices (3D) or layers after the view's base layer.
        auto inside = [](const State::Texture& t, const nvn::CmdCopyTextureToTexture::Side& r) {
            if (r.level >= std::max(t.desc.levels, 1u) || r.x < 0 || r.y < 0 || r.z < 0 || r.width <= 0 ||
                r.height <= 0 || r.depth <= 0)
                return false;
            const uint64_t w = std::max(t.desc.width >> r.level, 1u), h = std::max(t.desc.height >> r.level, 1u);
            const uint64_t d = t.is_3d() ? std::max(t.desc.depth >> r.level, 1u) : std::max(t.desc.depth, 1u);
            return uint64_t(r.x) + uint64_t(r.width) <= w && uint64_t(r.y) + uint64_t(r.height) <= h &&
                   uint64_t(r.z) + (t.is_3d() ? 0 : r.layer) + uint64_t(r.depth) <= d;
        };
        if (!inside(src, guest.src) || !inside(dst, guest.dst))
            return "copy texture: region outside the texture";
        // From here on in host texels: a scaled render target's regions map
        // to its size (State::render_scale); two sides that end up of
        // different sizes take the blit.
        // A scaled source into a destination that holds nothing yet: the
        // destination is scaled as well, so the copy stays a plain copy
        // (depth/stencil copies cannot take the blit).
        if (src.scaled && !dst.scaled && s.scale_refusal(dst) == nullptr && s.scale_target(dst) &&
            dst.texture == nullptr)
            return "copy texture: unknown or unsupported texture";
        nvn::CmdCopyTextureToTexture c = guest;
        if (src.scaled || dst.scaled) {
            auto to_host = [](const State::Texture& t, nvn::CmdCopyTextureToTexture::Side& r) {
                const int64_t x0 = t.host_x(r.x, r.level), x1 = t.host_x(int64_t{r.x} + r.width, r.level);
                const int64_t y0 = t.host_y(r.y, r.level), y1 = t.host_y(int64_t{r.y} + r.height, r.level);
                r.x = int32_t(x0);
                r.y = int32_t(y0);
                r.width = int32_t(std::max<int64_t>(x1 - x0, 1));
                r.height = int32_t(std::max<int64_t>(y1 - y0, 1));
            };
            to_host(src, c.src);
            to_host(dst, c.dst);
            static const std::string what = "render scale: copies with a scaled texture";
            ++delta.diagnostics[what];
        }
        // Contents that so far only exist in the pool storage go up first.
        if (!src.gpu_written) {
            check_texture(src, "texture copy source");
            s.upload_from_pool(src, memory_, presents_seen, &delta);
        }
        auto origin_z = [](const State::Texture& t, const nvn::CmdCopyTextureToTexture::Side& r) {
            return uint32_t(r.z) + (t.is_3d() ? 0 : r.layer);
        };
        const bool same_size =
            c.src.width == c.dst.width && c.src.height == c.dst.height && c.src.depth == c.dst.depth;
        const bool mirror = (c.flags & (nvn::kCopyFlagMirrorX | nvn::kCopyFlagMirrorY | nvn::kCopyFlagMirrorZ)) != 0;
        const bool compatible = pw::linear_variant(src.format) == pw::linear_variant(dst.format);
        const bool ds = has_depth(src.format) || has_stencil(src.format);

        // A copy of the source region into a fresh texture (same format):
        // copies within one subresource must not overlap in WebGPU.
        auto detach = [&](WGPUTexture* tex, nvn::CmdCopyTextureToTexture::Side* side) {
            WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
            td.label = pw::string_view("nvn copy temp");
            td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst | WGPUTextureUsage_CopySrc;
            td.dimension = src.is_3d() ? WGPUTextureDimension_3D : WGPUTextureDimension_2D;
            td.size = {uint32_t(c.src.width), uint32_t(c.src.height), uint32_t(c.src.depth)};
            td.format = src.format;
            *tex = gpu_create_texture(s.device, &td);
            temp_textures.push_back(*tex);
            WGPUTexelCopyTextureInfo from = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
            from.texture = src.texture;
            from.mipLevel = c.src.level;
            from.origin = {uint32_t(c.src.x), uint32_t(c.src.y), origin_z(src, c.src)};
            WGPUTexelCopyTextureInfo to = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
            to.texture = *tex;
            wgpuCommandEncoderCopyTextureToTexture(encoder, &from, &to, &td.size);
            *side = c.src;
            side->level = side->layer = 0;
            side->x = side->y = side->z = 0;
        };
        const bool same_subresource = &src == &dst && c.src.level == c.dst.level &&
                                      origin_z(src, c.src) < origin_z(dst, c.dst) + uint32_t(c.dst.depth) &&
                                      origin_z(dst, c.dst) < origin_z(src, c.src) + uint32_t(c.src.depth);

        if (same_size && !mirror && compatible) {
            if (is_compressed(src.format)) {
                // Native BCn/ASTC: whole blocks only (ASTC decoded on the CPU is RGBA8 and copies freely).
                const auto info = nvn::format_info(src.desc.format);
                const int32_t bw = info ? int32_t(info->block_width) : 4, bh = info ? int32_t(info->block_height) : 4;
                if (c.src.x % bw || c.dst.x % bw || c.src.width % bw || c.src.y % bh || c.dst.y % bh ||
                    c.src.height % bh)
                    return "copy texture: compressed region not on whole blocks";
            }
            if (ds && (c.src.x != 0 || c.src.y != 0 || c.dst.x != 0 || c.dst.y != 0 ||
                       uint32_t(c.src.width) != src.width(c.src.level) ||
                       uint32_t(c.src.height) != src.height(c.src.level)))
                return "copy texture: partial depth/stencil copy";
            WGPUTexture from_tex = src.texture;
            nvn::CmdCopyTextureToTexture::Side from_side = c.src;
            if (same_subresource)
                detach(&from_tex, &from_side);
            WGPUTexelCopyTextureInfo from = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
            from.texture = from_tex;
            from.mipLevel = from_side.level;
            from.origin = {uint32_t(from_side.x), uint32_t(from_side.y),
                           from_tex == src.texture ? origin_z(src, from_side) : 0u};
            WGPUTexelCopyTextureInfo to = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
            to.texture = dst.texture;
            to.mipLevel = c.dst.level;
            to.origin = {uint32_t(c.dst.x), uint32_t(c.dst.y), origin_z(dst, c.dst)};
            const WGPUExtent3D extent = {uint32_t(c.src.width), uint32_t(c.src.height), uint32_t(c.src.depth)};
            wgpuCommandEncoderCopyTextureToTexture(encoder, &from, &to, &extent);
            dst.gpu_written = true;
            ++delta.texture_copies;
            return nullptr;
        }

        // Blit: one 2D slice, colour formats, a renderable destination.
        if (ds || has_depth(dst.format) || has_stencil(dst.format)) {
            if (src.scaled != dst.scaled) {
                static std::atomic<int> logged{0};
                if (logged.fetch_add(1) < 16) {
                    const State::Texture& u = src.scaled ? dst : src;
                    const char* why = s.scale_refusal(u);
                    rt::log(rt::LogLevel::Info,
                            "nvn webgpu: render scale: depth/stencil copy %ux%u%s -> %ux%u%s skipped (unscaled: %s)",
                            src.desc.width, src.desc.height, src.scaled ? " scaled" : "", dst.desc.width,
                            dst.desc.height, dst.scaled ? " scaled" : "", why != nullptr ? why : "-");
                }
            }
            return "copy texture: scaled or converting depth/stencil copy";
        }
        if (src.is_3d() || dst.is_3d() || c.src.depth != 1 || c.dst.depth != 1)
            return "copy texture: scaled copy of 3D textures or several layers";
        if (is_bc(dst.format) || dst.attachment == nullptr)
            return "copy texture: destination not renderable";
        WGPUTexture from_tex = src.texture;
        nvn::CmdCopyTextureToTexture::Side from_side = c.src;
        uint32_t from_w = src.width(c.src.level), from_h = src.height(c.src.level);
        if (same_subresource) {
            detach(&from_tex, &from_side);
            from_w = uint32_t(c.src.width);
            from_h = uint32_t(c.src.height);
        }
        WGPUTextureViewDescriptor vd = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
        vd.format = src.format;
        vd.dimension = WGPUTextureViewDimension_2D;
        vd.baseMipLevel = from_side.level;
        vd.mipLevelCount = 1;
        vd.baseArrayLayer = from_tex == src.texture ? origin_z(src, from_side) : 0;
        vd.arrayLayerCount = 1;
        WGPUTextureView from_view = wgpuTextureCreateView(from_tex, &vd);
        temp_views.push_back(from_view);
        vd.format = dst.format;
        vd.baseMipLevel = c.dst.level;
        vd.baseArrayLayer = origin_z(dst, c.dst);
        WGPUTextureView to_view = wgpuTextureCreateView(dst.texture, &vd);
        temp_views.push_back(to_view);

        BlitUniform u{};
        u.src[0] = float(from_side.x) / float(from_w);
        u.src[1] = float(from_side.y) / float(from_h);
        u.src[2] = float(from_side.x + from_side.width) / float(from_w);
        u.src[3] = float(from_side.y + from_side.height) / float(from_h);
        if (c.flags & nvn::kCopyFlagMirrorX) std::swap(u.src[0], u.src[2]);
        if (c.flags & nvn::kCopyFlagMirrorY) std::swap(u.src[1], u.src[3]);
        const int32_t rect[4] = {c.dst.x, c.dst.y, c.dst.width, c.dst.height};
        blit(from_view, u, dst, c.dst.texture, to_view, rect, (c.flags & nvn::kCopyFlagLinearFilter) != 0);
        dst.gpu_written = true;
        ++delta.texture_copies;
        ++delta.blits;
        return nullptr;
    };
    // Scales a render target (State::scale_target). A colour target that
    // already holds something keeps it: the old texture is drawn into the
    // scaled one (filtered, level 0 only) and freed after the submit. Depth
    // and stencil targets with contents stay unscaled (no depth blit yet).
    // Whether scale_with_contents can scale a target that already holds something.
    // Why scale_with_contents cannot copy a target's contents over (nullptr = it can).
    auto contents_refusal = [&](const State::Texture& t) -> const char* {
        if (const char* why = s.scale_refusal(t, true))
            return why;
        if (has_depth(t.format) || has_stencil(t.format))
            return "depth or stencil with contents";
        if (t.sample_count != 1)
            return "multisampled with contents";
        if (t.lazy || t.texture == nullptr)
            return "not created";
        if (t.plan.td.mipLevelCount != 1)
            return "levels with contents";
        if (is_bc(t.format))
            return "BC";
        return nullptr;
    };
    auto scalable_with_contents = [&](const State::Texture& t) {
        return s.scale_refusal(t) != nullptr && contents_refusal(t) == nullptr;
    };
    auto scale_with_contents = [&](nvn::TextureId id) {
        auto it = s.textures.find(id);
        if (id == 0 || it == s.textures.end())
            return;
        State::Texture& t = it->second;
        if (t.scaled || s.scale_target(t))
            return;
        const char* why = s.scale_refusal(t);
        const bool can_copy = scalable_with_contents(t);
        static std::atomic<int> logged{0};
        if (!can_copy) {
            if (logged.fetch_add(1) < 16)
                rt::log(rt::LogLevel::Info, "nvn webgpu: render scale: target %ux%u%s stays unscaled (%s)",
                        t.desc.width, t.desc.height, has_depth(t.format) || has_stencil(t.format) ? " depth" : "",
                        why != nullptr ? why : "-");
            return;
        }
        end_draw_pass("render target scaled");
        if (!t.gpu_written)
            s.upload_from_pool(t, memory_, presents_seen, &delta);
        // Keep the old texture alive until the submit; its views go now.
        WGPUTexture old = t.texture;
        const uint32_t old_w = t.plan.td.size.width, old_h = t.plan.td.size.height;
        s.texture_live -= std::min(s.texture_live, texture_bytes(t.plan.td));
        t.texture = nullptr;
        WGPUTextureViewDescriptor vd = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
        vd.format = t.format;
        vd.dimension = WGPUTextureViewDimension_2D;
        vd.mipLevelCount = 1;
        vd.arrayLayerCount = 1;
        WGPUTextureView from_view = wgpuTextureCreateView(old, &vd);
        temp_views.push_back(from_view);
        temp_textures.push_back(old);
        if (!s.scale_target(t, true) || t.texture == nullptr) {
            rt::log(rt::LogLevel::Warn, "nvn webgpu: render scale: target %ux%u lost its contents",
                    t.desc.width, t.desc.height);
            return;
        }
        WGPUTextureView to_view = wgpuTextureCreateView(t.texture, &vd);
        temp_views.push_back(to_view);
        BlitUniform u{};
        u.src[2] = u.src[3] = 1.0f;
        const int32_t rect[4] = {0, 0, int32_t(t.width()), int32_t(t.height())};
        blit(from_view, u, t, id, to_view, rect, true);
        static const std::string what = "render scale: targets scaled with their contents";
        ++delta.diagnostics[what];
        if (logged.fetch_add(1) < 16)
            rt::log(rt::LogLevel::Info, "nvn webgpu: render scale: target %ux%u scaled with its contents (%s), %ux%u",
                    old_w, old_h, why, t.width(), t.height());
    };
    // One dispatch; false = skipped (no valid compute program).
    auto dispatch = [&](const nvn::CmdDispatchCompute& c) -> bool {
        pending_astc = pending_3d = 0;
        auto pit = s.programs.find(st.compute_program);
        if (pit == s.programs.end() || !pit->second->valid || pit->second->compute_pipeline == nullptr)
            return false;
        const Program& prog = *pit->second;
        // Indirect arguments are read from guest memory as it was before the list:
        // counts a dispatch earlier in the same list wrote are not seen (LGP does
        // not call DispatchComputeIndirect, docs/analysis/03-grafik.md).
        uint32_t groups[3] = {c.groups[0], c.groups[1], c.groups[2]};
        if (c.indirect_address != 0)
            check_read(c.indirect_address, 12, "indirect dispatch arguments");
        if (c.indirect_address != 0 && (memory_ == nullptr || !memory_->read(c.indirect_address, groups, 12))) {
            rt::log(rt::LogLevel::Warn, "nvn webgpu: indirect dispatch arguments 0x%llx not readable",
                    (unsigned long long)c.indirect_address);
            return false;
        }
        if (groups[0] == 0 || groups[1] == 0 || groups[2] == 0)
            return true;  // nothing to run
        if (groups[0] > 65535 || groups[1] > 65535 || groups[2] > 65535) {
            rt::log(rt::LogLevel::Warn, "nvn webgpu: dispatch %ux%ux%u exceeds WebGPU's 65535 groups per dimension",
                    groups[0], groups[1], groups[2]);
            return false;
        }
        constexpr int kStage = int(nvn::ShaderStage::Compute);
        // State::compute_first: ahead of the list's other commands unless it
        // samples a texture they write or uses a storage range they use.
        bool first = s.compute_first;
        for (uint32_t slot : prog.texture_slots[kStage]) {
            auto b = bound_textures.find({kStage, slot});
            if (b != bound_textures.end() && written_textures.count(b->second.texture))
                first = false;
        }
        for (uint32_t k = 0; k < wgsl::kGlobalSlots && first; ++k) {
            auto b = storage_bindings.find({kStage, k});
            if (b == storage_bindings.end())
                continue;
            auto known = storage_index.find({b->second.address, b->second.size});
            if (known != storage_index.end() && storage[known->second].main_use)
                first = false;
            // A range overlapping newer output of a dispatch kept in order:
            // its bytes are copied over in `encoder` (copy_dispatch_output).
            const uint64_t own_order = known != storage_index.end() ? storage[known->second].write_order : 0;
            if (s.patch_dispatch_output)
                for (const StorageBuffer& o : storage)
                    if (o.written && o.main_write && o.write_order > own_order &&
                        b->second.address < o.address + o.size && o.address < b->second.address + b->second.size)
                        first = false;
        }
        // An in-order dispatch records into `encoder`, which must not have
        // a pass open; one ahead records elsewhere.
        if (!first || !s.keep_pass_on_dispatch)
            end_draw_pass(first ? "dispatch (ahead)" : "dispatch (in order)");
        if (!first) {
            flush_clears("dispatch (in order)");
            note_event({}, true);
        }
        if (first && compute_encoder == nullptr)
            compute_encoder = wgpuDeviceCreateCommandEncoder(s.device, nullptr);
        WGPUCommandEncoder enc = first ? compute_encoder : encoder;
        compute_used |= first;
        ++(first ? delta.dispatches_first : delta.dispatches_in_order);
        dispatch_in_order |= !first;
        // Group 0: constant buffers; the driver bank carries the storage
        // buffer descriptors (abi.hpp) over what the game bound there.
        std::vector<WGPUBindGroupEntry> cbufs;
        for (uint32_t bank : prog.banks[kStage]) {
            scratch.assign(size_t(kCbufBytes), 0);
            if (auto u = uniform_buffers.find({kStage, bank}); u != uniform_buffers.end()) {
                const uint64_t n = std::min<uint64_t>(u->second.size, kCbufBytes);
                check_read(u->second.address, n, "constant buffer");
                if (n > 0 && (memory_ == nullptr || !memory_->read(u->second.address, scratch.data(), n)))
                    rt::log(rt::LogLevel::Warn, "nvn webgpu: buffer 0x%llx+0x%llx not readable",
                            (unsigned long long)u->second.address, (unsigned long long)n);
            }
            if (bank == nvn::kDriverConstantBank) {
                for (const auto& [key, b] : storage_bindings) {
                    const uint64_t off =
                        nvn::kStorageBufferDescOffsetCompute + uint64_t{key.second} * nvn::kStorageBufferDescStride;
                    if (key.first != kStage || off + 12 > kCbufBytes) continue;
                    const uint32_t desc[3] = {uint32_t(b.address), uint32_t(b.address >> 32), uint32_t(b.size)};
                    std::memcpy(scratch.data() + off, desc, sizeof(desc));
                }
                // Constant buffers the shader reads through global memory (abi.hpp).
                for (const auto& [key, u] : uniform_buffers) {
                    const uint32_t block = key.second - nvn::kUniformBufferBankBase;
                    if (key.first != kStage || key.second < nvn::kUniformBufferBankBase ||
                        block >= nvn::kComputeUniformDescCount || u.size == 0)
                        continue;
                    const uint64_t off = nvn::kComputeUniformDescOffset + uint64_t{block} * 16;
                    const uint32_t desc[3] = {uint32_t(u.address), uint32_t(u.address >> 32), uint32_t(u.size)};
                    std::memcpy(scratch.data() + off, desc, sizeof(desc));
                }
            }
            const auto where = s.cbuf_arena.put(s.device, scratch.data(), scratch.size());
            WGPUBindGroupEntry e = WGPU_BIND_GROUP_ENTRY_INIT;
            e.binding = bank;
            e.buffer = where.first;
            e.offset = where.second;
            e.size = kCbufBytes;
            ++delta.cbuf_uploads;
            delta.cbuf_upload_bytes += kCbufBytes;
            cbufs.push_back(e);
        }
        WGPUBindGroupDescriptor bd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        bd.layout = prog.groups[0];
        bd.entryCount = cbufs.size();
        bd.entries = cbufs.data();
        WGPUBindGroup g0 = wgpuDeviceCreateBindGroup(s.device, &bd);
        bind_groups.push_back(g0);
        WGPUBindGroup g1 = texture_group(prog, kStage, prog.groups[1]);
        // Group 2: storage buffer k in global memory slot k, plus the slot table.
        // WebGPU rejects one writable buffer in two bindings of a dispatch
        // (Dawn validates it, wgpu-native did not): a range bound twice is
        // bound once (the shader picks the first slot holding an address
        // anyway), unused slots get distinct dummies.
        std::vector<WGPUBindGroupEntry> gmem;
        WGPUBuffer probe_buffer = nullptr;
        std::map<uint32_t, uint32_t> moved;  // Program::alias_pipelines
        if (prog.global_memory) {
            // Constant buffer windows (wgsl.h); unused entries get the zero buffer.
            WGPUBindGroupEntry constant_entries[wgsl::kConstantSlots];
            for (uint32_t j = 0; j < wgsl::kConstantSlots; ++j) {
                constant_entries[j] = WGPU_BIND_GROUP_ENTRY_INIT;
                constant_entries[j].binding = wgsl::kConstantSlotBinding + j;
                constant_entries[j].buffer = s.zero_cbuf;
                constant_entries[j].size = kCbufBytes;
            }
            uint32_t table[(wgsl::kGlobalSlots + wgsl::kConstantSlots) * 4] = {};
            std::vector<WGPUBuffer> used;
            std::vector<uint32_t> used_slot;  // the slot holding used[i]
            for (uint32_t k = 0; k < wgsl::kGlobalSlots; ++k) {
                WGPUBindGroupEntry e = WGPU_BIND_GROUP_ENTRY_INIT;
                e.binding = k;
                auto b = storage_bindings.find({kStage, k});
                const bool bound = b != storage_bindings.end() && b->second.size > 0 &&
                                   b->second.size <= kMaxStorageBytes;
                const auto first_use =
                    bound && storage_index.count({b->second.address, b->second.size})
                        ? std::find(used.begin(), used.end(),
                                    storage[storage_index.at({b->second.address, b->second.size})].buffer)
                        : used.end();
                const bool twice = first_use != used.end();
                // A slot the shader reads directly (Program::static_slots) reads the other one.
                if (twice && std::count(prog.static_slots.begin(), prog.static_slots.end(), k))
                    moved[k] = used_slot[size_t(first_use - used.begin())];
                if (bound && !twice) {
                    StorageBuffer& sb = storage_buffer(b->second.address, b->second.size);
                    if (s.patch_dispatch_output)
                        copy_dispatch_output(sb, enc);
                    used.push_back(sb.buffer);
                    used_slot.push_back(k);
                    sb.written = true;
                    sb.write_order = ++storage_writes;
                    if (!first)
                        sb.main_use = sb.main_write = true;
                    dispatch_binds_storage = true;
                    if (diag)
                        dispatched.emplace_back(sb.address, sb.size);
                    e.buffer = sb.buffer;
                    e.size = wgpuBufferGetSize(sb.buffer);
                    table[4 * k] = uint32_t(sb.address);
                    table[4 * k + 1] = uint32_t(sb.address >> 32);
                    table[4 * k + 2] = uint32_t(sb.size);
                } else {
                    if (b != storage_bindings.end() && b->second.size > kMaxStorageBytes)
                        rt::log(rt::LogLevel::Warn, "nvn webgpu: storage buffer %u (0x%llx bytes) too large, unbound",
                                k, (unsigned long long)b->second.size);
                    e.buffer = s.dummy_storage(k);
                    e.size = 16;
                }
                gmem.push_back(e);
            }
            // Constant buffers read through global memory (c0[kComputeUniformDescOffset],
            // abi.hpp) go to the constant entries of the table (wgsl.h), unless a
            // storage slot's range already holds them. Global loads have no
            // bounds: shaders read past the bound size (LGP: within 64 KiB of the
            // base), so an entry maps a 64 KiB window, as far as it is mapped.
            // Blocks the shader reads directly first, in their own windows
            // (Program::static_blocks); then the rest in the free ones. A
            // static block that is unbound or inside a storage slot leaves
            // its window to the others: the shader's direct reads check the
            // window's range (gmem_addr_at) and read 0 there, as a search would.
            bool window_used[wgsl::kConstantSlots] = {};
            std::vector<std::pair<std::pair<int, uint32_t>, const nvn::CmdBindUniformBuffer*>> blocks;  // ((window, bank), u)
            for (const auto& [key, u] : uniform_buffers) {
                if (key.first != kStage || key.second < nvn::kUniformBufferBankBase ||
                    key.second - nvn::kUniformBufferBankBase >= nvn::kComputeUniformDescCount || u.size == 0)
                    continue;
                const uint32_t block = key.second - nvn::kUniformBufferBankBase;
                const auto st_it = std::find(prog.static_blocks.begin(), prog.static_blocks.end(), block);
                const int fixed = st_it == prog.static_blocks.end() ? -1 : int(st_it - prog.static_blocks.begin());
                blocks.push_back({{fixed < 0 ? int(wgsl::kConstantSlots) : fixed, key.second}, &u});
            }
            std::sort(blocks.begin(), blocks.end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });
            for (const auto& [where_key, up] : blocks) {
                const nvn::CmdBindUniformBuffer& u = *up;
                const uint32_t block = where_key.second - nvn::kUniformBufferBankBase;
                const bool fixed = where_key.first < int(wgsl::kConstantSlots);
                int covered = -1;
                for (uint32_t k = 0; k < wgsl::kGlobalSlots && covered < 0; ++k) {
                    const uint64_t a = table[4 * k] | uint64_t{table[4 * k + 1]} << 32;
                    if (table[4 * k + 2] != 0 && u.address >= a && u.address + u.size <= a + table[4 * k + 2])
                        covered = int(k);
                }
                if (covered >= 0) {
                    if (fixed)
                        moved[wgsl::kStaticUniformId + block] = uint32_t(covered);
                    continue;
                }
                uint32_t constant = fixed ? uint32_t(where_key.first) : 0;
                while (!fixed && constant < wgsl::kConstantSlots && window_used[constant]) ++constant;
                if (constant == wgsl::kConstantSlots) {
                    ++delta.skip_reasons["dispatch: more constant buffers in global memory than table entries"];
                    break;
                }
                window_used[constant] = true;
                uint64_t window = std::min<uint64_t>(u.size, kCbufBytes);
                if (memory_ != nullptr) {
                    uint8_t byte = 0;
                    for (uint64_t n = kCbufBytes; n > window; n = std::max(window, n / 2))
                        if (memory_->read(u.address + n - 1, &byte, 1)) {
                            window = n;
                            break;
                        }
                }
                scratch.assign(size_t(kCbufBytes), 0);
                check_read(u.address, window, "constant buffer (global memory)");
                if (memory_ == nullptr || !memory_->read(u.address, scratch.data(), window))
                    rt::log(rt::LogLevel::Warn, "nvn webgpu: buffer 0x%llx+0x%llx not readable",
                            (unsigned long long)u.address, (unsigned long long)window);
                const auto where = s.cbuf_arena.put(s.device, scratch.data(), scratch.size());
                const uint32_t k = wgsl::kGlobalSlots + constant;
                constant_entries[constant].buffer = where.first;
                constant_entries[constant].offset = where.second;
                table[4 * k] = uint32_t(u.address);
                table[4 * k + 1] = uint32_t(u.address >> 32);
                table[4 * k + 2] = uint32_t(window);
                ++delta.compute_uniform_slots;
            }
            for (const auto& [key, b] : storage_bindings)
                if (key.first == kStage && key.second >= wgsl::kGlobalSlots && b.size > 0)
                    rt::log(rt::LogLevel::Warn, "nvn webgpu: storage buffer %u bound, the shader reaches only %u",
                            key.second, wgsl::kGlobalSlots);
            WGPUBufferDescriptor td = WGPU_BUFFER_DESCRIPTOR_INIT;
            td.label = pw::string_view("nvn global memory slots");
            td.size = sizeof(table);
            td.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
            WGPUBuffer tab = s.list_buffer(&td);
            temp_buffers.push_back(tab);
            queue_write_buffer(s.queue, tab, 0, table, sizeof(table));
            WGPUBindGroupEntry e = WGPU_BIND_GROUP_ENTRY_INIT;
            e.binding = wgsl::kGlobalSlots;
            e.buffer = tab;
            e.size = sizeof(table);
            gmem.push_back(e);
            gmem.insert(gmem.end(), std::begin(constant_entries), std::end(constant_entries));
            if (prog.gmem_probe) {
                // Counters start zeroed (WebGPU clears new buffers).
                WGPUBufferDescriptor pd = WGPU_BUFFER_DESCRIPTOR_INIT;
                pd.label = pw::string_view("nvn compute probe");
                pd.size = wgsl::kGmemProbeWords * 4;
                pd.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc;
                WGPUBuffer pb = gpu_create_buffer(s.device, &pd);
                temp_buffers.push_back(pb);
                probe_buffer = pb;
                WGPUBindGroupEntry pe = WGPU_BIND_GROUP_ENTRY_INIT;
                pe.binding = wgsl::kGlobalSlots + 1;
                pe.buffer = pb;
                pe.size = pd.size;
                gmem.push_back(pe);
                pd.label = pw::string_view("nvn compute probe readback");
                pd.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
                Probe probe;
                probe.readback = gpu_create_buffer(s.device, &pd);
                probe.program = st.compute_program;
                char line[96];
                std::snprintf(line, sizeof(line), "groups %ux%ux%u, banks", groups[0], groups[1], groups[2]);
                probe.bound = line;
                for (uint32_t bank : prog.banks[kStage]) probe.bound += " c" + std::to_string(bank);
                for (const auto& [key, u] : uniform_buffers)
                    if (key.first == kStage) {
                        std::snprintf(line, sizeof(line), "; constant buffer %u 0x%llx+0x%llx", key.second,
                                      (unsigned long long)u.address, (unsigned long long)u.size);
                        probe.bound += line;
                    }
                for (uint32_t k = 0; k < wgsl::kGlobalSlots + wgsl::kConstantSlots; ++k)
                    if (table[4 * k + 2] != 0) {
                        std::snprintf(line, sizeof(line), "; %s %u 0x%llx+0x%x",
                                      k < wgsl::kGlobalSlots ? "slot" : "constant window", k,
                                      (unsigned long long)(table[4 * k] | uint64_t{table[4 * k + 1]} << 32),
                                      table[4 * k + 2]);
                        probe.bound += line;
                    }
                probes.push_back(std::move(probe));
                ++delta.diagnostics[uniform_buffers.count({kStage, nvn::kCompilerConstantBank})
                                        ? "probe: dispatch with c1 (constant pool) bound"
                                        : "probe: dispatch without c1 (constant pool)"];
            }
        }
        bd.layout = prog.groups[2];
        bd.entryCount = gmem.size();
        bd.entries = gmem.data();
        WGPUBindGroup g2 = wgpuDeviceCreateBindGroup(s.device, &bd);
        bind_groups.push_back(g2);

        if (dispatch_binds_storage) {
            if (writing_dispatches++ == 0) draws_at_first = draws_recorded;
            draws_at_last = draws_recorded;
            dispatch_binds_storage = false;
        }
        WGPUComputePassDescriptor cpd = WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
        if (pass_labels) {
            std::snprintf(pass_label_text, sizeof(pass_label_text), "cs %016llx %ux%ux%u",
                          (unsigned long long)prog.hash, groups[0], groups[1], groups[2]);
            cpd.label = pw::string_view(pass_label_text);
        }
        WGPUPassTimestampWrites cpd_time;
        cpd.timestampWrites = time_pass(prog.hash, &cpd_time);
        WGPUComputePipeline pipeline = prog.compute_pipeline;
        if (!moved.empty()) {
            pipeline = s.alias_pipeline(*pit->second, moved);
            ++delta.diagnostics["dispatch: a range the shader reads directly is bound in another slot"];
            if (pipeline == nullptr)
                return false;
        }
        WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(enc, &cpd);
        wgpuComputePassEncoderSetPipeline(pass, pipeline);
        wgpuComputePassEncoderSetBindGroup(pass, 0, g0, 0, nullptr);
        wgpuComputePassEncoderSetBindGroup(pass, 1, g1, 0, nullptr);
        wgpuComputePassEncoderSetBindGroup(pass, 2, g2, 0, nullptr);
        wgpuComputePassEncoderDispatchWorkgroups(pass, groups[0], groups[1], groups[2]);
        wgpuComputePassEncoderEnd(pass);
        wgpuComputePassEncoderRelease(pass);
        if (probe_buffer != nullptr)
            wgpuCommandEncoderCopyBufferToBuffer(enc, probe_buffer, 0, probes.back().readback, 0,
                                                 wgsl::kGmemProbeWords * 4);
        return true;
    };

    for (const nvn::Command& command : list.commands) {
        st.apply(command);
        if (is_draw(command)) {
            ++draws_recorded;
            note_watched();
        }
        dispatch_binds_storage = false;
        if (s.compute_first) {
            if (auto* c = std::get_if<nvn::CmdSetRenderTargets>(&command)) {
                written_textures.insert(c->colors.begin(), c->colors.end());
                written_textures.insert(c->depth_stencil);
            } else if (auto* c = std::get_if<nvn::CmdClearTexture>(&command)) {
                written_textures.insert(c->texture);
            } else if (auto* c = std::get_if<nvn::CmdCopyBufferToTexture>(&command)) {
                written_textures.insert(c->texture);
            } else if (auto* c = std::get_if<nvn::CmdCopyTextureToTexture>(&command)) {
                written_textures.insert(c->dst.texture);
            }
        }
        if (auto* c = std::get_if<nvn::CmdSetRenderTargets>(&command)) {
            for (nvn::TextureId color : c->colors)
                if (color != 0)
                    s.depth_partner[color] = c->depth_stencil;
            s.depth_partner_count.store(s.depth_partner.size(), std::memory_order_relaxed);
            if (s.render_scale != 1.0f) {
                // The colour targets of one pass share a size: all of them are
                // scaled or none (WebGPU rejects mixed sizes). One that is
                // already scaled makes the others try.
                bool all = true, any_scaled = false;
                for (nvn::TextureId id : c->colors)
                    if (auto t = s.textures.find(id); id != 0 && t != s.textures.end()) {
                        any_scaled = any_scaled || t->second.scaled;
                        all = all && (t->second.scaled || s.scale_refusal(t->second) == nullptr ||
                                      scalable_with_contents(t->second));
                    }
                if (all || any_scaled) {
                    for (nvn::TextureId id : c->colors)
                        scale_with_contents(id);
                } else {
                    // Which targets keep the pass unscaled, by size, format and reason.
                    size_t blockers = 0, colors = 0;
                    for (nvn::TextureId id : c->colors)
                        if (auto t = s.textures.find(id); id != 0 && t != s.textures.end()) {
                            ++colors;
                            const State::Texture& u = t->second;
                            if (s.scale_refusal(u) == nullptr)
                                continue;
                            const char* why = contents_refusal(u);
                            if (why == nullptr)
                                continue;
                            ++blockers;
                            char key[160];
                            std::snprintf(key, sizeof key,
                                          "render scale: colour target %ux%u format 0x%x unscaled (%s)",
                                          u.desc.width, u.desc.height, unsigned(u.format), why);
                            ++delta.diagnostics[key];
                        }
                    ++delta.diagnostics[blockers < colors
                                            ? "render scale: colour targets left unscaled (one of them cannot scale)"
                                            : "render scale: colour targets left unscaled (none of them can scale)"];
                }
                scale_with_contents(c->depth_stencil);
            }
            const bool same = std::equal(st.colors.begin(), st.colors.end(), draw_pass_colors.begin(),
                                         draw_pass_colors.end()) &&
                              st.depth_stencil == draw_pass_depth;
            if (same && s.keep_pass_same_targets) {
                if (draw_pass != nullptr)
                    ++delta.pass_reasons["kept: render targets set again (same)"];
            } else {
                end_draw_pass(same ? "render targets set again (same)" : "render targets changed");
            }
        } else if (auto* c = std::get_if<nvn::CmdSetScissors>(&command)) {
            if (c->first == 0 && !c->rects.empty()) {
                scissor = c->rects[0];
                scissor_set = true;
            }
        } else if (auto* c = std::get_if<nvn::CmdSetViewports>(&command)) {
            if (c->first == 0 && c->xywh.size() >= 4) {
                std::copy(c->xywh.begin(), c->xywh.begin() + 4, viewport);
                viewport_set = true;
            }
        } else if (auto* c = std::get_if<nvn::CmdSetDepthRanges>(&command)) {
            if (c->first == 0 && c->near_far.size() >= 2) {
                depth_range[0] = std::clamp(c->near_far[0], 0.0f, 1.0f);
                depth_range[1] = std::clamp(c->near_far[1], 0.0f, 1.0f);
            }
        } else if (auto* c = std::get_if<nvn::CmdBindVertexBuffer>(&command)) {
            vertex_buffers[c->stream] = *c;
        } else if (auto* c = std::get_if<nvn::CmdBindUniformBuffer>(&command)) {
            uniform_buffers[{int(c->stage), c->bank}] = *c;
        } else if (auto* c = std::get_if<nvn::CmdBindTexture>(&command)) {
            bound_textures[{int(c->stage), c->slot}] = *c;
        } else if (auto* c = std::get_if<nvn::CmdSetBlendColor>(&command)) {
            blend_color = WGPUColor{c->rgba[0], c->rgba[1], c->rgba[2], c->rgba[3]};
        } else if (auto* c = std::get_if<nvn::CmdSetStencilValues>(&command)) {
            if (c->kind == nvn::CmdSetStencilValues::Reference && (c->faces & 1))
                stencil_ref = c->value;
        } else if (is_draw(command) && st.rasterizer_discard) {
            // No fragments: nothing a WebGPU draw would change (no transform feedback).
            ++delta.discarded_draws;
        } else if (auto* c = std::get_if<nvn::CmdDraw>(&command)) {
            HalfResScope half;
            if (c->vertex_count != 0 && c->instance_count != 0 && begin_halfres())
                half.done = end_halfres;
            if (std::exchange(halfres_skip, false)) {
                ++delta.skip_reasons["draw: half-resolution program left out (halfskip)"];
                ++delta.skipped_commands;
                continue;
            }
            if (c->vertex_count == 0 || c->instance_count == 0 ||
                !prepare_draw(c->topology, WGPUIndexFormat_Undefined) ||
                !instanced_runs(c->first_instance, c->instance_count, uint64_t(c->first_vertex) + c->vertex_count,
                                [&](uint32_t first, uint32_t n) {
                                    wgpuRenderPassEncoderDraw(draw_pass, c->vertex_count, n, c->first_vertex, first);
                                })) {
                if (c->vertex_count == 0 || c->instance_count == 0) ++delta.skip_reasons["draw: zero vertices"];
                ++delta.skipped_commands;
                continue;
            }
            ++delta.draws;
            count_timed_draw();
            delta.astc_bindings += pending_astc;
            delta.bindings_3d += pending_3d;
        } else if (auto* c = std::get_if<nvn::CmdDrawIndexed>(&command)) {
            const uint32_t count = std::min<uint32_t>(c->index_count, 1u << 24);
            HalfResScope half;
            if (count != 0 && c->instance_count != 0 && begin_halfres())
                half.done = end_halfres;
            if (std::exchange(halfres_skip, false)) {
                ++delta.skip_reasons["draw: half-resolution program left out (halfskip)"];
                ++delta.skipped_commands;
                continue;
            }
            if (count == 0 || c->instance_count == 0 || !prepare_draw(c->topology, strip_index_format(command))) {
                if (count == 0) ++delta.skip_reasons["draw indexed: no indices"];
                ++delta.skipped_commands;
                continue;
            }
            // WebGPU has no 8-bit indices: widened to 16 bits. Same indices in
            // the list: one upload (kind 2 + index type).
            const uint32_t in_size = c->type == nvn::IndexType::U8 ? 1 : c->type == nvn::IndexType::U16 ? 2 : 4;
            const uint32_t out_size = c->type == nvn::IndexType::U32 ? 4 : 2;
            const uint64_t slot = (uint64_t(count) * out_size + 255) & ~uint64_t{255};
            check_read(c->index_address, uint64_t(count) * in_size, "index buffer");
            const auto key = std::make_tuple(c->index_address, uint64_t(count), 2u + uint32_t(c->type));
            auto known = s.geometry_arena.uploaded.find(key);
            if (known == s.geometry_arena.uploaded.end()) {
                const uint64_t n = uint64_t(count) * in_size;
                auto widen = [&](const std::vector<uint8_t>& raw) {
                    scratch.assign(size_t(slot), 0);
                    if (c->type == nvn::IndexType::U8) {
                        for (uint32_t i = 0; i < count; ++i) {
                            const uint16_t v = raw[i];
                            std::memcpy(scratch.data() + size_t(i) * 2, &v, 2);
                        }
                    } else {
                        std::memcpy(scratch.data(), raw.data(), raw.size());
                    }
                };
                auto where = cached_geometry(key, c->index_address, n, slot, widen);
                if (where.first == nullptr) {
                    std::vector<uint8_t> raw(size_t(n), 0);
                    if (memory_ == nullptr || !memory_->read(c->index_address, raw.data(), raw.size()))
                        rt::log(rt::LogLevel::Warn, "nvn webgpu: index buffer 0x%llx not readable",
                                (unsigned long long)c->index_address);
                    widen(raw);
                    where = s.geometry_arena.put(s.device, scratch.data(), scratch.size());
                    ++delta.geometry_uploads;
                    delta.geometry_upload_bytes += slot;
                }
                known = s.geometry_arena.uploaded.emplace(key, where).first;
            } else {
                ++delta.geometry_reused;
            }
            pass_state.index_buffer(draw_pass, known->second.first,
                                    out_size == 2 ? WGPUIndexFormat_Uint16 : WGPUIndexFormat_Uint32,
                                    known->second.second, slot);
            if (!instanced_runs(c->first_instance, c->instance_count, 0, [&](uint32_t first, uint32_t n) {
                    wgpuRenderPassEncoderDrawIndexed(draw_pass, count, n, 0, c->base_vertex, first);
                })) {
                ++delta.skipped_commands;
                continue;
            }
            ++delta.draws;
            count_timed_draw();
            delta.astc_bindings += pending_astc;
            delta.bindings_3d += pending_3d;
        } else if (auto* c = std::get_if<nvn::CmdCopyBufferToTexture>(&command)) {
            end_draw_pass("copy buffer to texture");
            flush_clear(c->texture);
            note_event({c->texture}, false);
            auto it = s.find_texture(c->texture);
            const uint32_t bpp = copy_bpp(c->texture);
            // Level size; slices are 3D depth (also halved per level) or array layers.
            const nvn::TextureDesc* td = it != s.textures.end() ? &it->second.desc : nullptr;
            const uint32_t level_w = td ? std::max(td->width >> c->level, 1u) : 0;
            const uint32_t level_h = td ? std::max(td->height >> c->level, 1u) : 0;
            const uint32_t slices = !td ? 0 : it->second.is_3d() ? std::max(td->depth >> c->level, 1u) : td->depth;
            if (bpp == 0 || it->second.texture == nullptr || it->second.sample_count != 1 || c->width <= 0 ||
                c->height <= 0 || c->depth <= 0 || c->x < 0 || c->y < 0 || c->z < 0 || c->level >= td->levels ||
                uint64_t(c->x) + uint64_t(c->width) > level_w || uint64_t(c->y) + uint64_t(c->height) > level_h ||
                uint64_t(c->z) + uint64_t(c->depth) > slices) {
                ++delta.skip_reasons["copy to texture: unsupported texture or region"];
                ++delta.skipped_commands;
                continue;
            }
            // Repack rows to the 256-byte pitch buffer-to-texture copies need.
            const uint32_t row = uint32_t(c->width) * bpp;
            const uint32_t src_pitch = c->row_stride != 0 ? c->row_stride : row;
            const uint64_t src_image =
                c->image_stride != 0 ? c->image_stride : uint64_t(src_pitch) * uint32_t(c->height);
            const uint32_t dst_pitch = (row + 255) & ~255u;
            const size_t dst_image = size_t(dst_pitch) * uint32_t(c->height);
            // The arena was sized from the textures before the list; one that
            // changed since does not fit (not expected: the list changes no texture).
            if (copy_arena == nullptr || copy_used + uint64_t(dst_image) * uint32_t(c->depth) >
                                             wgpuBufferGetSize(copy_arena)) {
                ++delta.skip_reasons["copy to texture: copy arena too small"];
                ++delta.skipped_commands;
                continue;
            }
            std::vector<uint8_t> src(size_t(src_image) * uint32_t(c->depth - 1) +
                                         size_t(src_pitch) * uint32_t(c->height - 1) + row,
                                     0);
            check_read(c->address, src.size(), "buffer-to-texture copy source");
            if (memory_ == nullptr || !memory_->read(c->address, src.data(), src.size()))
                rt::log(rt::LogLevel::Warn, "nvn webgpu: copy source 0x%llx not readable",
                        (unsigned long long)c->address);
            scratch.assign(dst_image * uint32_t(c->depth), 0);
            for (int32_t z = 0; z < c->depth; ++z)
                for (int32_t y = 0; y < c->height; ++y) {
                    uint8_t* to = scratch.data() + dst_image * uint32_t(z) + size_t(y) * dst_pitch;
                    std::memcpy(to, src.data() + size_t(src_image) * uint32_t(z) + size_t(y) * src_pitch, row);
                    if (is_norm16(it->second.desc.format))
                        norm16_to_half(it->second.desc.format, to, row);
                }
            const uint64_t offset = copy_used;
            queue_write_buffer(s.queue, copy_arena, offset, scratch.data(), scratch.size());
            copy_used += scratch.size();
            WGPUTexelCopyBufferInfo from = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
            from.buffer = copy_arena;
            from.layout.offset = offset;
            from.layout.bytesPerRow = dst_pitch;
            from.layout.rowsPerImage = uint32_t(c->height);
            WGPUTexelCopyTextureInfo to = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
            to.texture = it->second.texture;
            to.mipLevel = c->level;
            to.origin = {uint32_t(c->x), uint32_t(c->y), uint32_t(c->z)};
            const WGPUExtent3D extent = {uint32_t(c->width), uint32_t(c->height), uint32_t(c->depth)};
            State::Texture& t = it->second;
            if (t.scaled) {
                // Guest-sized texels for a scaled render target (State::render_scale):
                // into a texture of the region's size, then drawn into the target's
                // host rectangle (filtered).
                if (c->depth != 1 || c->z != 0 || c->level >= t.plan.td.mipLevelCount || has_depth(t.format) ||
                    has_stencil(t.format) || is_bc(t.format) || t.attachment == nullptr) {
                    ++delta.skip_reasons["copy to texture: scaled render target (render scale)"];
                    ++delta.skipped_commands;
                    continue;
                }
                WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
                td.label = pw::string_view("nvn scaled upload temp");
                td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
                td.dimension = WGPUTextureDimension_2D;
                td.size = {uint32_t(c->width), uint32_t(c->height), 1};
                td.format = t.format;
                WGPUTexture temp = gpu_create_texture(s.device, &td);
                temp_textures.push_back(temp);
                to.texture = temp;
                to.mipLevel = 0;
                to.origin = {0, 0, 0};
                wgpuCommandEncoderCopyBufferToTexture(encoder, &from, &to, &extent);
                WGPUTextureViewDescriptor vd = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
                vd.format = t.format;
                vd.dimension = WGPUTextureViewDimension_2D;
                vd.mipLevelCount = 1;
                vd.arrayLayerCount = 1;
                WGPUTextureView from_view = wgpuTextureCreateView(temp, &vd);
                temp_views.push_back(from_view);
                vd.baseMipLevel = c->level;
                WGPUTextureView to_view = wgpuTextureCreateView(t.texture, &vd);
                temp_views.push_back(to_view);
                BlitUniform u{};
                u.src[2] = u.src[3] = 1.0f;
                const int64_t x0 = t.host_x(c->x, c->level), y0 = t.host_y(c->y, c->level);
                const int32_t rect[4] = {int32_t(x0), int32_t(y0),
                                         int32_t(std::max<int64_t>(t.host_x(int64_t{c->x} + c->width, c->level) - x0, 1)),
                                         int32_t(std::max<int64_t>(t.host_y(int64_t{c->y} + c->height, c->level) - y0, 1))};
                blit(from_view, u, t, c->texture, to_view, rect, true);
                t.gpu_written = true;
                ++delta.copies;
                ++delta.blits;
                static const std::string what = "render scale: uploads into a scaled target (drawn scaled)";
                ++delta.diagnostics[what];
                continue;
            }
            wgpuCommandEncoderCopyBufferToTexture(encoder, &from, &to, &extent);
            it->second.gpu_written = true;
            ++delta.copies;
        } else if (auto* c = std::get_if<nvn::CmdCopyTextureToTexture>(&command)) {
            end_draw_pass("copy texture to texture");
            flush_clear(c->src.texture);
            flush_clear(c->dst.texture);
            note_event({c->src.texture, c->dst.texture}, false);
            if (const char* why = copy_texture(*c)) {
                ++delta.skip_reasons[why];
                ++delta.skipped_commands;
            }
        } else if (auto* c = std::get_if<nvn::CmdClearColor>(&command)) {
            end_draw_pass("colour clear");
            auto it = c->index < st.colors.size() ? s.find_texture(st.colors[c->index]) : s.textures.end();
            if (it == s.textures.end() || it->second.attachment == nullptr || has_depth(it->second.format) ||
                has_stencil(it->second.format)) {
                ++delta.skip_reasons["clear color: target missing or unsupported"];
                ++delta.skipped_commands;
                continue;
            }
            it->second.gpu_written = true;  // its pool storage is stale now (texture budget)
            const State::Texture& t = it->second;
            ++delta.clears;
            const Region r = region_of(t);
            if (s.fold_clears && r.full && (c->mask & 0xF) == 0xF) {
                std::memcpy(pending_color[st.colors[c->index]].rgba, c->rgba, sizeof(c->rgba));
                continue;
            }
            flush_clear(st.colors[c->index]);
            clear_region(st.colors[c->index], t, r, c->rgba, c->mask & 0xF);
            if (r.full && (c->mask & 0xF) == 0xF)
                note_event({}, false, {st.colors[c->index]});
            else
                note_event({st.colors[c->index]}, false);
        } else if (auto* c = std::get_if<nvn::CmdClearTexture>(&command)) {
            end_draw_pass("texture clear");
            auto it = s.find_texture(c->texture);
            if (it == s.textures.end() || it->second.attachment == nullptr || has_depth(it->second.format) ||
                has_stencil(it->second.format)) {
                ++delta.skip_reasons["clear texture: texture missing or unsupported"];
                ++delta.skipped_commands;
                continue;
            }
            State::Texture& t = it->second;
            const int64_t w = t.width(), h = t.height();
            const int64_t x0 = std::clamp<int64_t>(t.host_x(c->region.x), 0, w);
            const int64_t x1 = std::clamp<int64_t>(t.host_x(int64_t{c->region.x} + c->region.width), 0, w);
            const int64_t y0 = std::clamp<int64_t>(t.host_y(c->region.y), 0, h);
            const int64_t y1 = std::clamp<int64_t>(t.host_y(int64_t{c->region.y} + c->region.height), 0, h);
            Region r;
            r.x = uint32_t(x0);
            r.y = uint32_t(y0);
            r.width = uint32_t(std::max<int64_t>(x1 - x0, 0));
            r.height = uint32_t(std::max<int64_t>(y1 - y0, 0));
            r.full = r.x == 0 && r.y == 0 && r.width == uint32_t(w) && r.height == uint32_t(h);
            t.gpu_written = true;
            ++delta.clears;
            if (s.fold_clears && r.full && (c->mask & 0xF) == 0xF) {
                std::memcpy(pending_color[c->texture].rgba, c->rgba, sizeof(c->rgba));
                continue;
            }
            flush_clear(c->texture);
            clear_region(c->texture, t, r, c->rgba, c->mask & 0xF);
            if (r.full && (c->mask & 0xF) == 0xF)
                note_event({}, false, {c->texture});
            else
                note_event({c->texture}, false);
        } else if (auto* c = std::get_if<nvn::CmdCopyBuffer>(&command)) {
            // Guest memory is the buffers' storage: copy now, in command
            // order, so later draws of this list upload the copied bytes.
            check_read(c->src, c->size, "buffer copy source");
            std::vector<uint8_t> bytes(size_t(std::min<uint64_t>(c->size, 64ull << 20)));
            if (memory_ == nullptr || bytes.size() != c->size || !memory_->read(c->src, bytes.data(), bytes.size()) ||
                !memory_->write(c->dst, bytes.data(), bytes.size())) {
                ++delta.skip_reasons["copy buffer: guest range not readable/writable"];
                ++delta.skipped_commands;
                continue;
            }
            // Later draws must see the copied bytes: no reuse of earlier uploads.
            s.cbuf_arena.uploaded.clear();
            s.geometry_arena.uploaded.clear();
            patched_ranges.clear();  // built from the guest bytes before the copy
            // A dispatch's output buffer holding the destination gets the bytes
            // too, in command order: draws reading it (dispatch_output) and its
            // writeback after the list see the copy.
            for (StorageBuffer& b : storage) {
                const uint64_t lo = std::max(b.address, c->dst), hi = std::min(b.address + b.size, c->dst + c->size);
                if (!b.written || lo >= hi)
                    continue;
                if ((lo - b.address) % 4 != 0 || (hi - lo) % 4 != 0) {
                    b.superseded = true;  // WebGPU copies whole words only
                    continue;
                }
                end_draw_pass("copy into a dispatch output");
                note_event({}, true);
                WGPUBufferDescriptor td = WGPU_BUFFER_DESCRIPTOR_INIT;
                td.label = pw::string_view("nvn copy into dispatch output");
                td.size = hi - lo;
                td.usage = WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst;
                WGPUBuffer tb = s.list_buffer(&td);
                temp_buffers.push_back(tb);
                queue_write_buffer(s.queue, tb, 0, bytes.data() + (lo - c->dst), size_t(hi - lo));
                wgpuCommandEncoderCopyBufferToBuffer(encoder, tb, 0, b.buffer, lo - b.address, hi - lo);
                b.main_use = b.main_write = true;
            }
            ++delta.copies;
        } else if (auto* c = std::get_if<nvn::CmdClearDepthStencil>(&command)) {
            end_draw_pass("depth/stencil clear");
            auto it = s.find_texture(st.depth_stencil);
            if (st.depth_stencil == 0 || it == s.textures.end() || it->second.attachment == nullptr ||
                !(has_depth(it->second.format) || has_stencil(it->second.format))) {
                ++delta.skip_reasons["clear depth/stencil: target missing or unsupported"];
                ++delta.skipped_commands;
                continue;
            }
            it->second.gpu_written = true;
            const State::Texture& t = it->second;
            const Region r = region_of(t);
            ++delta.clears;
            const bool depth_op = has_depth(t.format) && c->depth_mask;
            const uint32_t stencil_mask = has_stencil(t.format) ? (c->stencil_mask & 0xFF) : 0;
            if (r.width == 0 || r.height == 0 || (!depth_op && stencil_mask == 0))
                continue;
            const bool direct = r.full && (stencil_mask == 0 || stencil_mask == 0xFF);
            const float depth = std::clamp(c->depth, 0.0f, 1.0f);
            if (s.fold_clears && direct) {
                PendingDepthClear& p = pending_depth[st.depth_stencil];
                if (depth_op) {
                    p.depth = true;
                    p.depth_value = depth;
                }
                if (stencil_mask == 0xFF) {
                    p.stencil = true;
                    p.stencil_value = c->stencil & 0xFF;
                }
                continue;
            }
            flush_clear(st.depth_stencil);
            depth_clear_pass(st.depth_stencil, t, r, depth_op, stencil_mask, depth, c->stencil & 0xFF);
            if (r.full && (depth_op || !has_depth(t.format)) && (stencil_mask == 0xFF || !has_stencil(t.format)))
                note_event({}, false, {st.depth_stencil});
            else
                note_event({st.depth_stencil}, false);
        } else if (auto* c = std::get_if<nvn::CmdBindStorageBuffer>(&command)) {
            storage_bindings[{int(c->stage), c->index}] = *c;
        } else if (auto* c = std::get_if<nvn::CmdDispatchCompute>(&command)) {
            dispatch_writes.dispatch(*c, storage_bindings);
            if (dispatch(*c)) {
                ++delta.dispatches;
                delta.astc_bindings += pending_astc;
                delta.bindings_3d += pending_3d;
            } else {
                ++delta.skip_reasons["dispatch: no compute program"];
                ++delta.skipped_commands;
            }
        } else if (auto* u = std::get_if<nvn::CmdUnsupported>(&command)) {
            ++delta.skip_reasons[std::string("unimplemented command ") + (u->name ? u->name : std::to_string(u->proc).c_str())];
            ++delta.skipped_commands;
        }
        // Barriers: WebGPU orders passes itself. Pipeline state commands
        // only change `st` (DrawState::apply above).
    }
    end_draw_pass("end of list");
    flush_clears("end of list");
    // A pass going back to the targets of an earlier pass j; its draws could
    // have joined j if nothing between reads what it writes or writes what
    // it reads, and it did not begin with a clear.
    for (size_t i = 0; i < pass_records.size(); ++i) {
        const PassRecord& p = pass_records[i];
        if (p.targets.empty())
            continue;
        size_t j = i;
        while (j-- > 0)
            if (pass_records[j].targets == p.targets)
                break;
        if (j == size_t(-1))
            continue;
        ++delta.pass_reasons["target set: back to an earlier one"];
        // A read-only depth pass and a writable one cannot be one pass.
        bool movable = !p.cleared && p.depth_read_only == pass_records[j].depth_read_only;
        for (size_t k = j + 1; k < i && movable; ++k) {
            const PassRecord& b = pass_records[k];
            if (b.barrier) {
                movable = false;
                break;
            }
            const auto touches = [](const std::vector<nvn::TextureId>& ids, const std::set<nvn::TextureId>& set) {
                for (nvn::TextureId id : ids)
                    if (set.count(id))
                        return true;
                return false;
            };
            if (b.targets.empty()) {  // an event: touches p's targets or what p samples
                if (touches(p.targets, b.sampled) ||
                    std::any_of(p.sampled.begin(), p.sampled.end(), [&](nvn::TextureId id) { return b.sampled.count(id) != 0; }))
                    movable = false;
                continue;
            }
            const bool shares_target = std::any_of(p.targets.begin(), p.targets.end(), [&](nvn::TextureId id) {
                return std::binary_search(b.targets.begin(), b.targets.end(), id);  // targets are sorted
            });
            if (shares_target || touches(p.targets, b.sampled) || touches(b.targets, p.sampled))
                movable = false;
        }
        if (movable)
            ++delta.pass_reasons["target set: back to an earlier one, draws could join it"];
    }
    // Attachments a pass stores (StoreOp_Store) and what comes next for them
    // in this list: a whole overwrite before any read means the store could
    // be a discard (tile bandwidth on mobile GPUs). Diagnosis only.
    for (size_t i = 0; i < pass_records.size(); ++i) {
        const PassRecord& p = pass_records[i];
        std::vector<nvn::TextureId> stored = p.stored_colors;
        if (p.stored_depth != 0)
            stored.push_back(p.stored_depth);
        for (nvn::TextureId t : stored) {
            const bool depth = t == p.stored_depth;
            const char* next = "not used again in the list";
            for (size_t k = i + 1; k < pass_records.size(); ++k) {
                const PassRecord& b = pass_records[k];
                if (b.barrier) {
                    next = "read later";  // an in-order dispatch: assume it reads
                    break;
                }
                if (b.overwritten.count(t)) {
                    next = "overwritten later before any read (discard candidate)";
                    break;
                }
                if (b.sampled.count(t) || std::find(b.targets.begin(), b.targets.end(), t) != b.targets.end()) {
                    next = "read later";
                    break;
                }
            }
            ++delta.pass_reasons[std::string("store: ") + (depth ? "depth" : "colour") + " attachment, " + next];
        }
    }

    // Clear parameters reach the GPU before the command buffer runs: queue
    // writes are ordered before the following submit.
    for (size_t i = 0; i < uniforms.size(); ++i)
        queue_write_buffer(s.queue, s.clear_uniforms, i * kUniformStride, &uniforms[i], sizeof(ClearUniform));
    // Storage buffers the list's dispatches used go back to guest memory.
    struct Writeback {
        struct Entry {
            uint64_t address = 0, size = 0;
            WGPUBuffer readback = nullptr;
            uint64_t initial_fp = 0;
            std::vector<uint8_t> uploaded;  // StorageBuffer::uploaded
            // computeprobe counters instead of guest memory (address/size unused).
            bool probe = false;
            nvn::ProgramId program = 0;
            std::string bound;
        };
        std::vector<Entry> entries;
        // Callbacks may run on any thread (AllowSpontaneous), even inside
        // wgpuBufferMapAsync: one extra count held by the registering loop
        // keeps `this` alive until every map is registered.
        std::atomic<size_t> pending{0};
        const nvn::GuestMemoryView* memory = nullptr;
        std::function<void()> done;  // empty: asynchronous (State::writeback_finished)
        State* state = nullptr;
        std::shared_ptr<BufferPool> pool;  // read-backs go back here (unmapped)
        State* diag_state = nullptr;  // computeoverlap only
        WebGpuBackend* backend = nullptr;
        uint64_t list = 0;
        uint64_t seq = 0;  // State::lists_submitted of the list (fence marks)
        std::chrono::steady_clock::time_point submitted;  // waited from here when synchronous
    };
    auto* wb = new Writeback;
    wb->state = &s;
    wb->pool = s.buffer_pool;
    wb->memory = memory_;
    wb->diag_state = diag ? &s : nullptr;
    wb->backend = this;
    wb->list = list_no;
    wb->done = std::move(done);
    // State::compute_first: when every dispatch went first and nothing in
    // `encoder` writes their ranges, their results are read back from the
    // compute submit; the guest then waits for compute and the map only.
    bool readback_first = compute_used && !dispatch_in_order && !dbg.cfreadbackmain;
    for (const StorageBuffer& b : storage)
        if (b.written && b.main_write) readback_first = false;
    WGPUCommandEncoder readback_encoder = readback_first ? compute_encoder : encoder;
    // State::poll_submit_index: the submission with the read-back copies.
    uint64_t readback_submission = 0;
    bool have_readback_submission = false;
    // State::patch_dispatch_output: overlapping ranges get the newer
    // dispatches' bytes before they are read back (newest first, so each
    // copies from ranges already final): the write-backs then agree in the
    // overlap whatever order they land in.
    if (s.patch_dispatch_output) {
        std::vector<StorageBuffer*> newest_first;
        for (StorageBuffer& b : storage)
            if (b.written) newest_first.push_back(&b);
        std::sort(newest_first.begin(), newest_first.end(),
                  [](const StorageBuffer* a, const StorageBuffer* b) { return a->write_order > b->write_order; });
        for (StorageBuffer* b : newest_first)
            copy_dispatch_output(*b, readback_encoder);
    }
    for (StorageBuffer& b : storage) {
        if (!b.written) continue;
        const uint64_t gpu_size = wgpuBufferGetSize(b.buffer);
        WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
        bd.label = pw::string_view("nvn storage readback");
        bd.size = gpu_size;
        bd.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
        WGPUBuffer rb = s.list_buffer(&bd);
        wgpuCommandEncoderCopyBufferToBuffer(readback_encoder, b.buffer, 0, rb, 0, gpu_size);
        wb->entries.push_back({b.address, b.size, rb, b.initial_fp, std::move(b.uploaded)});
        if (diag) {
            std::lock_guard lock(s.diag_mutex);
            if (s.written_back.size() < 4096)
                s.written_back.push_back({b.address, b.size, list_no, 0, false, 0});
            else
                ++delta.diagnostics["writeback: more than 4096 ranges tracked, not tracked"];
        }
    }
    for (Probe& p : probes) {
        Writeback::Entry e;
        e.readback = p.readback;
        e.probe = true;
        e.program = p.program;
        e.bound = std::move(p.bound);
        wb->entries.push_back(std::move(e));
    }
    s.carried.st = st;
    s.carried.scissor_set = scissor_set;
    s.carried.scissor = scissor;
    s.carried.viewport_set = viewport_set;
    std::memcpy(s.carried.viewport, viewport, sizeof(viewport));
    s.carried.depth_range[0] = depth_range[0];
    s.carried.depth_range[1] = depth_range[1];
    s.carried.vertex_buffers = std::move(vertex_buffers);
    s.carried.uniform_buffers = std::move(uniform_buffers);
    s.carried.bound_textures = std::move(bound_textures);
    s.carried.storage_bindings = std::move(storage_bindings);
    s.carried.blend_color = blend_color;
    s.carried.stencil_ref = stencil_ref;
    ++delta.lists;
    delta.list_commands += list.commands.size();
    raise_peak(g_counters.list_commands_peak, list.commands.size());
    if (writing_dispatches != 0) {
        ++delta.dispatch_lists;
        delta.draws_before_dispatch += draws_at_first;
        delta.draws_between_dispatches += draws_at_last - draws_at_first;
        delta.draws_after_dispatch += draws_recorded - draws_at_last;
    }
    s.cbuf_arena.flush(s.queue);
    s.geometry_arena.flush(s.queue);
    if (compute_encoder != nullptr) {
        WGPUCommandBuffer compute_commands = wgpuCommandEncoderFinish(compute_encoder, nullptr);
        const auto t_submit = std::chrono::steady_clock::now();
#if defined(LGP_WEBGPU_WGPU_NATIVE)
        if (s.poll_submit_index && readback_first) {
            readback_submission = wgpuQueueSubmitForIndex(s.queue, 1, &compute_commands);
            have_readback_submission = true;
        } else
#endif
            wgpuQueueSubmit(s.queue, 1, &compute_commands);
        s.list_submits.fetch_add(1, std::memory_order_relaxed);
        s.list_submit_ns.fetch_add(nanos_since(t_submit), std::memory_order_relaxed);
        wgpuCommandBufferRelease(compute_commands);
        wgpuCommandEncoderRelease(compute_encoder);
        if (compute_used) ++delta.lists_compute_first;
        if (readback_first) ++delta.lists_written_back_first;
    }
    // gputime: resolved after both submits, read back on its own (the list
    // does not wait for it); the results go to Stats::gpu_time_*.
    auto submit_timestamps = [&] {
        if (timer.untimed != 0) {
            std::lock_guard lock(s.gpu_times->mutex);
            s.gpu_times->untimed += timer.untimed;
        }
        if (timer.tags.empty()) return;
        struct Times {
            std::shared_ptr<State::GpuTimes> sink;
            std::vector<uint64_t> tags;
            std::vector<std::string> targets;
            std::vector<std::map<uint64_t, uint32_t>> draws;
            WGPUBuffer readback;
        };
        const uint64_t bytes = 16 * timer.tags.size();
        WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
        bd.label = pw::string_view("nvn gputime resolve");
        bd.size = bytes;
        bd.usage = WGPUBufferUsage_QueryResolve | WGPUBufferUsage_CopySrc;
        WGPUBuffer resolved = gpu_create_buffer(s.device, &bd);
        bd.label = pw::string_view("nvn gputime readback");
        bd.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
        auto* t = new Times{s.gpu_times, std::move(timer.tags), std::move(timer.targets), std::move(timer.draws),
                            gpu_create_buffer(s.device, &bd)};
        WGPUCommandEncoder te = wgpuDeviceCreateCommandEncoder(s.device, nullptr);
        wgpuCommandEncoderResolveQuerySet(te, timer.set, 0, uint32_t(2 * t->tags.size()), resolved, 0);
        wgpuCommandEncoderCopyBufferToBuffer(te, resolved, 0, t->readback, 0, bytes);
        WGPUCommandBuffer tc = wgpuCommandEncoderFinish(te, nullptr);
        wgpuQueueSubmit(s.queue, 1, &tc);
        wgpuCommandBufferRelease(tc);
        wgpuCommandEncoderRelease(te);
        gpu_destroy_buffer(resolved);
        WGPUBufferMapCallbackInfo cb = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
        cb.mode = WGPUCallbackMode_AllowSpontaneous;
        cb.userdata1 = t;
        cb.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void* u1, void*) {
            std::unique_ptr<Times> t(static_cast<Times*>(u1));
            const size_t n = t->tags.size();
            const void* data =
                status == WGPUMapAsyncStatus_Success ? wgpuBufferGetConstMappedRange(t->readback, 0, 16 * n) : nullptr;
            if (data != nullptr) {
                std::vector<uint64_t> ts(2 * n);
                std::memcpy(ts.data(), data, 16 * n);
                State::GpuTimes& st = *t->sink;
                std::lock_guard lock(st.mutex);
                for (size_t i = 0; i < n; ++i) {
                    // Timestamps in ticks of ns_per_tick; a pass the
                    // implementation could not time (or reordered) reads as end < begin.
                    const uint64_t ns =
                        ts[2 * i + 1] > ts[2 * i] ? uint64_t(double(ts[2 * i + 1] - ts[2 * i]) * st.ns_per_tick) : 0;
                    ++st.passes;
                    Stats::GpuTime& total = t->tags[i] == kRenderPass ? st.render : st.compute;
                    ++total.passes;
                    total.ns += ns;
                    if (t->tags[i] != kRenderPass) {
                        Stats::GpuTime& p = st.programs[t->tags[i]];
                        ++p.passes;
                        p.ns += ns;
                    } else if (!t->targets[i].empty()) {
                        Stats::GpuTime& p = st.targets[t->targets[i]];
                        ++p.passes;
                        p.ns += ns;
                    }
                    // A draw pass's time split over its programs by draw
                    // count (an estimate: draws differ in cost).
                    uint64_t all = 0;
                    for (const auto& [hash, n] : t->draws[i]) all += n;
                    for (const auto& [hash, n] : t->draws[i]) {
                        Stats::GpuTime& d = st.draw_programs[hash];
                        d.passes += n;
                        d.ns += ns * n / all;
                    }
                }
            }
            if (data != nullptr) wgpuBufferUnmap(t->readback);
            gpu_destroy_buffer(t->readback);
        };
        wgpuBufferMapAsync(t->readback, WGPUMapMode_Read, 0, bytes, cb);
        wgpuQuerySetRelease(timer.set);
        timer.set = nullptr;
    };
    WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder, nullptr);
    // With readback_first the maps are registered before `encoder` is
    // submitted, so they complete without waiting for the draws.
    auto submit_main = [&] {
        const auto t_submit = std::chrono::steady_clock::now();
#if defined(LGP_WEBGPU_WGPU_NATIVE)
        if (s.poll_submit_index && !readback_first) {
            readback_submission = wgpuQueueSubmitForIndex(s.queue, 1, &commands);
            have_readback_submission = true;
        } else
#endif
            wgpuQueueSubmit(s.queue, 1, &commands);
        g_upload_flush.pending.store(0, std::memory_order_relaxed);  // this submit carried the writes
        s.list_submits.fetch_add(1, std::memory_order_relaxed);
        s.list_submit_ns.fetch_add(nanos_since(t_submit), std::memory_order_relaxed);
        wgpuCommandBufferRelease(commands);
        wgpuCommandEncoderRelease(encoder);
        for (WGPUBindGroup g : bind_groups) wgpuBindGroupRelease(g);
        // Per-list buffers are destroyed, not only released (gpu_destroy_buffer),
        // or with the pool returned to it once the GPU ran the list.
        auto drop = gpu_destroy_buffer;
        if (s.buffer_pool) {
            struct Retired {
                std::shared_ptr<BufferPool> pool;
                std::vector<WGPUBuffer> buffers;
            };
            auto* r = new Retired{s.buffer_pool, std::move(temp_buffers)};
            if (copy_arena != nullptr) r->buffers.push_back(copy_arena);
            for (const StorageBuffer& b : storage)
                if (b.buffer != nullptr) r->buffers.push_back(b.buffer);
            temp_buffers.clear();
            copy_arena = nullptr;
            storage.clear();
            WGPUQueueWorkDoneCallbackInfo cb = WGPU_QUEUE_WORK_DONE_CALLBACK_INFO_INIT;
            cb.mode = WGPUCallbackMode_AllowSpontaneous;
            cb.userdata1 = r;
            cb.callback = [](WGPUQueueWorkDoneStatus status, WGPUStringView, void* u, void*) {
                std::unique_ptr<Retired> r(static_cast<Retired*>(u));
                for (WGPUBuffer b : r->buffers)
                    if (status == WGPUQueueWorkDoneStatus_Success)
                        r->pool->give(b);
                    else
                        gpu_destroy_buffer(b);
            };
            wgpuQueueOnSubmittedWorkDone(s.queue, cb);
        }
        if (copy_arena != nullptr) drop(copy_arena);
        for (WGPUBuffer b : temp_buffers) drop(b);
        for (WGPUTextureView v : temp_views) wgpuTextureViewRelease(v);
        for (WGPUTexture t : temp_textures) {
            gpu_destroy_texture(t);
        }
        for (WGPUTexture t : s.retired_textures) gpu_destroy_texture(t);
        s.retired_textures.clear();
        for (const StorageBuffer& b : storage) drop(b.buffer);
        if (dbg.errorscope) {
            // Popped in reverse: validation, internal, out of memory.
#if defined(__EMSCRIPTEN__)
            static const char* const kScopes[] = {"validation", "internal", "out of memory"};
#else
            static const char* const kScopes[] = {"validation", "out of memory"};
#endif
            for (const char* const& what : kScopes) {
                WGPUPopErrorScopeCallbackInfo cb = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
                cb.mode = WGPUCallbackMode_AllowSpontaneous;
                cb.userdata1 = const_cast<char*>(what);
                cb.userdata2 = reinterpret_cast<void*>(uintptr_t(list_no));
                cb.callback = [](WGPUPopErrorScopeStatus status, WGPUErrorType type, WGPUStringView message,
                                 void* u1, void* u2) {
                    if (status != WGPUPopErrorScopeStatus_Success || type != WGPUErrorType_NoError)
                        rt::log(rt::LogLevel::Warn, "nvn webgpu: errorscope: list %llu: %s error (status %d): %s",
                                (unsigned long long)uintptr_t(u2), static_cast<const char*>(u1), int(status),
                                pw::to_string(message).c_str());
                };
                wgpuDevicePopErrorScope(s.device, cb);
            }
        }
        submit_timestamps();
    };
    if (!readback_first || wb->entries.empty())
        submit_main();
    // Pending before the list counts as submitted: a fence another guest
    // thread places from now on waits for its write-back.
    const bool async = s.async_writeback && !wb->entries.empty();
    if (async)
        s.writeback_started(s.lists_submitted.load(std::memory_order_acquire),
                            have_readback_submission ? &readback_submission : nullptr);
    else if (have_readback_submission && !wb->entries.empty() && wait_for)
        wait_for(readback_submission);
    wb->seq = s.lists_submitted.fetch_add(1, std::memory_order_acq_rel);
    wb->submitted = std::chrono::steady_clock::now();

    if (wb->entries.empty()) {
        wb->done();
        delete wb;
        return;
    }
    wb->pending = wb->entries.size() + 1;
    // Asynchronous: the call finishes now, later calls wait (after_writebacks).
    std::function<void()> caller_done;
    ++s.writebacks;
    if (async) {
        caller_done = std::move(wb->done);
        wb->done = nullptr;
    }
    auto release = [](Writeback* w) {
        if (--w->pending == 0) {
            State* state = w->done ? nullptr : w->state;
            const uint64_t seq = w->seq;
            if (w->done) {
                // Synchronous: the guest thread waited in execute() from the
                // submit until now (GPU work of the whole list, then the map).
                ++w->state->writeback_waits;
                w->state->writeback_wait_us += micros_since(w->submitted);
                w->done();
            }
            delete w;
            if (state != nullptr) state->writeback_finished(seq);
        }
    };
    for (Writeback::Entry& e : wb->entries) {
        WGPUBufferMapCallbackInfo cb = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
        cb.mode = WGPUCallbackMode_AllowSpontaneous;
        cb.userdata1 = wb;
        cb.userdata2 = &e;
        cb.callback = [](WGPUMapAsyncStatus status, WGPUStringView message, void* u1, void* u2) {
            auto* w = static_cast<Writeback*>(u1);
            auto* entry = static_cast<Writeback::Entry*>(u2);
            if (status == WGPUMapAsyncStatus_Success && entry->probe) {
                const void* data = wgpuBufferGetConstMappedRange(entry->readback, 0, wgsl::kGmemProbeWords * 4);
                if (data != nullptr) {
                    // computeprobe: what one dispatch did with global memory.
                    uint32_t v[wgsl::kGmemProbeWords];
                    std::memcpy(v, data, sizeof(v));
                    const uint32_t stores = v[wgsl::kProbeStoreHit] + v[wgsl::kProbeAtomicHit];
                    const uint32_t missed = v[wgsl::kProbeStoreMiss] + v[wgsl::kProbeAtomicMiss];
                    {
                        std::lock_guard lock(w->backend->stats_mutex_);
                        auto& d = w->backend->stats_.diagnostics;
                        ++d["probe: dispatches counted"];
                        if (v[wgsl::kProbeInvocations] == 0) ++d["probe: dispatch ran no invocation"];
                        if (stores > 0) ++d["probe: dispatch stores into a bound range"];
                        if (stores == 0 && missed > 0) ++d["probe: dispatch stores only outside every range (dropped)"];
                        if (stores == 0 && missed == 0) ++d["probe: dispatch stores nothing"];
                        if (v[wgsl::kProbeLoadMiss] > 0) ++d["probe: dispatch loads outside every range (read 0)"];
                        if (v[wgsl::kProbeLoadHit] == 0) ++d["probe: dispatch loads nothing from a bound range"];
                    }
                    // The first dispatches and the first two of every program in detail.
                    static std::mutex log_mutex;
                    static std::map<nvn::ProgramId, int> per_program;
                    static int logged = 0;
                    std::lock_guard lock(log_mutex);
                    if (logged < 48 || per_program[entry->program] < 2) {
                        ++logged;
                        ++per_program[entry->program];
                        std::string slots;
                        for (uint32_t k = 0; k < wgsl::kGlobalSlots; ++k)
                            slots += (k ? "/" : "") + std::to_string(v[wgsl::kProbeSlotStores + k]);
                        rt::log(rt::LogLevel::Warn,
                                "nvn webgpu: computeprobe: program %llu: invocations %u, loads %u in range / %u outside, "
                                "stores %u / %u (per slot %s), atomics %u / %u, outside addresses %u (first 0x%llx); %s",
                                (unsigned long long)entry->program, v[wgsl::kProbeInvocations],
                                v[wgsl::kProbeLoadHit], v[wgsl::kProbeLoadMiss], v[wgsl::kProbeStoreHit],
                                v[wgsl::kProbeStoreMiss], slots.c_str(), v[wgsl::kProbeAtomicHit],
                                v[wgsl::kProbeAtomicMiss], v[wgsl::kProbeAddressMiss],
                                (unsigned long long)(v[wgsl::kProbeFirstMissLo] |
                                                     uint64_t{v[wgsl::kProbeFirstMissHi]} << 32),
                                entry->bound.c_str());
                    }
                }
                wgpuBufferUnmap(entry->readback);
            } else if (status == WGPUMapAsyncStatus_Success) {
                const void* data = wgpuBufferGetConstMappedRange(entry->readback, 0, wgpuBufferGetSize(entry->readback));
                if (w->diag_state != nullptr && data != nullptr && w->memory != nullptr) {
                    // computeoverlap: what the writeback does to guest memory.
                    const auto* bytes = static_cast<const uint8_t*>(data);
                    const uint64_t fp = fingerprint(bytes, size_t(entry->size));
                    std::vector<uint8_t> before(size_t(entry->size));
                    const char* what =
                        fp == entry->initial_fp ? "writeback: compute left the buffer as uploaded"
                        : w->memory->read(entry->address, before.data(), before.size()) &&
                                std::memcmp(before.data(), bytes, before.size()) == 0
                            ? "writeback: compute output equals guest memory already"
                            : "writeback: compute output changes guest memory";
                    {
                        std::lock_guard lock(w->diag_state->diag_mutex);
                        for (State::WrittenBack& r : w->diag_state->written_back)
                            if (r.list == w->list && r.address == entry->address && r.size == entry->size) {
                                r.fp = fp;
                                r.done = true;
                            }
                    }
                    std::lock_guard lock(w->backend->stats_mutex_);
                    ++w->backend->stats_.diagnostics[what];
                }
                // The guest wrote into the range after the list's call
                // returned (asynchronous write-back): its bytes win where
                // they differ from what was uploaded, the results elsewhere.
                std::vector<uint8_t> merged;
                if (data != nullptr && w->memory != nullptr && entry->uploaded.size() == entry->size &&
                    !w->memory->equal(entry->address, entry->uploaded.data(), entry->size)) {
                    merged.resize(size_t(entry->size));
                    if (w->memory->read(entry->address, merged.data(), merged.size())) {
                        const auto* result = static_cast<const uint8_t*>(data);
                        for (size_t i = 0; i < merged.size(); ++i)
                            if (merged[i] == entry->uploaded[i]) merged[i] = result[i];
                        data = merged.data();
                        std::lock_guard lock(w->backend->stats_mutex_);
                        ++w->backend->stats_.diagnostics["writeback: guest wrote into the range meanwhile (its bytes kept)"];
                    }
                }
                if (data == nullptr || w->memory == nullptr || !w->memory->write(entry->address, data, entry->size))
                    rt::log(rt::LogLevel::Warn, "nvn webgpu: storage buffer 0x%llx+0x%llx not written back",
                            (unsigned long long)entry->address, (unsigned long long)entry->size);
                wgpuBufferUnmap(entry->readback);
            } else {
                rt::log(rt::LogLevel::Warn, "nvn webgpu: storage buffer readback failed: %s",
                        pw::to_string(message).c_str());
            }
            // A failed map leaves the buffer in an unknown state: not reused.
            if (w->pool && !entry->probe && status == WGPUMapAsyncStatus_Success)
                w->pool->give(entry->readback);
            else
                gpu_destroy_buffer(entry->readback);
            if (--w->pending == 0) {
                State* state = w->done ? nullptr : w->state;
                const uint64_t seq = w->seq;
                if (w->done) {
                    ++w->state->writeback_waits;  // as in `release`
                    w->state->writeback_wait_us += micros_since(w->submitted);
                    w->done();
                }
                delete w;
                if (state != nullptr) state->writeback_finished(seq);
            }
        };
        wgpuBufferMapAsync(e.readback, WGPUMapMode_Read, 0, wgpuBufferGetSize(e.readback), cb);
    }
    if (readback_first)
        submit_main();
    release(wb);
    if (caller_done) caller_done();
}

void WebGpuBackend::write_texture(nvn::TextureId id, uint32_t level, const nvn::Rect& region, const void* data,
                                  uint32_t row_stride, const nvn::TexelSlices& slices) {
    renderer_.run_sync([&] {
        State& s = *state_;
        auto it = s.find_texture(id);
        // Compressed texels (BC, ASTC, also ASTC decoded on the CPU), widened
        // 16-bit formats and substituted host formats are not uploaded yet;
        // every other format has the guest's bytes per texel.
        uint32_t bpp = 0;
        if (it != s.textures.end() && !it->second.cpu_decoded() && !is_widened_16bit(it->second.desc.format) &&
            it->second.format == texture_format(it->second.desc.format))
            if (const auto info = nvn::format_info(it->second.desc.format);
                info && info->block_width == 1 && info->block_height == 1)
                bpp = info->bytes_per_pixel;
        // A scaled render target holds host-sized texels (State::render_scale).
        if (bpp == 0 || it->second.texture == nullptr || it->second.scaled || it->second.sample_count != 1 ||
            region.width <= 0 ||
            region.height <= 0 || region.x < 0 || region.y < 0 ||
            uint64_t(region.x) + uint64_t(region.width) > std::max(it->second.desc.width >> level, 1u) ||
            uint64_t(region.y) + uint64_t(region.height) > std::max(it->second.desc.height >> level, 1u) ||
            slices.z < 0 || slices.depth <= 0 ||
            uint64_t(slices.z) + uint64_t(slices.depth) > std::max(it->second.desc.depth, 1u)) {
            rt::log(rt::LogLevel::Warn, "nvn webgpu: texel upload into texture #%u not supported", id);
            return;
        }
        // The contents now come from the API, not from the pool storage.
        it->second.gpu_written = true;
        WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
        dst.texture = it->second.texture;
        dst.mipLevel = level;
        dst.origin = {uint32_t(region.x), uint32_t(region.y), uint32_t(slices.z)};
        WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
        layout.bytesPerRow = row_stride;
        layout.rowsPerImage = uint32_t(region.height);
        const WGPUExtent3D extent = {uint32_t(region.width), uint32_t(region.height), uint32_t(slices.depth)};
        const size_t image = size_t(row_stride) * uint32_t(region.height);
        const size_t last = size_t(row_stride) * uint32_t(region.height - 1) + size_t(region.width) * bpp;
        std::vector<uint8_t> converted;
        if (is_norm16(it->second.desc.format)) {
            const size_t total = slices.depth == 1 || slices.image_stride == 0
                                     ? image * uint32_t(slices.depth - 1) + last
                                     : size_t(slices.image_stride) * uint32_t(slices.depth - 1) + last;
            converted.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + total);
            norm16_to_half(it->second.desc.format, converted.data(), converted.size());
            data = converted.data();
        }
        if (slices.depth == 1 || slices.image_stride == 0 || slices.image_stride == image) {
            queue_write_texture(s.queue, &dst, data, image * uint32_t(slices.depth - 1) + last, &layout, &extent);
        } else {
            // Slices image_stride apart: one write per slice.
            const WGPUExtent3D one = {extent.width, extent.height, 1};
            for (int32_t k = 0; k < slices.depth; ++k) {
                dst.origin.z = uint32_t(slices.z + k);
                queue_write_texture(s.queue, &dst, static_cast<const uint8_t*>(data) + size_t(k) * slices.image_stride,
                                      last, &layout, &one);
            }
        }
    });
    std::lock_guard lock(stats_mutex_);
    ++stats_.uploads;
}

bool WebGpuBackend::present(const nvn::PresentInfo& info) {
    Stats delta;
    // Presenting reads no guest memory: compute write-backs may still be on
    // their way (the guest sees them after nvnSyncWait, as on hardware).
    renderer_.run([&](pw::WebGpuRenderer::Call& call) {
        state_->after_writebacks([&] {
            State& s = *state_;
            // A few cached programs ahead per frame (pipeline cache); with
            // warm-all, the rest of a start the background interrupted.
            const auto t_warm = std::chrono::steady_clock::now();
            s.warm_step(s.warm_all ? std::numeric_limits<uint64_t>::max() : kWarmBudgetUs, delta);
            s.warm_steps.fetch_add(1, std::memory_order_relaxed);
            s.warm_step_ns.fetch_add(nanos_since(t_warm), std::memory_order_relaxed);
            auto it = s.find_texture(info.texture);
            // A pool texture shown directly (created lazily or evicted) gets
            // its contents first, as when sampled.
            if (it != s.textures.end() && it->second.texture != nullptr && !it->second.uploaded)
                s.upload_from_pool(it->second, memory_, s.frame, &delta);
            const uint64_t evicted_before = s.textures_evicted;
            s.evict_textures();
            delta.textures_evicted += s.textures_evicted - evicted_before;
            if (s.texture_report_every != 0 && s.frame % s.texture_report_every == 0) {
                s.texture_report();
                s.cpu_report();
            }
            ++s.frame;
            if (s.frame == 600 && s.fill_scale < 1.0f && !s.fill_targets.empty() && s.fill_hits == 0)
                rt::log(rt::LogLevel::Warn, "nvn webgpu: LGP_WEBGPU_FILL_TARGETS met no render target in 600 frames "
                        "(wrong size? a widescreen mod changes it)");
            if (it == s.textures.end() || it->second.raw == nullptr) {
                rt::log(rt::LogLevel::Warn, "nvn webgpu: present of texture #%u it cannot show", info.texture);
            } else {
                const State::Texture& t = it->second;
                // Crop in host texels of a scaled target (State::render_scale).
                const int64_t x0 = t.host_x(info.crop.x), y0 = t.host_y(info.crop.y);
                pw::PresentRect rect{int32_t(x0), int32_t(y0),
                                     int32_t(t.host_x(int64_t{info.crop.x} + info.crop.width) - x0),
                                     int32_t(t.host_y(int64_t{info.crop.y} + info.crop.height) - y0), info.flip_y,
                                     blit_filterable(t.format) && t.sample_count == 1};
                renderer_.present_texture(t.raw, t.width(), t.height(), rect);
            }
            if (s.max_frames_in_flight == 0) {
                call.finish();
                return;
            }
            s.frame_submitted();
            s.when_frames_at_most(s.max_frames_in_flight - 1, [&call] { call.finish(); });
        }, /*before=*/0);
    });
    {
        std::lock_guard lock(stats_mutex_);
        ++stats_.presents;
        stats_.programs_warmed += delta.programs_warmed;
        stats_.textures_evicted += delta.textures_evicted;
        stats_.astc_cpu_decoded += delta.astc_cpu_decoded;
        stats_.astc_error_blocks += delta.astc_error_blocks;
        stats_.bc_cpu_decoded += delta.bc_cpu_decoded;
        stats_.last_presented = info.texture;
        state_->presented[state_->presents_seen.fetch_add(1, std::memory_order_relaxed) % 4].store(
            info.texture, std::memory_order_relaxed);
        stats_.last_crop = info.crop;
        stats_.last_flip_y = info.flip_y;
    }
    return renderer_.end_frame();
}

bool WebGpuBackend::read_presented(std::vector<uint8_t>* out, uint32_t* width, uint32_t* height) {
    const Stats st = stats();
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> px;
    if (!read_texture(st.last_presented, &px, &w, &h))
        return false;
    nvn::Rect c = st.last_crop;
    // A scaled render target reads back at its host size; the crop is in
    // guest texels (State::render_scale).
    uint32_t gw = 0, gh = 0;
    renderer_.run_sync([&] {
        if (auto it = state_->textures.find(st.last_presented);
            it != state_->textures.end() && it->second.scaled) {
            gw = it->second.desc.width;
            gh = it->second.desc.height;
        }
    });
    if (gw != 0 && gh != 0 && (gw != w || gh != h)) {
        auto map = [](int64_t v, uint32_t host, uint32_t guest) {
            return int32_t(std::llround(double(v) * host / guest));
        };
        const int32_t x0 = map(c.x, w, gw), y0 = map(c.y, h, gh);
        const int32_t x1 = map(int64_t{c.x} + c.width, w, gw), y1 = map(int64_t{c.y} + c.height, h, gh);
        c = {x0, y0, x1 - x0, y1 - y0};
    }
    if (c.width > 0 && c.height > 0 && c.x >= 0 && c.y >= 0 && uint32_t(c.x + c.width) <= w &&
        uint32_t(c.y + c.height) <= h) {
        std::vector<uint8_t> part(size_t(c.width) * c.height * 4);
        for (int32_t y = 0; y < c.height; ++y)
            std::memcpy(part.data() + size_t(y) * c.width * 4, px.data() + (size_t(c.y + y) * w + c.x) * 4,
                        size_t(c.width) * 4);
        px = std::move(part);
        w = uint32_t(c.width);
        h = uint32_t(c.height);
    }
    if (st.last_flip_y)
        for (uint32_t y = 0; y < h / 2; ++y)
            std::swap_ranges(px.begin() + size_t(y) * w * 4, px.begin() + size_t(y + 1) * w * 4,
                             px.begin() + size_t(h - 1 - y) * w * 4);
    *out = std::move(px);
    if (width != nullptr) *width = w;
    if (height != nullptr) *height = h;
    return true;
}

void WebGpuBackend::set_guest_memory(const nvn::GuestMemoryView* memory) {
    // Pending write-backs hold the old view: at process exit it goes away
    // with the guest memory right after it is detached.
    if (memory_ != nullptr && memory != memory_)
        finish();
    memory_ = memory;
}

void WebGpuBackend::finish() { finish_until(UINT64_MAX); }

uint64_t WebGpuBackend::fence_mark() { return state_->lists_submitted.load(std::memory_order_acquire); }

void WebGpuBackend::finish_until(uint64_t mark) {
    ++state_->finish_calls;
    if (state_->writebacks_pending.load(std::memory_order_acquire) == 0 || !state_->pending_before(mark))
        return;  // nothing before the mark in flight: no round trip to the GPU thread
    ++state_->finish_pending;
    renderer_.run([&](pw::WebGpuRenderer::Call& call) {
        state_->after_writebacks([&call] { call.finish(); }, mark);
    });
}

void WebGpuBackend::submit_times(uint64_t* list_submits, uint64_t* list_submit_ns, uint64_t* warm_steps,
                                 uint64_t* warm_step_ns) const {
    const State& s = *state_;
    *list_submits = s.list_submits.load(std::memory_order_relaxed);
    *list_submit_ns = s.list_submit_ns.load(std::memory_order_relaxed);
    *warm_steps = s.warm_steps.load(std::memory_order_relaxed);
    *warm_step_ns = s.warm_step_ns.load(std::memory_order_relaxed);
}

WebGpuBackend::Stats WebGpuBackend::stats() const {
    Stats st;
    {
        std::lock_guard lock(stats_mutex_);
        st = stats_;
    }
    const State& s = *state_;
    st.writebacks = s.writebacks.load();
    st.writeback_waits = s.writeback_waits.load();
    st.writeback_wait_us = s.writeback_wait_us.load();
    st.finish_calls = s.finish_calls.load();
    st.finish_pending = s.finish_pending.load();
    st.frames_in_flight = s.frames_in_flight.load(std::memory_order_relaxed);
    st.frames_in_flight_max = s.frames_in_flight_max.load(std::memory_order_relaxed);
    st.frame_waits = s.frame_waits.load(std::memory_order_relaxed);
    st.frame_wait_us = s.frame_wait_us.load(std::memory_order_relaxed);
    st.list_submits = s.list_submits.load(std::memory_order_relaxed);
    st.list_submit_ns = s.list_submit_ns.load(std::memory_order_relaxed);
    st.warm_steps = s.warm_steps.load(std::memory_order_relaxed);
    st.warm_step_ns = s.warm_step_ns.load(std::memory_order_relaxed);
    st.samplers = s.sampler_count.load(std::memory_order_relaxed);
    st.depth_partners = s.depth_partner_count.load(std::memory_order_relaxed);
    if (s.buffer_pool) {
        st.buffer_pool_reused = s.buffer_pool->reused.load(std::memory_order_relaxed);
        st.buffer_pool_created = s.buffer_pool->created.load(std::memory_order_relaxed);
        std::lock_guard lock(s.buffer_pool->mutex);
        st.buffer_pool_free_bytes = s.buffer_pool->free_bytes;
    }
    {
        std::lock_guard lock(s.gpu_times->mutex);
        st.gpu_time_passes = s.gpu_times->passes;
        st.gpu_time_untimed = s.gpu_times->untimed;
        st.gpu_time_compute = s.gpu_times->compute;
        st.gpu_time_render = s.gpu_times->render;
        st.gpu_time_programs = s.gpu_times->programs;
        st.gpu_time_targets = s.gpu_times->targets;
        st.gpu_time_draw_programs = s.gpu_times->draw_programs;
    }
    const auto get = [](const std::atomic<uint64_t>& a) { return a.load(std::memory_order_relaxed); };
    st.buffers_created = get(g_counters.buffers_created);
    st.buffers_destroyed = get(g_counters.buffers_destroyed);
    st.buffer_bytes_created = get(g_counters.buffer_bytes_created);
    st.buffer_bytes_destroyed = get(g_counters.buffer_bytes_destroyed);
    st.textures_created = get(g_counters.textures_created);
    st.textures_destroyed = get(g_counters.textures_destroyed);
    st.texture_bytes_created = get(g_counters.texture_bytes_created);
    st.texture_bytes_destroyed = get(g_counters.texture_bytes_destroyed);
    st.queue_writes = get(g_counters.queue_writes);
    st.queue_write_bytes = get(g_counters.queue_write_bytes);
    return st;
}

WebGpuBackend::WindowPeaks WebGpuBackend::take_window_peaks() {
    WindowPeaks p;
    // The next window starts from what is live now.
    p.buffer_bytes = g_counters.buffer_bytes_peak.exchange(
        live_bytes(g_counters.buffer_bytes_created, g_counters.buffer_bytes_destroyed), std::memory_order_relaxed);
    p.texture_bytes = g_counters.texture_bytes_peak.exchange(
        live_bytes(g_counters.texture_bytes_created, g_counters.texture_bytes_destroyed), std::memory_order_relaxed);
    p.list_commands = g_counters.list_commands_peak.exchange(0, std::memory_order_relaxed);
    return p;
}

std::vector<WebGpuBackend::RenderedTexture> WebGpuBackend::rendered_textures() {
    std::vector<RenderedTexture> out;
    renderer_.run_sync([&] {
        for (const auto& [id, t] : state_->textures)
            if (t.gpu_written && t.attachment != nullptr)
                out.push_back({id, t.desc.width, t.desc.height, t.desc.format});
    });
    return out;
}

bool WebGpuBackend::read_texture(nvn::TextureId id, std::vector<uint8_t>* out, uint32_t* width, uint32_t* height) {
    struct Readback {
        WGPUBuffer buffer = nullptr;
        uint32_t width = 0, height = 0, row_pitch = 0;
        std::vector<uint8_t>* out = nullptr;
        bool ok = false;
        pw::WebGpuRenderer::Call* call = nullptr;
    } rb;
    rb.out = out;
    auto run = [&](pw::WebGpuRenderer::Call& call) {
        State& s = *state_;
        auto it = s.find_texture(id);
        if (it == s.textures.end() || it->second.texture == nullptr || it->second.sample_count != 1 ||
            bytes_per_texel(it->second.format) != 4) {
            call.finish();
            return;
        }
        // A texture that only lives in its pool storage reads back as such.
        if (!it->second.uploaded) {
            Stats delta;
            s.upload_from_pool(it->second, memory_, 0, &delta);
            std::lock_guard lock(stats_mutex_);
            stats_.astc_cpu_decoded += delta.astc_cpu_decoded;
            stats_.astc_error_blocks += delta.astc_error_blocks;
            stats_.bc_cpu_decoded += delta.bc_cpu_decoded;
        }
        const State::Texture& t = it->second;
        rb.width = t.width();  // host size: a scaled target reads back scaled
        rb.height = t.height();
        rb.row_pitch = (rb.width * 4 + 255) & ~255u;
        WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
        bd.label = pw::string_view("readback");
        bd.size = uint64_t{rb.row_pitch} * rb.height;
        bd.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
        rb.buffer = gpu_create_buffer(s.device, &bd);
        WGPUTexelCopyTextureInfo src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
        src.texture = t.texture;
        WGPUTexelCopyBufferInfo dst = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
        dst.buffer = rb.buffer;
        dst.layout.bytesPerRow = rb.row_pitch;
        dst.layout.rowsPerImage = rb.height;
        const WGPUExtent3D extent = {rb.width, rb.height, 1};
        WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(s.device, nullptr);
        wgpuCommandEncoderCopyTextureToBuffer(encoder, &src, &dst, &extent);
        WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder, nullptr);
        wgpuQueueSubmit(s.queue, 1, &commands);
        wgpuCommandBufferRelease(commands);
        wgpuCommandEncoderRelease(encoder);

        rb.call = &call;
        WGPUBufferMapCallbackInfo cb = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
        cb.mode = WGPUCallbackMode_AllowSpontaneous;
        cb.userdata1 = &rb;
        cb.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void* userdata1, void*) {
            auto* r = static_cast<Readback*>(userdata1);
            if (status == WGPUMapAsyncStatus_Success) {
                const auto* p = static_cast<const uint8_t*>(
                    wgpuBufferGetConstMappedRange(r->buffer, 0, uint64_t{r->row_pitch} * r->height));
                r->out->resize(size_t{r->width} * r->height * 4);
                for (uint32_t y = 0; y < r->height; ++y)
                    std::memcpy(r->out->data() + size_t{y} * r->width * 4, p + size_t{y} * r->row_pitch,
                                size_t{r->width} * 4);
                wgpuBufferUnmap(r->buffer);
                r->ok = true;
            }
            gpu_destroy_buffer(r->buffer);
            r->call->finish();
        };
        wgpuBufferMapAsync(rb.buffer, WGPUMapMode_Read, 0, uint64_t{rb.row_pitch} * rb.height, cb);
    };
    renderer_.run([&](pw::WebGpuRenderer::Call& call) { state_->after_writebacks([&] { run(call); }); });
    if (width != nullptr) *width = rb.width;
    if (height != nullptr) *height = rb.height;
    return rb.ok;
}

}  // namespace lgp::gpu::webgpu
