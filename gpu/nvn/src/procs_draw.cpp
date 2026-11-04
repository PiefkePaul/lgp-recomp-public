// NVN functions for drawing (P4.1 part 2): programs, state objects, vertex
// and uniform bindings, textures and samplers through their pools, draws.
//
// Same rules as procs.cpp: signatures follow the public shape of the NVN C
// API and are UNVERIFIED until checked against the driver in the game's sdk
// (docs/knowledge/nvn-hle.md, checklist ("Prüfliste")); enum values come from abi.hpp.
// The small state objects keep only a reference in guest storage
// (states.hpp).
#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "guest_access.hpp"
#include "hle_impl.hpp"
#include "lgp/gpu/shader/sph.h"
#include "lgp/runtime/log.hpp"
#include "lgp/runtime/render_hints.hpp"

namespace lgp::gpu::nvn {

// LGP_NVN_PROJECTION_SCAN=1 (diagnosis for wider pictures,
// docs/analysis/50-breitbild-ohne-strecken.md): looks in every bound uniform
// buffer (first 4 KiB) for a perspective camera matrix, once when it is bound
// and once when its command list is submitted (games often fill the buffer in
// between). Found are a projection P and view-projection P*V (also with a
// uniformly scaled model): in rows or columns r0..r3 (xyz parts) r0, r1 and r3
// are orthogonal and r2 is parallel to r3, which holds for every rigid view.
// aspect = |r1| / |r0|, vertical field of view 2 atan(|r3| / |r1|). One line
// per distinct (lr or submit, offset, kind, layout), the first time it is seen:
//   nvn projection: frame N cbuf 0x... +0x... view-projection (columns) aspect A fovy F deg from lr|submit
// LGP_NVN_PROJECTION_FROM_FRAME=N starts at present N (past an intro),
// LGP_NVN_PROJECTION_MAX_LINES=N (default 64). The buffer then goes to
// LGP_WATCH_WRITE to find the camera code.
bool projection_scan_on() {
    static const bool on = [] {
        const char* e = std::getenv("LGP_NVN_PROJECTION_SCAN");
        return e != nullptr && *e != '\0' && *e != '0';
    }();
    return on;
}

namespace {

struct Vec3 {
    double x, y, z;
};
double dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// a[16] as stored. *pure: P alone (the view part is the identity).
bool camera_matrix(const float* a, bool columns, double* aspect, double* fovy, bool* pure, double* w_norm) {
    Vec3 v[4];
    for (int i = 0; i < 4; ++i)
        v[i] = columns ? Vec3{a[i], a[4 + i], a[8 + i]} : Vec3{a[4 * i], a[4 * i + 1], a[4 * i + 2]};
    double n[4];
    for (int i = 0; i < 4; ++i) {
        n[i] = std::sqrt(dot(v[i], v[i]));
        if (!(n[i] > 1e-3 && n[i] < 1e4))  // also NaN
            return false;
    }
    constexpr double kOrtho = 2e-3, kParallel = 0.9995;
    if (std::fabs(dot(v[0], v[1])) > kOrtho * n[0] * n[1] || std::fabs(dot(v[0], v[3])) > kOrtho * n[0] * n[3] ||
        std::fabs(dot(v[1], v[3])) > kOrtho * n[1] * n[3] || std::fabs(dot(v[2], v[3])) < kParallel * n[2] * n[3])
        return false;
    *aspect = n[1] / n[0];
    *fovy = 2.0 * std::atan(n[3] / n[1]) * 180.0 / 3.14159265358979323846;
    // A rotation (+ translation) alone has aspect 1; square cameras are not
    // what we look for.
    if (!(*aspect > 0.2 && *aspect < 5.0) || std::fabs(*aspect - 1.0) < 0.01 || *fovy < 1.0 || *fovy > 170.0)
        return false;
    *w_norm = n[3];
    *pure = std::fabs(v[0].y) + std::fabs(v[0].z) < 1e-6 * n[0] && std::fabs(v[1].x) + std::fabs(v[1].z) < 1e-6 * n[1];
    return true;
}

struct CameraHit {
    bool columns = false, pure = false;
    double aspect = 0, fovy = 0, w = 0;
};
// Both layouts can pass (a projection read the wrong way round has a parallel
// pair too); the right one has a unit w row/column (the camera's forward
// axis), unless a model scale is in.
bool best_camera(const float* a, CameraHit* out) {
    CameraHit hits[2];
    bool ok[2];
    for (int l = 0; l < 2; ++l) {
        hits[l].columns = l == 1;
        ok[l] = camera_matrix(a, l == 1, &hits[l].aspect, &hits[l].fovy, &hits[l].pure, &hits[l].w);
    }
    if (!ok[0] && !ok[1])
        return false;
    const bool columns = !ok[0] || (ok[1] && std::fabs(std::log(hits[1].w)) < std::fabs(std::log(hits[0].w)));
    *out = hits[columns ? 1 : 0];
    return true;
}

}  // namespace

void projection_scan(uint64_t lr, bool at_submit, uint64_t va, uint64_t size) {
    if (va == 0)
        return;
    static const uint64_t from_frame = [] {
        const char* e = std::getenv("LGP_NVN_PROJECTION_FROM_FRAME");
        return e != nullptr ? std::strtoull(e, nullptr, 0) : 0;
    }();
    static const size_t max_lines = [] {
        const char* e = std::getenv("LGP_NVN_PROJECTION_MAX_LINES");
        const unsigned long long n = e != nullptr ? std::strtoull(e, nullptr, 0) : 0;
        return n != 0 ? size_t(n) : size_t(64);
    }();
    const uint64_t frame = presents_so_far();
    if (frame < from_frame)
        return;
    static std::mutex m;
    static std::set<std::tuple<uint64_t, uint64_t, bool, bool>> seen;
    {
        std::lock_guard lock(m);
        if (seen.size() >= max_lines)
            return;
    }
    const uint64_t n = std::min<uint64_t>(size, 4096) & ~uint64_t(15);
    if (n < 64)
        return;
    float data[1024];
    if (!guest::read(data, va, n))
        return;
    for (uint64_t off = 0; off + 64 <= n; off += 16) {
        CameraHit hit;
        if (!best_camera(data + off / 4, &hit))
            continue;
        const bool columns = hit.columns;
        {
            std::lock_guard lock(m);
            if (seen.size() >= max_lines || !seen.insert({at_submit ? ~uint64_t(0) : lr, off, hit.pure, columns}).second)
                continue;
        }
        char from[32] = "submit";
        if (!at_submit)
            std::snprintf(from, sizeof(from), "0x%" PRIx64, lr);
        rt::log(rt::LogLevel::Warn,
                "nvn projection: frame %" PRIu64 " cbuf 0x%" PRIx64 " +0x%" PRIx64 " %s (%s) aspect %.4f fovy %.2f deg from %s",
                frame, va, off, hit.pure ? "projection" : "view-projection", columns ? "columns" : "rows", hit.aspect, hit.fovy, from);
    }
}


namespace {

using rt::LogLevel;
using H = Hle::Impl;
using guest::bool_arg;
using guest::float_arg;
using guest::int_arg;
using guest::ptr_arg;
using guest::uint_arg;

void ret(CpuState* c, uint64_t v) { c->x[0] = v; }
void ret_bool(CpuState* c, bool v) { c->x[0] = v ? 1 : 0; }

uint32_t float_bits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}
float bits_float(uint32_t u) {
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

// Logs `what` once per process (unsupported features the game turned on).
#define LGP_WARN_ONCE(...)                                         \
    do {                                                           \
        static std::atomic<bool> warned_{false};                   \
        if (!warned_.exchange(true))                               \
            rt::log(LogLevel::Warn, __VA_ARGS__);                  \
    } while (0)

// ---------------------------------------------------------------------------
// Interned state objects (states.hpp).

template <typename T>
T load_value(H& h, uint64_t va, const char* fn) {
    return h.states.table<T>().lookup(guest::load<uint32_t>(va), fn);
}
template <typename T>
void store_value(H& h, uint64_t va, const T& v) {
    guest::store<uint32_t>(va, h.states.table<T>().intern(v));
}
template <typename T, typename F>
void update_value(H& h, uint64_t va, const char* fn, F&& f) {
    T v = load_value<T>(h, va, fn);
    f(v);
    store_value(h, va, v);
}
template <typename T>
void set_defaults(H& h, CpuState* c) {
    store_value(h, ptr_arg(c, 0), T{});
}

#define LGP_STATE_SETTER(Name, T, body)                                                         \
    void Name(H& h, CpuState* c) {                                                              \
        update_value<T>(h, ptr_arg(c, 0), "nvn" #Name, [&](T& s) { body; });                   \
    }

// BlendState
LGP_STATE_SETTER(BlendStateSetBlendTarget, BlendStateValue, s.target = uint_arg(c, 1))
LGP_STATE_SETTER(BlendStateSetBlendFunc, BlendStateValue, {
    s.src_color = uint_arg(c, 1);
    s.dst_color = uint_arg(c, 2);
    s.src_alpha = uint_arg(c, 3);
    s.dst_alpha = uint_arg(c, 4);
})
LGP_STATE_SETTER(BlendStateSetBlendEquation, BlendStateValue, {
    s.color_eq = uint_arg(c, 1);
    s.alpha_eq = uint_arg(c, 2);
})
// ColorState
LGP_STATE_SETTER(ColorStateSetBlendEnable, ColorStateValue, {
    const uint32_t index = uint_arg(c, 1);
    if (index < kMaxColorTargets) {
        if (bool_arg(c, 2))
            s.blend_enable_mask |= 1u << index;
        else
            s.blend_enable_mask &= ~(1u << index);
    }
})
LGP_STATE_SETTER(ColorStateSetLogicOp, ColorStateValue, s.logic_op = uint_arg(c, 1))
LGP_STATE_SETTER(ColorStateSetAlphaTest, ColorStateValue, s.alpha_func = uint_arg(c, 1))
// ChannelMaskState
LGP_STATE_SETTER(ChannelMaskStateSetChannelMask, ChannelMaskStateValue, {
    const uint32_t index = uint_arg(c, 1);
    if (index < kMaxColorTargets) {
        const uint32_t bits = (bool_arg(c, 2) ? 1u : 0u) | (bool_arg(c, 3) ? 2u : 0u) | (bool_arg(c, 4) ? 4u : 0u) |
                              (bool_arg(c, 5) ? 8u : 0u);
        s.masks = (s.masks & ~(0xFu << (4 * index))) | (bits << (4 * index));
    }
})
// DepthStencilState
LGP_STATE_SETTER(DepthStencilStateSetDepthTestEnable, DepthStencilStateValue, s.depth_test = bool_arg(c, 1))
LGP_STATE_SETTER(DepthStencilStateSetDepthWriteEnable, DepthStencilStateValue, s.depth_write = bool_arg(c, 1))
LGP_STATE_SETTER(DepthStencilStateSetDepthFunc, DepthStencilStateValue, s.depth_func = uint_arg(c, 1))
LGP_STATE_SETTER(DepthStencilStateSetStencilTestEnable, DepthStencilStateValue, s.stencil_test = bool_arg(c, 1))
LGP_STATE_SETTER(DepthStencilStateSetStencilFunc, DepthStencilStateValue, {
    const uint32_t faces = uint_arg(c, 1);
    for (int f = 0; f < 2; ++f)
        if (faces & (1u << f)) s.func[f] = uint_arg(c, 2);
})
LGP_STATE_SETTER(DepthStencilStateSetStencilOp, DepthStencilStateValue, {
    const uint32_t faces = uint_arg(c, 1);
    for (int f = 0; f < 2; ++f) {
        if (faces & (1u << f)) {
            s.fail[f] = uint_arg(c, 2);
            s.depth_fail[f] = uint_arg(c, 3);
            s.pass[f] = uint_arg(c, 4);
        }
    }
})
// MultisampleState
LGP_STATE_SETTER(MultisampleStateSetMultisampleEnable, MultisampleStateValue, s.enable = bool_arg(c, 1))
LGP_STATE_SETTER(MultisampleStateSetSamples, MultisampleStateValue, s.samples = uint_arg(c, 1))
LGP_STATE_SETTER(MultisampleStateSetAlphaToCoverageEnable, MultisampleStateValue,
                 s.alpha_to_coverage = bool_arg(c, 1))
// PolygonState
LGP_STATE_SETTER(PolygonStateSetCullFace, PolygonStateValue, s.cull = uint_arg(c, 1))
LGP_STATE_SETTER(PolygonStateSetFrontFace, PolygonStateValue, s.front_face = uint_arg(c, 1))
LGP_STATE_SETTER(PolygonStateSetPolygonMode, PolygonStateValue, s.mode = uint_arg(c, 1))
LGP_STATE_SETTER(PolygonStateSetPolygonOffsetEnables, PolygonStateValue, s.offset_enables = uint_arg(c, 1))
// VertexAttribState / VertexStreamState
LGP_STATE_SETTER(VertexAttribStateSetFormat, VertexAttribStateValue, {
    s.format = uint_arg(c, 1);
    s.offset = uint_arg(c, 2);
})
LGP_STATE_SETTER(VertexAttribStateSetStreamIndex, VertexAttribStateValue, s.stream = uint_arg(c, 1))
LGP_STATE_SETTER(VertexStreamStateSetStride, VertexStreamStateValue, s.stride = uint_arg(c, 1))
LGP_STATE_SETTER(VertexStreamStateSetDivisor, VertexStreamStateValue, s.divisor = uint_arg(c, 1))
// SamplerBuilder
LGP_STATE_SETTER(SamplerBuilderSetMinMagFilter, SamplerBuilderValue, {
    s.min_filter = uint_arg(c, 1);
    s.mag_filter = uint_arg(c, 2);
})
LGP_STATE_SETTER(SamplerBuilderSetWrapMode, SamplerBuilderValue, {
    s.wrap[0] = uint_arg(c, 1);
    s.wrap[1] = uint_arg(c, 2);
    s.wrap[2] = uint_arg(c, 3);
})
LGP_STATE_SETTER(SamplerBuilderSetLodClamp, SamplerBuilderValue, {
    s.min_lod_bits = float_bits(float_arg(c, 0));
    s.max_lod_bits = float_bits(float_arg(c, 1));
})
LGP_STATE_SETTER(SamplerBuilderSetLodBias, SamplerBuilderValue, s.lod_bias_bits = float_bits(float_arg(c, 0)))
LGP_STATE_SETTER(SamplerBuilderSetMaxAnisotropy, SamplerBuilderValue,
                 s.max_anisotropy_bits = float_bits(float_arg(c, 0)))
LGP_STATE_SETTER(SamplerBuilderSetCompare, SamplerBuilderValue, {
    s.compare_mode = uint_arg(c, 1);
    s.compare_func = uint_arg(c, 2);
})
LGP_STATE_SETTER(SamplerBuilderSetReductionFilter, SamplerBuilderValue, s.reduction = uint_arg(c, 1))
#undef LGP_STATE_SETTER
void SamplerBuilderSetBorderColor(H& h, CpuState* c) {
    float rgba[4];
    guest::read(rgba, ptr_arg(c, 1), sizeof(rgba));
    update_value<SamplerBuilderValue>(h, ptr_arg(c, 0), __func__, [&](SamplerBuilderValue& s) {
        for (int i = 0; i < 4; ++i) s.border[i] = float_bits(rgba[i]);
    });
}
void SamplerBuilderSetDevice(H&, CpuState*) {}

#define LGP_DEFAULTS(Type, T) \
    void Type##SetDefaults(H& h, CpuState* c) { set_defaults<T>(h, c); }
LGP_DEFAULTS(BlendState, BlendStateValue)
LGP_DEFAULTS(ColorState, ColorStateValue)
LGP_DEFAULTS(ChannelMaskState, ChannelMaskStateValue)
LGP_DEFAULTS(DepthStencilState, DepthStencilStateValue)
LGP_DEFAULTS(MultisampleState, MultisampleStateValue)
LGP_DEFAULTS(PolygonState, PolygonStateValue)
LGP_DEFAULTS(VertexAttribState, VertexAttribStateValue)
LGP_DEFAULTS(VertexStreamState, VertexStreamStateValue)
LGP_DEFAULTS(SamplerBuilder, SamplerBuilderValue)
#undef LGP_DEFAULTS

// ---------------------------------------------------------------------------
// NVN enums -> backend model.

CompareFunc compare_func(uint32_t v) {
    switch (static_cast<CompareFuncNvn>(v)) {
    case CompareFuncNvn::Never: return CompareFunc::Never;
    case CompareFuncNvn::Less: return CompareFunc::Less;
    case CompareFuncNvn::Equal: return CompareFunc::Equal;
    case CompareFuncNvn::LessEqual: return CompareFunc::LessEqual;
    case CompareFuncNvn::Greater: return CompareFunc::Greater;
    case CompareFuncNvn::NotEqual: return CompareFunc::NotEqual;
    case CompareFuncNvn::GreaterEqual: return CompareFunc::GreaterEqual;
    case CompareFuncNvn::Always: return CompareFunc::Always;
    }
    LGP_WARN_ONCE("nvn: unknown compare function %u, using ALWAYS", v);
    return CompareFunc::Always;
}

StencilOp stencil_op(uint32_t v) {
    switch (static_cast<StencilOpNvn>(v)) {
    case StencilOpNvn::Keep: return StencilOp::Keep;
    case StencilOpNvn::Zero: return StencilOp::Zero;
    case StencilOpNvn::Replace: return StencilOp::Replace;
    case StencilOpNvn::Increment: return StencilOp::IncrementClamp;
    case StencilOpNvn::Decrement: return StencilOp::DecrementClamp;
    case StencilOpNvn::Invert: return StencilOp::Invert;
    case StencilOpNvn::IncrementWrap: return StencilOp::IncrementWrap;
    case StencilOpNvn::DecrementWrap: return StencilOp::DecrementWrap;
    }
    LGP_WARN_ONCE("nvn: unknown stencil op %u, using KEEP", v);
    return StencilOp::Keep;
}

BlendFactor blend_factor(uint32_t v) {
    switch (static_cast<BlendFuncNvn>(v)) {
    case BlendFuncNvn::Zero: return BlendFactor::Zero;
    case BlendFuncNvn::One: return BlendFactor::One;
    case BlendFuncNvn::SrcColor: return BlendFactor::SrcColor;
    case BlendFuncNvn::OneMinusSrcColor: return BlendFactor::OneMinusSrcColor;
    case BlendFuncNvn::SrcAlpha: return BlendFactor::SrcAlpha;
    case BlendFuncNvn::OneMinusSrcAlpha: return BlendFactor::OneMinusSrcAlpha;
    case BlendFuncNvn::DstAlpha: return BlendFactor::DstAlpha;
    case BlendFuncNvn::OneMinusDstAlpha: return BlendFactor::OneMinusDstAlpha;
    case BlendFuncNvn::DstColor: return BlendFactor::DstColor;
    case BlendFuncNvn::OneMinusDstColor: return BlendFactor::OneMinusDstColor;
    case BlendFuncNvn::SrcAlphaSaturate: return BlendFactor::SrcAlphaSaturated;
    case BlendFuncNvn::ConstantColor: return BlendFactor::Constant;
    case BlendFuncNvn::OneMinusConstantColor: return BlendFactor::OneMinusConstant;
    }
    LGP_WARN_ONCE("nvn: blend function %u not supported, using ONE", v);
    return BlendFactor::One;
}

BlendOp blend_op(uint32_t v) {
    switch (static_cast<BlendEquationNvn>(v)) {
    case BlendEquationNvn::Add: return BlendOp::Add;
    case BlendEquationNvn::Subtract: return BlendOp::Subtract;
    case BlendEquationNvn::ReverseSubtract: return BlendOp::ReverseSubtract;
    case BlendEquationNvn::Min: return BlendOp::Min;
    case BlendEquationNvn::Max: return BlendOp::Max;
    }
    LGP_WARN_ONCE("nvn: blend equation %u not supported, using ADD", v);
    return BlendOp::Add;
}

// false = topology WebGPU cannot draw (logged, the draw is skipped).
bool topology(uint32_t mode, Topology* out) {
    switch (static_cast<DrawPrimitive>(mode)) {
    case DrawPrimitive::Points: *out = Topology::Points; return true;
    case DrawPrimitive::Lines: *out = Topology::Lines; return true;
    case DrawPrimitive::LineStrip: *out = Topology::LineStrip; return true;
    case DrawPrimitive::Triangles: *out = Topology::Triangles; return true;
    case DrawPrimitive::TriangleStrip: *out = Topology::TriangleStrip; return true;
    default: break;
    }
    LGP_WARN_ONCE("nvn: draw primitive %u not supported yet (draw skipped)", mode);
    return false;
}

bool index_type(uint32_t type, IndexType* out) {
    switch (static_cast<IndexTypeNvn>(type)) {
    case IndexTypeNvn::UnsignedByte: *out = IndexType::U8; return true;
    case IndexTypeNvn::UnsignedShort: *out = IndexType::U16; return true;
    case IndexTypeNvn::UnsignedInt: *out = IndexType::U32; return true;
    }
    LGP_WARN_ONCE("nvn: index type %u unknown (draw skipped)", type);
    return false;
}

bool vertex_format(uint32_t format, VertexFormat* out) {
    switch (format) {
    case kFormatR32F: *out = VertexFormat::Float32; return true;
    case kFormatRG32F: *out = VertexFormat::Float32x2; return true;
    case kFormatRGB32F: *out = VertexFormat::Float32x3; return true;
    case uint32_t(Format::RGBA32F): *out = VertexFormat::Float32x4; return true;
    case uint32_t(Format::RGBA8): *out = VertexFormat::Unorm8x4; return true;
    // The other float-readable formats by NVN value (public enum order, see
    // nvn::format_info). 3-component 8/16-bit formats have no WebGPU match.
    case uint32_t(Format::R8): *out = VertexFormat::Unorm8; return true;
    case 2: *out = VertexFormat::Snorm8; return true;            // R8SN
    case 5: *out = VertexFormat::Float16; return true;           // R16F
    case 6: *out = VertexFormat::Unorm16; return true;           // R16
    case 7: *out = VertexFormat::Snorm16; return true;           // R16SN
    case uint32_t(Format::RG8): *out = VertexFormat::Unorm8x2; return true;
    case 14: *out = VertexFormat::Snorm8x2; return true;         // RG8SN
    case 17: *out = VertexFormat::Float16x2; return true;        // RG16F
    case 18: *out = VertexFormat::Unorm16x2; return true;        // RG16
    case 19: *out = VertexFormat::Snorm16x2; return true;        // RG16SN
    case 38: *out = VertexFormat::Snorm8x4; return true;         // RGBA8SN
    case uint32_t(Format::RGBA16F): *out = VertexFormat::Float16x4; return true;
    case 42: *out = VertexFormat::Unorm16x4; return true;        // RGBA16
    case 43: *out = VertexFormat::Snorm16x4; return true;        // RGBA16SN
    case uint32_t(Format::RGB10A2): *out = VertexFormat::Unorm10_10_10_2; return true;
    case uint32_t(Format::BGRA8): *out = VertexFormat::Unorm8x4Bgra; return true;
    // Integer formats (raw integers in the shader, e.g. bone indices).
    // RGB8/RGB16 integer formats (vertex-only 25-33) have no WebGPU match.
    case 3: *out = VertexFormat::Uint8; return true;              // R8UI
    case 4: *out = VertexFormat::Sint8; return true;              // R8I
    case 8: *out = VertexFormat::Uint16; return true;             // R16UI
    case 9: *out = VertexFormat::Sint16; return true;             // R16I
    case 11: *out = VertexFormat::Uint32; return true;            // R32UI
    case 12: *out = VertexFormat::Sint32; return true;            // R32I
    case 15: *out = VertexFormat::Uint8x2; return true;           // RG8UI
    case 16: *out = VertexFormat::Sint8x2; return true;           // RG8I
    case 20: *out = VertexFormat::Uint16x2; return true;          // RG16UI
    case 21: *out = VertexFormat::Sint16x2; return true;          // RG16I
    case 23: *out = VertexFormat::Uint32x2; return true;          // RG32UI
    case 24: *out = VertexFormat::Sint32x2; return true;          // RG32I
    case 35: *out = VertexFormat::Uint32x3; return true;          // RGB32UI
    case 36: *out = VertexFormat::Sint32x3; return true;          // RGB32I
    case 39: *out = VertexFormat::Uint8x4; return true;           // RGBA8UI
    case 40: *out = VertexFormat::Sint8x4; return true;           // RGBA8I
    case 44: *out = VertexFormat::Uint16x4; return true;          // RGBA16UI
    case 45: *out = VertexFormat::Sint16x4; return true;          // RGBA16I
    case 47: *out = VertexFormat::Uint32x4; return true;          // RGBA32UI
    case 48: *out = VertexFormat::Sint32x4; return true;          // RGBA32I
    case 62: *out = VertexFormat::Uint10_10_10_2; return true;    // RGB10A2UI
    // Vertex-only formats 81-108 (public enum order, UNVERIFIED like the
    // names above; checklist ("Prüfliste") in nvn-hle.md): integers converted to float
    // without normalising (_UI2F/_I2F) and packed 10:10:10:2 words.
    // RGB8/RGB16 _UI2F/_I2F (93-96) have no 3-component WebGPU match.
    case 81: *out = VertexFormat::UScaled8; return true;          // R8_UI2F
    case 82: *out = VertexFormat::SScaled8; return true;          // R8_I2F
    case 83: *out = VertexFormat::UScaled16; return true;         // R16_UI2F
    case 84: *out = VertexFormat::SScaled16; return true;         // R16_I2F
    case 85: *out = VertexFormat::UScaled32; return true;         // R32_UI2F
    case 86: *out = VertexFormat::SScaled32; return true;         // R32_I2F
    case 87: *out = VertexFormat::UScaled8x2; return true;        // RG8_UI2F
    case 88: *out = VertexFormat::SScaled8x2; return true;        // RG8_I2F
    case 89: *out = VertexFormat::UScaled16x2; return true;       // RG16_UI2F
    case 90: *out = VertexFormat::SScaled16x2; return true;       // RG16_I2F
    case 91: *out = VertexFormat::UScaled32x2; return true;       // RG32_UI2F
    case 92: *out = VertexFormat::SScaled32x2; return true;       // RG32_I2F
    case 97: *out = VertexFormat::UScaled32x3; return true;       // RGB32_UI2F
    case 98: *out = VertexFormat::SScaled32x3; return true;       // RGB32_I2F
    case 99: *out = VertexFormat::UScaled8x4; return true;        // RGBA8_UI2F
    case 100: *out = VertexFormat::SScaled8x4; return true;       // RGBA8_I2F
    case 101: *out = VertexFormat::UScaled16x4; return true;      // RGBA16_UI2F
    case 102: *out = VertexFormat::SScaled16x4; return true;      // RGBA16_I2F
    case 103: *out = VertexFormat::UScaled32x4; return true;      // RGBA32_UI2F
    case 104: *out = VertexFormat::SScaled32x4; return true;      // RGBA32_I2F
    case 105: *out = VertexFormat::Snorm10_10_10_2; return true;  // RGB10A2SN
    case 106: *out = VertexFormat::Sint10_10_10_2; return true;   // RGB10A2I
    case 107: *out = VertexFormat::UScaled10_10_10_2; return true; // RGB10A2_UI2F
    case 108: *out = VertexFormat::SScaled10_10_10_2; return true; // RGB10A2_I2F
    default: break;
    }
    return false;
}

// Bound texture, uniform and storage buffer stages: vertex, fragment, compute.
bool shader_stage(uint32_t stage, ShaderStage* out, const char* fn) {
    switch (static_cast<ShaderStageNvn>(stage)) {
    case ShaderStageNvn::Vertex: *out = ShaderStage::Vertex; return true;
    case ShaderStageNvn::Fragment: *out = ShaderStage::Fragment; return true;
    case ShaderStageNvn::Compute: *out = ShaderStage::Compute; return true;
    default: break;
    }
    LGP_WARN_ONCE("nvn: %s for shader stage %u, only vertex, fragment and compute are supported", fn, stage);
    return false;
}

Filter mag_filter(uint32_t v) { return v == uint32_t(MagFilterNvn::Nearest) ? Filter::Nearest : Filter::Linear; }

AddressMode address_mode(uint32_t v) {
    switch (static_cast<WrapModeNvn>(v)) {
    case WrapModeNvn::Repeat: return AddressMode::Repeat;
    case WrapModeNvn::MirroredRepeat: return AddressMode::MirrorRepeat;
    case WrapModeNvn::ClampToEdge:
    case WrapModeNvn::Clamp: return AddressMode::ClampToEdge;
    case WrapModeNvn::ClampToBorder: return AddressMode::ClampToBorder;
    default: break;
    }
    // Once per value: a single warning would hide the other modes.
    static std::atomic<uint32_t> warned{0};
    if (v < 32 && !(warned.fetch_or(1u << v) & (1u << v)))
        rt::log(LogLevel::Warn, "nvn: wrap mode %u approximated as CLAMP_TO_EDGE", v);
    return AddressMode::ClampToEdge;
}

SamplerDesc sampler_desc(const SamplerBuilderValue& b) {
    SamplerDesc d;
    d.mag = mag_filter(b.mag_filter);
    switch (static_cast<MinFilterNvn>(b.min_filter)) {
    case MinFilterNvn::Nearest: d.min = Filter::Nearest; break;
    case MinFilterNvn::Linear: d.min = Filter::Linear; break;
    case MinFilterNvn::NearestMipmapNearest: d.min = Filter::Nearest, d.mip = Filter::Nearest, d.mipmapped = true; break;
    case MinFilterNvn::LinearMipmapNearest: d.min = Filter::Linear, d.mip = Filter::Nearest, d.mipmapped = true; break;
    case MinFilterNvn::NearestMipmapLinear: d.min = Filter::Nearest, d.mip = Filter::Linear, d.mipmapped = true; break;
    case MinFilterNvn::LinearMipmapLinear: d.min = Filter::Linear, d.mip = Filter::Linear, d.mipmapped = true; break;
    default: LGP_WARN_ONCE("nvn: min filter %u unknown, using LINEAR", b.min_filter); break;
    }
    for (int i = 0; i < 3; ++i) d.wrap[i] = address_mode(b.wrap[i]);
    for (int i = 0; i < 4; ++i) d.border[i] = bits_float(b.border[i]);
    d.min_lod = bits_float(b.min_lod_bits);
    d.max_lod = bits_float(b.max_lod_bits);
    d.max_anisotropy = uint16_t(std::clamp(bits_float(b.max_anisotropy_bits), 1.0f, 16.0f));
    // NVNcompareMode: 0 = NONE, 1 = COMPARE_R_TO_TEXTURE (UNVERIFIED, public header order).
    d.compare = b.compare_mode != 0;
    if (d.compare) d.compare_func = compare_func(b.compare_func);
    // The hardware keeps the bias as signed fixed point with 8 fraction bits
    // in [-16, 16); the shader receives it the same way (wgsl.h).
    d.lod_bias = std::clamp(bits_float(b.lod_bias_bits), -16.0f, 15.99609375f);
    return d;
}

// ---------------------------------------------------------------------------
// Programs. nvnProgramSetShaders(program, count, NVNshaderData[count]):
// every entry points at stage code in GPU memory (NVN code header, SPH,
// Maxwell code); the stage comes from the SPH's shader type. The control
// section (driver metadata) is not needed.

// Bytes from va to the end of the memory pool containing it, at most 1 MiB:
// the translator stops at EXIT, it only needs an upper bound.
uint64_t readable_extent(H& h, uint64_t va) {
    for (const auto& [obj, pool] : h.pools)
        if (va >= pool.storage && va < pool.storage + pool.size)
            return std::min<uint64_t>(pool.storage + pool.size - va, 1u << 20);
    return 0;
}

void ProgramInitialize(H& h, CpuState* c) {
    h.programs[ptr_arg(c, 0)] = {};
    ret_bool(c, true);
}
void ProgramFinalize(H& h, CpuState* c) { h.programs.erase(ptr_arg(c, 0)); }
void ProgramSetDebugLabel(H&, CpuState*) {}

// A valid binary the translator cannot handle (yet): fails SetShaders, or with
// accept_untranslated_programs succeeds and leaves the program without shaders
// (its draws are skipped). The reason is grouped by the translator's last
// message part ("vertex: 0x58: global memory ..." -> "global memory ...").
bool untranslated(H& h, H::Program& program, const std::string& message) {
    program = {};
    const bool accept = h.options.accept_untranslated_programs;
    rt::log(accept ? LogLevel::Warn : LogLevel::Error, "nvn: nvnProgramSetShaders: %s%s", message.c_str(),
            accept ? " (program accepted without shaders, its draws are skipped)" : "");
    if (!accept)
        return false;
    const size_t colon = message.rfind(": ");
    ++h.untranslated;
    ++h.untranslated_reasons[colon == std::string::npos ? message : message.substr(colon + 2)];
    program.untranslated = std::make_shared<const std::string>(message);
    return true;
}

// Translates (or finds in the cache) and binds the result to the program object.
bool set_program(H& h, H::Program& program, std::string key, bool compute,
                 const std::function<bool(ProgramDesc*, std::string*)>& translate) {
    if (auto cached = h.program_cache.find(key); cached != h.program_cache.end()) {
        program = cached->second;
        // id 0: a binary the translator rejected before (not tried again).
        return program.id != 0 || h.options.accept_untranslated_programs;
    }
    ProgramDesc desc;
    std::string error;
    const auto t0 = std::chrono::steady_clock::now();
    const bool translated = translate(&desc, &error);
    desc.guest_hash = 0xcbf29ce484222325ull;  // FNV-1a 64 of the key (ProgramDesc::guest_hash)
    for (const char ch : key)
        desc.guest_hash = (desc.guest_hash ^ static_cast<uint8_t>(ch)) * 0x100000001b3ull;
    // Translation runs on the guest thread when the game sets the shaders
    // (first use of an effect); slow ones are logged.
    if (const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ms >= 2.0)
        rt::log(LogLevel::Info, "nvn: %s shader translated in %.1f ms", compute ? "compute" : "graphics", ms);
    if (!translated) {
        const bool ok = untranslated(h, program, "translation failed: " + error);
        H::Program failed;
        failed.untranslated = program.untranslated;
        h.program_cache.emplace(std::move(key), failed);
        return ok;
    }
    program = {h.next_program_id++, compute};
    h.backend.create_program(program.id, desc);
    h.program_cache.emplace(std::move(key), program);
    return true;
}

// A compute program: one stage whose code starts at the control section's
// entry offset (no NVN code header, no SPH); workgroup and shared memory
// size come from the control section through HleOptions::compute_info or
// the built-in reader (parse_compute_control).
bool set_compute_shader(H& h, H::Program& program, std::vector<uint8_t> code, uint64_t control) {
    ComputeInfo info;
    std::string error;
    bool ok;
    if (h.options.compute_info) {
        ok = h.options.compute_info(control, code, &info, &error);
    } else if (control == 0 || control % 4 != 0) {
        error = "no control section";
        ok = false;
    } else {
        uint8_t bytes[kNvnControlComputeEnd];
        guest::read(bytes, control, sizeof(bytes));
        ok = parse_compute_control(bytes, sizeof(bytes), &info, &error);
    }
    if (!ok)
        return untranslated(h, program, "compute control section: " + error);
    // The key holds what the translation depends on: code and ComputeInfo.
    std::string key = "C";
    key.append(reinterpret_cast<const char*>(&info), sizeof(info));
    key.append(reinterpret_cast<const char*>(code.data()), code.size());
    const uint32_t base = h.options.bound_texture_handle_base;
    return set_program(h, program, std::move(key), true, [&](ProgramDesc* d, std::string* e) {
        return translate_maxwell_compute(code, info, base, d, e);
    });
}

// The compiler constant pool of a graphics stage whose code (with NVN code
// header) starts at `data`: offset and size from the control section (CPU
// memory), see kNvnControlConstantsOffset. Nothing for an unknown control
// section or a pool that does not fit the code blob.
H::Program::Constants compiler_constants(H& h, uint64_t data, uint64_t control) {
    if (control == 0 || control % 4 != 0 || guest::load<uint32_t>(control) != kNvnControlMagic)
        return {};
    const uint32_t size = guest::load<uint32_t>(control + kNvnControlConstantsSize);
    const uint32_t offset = guest::load<uint32_t>(control + kNvnControlConstantsOffset);
    const uint32_t blob = guest::load<uint32_t>(control + kNvnControlBlobSize);
    if (size == 0 || offset < kNvnCodeHeaderSize || uint64_t(offset) + size > blob ||
        readable_extent(h, data) < uint64_t(offset) + size) {
        if (size != 0)
            LGP_WARN_ONCE("nvn: shader constant pool (offset 0x%x, size 0x%x, code 0x%x) does not fit, ignored",
                          offset, size, blob);
        return {};
    }
    return {data + offset, size};
}

// The backend program for a vertex + fragment pair (translated once, cached
// by both stages' code). False if the translator rejects it and untranslated
// programs are not accepted; *id = 0 for a program without shaders.
bool graphics_program(H& h, const std::vector<uint8_t>& vs, const std::vector<uint8_t>& fs, ProgramId* id,
                      std::shared_ptr<const std::string>* reason = nullptr) {
    std::string key;
    key.reserve(vs.size() + fs.size() + 1);
    key.append(reinterpret_cast<const char*>(vs.data()), vs.size());
    key.append("|");
    key.append(reinterpret_cast<const char*>(fs.data()), fs.size());
    const ProgramTranslator& translate = h.options.program_translator;
    const uint32_t base = h.options.bound_texture_handle_base;
    H::Program result;
    const bool ok = set_program(h, result, std::move(key), false, [&](ProgramDesc* d, std::string* e) {
        return translate ? translate(vs, fs, base, d, e) : translate_maxwell_program(vs, fs, base, d, e);
    });
    *id = result.id;
    if (reason != nullptr)
        *reason = result.untranslated;
    return ok;
}

void ProgramSetShaders(H& h, CpuState* c) {
    auto it = h.programs.find(ptr_arg(c, 0));
    if (it == h.programs.end()) {
        rt::log(LogLevel::Warn, "nvn: nvnProgramSetShaders on unknown program 0x%" PRIx64, ptr_arg(c, 0));
        ret_bool(c, false);
        return;
    }
    const int32_t count = std::clamp(int_arg(c, 1), 0, 6);
    std::vector<uint8_t> stages[2];  // vertex, fragment
    H::Program::Constants constants[2];
    for (int32_t i = 0; i < count; ++i) {
        const uint64_t entry = ptr_arg(c, 2) + uint64_t(i) * kShaderDataSize;
        const uint64_t data = guest::load<uint64_t>(entry);
        // GPU addresses are pool addresses (identity): code outside every
        // pool is no shader the game uploaded.
        uint64_t code = data;
        const uint64_t control = guest::load<uint64_t>(entry + 8);
        const bool has_control =
            control != 0 && control % 4 == 0 && guest::load<uint32_t>(control) == kNvnControlMagic;
        const bool nvn_header = readable_extent(h, data) >= 4 && guest::load<uint32_t>(data) == kNvnCodeHeaderMagic;
        // A compute program has exactly one stage: the control section says
        // so (stage 5), and compute code never has the NVN code header.
        const bool compute =
            count == 1 && (!nvn_header || (has_control && guest::load<uint32_t>(control + kNvnControlStage) ==
                                                              kNvnStageCompute));
        // Where the code starts: graphics behind the NVN code header (at the
        // SPH), compute at the control section's entry offset (0 in LGP,
        // 0x100 in newer glslc output; abi.hpp kNvnControlEntryOffset).
        uint64_t start = nvn_header ? kNvnCodeHeaderSize : 0;
        if (compute && has_control) {
            const uint32_t entry_offset = guest::load<uint32_t>(control + kNvnControlEntryOffset);
            if (entry_offset % 8 == 0 && readable_extent(h, data) >= uint64_t(entry_offset) + 0x20)
                start = entry_offset;
            else
                LGP_WARN_ONCE("nvn: compute entry offset 0x%x outside the code, code read from its start",
                              entry_offset);
        }
        code += start;
        uint64_t extent = readable_extent(h, code);
        // The control section knows the blob size: read no further (keeps
        // the translation cache keys small).
        if (has_control && (nvn_header || compute)) {
            const uint32_t blob = guest::load<uint32_t>(control + kNvnControlBlobSize);
            if (blob >= start + (compute ? 0x20 : 0x58) && blob - start <= extent)
                extent = blob - start;
        }
        // Graphics stages start with the 0x50-byte SPH; compute code needs at
        // least one bundle.
        if (extent < (compute ? 0x20 : 0x58)) {
            rt::log(LogLevel::Error, "nvn: nvnProgramSetShaders: stage %d code 0x%" PRIx64 " is not in a memory pool",
                    i, data);
            ret_bool(c, false);
            return;
        }
        std::vector<uint8_t> bytes(extent);
        guest::read(bytes.data(), code, extent);
        if (compute) {
            // LGP_WEBGPU_DEBUG=computeprobe: the control section's constant
            // pool fields (LGP: size 0 in all 24 compute programs).
            static const bool probe = [] {
                const char* env = std::getenv("LGP_WEBGPU_DEBUG");
                return env != nullptr && std::strstr(env, "computeprobe") != nullptr;
            }();
            if (probe && has_control)
                rt::log(LogLevel::Warn,
                        "nvn: computeprobe: compute program 0x%" PRIx64 ": constant pool offset 0x%x size 0x%x, "
                        "blob 0x%x, code 0x%zx bytes",
                        ptr_arg(c, 0), guest::load<uint32_t>(control + kNvnControlConstantsOffset),
                        guest::load<uint32_t>(control + kNvnControlConstantsSize),
                        guest::load<uint32_t>(control + kNvnControlBlobSize), bytes.size());
            ret_bool(c, set_compute_shader(h, it->second, std::move(bytes), control));
            return;
        }
        // The stage comes from the SPH's shader type.
        shader::Sph sph;
        std::string error;
        const bool sph_ok = shader::parse_sph(bytes, sph, error);
        const int slot = !sph_ok                                  ? -1
                         : sph.stage == shader::Stage::Vertex     ? 0
                         : sph.stage == shader::Stage::Fragment   ? 1
                                                                  : -1;
        if (slot < 0 || !stages[slot].empty()) {
            rt::log(LogLevel::Error, "nvn: nvnProgramSetShaders: stage %d at 0x%" PRIx64 " is no further vertex or "
                                     "fragment shader (%s); tessellation and geometry are not supported yet",
                    i, data, sph_ok ? "other stage or duplicate" : error.c_str());
            ret_bool(c, false);
            return;
        }
        stages[slot] = std::move(bytes);
        if (nvn_header)
            constants[slot] = compiler_constants(h, data, control);
    }
    H::Program& program = it->second;
    program = {};
    for (int st = 0; st < 2; ++st) {
        program.constants[st] = constants[st];
        if (!stages[st].empty())
            program.code[st] = std::make_shared<const std::vector<uint8_t>>(std::move(stages[st]));
    }
    // A separable program (one graphics stage) is combined with the other
    // stage when it is bound (BindProgram with stage bits).
    if (!program.code[0] || !program.code[1]) {
        ret_bool(c, true);
        return;
    }
    const bool ok = graphics_program(h, *program.code[0], *program.code[1], &program.id, &program.untranslated);
    ret_bool(c, ok);
}

// ---------------------------------------------------------------------------
// Samplers and pools.

void SamplerInitialize(H& h, CpuState* c) {
    h.samplers[ptr_arg(c, 0)] = {sampler_desc(load_value<SamplerBuilderValue>(h, ptr_arg(c, 1), __func__))};
    ret_bool(c, true);
}
void SamplerFinalize(H& h, CpuState* c) { h.samplers.erase(ptr_arg(c, 0)); }
void SamplerSetDebugLabel(H&, CpuState*) {}

// nvn{Texture,Sampler}PoolInitialize(pool, memoryPool, offset, numDescriptors).
template <typename Pool>
bool init_pool(H& h, CpuState* c, std::unordered_map<uint64_t, Pool>& pools, const char* fn) {
    const uint64_t mem = ptr_arg(c, 1);
    auto mp = h.pools.find(mem);
    const int32_t n = int_arg(c, 3);
    if (mp == h.pools.end() || n <= 0) {
        rt::log(LogLevel::Error, "nvn: %s: memory pool 0x%" PRIx64 " unknown or %d descriptors", fn, mem, n);
        return false;
    }
    Pool p;
    p.storage = mp->second.storage + ptr_arg(c, 2);
    p.size = n;
    pools[ptr_arg(c, 0)] = std::move(p);
    return true;
}
void TexturePoolInitialize(H& h, CpuState* c) { ret_bool(c, init_pool(h, c, h.texture_pools, __func__)); }
void SamplerPoolInitialize(H& h, CpuState* c) { ret_bool(c, init_pool(h, c, h.sampler_pools, __func__)); }
void TexturePoolFinalize(H& h, CpuState* c) { h.texture_pools.erase(ptr_arg(c, 0)); }
void SamplerPoolFinalize(H& h, CpuState* c) { h.sampler_pools.erase(ptr_arg(c, 0)); }
void TexturePoolGetSize(H& h, CpuState* c) {
    auto it = h.texture_pools.find(ptr_arg(c, 0));
    ret(c, it == h.texture_pools.end() ? 0 : uint32_t(it->second.size));
}
void SamplerPoolGetSize(H& h, CpuState* c) {
    auto it = h.sampler_pools.find(ptr_arg(c, 0));
    ret(c, it == h.sampler_pools.end() ? 0 : uint32_t(it->second.size));
}
// TexturePoolRegisterTexture(pool, id, texture, view): of the view only the
// swizzle is applied yet (levels, layers and format views are not); the
// backend samples the whole texture.
void TexturePoolRegisterTexture(H& h, CpuState* c) {
    auto pool = h.texture_pools.find(ptr_arg(c, 0));
    auto tex = h.textures.find(ptr_arg(c, 2));
    const int32_t id = int_arg(c, 1);
    if (pool == h.texture_pools.end() || tex == h.textures.end() || id < 0 || id >= pool->second.size) {
        rt::log(LogLevel::Warn, "nvn: nvnTexturePoolRegisterTexture(0x%" PRIx64 ", %d, 0x%" PRIx64 ") ignored",
                ptr_arg(c, 0), id, ptr_arg(c, 2));
        return;
    }
    pool->second.textures[id] = {tex->second.id, texture_view_swizzle(ptr_arg(c, 3)), ptr_arg(c, 2)};
    auto& slots = h.texture_slots[ptr_arg(c, 2)];
    if (std::find(slots.begin(), slots.end(), std::make_pair(ptr_arg(c, 0), id)) == slots.end())
        slots.emplace_back(ptr_arg(c, 0), id);
    note_registered_view(ptr_arg(c, 3), tex->second.desc, tex->second.id, id);
}
void SamplerPoolRegisterSampler(H& h, CpuState* c) {
    auto pool = h.sampler_pools.find(ptr_arg(c, 0));
    auto smp = h.samplers.find(ptr_arg(c, 2));
    const int32_t id = int_arg(c, 1);
    if (pool == h.sampler_pools.end() || smp == h.samplers.end() || id < 0 || id >= pool->second.size) {
        rt::log(LogLevel::Warn, "nvn: nvnSamplerPoolRegisterSampler(0x%" PRIx64 ", %d, 0x%" PRIx64 ") ignored",
                ptr_arg(c, 0), id, ptr_arg(c, 2));
        return;
    }
    pool->second.samplers[id] = smp->second.desc;
}

// ---------------------------------------------------------------------------
// Texture uploads from the CPU.

// nvnTextureWriteTexels(texture, view, region, data) and the strided form
// (+ rowStride, imageStride). Level from the view; 2D regions and z ranges of
// 3D textures / layers of arrays (LGP writes a 32x32x32 RGBA8 3D texture).
void write_texels(H& h, CpuState* c, uint32_t row_stride, uint64_t image_stride, const char* fn) {
    auto tex = h.textures.find(ptr_arg(c, 0));
    if (tex == h.textures.end()) {
        rt::log(LogLevel::Warn, "nvn: %s on unknown texture 0x%" PRIx64, fn, ptr_arg(c, 0));
        return;
    }
    int32_t region[6];
    guest::read(region, ptr_arg(c, 2), kCopyRegionSize);
    const auto info = format_info(tex->second.desc.format);
    const int32_t depth = std::max(region[5], 1);
    if (!info || info->block_width != 1 || region[3] <= 0 || region[4] <= 0 || region[2] < 0 || depth > 2048) {
        LGP_WARN_ONCE("nvn: %s: only uncompressed regions of known formats are uploaded", fn);
        return;
    }
    const uint32_t row = uint32_t(region[3]) * info->bytes_per_pixel;
    const uint32_t stride = row_stride != 0 ? row_stride : row;
    const uint64_t image = image_stride != 0 ? image_stride : uint64_t(stride) * uint32_t(region[4]);
    const uint64_t bytes = image * uint32_t(depth - 1) + uint64_t(stride) * uint32_t(region[4] - 1) + row;
    std::vector<uint8_t> data(bytes);
    guest::read(data.data(), ptr_arg(c, 3), bytes);
    h.backend.write_texture(tex->second.id, 0, Rect{region[0], region[1], region[3], region[4]}, data.data(), stride,
                            TexelSlices{region[2], depth, image});
}
void TextureWriteTexels(H& h, CpuState* c) { write_texels(h, c, 0, 0, __func__); }
void TextureWriteTexelsStrided(H& h, CpuState* c) {
    write_texels(h, c, uint32_t(ptr_arg(c, 4)), ptr_arg(c, 5), __func__);
}
void TextureFlushTexels(H&, CpuState*) {}

// ---------------------------------------------------------------------------
// Command buffer: bindings, state, draws.

void push(H::CommandBuffer* cb, Command cmd) { cb->list->commands.push_back(std::move(cmd)); }

// Once per reason: which translator gaps cost draws in practice.
void log_unbound_program(ProgramId id, const std::shared_ptr<const std::string>& reason) {
    if (id != 0 || !reason)
        return;
    static std::mutex mutex;
    static std::set<std::string> logged;
    std::lock_guard lock(mutex);
    if (logged.insert(*reason).second)
        rt::log(LogLevel::Warn, "nvn: a program without shaders is bound for drawing (%s)", reason->c_str());
}

void CommandBufferBindProgram(H& h, CpuState* c) {
    auto* cb = h.recording(ptr_arg(c, 0), __func__);
    if (cb == nullptr)
        return;
    H::Program program;  // NULL unbinds the stages in the bits
    if (auto it = h.programs.find(ptr_arg(c, 1)); it != h.programs.end())
        program = it->second;
    else if (ptr_arg(c, 1) != 0)
        rt::log(LogLevel::Warn, "nvn: nvnCommandBufferBindProgram with unknown program 0x%" PRIx64, ptr_arg(c, 1));
    const uint32_t bits = uint_arg(c, 2);
    if (program.compute) {
        push(cb, CmdBindProgram{program.id, true});
        return;
    }
    // Graphics: every stage named in the bits comes from this program (a
    // stage the program lacks is unbound); the pipeline is the pair bound now.
    const uint32_t stage_bits[2] = {kShaderStageBitVertex, kShaderStageBitFragment};
    ProgramId id = program.id;
    bool from_program = true;
    for (int st = 0; st < 2; ++st) {
        if (bits & stage_bits[st]) {
            cb->bound_code[st] = program.code[st];
            if (program.constants[st].size != 0)
                push(cb, CmdBindUniformBuffer{st == 0 ? ShaderStage::Vertex : ShaderStage::Fragment,
                                              kCompilerConstantBank, program.constants[st].address,
                                              program.constants[st].size});
        }
        from_program &= cb->bound_code[st] == program.code[st];
    }
    std::shared_ptr<const std::string> reason = program.untranslated;
    if (!from_program || id == 0) {
        id = 0;
        if (cb->bound_code[0] && cb->bound_code[1])
            (void)graphics_program(h, *cb->bound_code[0], *cb->bound_code[1], &id, &reason);
    }
    cb->vertex_only_pending = id == 0 && cb->bound_code[0] && !cb->bound_code[1];
    if (!cb->vertex_only_pending)
        log_unbound_program(id, reason);
    push(cb, CmdBindProgram{id, false});
}

// Before a draw: makes and binds the vertex-only program if only a vertex
// stage is bound (BindProgram(NULL, FRAGMENT) + BindProgram(vs, VERTEX) for
// depth and shadow passes).
void resolve_vertex_only(H& h, H::CommandBuffer* cb) {
    if (!cb->vertex_only_pending)
        return;
    cb->vertex_only_pending = false;
    ProgramId id = 0;
    std::shared_ptr<const std::string> reason;
    (void)graphics_program(h, *cb->bound_code[0], {}, &id, &reason);
    log_unbound_program(id, reason);
    push(cb, CmdBindProgram{id, false});
}

void emit_vertex_layout(H::CommandBuffer* cb) { push(cb, cb->vertex_layout); }

// BindVertexAttribState(cmd, count, states): attribute i = shader input i.
void CommandBufferBindVertexAttribState(H& h, CpuState* c) {
    auto* cb = h.recording(ptr_arg(c, 0), __func__);
    if (cb == nullptr)
        return;
    const int32_t n = std::clamp(int_arg(c, 1), 0, 32);
    cb->vertex_layout.attribs.clear();
    for (int32_t i = 0; i < n; ++i) {
        const auto v = load_value<VertexAttribStateValue>(h, ptr_arg(c, 2) + uint64_t(i) * kVertexAttribStateSize,
                                                          __func__);
        if (v.format == 0)
            continue;  // disabled
        VertexAttrib a;
        a.location = uint32_t(i);
        a.stream = v.stream;
        a.offset = v.offset;
        if (!vertex_format(v.format, &a.format)) {
            LGP_WARN_ONCE("nvn: vertex attribute format %u not supported yet (attribute dropped)", v.format);
            continue;
        }
        cb->vertex_layout.attribs.push_back(a);
    }
    emit_vertex_layout(cb);
}
void CommandBufferBindVertexStreamState(H& h, CpuState* c) {
    auto* cb = h.recording(ptr_arg(c, 0), __func__);
    if (cb == nullptr)
        return;
    const int32_t n = std::clamp(int_arg(c, 1), 0, 32);
    cb->vertex_layout.strides.assign(size_t(n), 0);
    cb->vertex_layout.divisors.assign(size_t(n), 0);
    for (int32_t i = 0; i < n; ++i) {
        const auto v = load_value<VertexStreamStateValue>(h, ptr_arg(c, 2) + uint64_t(i) * kVertexStreamStateSize,
                                                          __func__);
        cb->vertex_layout.strides[size_t(i)] = v.stride;
        cb->vertex_layout.divisors[size_t(i)] = v.divisor;
    }
    emit_vertex_layout(cb);
}

void CommandBufferBindVertexBuffer(H& h, CpuState* c) {
    auto* cb = h.recording(ptr_arg(c, 0), __func__);
    if (cb != nullptr)
        push(cb, CmdBindVertexBuffer{uint_arg(c, 1), ptr_arg(c, 2), ptr_arg(c, 3)});
}
void CommandBufferBindVertexBuffers(H& h, CpuState* c) {
    auto* cb = h.recording(ptr_arg(c, 0), __func__);
    if (cb == nullptr)
        return;
    const uint32_t first = uint_arg(c, 1);
    const int32_t n = std::clamp(int_arg(c, 2), 0, 32);
    for (int32_t i = 0; i < n; ++i) {
        uint64_t range[2];
        guest::read(range, ptr_arg(c, 3) + uint64_t(i) * kBufferRangeSize, sizeof(range));
        push(cb, CmdBindVertexBuffer{first + uint32_t(i), range[0], range[1]});
    }
}
}  // namespace

// LGP_NVN_CBUF_FIND=V[,V...] (diagnosis for wider pictures, analysis 50):
// where do the draws get a value from? Floats V (decimal, or f:A/B) are looked
// for in every uniform buffer bound for a draw of a submitted list (first
// 64 KiB, as the CPU left it at submit), relative tolerance
// LGP_NVN_CBUF_FIND_TOL (default 1e-4). One line per distinct (target size,
// stage, bank, offset, value), with LGP_NVN_PROJECTION_FROM_FRAME as start:
//   nvn cbuf find: frame N value V = X at cbuf 0x... +0x... (vertex c[B]) draw into WxH
// At most LGP_NVN_CBUF_FIND_MAX_LINES (default 64).
namespace {
struct CbufFind {
    std::vector<float> values;
    double tolerance = 1e-4;
    size_t max_lines = 64;
    uint64_t from_frame = 0;
};
const CbufFind& cbuf_find_config() {
    static const CbufFind c = [] {
        CbufFind c;
        if (const char* e = std::getenv("LGP_NVN_CBUF_FIND"); e != nullptr) {
            std::string spec(e);
            size_t start = 0;
            while (start < spec.size()) {
                const size_t comma = std::min(spec.find(',', start), spec.size());
                std::string item = spec.substr(start, comma - start);
                start = comma + 1;
                if (item.rfind("f:", 0) == 0)
                    item = item.substr(2);
                const size_t slash = item.find('/');
                char* end = nullptr;
                double v = std::strtod(item.c_str(), &end);
                if (slash != std::string::npos)
                    v /= std::strtod(item.c_str() + slash + 1, nullptr);
                if (end != item.c_str() && v != 0)
                    c.values.push_back(float(v));
                else
                    rt::log(rt::LogLevel::Warn, "LGP_NVN_CBUF_FIND: \"%s\" is no non-zero number, ignored", item.c_str());
            }
        }
        if (const char* e = std::getenv("LGP_NVN_CBUF_FIND_TOL"); e != nullptr && *e != '\0')
            c.tolerance = std::strtod(e, nullptr);
        if (const char* e = std::getenv("LGP_NVN_CBUF_FIND_MAX_LINES"); e != nullptr && *e != '\0')
            c.max_lines = std::max<size_t>(1, std::strtoull(e, nullptr, 0));
        if (const char* e = std::getenv("LGP_NVN_PROJECTION_FROM_FRAME"); e != nullptr)
            c.from_frame = std::strtoull(e, nullptr, 0);
        return c;
    }();
    return c;
}
}  // namespace

bool cbuf_find_on() { return !cbuf_find_config().values.empty(); }

void cbuf_find(Hle::Impl& h, const CommandList& list) {
    const CbufFind& cfg = cbuf_find_config();
    const uint64_t frame = presents_so_far();
    if (frame < cfg.from_frame)
        return;
    static std::mutex m;
    static std::set<std::tuple<uint64_t, int, uint32_t, uint64_t, size_t>> seen;  // target, stage, bank, offset, value
    {
        std::lock_guard lock(m);
        if (seen.size() >= cfg.max_lines)
            return;
    }
    std::map<TextureId, std::pair<uint32_t, uint32_t>> sizes;
    for (const auto& [va, t] : h.textures)
        sizes[t.id] = {t.desc.width, t.desc.height};
    std::pair<uint32_t, uint32_t> target{0, 0};
    std::map<std::pair<int, uint32_t>, std::pair<uint64_t, uint64_t>> bound;  // (stage, bank) -> (address, size)
    std::set<std::tuple<uint64_t, uint64_t, uint64_t>> scanned;               // (target, address, size)
    struct BoundTexture {
        TextureId id = 0;
        AddressMode u = AddressMode::Repeat, v = AddressMode::Repeat;
    };
    std::map<uint32_t, BoundTexture> fragment_textures;  // slot -> texture
    std::vector<float> data;
    for (const Command& cmd : list.commands) {
        if (const auto* rts = std::get_if<CmdSetRenderTargets>(&cmd)) {
            target = {0, 0};
            const TextureId id = !rts->colors.empty() && rts->colors[0] != 0 ? rts->colors[0] : rts->depth_stencil;
            if (auto it = sizes.find(id); it != sizes.end())
                target = it->second;
            continue;
        }
        if (const auto* u = std::get_if<CmdBindUniformBuffer>(&cmd)) {
            bound[{int(u->stage), u->bank}] = {u->address, u->size};
            continue;
        }
        if (const auto* t = std::get_if<CmdBindTexture>(&cmd)) {
            if (t->stage == ShaderStage::Fragment) {
                if (t->texture == 0)
                    fragment_textures.erase(t->slot);
                else
                    fragment_textures[t->slot] = {t->texture, t->sampler.wrap[0], t->sampler.wrap[1]};
            }
            continue;
        }
        if (!std::holds_alternative<CmdDraw>(cmd) && !std::holds_alternative<CmdDrawIndexed>(cmd))
            continue;
        // The fragment textures of this draw: "t<slot> WxH wrap U/V" (R repeat, M mirror, C clamp, B border).
        std::string textures;
        for (const auto& [slot, t] : fragment_textures) {
            static const char kWrap[] = "RMCB";
            const auto it = sizes.find(t.id);
            char part[64];
            std::snprintf(part, sizeof part, " t%u %ux%u wrap %c/%c", slot, it != sizes.end() ? it->second.first : 0,
                          it != sizes.end() ? it->second.second : 0, kWrap[int(t.u) & 3], kWrap[int(t.v) & 3]);
            textures += part;
        }
        const uint64_t target_key = uint64_t(target.first) << 32 | target.second;
        for (const auto& [slot, buffer] : bound) {
            const auto [address, size] = buffer;
            if (address == 0 || !scanned.insert({target_key, address, size}).second)
                continue;
            const uint64_t n = std::min<uint64_t>(size, 0x10000) & ~uint64_t(3);
            data.resize(n / 4);
            if (n == 0 || !guest::read(data.data(), address, n))
                continue;
            for (uint64_t i = 0; i < n / 4; ++i) {
                for (size_t k = 0; k < cfg.values.size(); ++k) {
                    const double want = cfg.values[k];
                    if (!(std::fabs(double(data[i]) - want) <= cfg.tolerance * std::fabs(want)))
                        continue;
                    {
                        std::lock_guard lock(m);
                        if (seen.size() >= cfg.max_lines ||
                            !seen.insert({target_key, slot.first, slot.second, i * 4, k}).second)
                            continue;
                    }
                    static const char* const kStages[] = {"vertex", "fragment", "compute"};
                    rt::log(rt::LogLevel::Warn,
                            "nvn cbuf find: frame %" PRIu64 " value %g = %.6g at cbuf 0x%" PRIx64 " +0x%" PRIx64
                            " (%s c[%u]) draw into %ux%u%s%s",
                            frame, want, double(data[i]), address, i * 4, kStages[std::min(slot.first, 2)], slot.second,
                            target.first, target.second, textures.empty() ? "" : ";", textures.c_str());
                }
            }
        }
    }
}

// LGP_NVN_DRAW_STATS_FRAME=N (diagnosis for wider pictures, analysis 50): for
// the draws submitted while frame N is drawn (between present N-1 and N, so
// N >= 1 counts from the first present), per render target size: how many
// draws there are and which camera matrices (LGP_NVN_PROJECTION_SCAN's test)
// their vertex uniform buffers hold. Logged once at present N:
//   nvn draws: frame N into WxH: D draws, P with no camera matrix; view-projection 2.3333 fovy 35.0 in K draws (cbuf 0x...+0x...); ...
namespace {
struct DrawStats {
    struct Target {
        uint64_t draws = 0, without = 0;
        std::map<std::string, uint64_t> matrices;  // "kind aspect fovy (cbuf+off)" -> draws
    };
    std::mutex m;
    std::map<std::pair<uint32_t, uint32_t>, Target> targets;
    bool done = false;
};
DrawStats& draw_stats() {
    static DrawStats s;
    return s;
}
uint64_t draw_stats_frame() {
    static const uint64_t n = [] {
        const char* e = std::getenv("LGP_NVN_DRAW_STATS_FRAME");
        return e != nullptr ? std::strtoull(e, nullptr, 0) : 0;
    }();
    return n;
}
// Camera matrices in [va, va + size): "kind aspect fovy (cbuf+off)" each.
std::vector<std::string> camera_matrices(uint64_t va, uint64_t size) {
    std::vector<std::string> out;
    const uint64_t n = std::min<uint64_t>(size, 4096) & ~uint64_t(15);
    float data[1024];
    if (n < 64 || !guest::read(data, va, n))
        return out;
    for (uint64_t off = 0; off + 64 <= n; off += 16) {
        CameraHit hit;
        if (!best_camera(data + off / 4, &hit))
            continue;
        char line[128];
        std::snprintf(line, sizeof(line), "%s %.4f fovy %.1f (cbuf 0x%" PRIx64 "+0x%" PRIx64 ")",
                      hit.pure ? "projection" : "view-projection", hit.aspect, hit.fovy, va, off);
        out.push_back(line);
    }
    return out;
}
}  // namespace

bool draw_stats_on() { return draw_stats_frame() != 0; }

void draw_stats_note(Hle::Impl& h, const CommandList& list) {
    DrawStats& st = draw_stats();
    // Frame N is drawn while N - 1 presents have happened.
    if (presents_so_far() + 1 != draw_stats_frame())
        return;
    std::map<TextureId, std::pair<uint32_t, uint32_t>> sizes;
    for (const auto& [va, t] : h.textures)
        sizes[t.id] = {t.desc.width, t.desc.height};
    std::pair<uint32_t, uint32_t> target{0, 0};
    std::map<uint32_t, std::pair<uint64_t, uint64_t>> vertex_banks;  // bank -> (address, size)
    std::map<std::pair<uint64_t, uint64_t>, std::vector<std::string>> cache;
    std::lock_guard lock(st.m);
    if (st.done)
        return;
    for (const Command& cmd : list.commands) {
        if (const auto* rts = std::get_if<CmdSetRenderTargets>(&cmd)) {
            target = {0, 0};
            const TextureId id = !rts->colors.empty() && rts->colors[0] != 0 ? rts->colors[0] : rts->depth_stencil;
            if (auto it = sizes.find(id); it != sizes.end())
                target = it->second;
            continue;
        }
        if (const auto* u = std::get_if<CmdBindUniformBuffer>(&cmd)) {
            if (u->stage == ShaderStage::Vertex)
                vertex_banks[u->bank] = {u->address, u->size};
            continue;
        }
        if (!std::holds_alternative<CmdDraw>(cmd) && !std::holds_alternative<CmdDrawIndexed>(cmd))
            continue;
        DrawStats::Target& t = st.targets[target];
        ++t.draws;
        bool any = false;
        for (const auto& [bank, buffer] : vertex_banks) {
            auto it = cache.find(buffer);
            if (it == cache.end())
                it = cache.emplace(buffer, camera_matrices(buffer.first, buffer.second)).first;
            for (const std::string& name : it->second) {
                ++t.matrices[name];
                any = true;
            }
        }
        if (!any)
            ++t.without;
    }
}

void draw_stats_flush() {
    DrawStats& st = draw_stats();
    if (presents_so_far() + 1 != draw_stats_frame())
        return;
    std::lock_guard lock(st.m);
    if (st.done)
        return;
    st.done = true;
    for (const auto& [size, t] : st.targets) {
        std::string line;
        for (const auto& [name, count] : t.matrices)
            line += "; " + name + " in " + std::to_string(count) + " draws";
        rt::log(rt::LogLevel::Warn, "nvn draws: frame %" PRIu64 " into %ux%u: %" PRIu64 " draws, %" PRIu64
                " with no camera matrix%s", draw_stats_frame(), size.first, size.second, t.draws, t.without, line.c_str());
    }
    if (st.targets.empty())
        rt::log(rt::LogLevel::Warn, "nvn draws: frame %" PRIu64 ": no draws", draw_stats_frame());
}

namespace {

void CommandBufferBindUniformBuffer(H& h, CpuState* c) {
    auto* cb = h.recording(ptr_arg(c, 0), __func__);
    ShaderStage stage;
    if (cb == nullptr || !shader_stage(uint_arg(c, 1), &stage, __func__))
        return;
    if (projection_scan_on())
        projection_scan(c->x[30], false, ptr_arg(c, 3), ptr_arg(c, 4));
    push(cb, CmdBindUniformBuffer{stage, kUniformBufferBankBase + uint_arg(c, 2), ptr_arg(c, 3), ptr_arg(c, 4)});
}

// Storage buffers (compute shaders' global memory), BindStorageBuffer(cmd,
// stage, index, address, size) and BindStorageBuffers(cmd, stage, first,
// count, NVNbufferRange*).
void CommandBufferBindStorageBuffer(H& h, CpuState* c) {
    auto* cb = h.recording(ptr_arg(c, 0), __func__);
    ShaderStage stage;
    if (cb == nullptr || !shader_stage(uint_arg(c, 1), &stage, __func__))
        return;
    push(cb, CmdBindStorageBuffer{stage, uint_arg(c, 2), ptr_arg(c, 3), ptr_arg(c, 4)});
}
void CommandBufferBindStorageBuffers(H& h, CpuState* c) {
    auto* cb = h.recording(ptr_arg(c, 0), __func__);
    ShaderStage stage;
    if (cb == nullptr || !shader_stage(uint_arg(c, 1), &stage, __func__))
        return;
    const uint32_t first = uint_arg(c, 2);
    const int32_t n = std::clamp(int_arg(c, 3), 0, 32);
    for (int32_t i = 0; i < n; ++i) {
        uint64_t range[2];
        guest::read(range, ptr_arg(c, 4) + uint64_t(i) * kBufferRangeSize, sizeof(range));
        push(cb, CmdBindStorageBuffer{stage, first + uint32_t(i), range[0], range[1]});
    }
}
// DispatchCompute(cmd, groupsX, groupsY, groupsZ), DispatchComputeIndirect(cmd, address).
void CommandBufferDispatchCompute(H& h, CpuState* c) {
    if (auto* cb = h.recording(ptr_arg(c, 0), __func__))
        push(cb, CmdDispatchCompute{{uint_arg(c, 1), uint_arg(c, 2), uint_arg(c, 3)}, 0});
}
void CommandBufferDispatchComputeIndirect(H& h, CpuState* c) {
    if (auto* cb = h.recording(ptr_arg(c, 0), __func__))
        push(cb, CmdDispatchCompute{{0, 0, 0}, ptr_arg(c, 1)});
}
// SetShaderScratchMemory(cmd, pool, offset, size): memory the driver spills
// local memory to. The translated shaders keep local memory in WGSL private
// variables, so the scratch area is not needed.
void CommandBufferSetShaderScratchMemory(H& h, CpuState* c) { (void)h.recording(ptr_arg(c, 0), __func__); }

void CommandBufferSetTexturePool(H& h, CpuState* c) {
    if (auto* cb = h.recording(ptr_arg(c, 0), __func__)) {
        cb->texture_pool = ptr_arg(c, 1);
        push(cb, CmdSetTexturePools{ptr_arg(c, 1), 0});
    }
}
void CommandBufferSetSamplerPool(H& h, CpuState* c) {
    if (auto* cb = h.recording(ptr_arg(c, 0), __func__)) {
        cb->sampler_pool = ptr_arg(c, 1);
        push(cb, CmdSetTexturePools{0, ptr_arg(c, 1)});
    }
}

// Handle (texture_handle() in abi.hpp) -> texture and sampler through the
// pools set on the command buffer, at record time.
CmdBindTexture bind_texture(H&, H::CommandBuffer*, ShaderStage stage, uint32_t slot, uint64_t handle) {
    CmdBindTexture cmd;
    cmd.stage = stage;
    cmd.slot = slot;
    cmd.handle = handle;  // resolved at submit (resolve_texture_bindings)
    return cmd;
}

}  // namespace

void resolve_texture_binding(H& h, uint64_t texture_pool, uint64_t sampler_pool, CmdBindTexture& cmd) {
    const uint64_t handle = cmd.handle;
    cmd.texture = 0;
    if (handle == 0)
        return;
    const int32_t tex_id = int32_t(handle & 0xFFFFF);
    const int32_t smp_id = int32_t((handle >> 20) & 0xFFF);
    auto tp = h.texture_pools.find(texture_pool);
    auto sp = h.sampler_pools.find(sampler_pool);
    if (tp == h.texture_pools.end() || sp == h.sampler_pools.end()) {
        LGP_WARN_ONCE("nvn: texture bound without a texture or sampler pool set");
        return;
    }
    if (auto t = tp->second.textures.find(tex_id); t != tp->second.textures.end()) {
        cmd.texture = t->second.texture;
        cmd.swizzle = t->second.swizzle;
    } else
        LGP_WARN_ONCE("nvn: texture handle 0x%" PRIx64 " names an unregistered texture id %d", handle, tex_id);
    if (auto s = sp->second.samplers.find(smp_id); s != sp->second.samplers.end()) {
        cmd.sampler = s->second;
        // A mod's repeat hint (widened 2D patterns): clamped axes of matching file textures repeat.
        if (bool u = false, v = false; cmd.texture != 0) {
            const auto t = tp->second.textures.find(tex_id);
            const auto obj = h.textures.find(t->second.object);
            if (obj != h.textures.end() && (obj->second.flags & kTextureFlagCompressible) == 0 &&
                rt::repeat_hint(obj->second.desc.width, obj->second.desc.height, &u, &v)) {
                auto widen = [](AddressMode& m) {
                    if (m == AddressMode::ClampToEdge || m == AddressMode::ClampToBorder) m = AddressMode::Repeat;
                };
                if (u) widen(cmd.sampler.wrap[0]);
                if (v) widen(cmd.sampler.wrap[1]);
            }
        }
    } else
        LGP_WARN_ONCE("nvn: texture handle 0x%" PRIx64 " names an unregistered sampler id %d", handle, smp_id);
}

namespace {
void CommandBufferBindTexture(H& h, CpuState* c) {
    auto* cb = h.recording(ptr_arg(c, 0), __func__);
    ShaderStage stage;
    if (cb == nullptr || !shader_stage(uint_arg(c, 1), &stage, __func__))
        return;
    push(cb, bind_texture(h, cb, stage, uint_arg(c, 2), ptr_arg(c, 3)));
}
void CommandBufferBindTextures(H& h, CpuState* c) {
    auto* cb = h.recording(ptr_arg(c, 0), __func__);
    ShaderStage stage;
    if (cb == nullptr || !shader_stage(uint_arg(c, 1), &stage, __func__))
        return;
    const uint32_t first = uint_arg(c, 2);
    const int32_t n = std::clamp(int_arg(c, 3), 0, 32);
    for (int32_t i = 0; i < n; ++i)
        push(cb, bind_texture(h, cb, stage, first + uint32_t(i), guest::load<uint64_t>(ptr_arg(c, 4) + uint64_t(i) * 8)));
}

void CommandBufferBindBlendState(H& h, CpuState* c) {
    auto* cb = h.recording(ptr_arg(c, 0), __func__);
    if (cb == nullptr)
        return;
    const auto v = load_value<BlendStateValue>(h, ptr_arg(c, 1), __func__);
    if (v.target >= kMaxColorTargets)
        return;
    CmdSetBlendEquation cmd;
    cmd.target = v.target;
    cmd.equation.src_color = blend_factor(v.src_color);
    cmd.equation.dst_color = blend_factor(v.dst_color);
    cmd.equation.src_alpha = blend_factor(v.src_alpha);
    cmd.equation.dst_alpha = blend_factor(v.dst_alpha);
    cmd.equation.color_op = blend_op(v.color_eq);
    cmd.equation.alpha_op = blend_op(v.alpha_eq);
    push(cb, cmd);
}
void CommandBufferBindColorState(H& h, CpuState* c) {
    auto* cb = h.recording(ptr_arg(c, 0), __func__);
    if (cb == nullptr)
        return;
    const auto v = load_value<ColorStateValue>(h, ptr_arg(c, 1), __func__);
    if (v.logic_op != kLogicOpCopy)
        LGP_WARN_ONCE("nvn: logic op %u is not supported by WebGPU, ignored", v.logic_op);
    push(cb, CmdSetBlendEnables{uint8_t(v.blend_enable_mask)});
    push(cb, CmdSetAlphaTest{compare_func(v.alpha_func)});
}
void CommandBufferBindChannelMaskState(H& h, CpuState* c) {
    auto* cb = h.recording(ptr_arg(c, 0), __func__);
    if (cb == nullptr)
        return;
    const auto v = load_value<ChannelMaskStateValue>(h, ptr_arg(c, 1), __func__);
    CmdSetChannelMasks cmd;
    for (uint32_t t = 0; t < kMaxColorTargets; ++t) cmd.masks[t] = uint8_t((v.masks >> (4 * t)) & 0xF);
    push(cb, cmd);
}
void CommandBufferBindDepthStencilState(H& h, CpuState* c) {
    auto* cb = h.recording(ptr_arg(c, 0), __func__);
    if (cb == nullptr)
        return;
    const auto v = load_value<DepthStencilStateValue>(h, ptr_arg(c, 1), __func__);
    CmdSetDepthStencil cmd;
    cmd.depth_test = v.depth_test != 0;
    cmd.depth_write = v.depth_write != 0;
    cmd.depth_func = compare_func(v.depth_func);
    cmd.stencil_test = v.stencil_test != 0;
    StencilFace* faces[2] = {&cmd.front, &cmd.back};
    for (int f = 0; f < 2; ++f) {
        faces[f]->func = compare_func(v.func[f]);
        faces[f]->fail = stencil_op(v.fail[f]);
        faces[f]->depth_fail = stencil_op(v.depth_fail[f]);
        faces[f]->pass = stencil_op(v.pass[f]);
    }
    push(cb, cmd);
}
void CommandBufferBindPolygonState(H& h, CpuState* c) {
    auto* cb = h.recording(ptr_arg(c, 0), __func__);
    if (cb == nullptr)
        return;
    const auto v = load_value<PolygonStateValue>(h, ptr_arg(c, 1), __func__);
    CmdSetRaster cmd;
    switch (static_cast<FaceNvn>(v.cull)) {
    case FaceNvn::None: cmd.cull = CullMode::None; break;
    case FaceNvn::Front: cmd.cull = CullMode::Front; break;
    case FaceNvn::Back: cmd.cull = CullMode::Back; break;
    case FaceNvn::FrontAndBack:
        // Nothing is drawn; WebGPU has no such mode (rasterizer discard would be needed).
        LGP_WARN_ONCE("nvn: cull FRONT_AND_BACK not supported, culling back faces only");
        cmd.cull = CullMode::Back;
        break;
    }
    cmd.front_face = v.front_face == uint32_t(FrontFaceNvn::Cw) ? FrontFace::Cw : FrontFace::Ccw;
    if (v.mode != uint32_t(PolygonModeNvn::Fill))
        LGP_WARN_ONCE("nvn: polygon mode %u is not supported by WebGPU, drawing filled", v.mode);
    cmd.depth_bias = uint8_t(((v.offset_enables & kPolygonOffsetPoint) ? kDepthBiasPoints : 0) |
                             ((v.offset_enables & kPolygonOffsetLine) ? kDepthBiasLines : 0) |
                             ((v.offset_enables & kPolygonOffsetFill) ? kDepthBiasTriangles : 0));
    push(cb, cmd);
}
void CommandBufferBindMultisampleState(H& h, CpuState* c) {
    auto* cb = h.recording(ptr_arg(c, 0), __func__);
    if (cb == nullptr)
        return;
    const auto v = load_value<MultisampleStateValue>(h, ptr_arg(c, 1), __func__);
    push(cb, CmdSetMultisample{v.alpha_to_coverage != 0});
}
void CommandBufferSetBlendColor(H& h, CpuState* c) {
    auto* cb = h.recording(ptr_arg(c, 0), __func__);
    if (cb == nullptr)
        return;
    CmdSetBlendColor cmd;
    guest::read(cmd.rgba, ptr_arg(c, 1), sizeof(cmd.rgba));
    push(cb, cmd);
}
void stencil_value(H& h, CpuState* c, CmdSetStencilValues::Kind kind, const char* fn) {
    if (auto* cb = h.recording(ptr_arg(c, 0), fn))
        push(cb, CmdSetStencilValues{kind, uint8_t(uint_arg(c, 1) & 3), uint_arg(c, 2)});
}
void CommandBufferSetStencilRef(H& h, CpuState* c) { stencil_value(h, c, CmdSetStencilValues::Reference, __func__); }
void CommandBufferSetStencilValueMask(H& h, CpuState* c) {
    stencil_value(h, c, CmdSetStencilValues::ValueMask, __func__);
}
void CommandBufferSetStencilMask(H& h, CpuState* c) { stencil_value(h, c, CmdSetStencilValues::WriteMask, __func__); }

// SetCopyRowStride / SetCopyImageStride(cmd, ptrdiff_t): bytes between rows
// and between slices of the linear side of later copies (0 = packed).
void CommandBufferSetCopyRowStride(H& h, CpuState* c) {
    if (auto* cb = h.recording(ptr_arg(c, 0), __func__))
        cb->copy_row_stride = uint32_t(ptr_arg(c, 1));
}
void CommandBufferSetCopyImageStride(H& h, CpuState* c) {
    if (auto* cb = h.recording(ptr_arg(c, 0), __func__))
        cb->copy_image_stride = ptr_arg(c, 1);
}
// CopyBufferToTexture(cmd, src, dstTexture, dstView, region, flags). Level
// and base layer come from the view; region z/depth select 3D slices or
// array layers (on top of the view's base layer).
void CommandBufferCopyBufferToTexture(H& h, CpuState* c) {
    auto* cb = h.recording(ptr_arg(c, 0), __func__);
    if (cb == nullptr)
        return;
    auto tex = h.textures.find(ptr_arg(c, 2));
    int32_t region[6];
    guest::read(region, ptr_arg(c, 4), kCopyRegionSize);
    if (tex == h.textures.end() || region[2] < 0 || region[5] > 2048) {
        LGP_WARN_ONCE("nvn: nvnCommandBufferCopyBufferToTexture: unknown texture or slice range (z %d, depth %d)",
                      region[2], region[5]);
        return;
    }
    CmdCopyBufferToTexture cmd;
    uint32_t layer = 0;
    texture_view_base(ptr_arg(c, 3), &cmd.level, &layer);
    cmd.address = ptr_arg(c, 1);
    cmd.texture = tex->second.id;
    cmd.x = region[0];
    cmd.y = region[1];
    cmd.z = region[2] + int32_t(layer);
    cmd.width = region[3];
    cmd.height = region[4];
    cmd.depth = std::max(region[5], 1);
    cmd.row_stride = cb->copy_row_stride;
    cmd.image_stride = cb->copy_image_stride;
    push(cb, cmd);
}

// CopyTextureToTexture(cmd, srcTexture, srcView, srcRegion, dstTexture,
// dstView, dstRegion, flags). Argument order UNVERIFIED (public header
// shape, consistent with CopyBufferToTexture); levels and layers come from
// the views.
void CommandBufferCopyTextureToTexture(H& h, CpuState* c) {
    auto* cb = h.recording(ptr_arg(c, 0), __func__);
    if (cb == nullptr)
        return;
    auto src = h.textures.find(ptr_arg(c, 1));
    auto dst = h.textures.find(ptr_arg(c, 4));
    if (src == h.textures.end() || dst == h.textures.end()) {
        LGP_WARN_ONCE("nvn: nvnCommandBufferCopyTextureToTexture: unknown source or destination texture");
        return;
    }
    CmdCopyTextureToTexture cmd;
    auto side = [&](CmdCopyTextureToTexture::Side& out, TextureId id, uint64_t view, uint64_t region_va) {
        int32_t region[6];
        guest::read(region, region_va, kCopyRegionSize);
        out.texture = id;
        texture_view_base(view, &out.level, &out.layer);
        out.x = region[0];
        out.y = region[1];
        out.z = region[2];
        out.width = region[3];
        out.height = region[4];
        out.depth = region[5];
    };
    side(cmd.src, src->second.id, ptr_arg(c, 2), ptr_arg(c, 3));
    side(cmd.dst, dst->second.id, ptr_arg(c, 5), ptr_arg(c, 6));
    cmd.flags = uint_arg(c, 7);
    push(cb, cmd);
}
// CopyTextureToBuffer(cmd, srcTexture, srcView, srcRegion, dst, flags).
// Argument order UNVERIFIED (mirror of CopyBufferToTexture). No backend reads
// textures back into guest memory yet, so the copy is dropped and the guest
// buffer keeps its old contents; LGP calls it from the X menu ("Play with
// Pikachu"), where halting the guest was worse than a stale image.
void CommandBufferCopyTextureToBuffer(H& h, CpuState* c) {
    if (h.recording(ptr_arg(c, 0), __func__) == nullptr)
        return;
    int32_t region[6];
    guest::read(region, ptr_arg(c, 3), kCopyRegionSize);
    LGP_WARN_ONCE("nvn: nvnCommandBufferCopyTextureToBuffer is not copied yet (region %dx%d at %d,%d to 0x%llx), "
                  "the buffer keeps its contents",
                  region[3], region[4], region[0], region[1], static_cast<unsigned long long>(ptr_arg(c, 4)));
}
// CopyBufferToBuffer(cmd, src, dst, size, flags).
void CommandBufferCopyBufferToBuffer(H& h, CpuState* c) {
    if (auto* cb = h.recording(ptr_arg(c, 0), __func__))
        push(cb, CmdCopyBuffer{ptr_arg(c, 1), ptr_arg(c, 2), ptr_arg(c, 3)});
}
// ClearTexture(cmd, texture, view, region, rgba float[4], mask). The view's
// level/layer selection is not applied (level 0, 2D region).
void CommandBufferClearTexture(H& h, CpuState* c) {
    auto* cb = h.recording(ptr_arg(c, 0), __func__);
    if (cb == nullptr)
        return;
    auto tex = h.textures.find(ptr_arg(c, 1));
    if (tex == h.textures.end()) {
        LGP_WARN_ONCE("nvn: nvnCommandBufferClearTexture on an unknown texture");
        return;
    }
    if (ptr_arg(c, 2) != 0)
        LGP_WARN_ONCE("nvn: clear views (level/layer selection) are not applied yet, clearing level 0");
    int32_t region[6];
    guest::read(region, ptr_arg(c, 3), kCopyRegionSize);
    CmdClearTexture cmd;
    cmd.texture = tex->second.id;
    cmd.region = {region[0], region[1], region[3], region[4]};
    guest::read(cmd.rgba, ptr_arg(c, 4), sizeof(cmd.rgba));
    cmd.mask = uint_arg(c, 5) & 0xF;
    push(cb, cmd);
}

// ---------------------------------------------------------------------------
// Rasterizer state set directly on the command buffer (not through state
// objects). Signatures UNVERIFIED like the rest (public header shape).

// SetPolygonOffsetClamp(cmd, factor, units, clamp).
void CommandBufferSetPolygonOffsetClamp(H& h, CpuState* c) {
    if (auto* cb = h.recording(ptr_arg(c, 0), __func__))
        push(cb, CmdSetDepthBias{float_arg(c, 0), float_arg(c, 1), float_arg(c, 2)});
}
// SetDepthClamp(cmd, enable).
void CommandBufferSetDepthClamp(H& h, CpuState* c) {
    if (auto* cb = h.recording(ptr_arg(c, 0), __func__))
        push(cb, CmdSetDepthClamp{bool_arg(c, 1)});
}
// SetRasterizerDiscard(cmd, discard).
void CommandBufferSetRasterizerDiscard(H& h, CpuState* c) {
    if (auto* cb = h.recording(ptr_arg(c, 0), __func__))
        push(cb, CmdSetRasterizerDiscard{bool_arg(c, 1)});
}
// SetSampleMask(cmd, mask).
void CommandBufferSetSampleMask(H& h, CpuState* c) {
    if (auto* cb = h.recording(ptr_arg(c, 0), __func__))
        push(cb, CmdSetSampleMask{uint_arg(c, 1)});
}
// WebGPU has no depth bounds test, no conservative rasterization and only
// 1-pixel lines: accepted, logged once when the game turns them on, no
// effect on rendering.
// SetDepthBounds(cmd, enable, near, far).
void CommandBufferSetDepthBounds(H& h, CpuState* c) {
    if (h.recording(ptr_arg(c, 0), __func__) != nullptr && bool_arg(c, 1))
        LGP_WARN_ONCE("nvn: depth bounds test [%g, %g] is not supported by WebGPU, ignored", double(float_arg(c, 0)),
                      double(float_arg(c, 1)));
}
// SetConservativeRasterEnable(cmd, enable).
void CommandBufferSetConservativeRasterEnable(H& h, CpuState* c) {
    if (h.recording(ptr_arg(c, 0), __func__) != nullptr && bool_arg(c, 1))
        LGP_WARN_ONCE("nvn: conservative rasterization is not supported by WebGPU, ignored");
}
// SetLineWidth(cmd, width).
// SetAlphaRef(cmd, float ref): reference of the fixed-function alpha test.
void CommandBufferSetAlphaRef(H& h, CpuState* c) {
    if (auto* cb = h.recording(ptr_arg(c, 0), __func__))
        push(cb, CmdSetAlphaRef{float_arg(c, 0)});
}
void CommandBufferSetLineWidth(H& h, CpuState* c) {
    if (h.recording(ptr_arg(c, 0), __func__) != nullptr && float_arg(c, 0) != 1.0f)
        LGP_WARN_ONCE("nvn: line width %g is not supported by WebGPU, lines are 1 pixel wide",
                      double(float_arg(c, 0)));
}
// ReportCounter(cmd, NVNcounterType, NVNbufferAddress) / ResetCounter(cmd,
// NVNcounterType): handled at submit (submit_counters in procs.cpp).
void CommandBufferReportCounter(H& h, CpuState* c) {
    if (auto* cb = h.recording(ptr_arg(c, 0), __func__)) {
        // Which counters the game reads (types UNVERIFIED, abi.hpp).
        static std::atomic<uint32_t> seen{0};
        const uint32_t type = uint_arg(c, 1);
        if (type < 32 && !(seen.fetch_or(1u << type) & (1u << type)))
            rt::log(LogLevel::Info, "nvn: nvnCommandBufferReportCounter: first report of counter type %u", type);
        push(cb, CmdReportCounter{type, ptr_arg(c, 2)});
    }
}
void CommandBufferResetCounter(H& h, CpuState* c) {
    if (auto* cb = h.recording(ptr_arg(c, 0), __func__))
        push(cb, CmdResetCounter{uint_arg(c, 1)});
}

void CommandBufferDrawArrays(H& h, CpuState* c) {
    auto* cb = h.recording(ptr_arg(c, 0), __func__);
    Topology t;
    if (cb == nullptr || !topology(uint_arg(c, 1), &t))
        return;
    resolve_vertex_only(h, cb);
    push(cb, CmdDraw{t, uint_arg(c, 2), uint_arg(c, 3), 0, 1});
}
// DrawArraysInstanced(cmd, mode, first, count, baseInstance, instanceCount).
void CommandBufferDrawArraysInstanced(H& h, CpuState* c) {
    auto* cb = h.recording(ptr_arg(c, 0), __func__);
    Topology t;
    if (cb == nullptr || !topology(uint_arg(c, 1), &t))
        return;
    resolve_vertex_only(h, cb);
    push(cb, CmdDraw{t, uint_arg(c, 2), uint_arg(c, 3), uint_arg(c, 4), uint_arg(c, 5)});
}
void draw_elements(H& h, CpuState* c, int32_t base_vertex, uint32_t base_instance, uint32_t instances,
                   const char* fn) {
    auto* cb = h.recording(ptr_arg(c, 0), fn);
    Topology t;
    IndexType type;
    if (cb == nullptr || !topology(uint_arg(c, 1), &t) || !index_type(uint_arg(c, 2), &type))
        return;
    resolve_vertex_only(h, cb);
    CmdDrawIndexed cmd;
    cmd.topology = t;
    cmd.type = type;
    cmd.index_count = uint_arg(c, 3);
    cmd.index_address = ptr_arg(c, 4);
    cmd.base_vertex = base_vertex;
    cmd.first_instance = base_instance;
    cmd.instance_count = instances;
    push(cb, cmd);
}
// DrawElements(cmd, mode, type, count, indexBuffer).
void CommandBufferDrawElements(H& h, CpuState* c) { draw_elements(h, c, 0, 0, 1, __func__); }
// DrawElementsBaseVertex(cmd, mode, type, count, indexBuffer, baseVertex).
void CommandBufferDrawElementsBaseVertex(H& h, CpuState* c) {
    draw_elements(h, c, int_arg(c, 5), 0, 1, __func__);
}
// DrawElementsInstanced(cmd, mode, type, count, indexBuffer, baseVertex, baseInstance, instanceCount).
void CommandBufferDrawElementsInstanced(H& h, CpuState* c) {
    draw_elements(h, c, int_arg(c, 5), uint_arg(c, 6), uint_arg(c, 7), __func__);
}

#define P(name) {"nvn" #name, &name}
// Sorted by name (checked at first lookup).
constexpr ProcImpl kDrawProcs[] = {
    P(BlendStateSetBlendEquation),
    P(BlendStateSetBlendFunc),
    P(BlendStateSetBlendTarget),
    P(BlendStateSetDefaults),
    P(ChannelMaskStateSetChannelMask),
    P(ChannelMaskStateSetDefaults),
    P(ColorStateSetAlphaTest),
    P(ColorStateSetBlendEnable),
    P(ColorStateSetDefaults),
    P(ColorStateSetLogicOp),
    P(CommandBufferBindBlendState),
    P(CommandBufferBindChannelMaskState),
    P(CommandBufferBindColorState),
    P(CommandBufferBindDepthStencilState),
    P(CommandBufferBindMultisampleState),
    P(CommandBufferBindPolygonState),
    P(CommandBufferBindProgram),
    P(CommandBufferBindStorageBuffer),
    P(CommandBufferBindStorageBuffers),
    P(CommandBufferBindTexture),
    P(CommandBufferBindTextures),
    P(CommandBufferBindUniformBuffer),
    P(CommandBufferBindVertexAttribState),
    P(CommandBufferBindVertexBuffer),
    P(CommandBufferBindVertexBuffers),
    P(CommandBufferBindVertexStreamState),
    P(CommandBufferClearTexture),
    P(CommandBufferCopyBufferToBuffer),
    P(CommandBufferCopyBufferToTexture),
    P(CommandBufferCopyTextureToBuffer),
    P(CommandBufferCopyTextureToTexture),
    P(CommandBufferDispatchCompute),
    P(CommandBufferDispatchComputeIndirect),
    P(CommandBufferDrawArrays),
    P(CommandBufferDrawArraysInstanced),
    P(CommandBufferDrawElements),
    P(CommandBufferDrawElementsBaseVertex),
    P(CommandBufferDrawElementsInstanced),
    P(CommandBufferReportCounter),
    P(CommandBufferResetCounter),
    P(CommandBufferSetAlphaRef),
    P(CommandBufferSetBlendColor),
    P(CommandBufferSetConservativeRasterEnable),
    P(CommandBufferSetCopyImageStride),
    P(CommandBufferSetCopyRowStride),
    P(CommandBufferSetDepthBounds),
    P(CommandBufferSetDepthClamp),
    P(CommandBufferSetLineWidth),
    P(CommandBufferSetPolygonOffsetClamp),
    P(CommandBufferSetRasterizerDiscard),
    P(CommandBufferSetSampleMask),
    P(CommandBufferSetSamplerPool),
    P(CommandBufferSetShaderScratchMemory),
    P(CommandBufferSetStencilMask),
    P(CommandBufferSetStencilRef),
    P(CommandBufferSetStencilValueMask),
    P(CommandBufferSetTexturePool),
    P(DepthStencilStateSetDefaults),
    P(DepthStencilStateSetDepthFunc),
    P(DepthStencilStateSetDepthTestEnable),
    P(DepthStencilStateSetDepthWriteEnable),
    P(DepthStencilStateSetStencilFunc),
    P(DepthStencilStateSetStencilOp),
    P(DepthStencilStateSetStencilTestEnable),
    P(MultisampleStateSetAlphaToCoverageEnable),
    P(MultisampleStateSetDefaults),
    P(MultisampleStateSetMultisampleEnable),
    P(MultisampleStateSetSamples),
    P(PolygonStateSetCullFace),
    P(PolygonStateSetDefaults),
    P(PolygonStateSetFrontFace),
    P(PolygonStateSetPolygonMode),
    P(PolygonStateSetPolygonOffsetEnables),
    P(ProgramFinalize),
    P(ProgramInitialize),
    P(ProgramSetDebugLabel),
    P(ProgramSetShaders),
    P(SamplerBuilderSetBorderColor),
    P(SamplerBuilderSetCompare),
    P(SamplerBuilderSetDefaults),
    P(SamplerBuilderSetDevice),
    P(SamplerBuilderSetLodBias),
    P(SamplerBuilderSetLodClamp),
    P(SamplerBuilderSetMaxAnisotropy),
    P(SamplerBuilderSetMinMagFilter),
    P(SamplerBuilderSetReductionFilter),
    P(SamplerBuilderSetWrapMode),
    P(SamplerFinalize),
    P(SamplerInitialize),
    P(SamplerPoolFinalize),
    P(SamplerPoolGetSize),
    P(SamplerPoolInitialize),
    P(SamplerPoolRegisterSampler),
    P(SamplerSetDebugLabel),
    P(TextureFlushTexels),
    P(TexturePoolFinalize),
    P(TexturePoolGetSize),
    P(TexturePoolInitialize),
    P(TexturePoolRegisterTexture),
    P(TextureWriteTexels),
    P(TextureWriteTexelsStrided),
    P(VertexAttribStateSetDefaults),
    P(VertexAttribStateSetFormat),
    P(VertexAttribStateSetStreamIndex),
    P(VertexStreamStateSetDefaults),
    P(VertexStreamStateSetDivisor),
    P(VertexStreamStateSetStride),
};
#undef P

}  // namespace

const ProcImpl* draw_proc_table(size_t* count) {
    *count = std::size(kDrawProcs);
    return kDrawProcs;
}

}  // namespace lgp::gpu::nvn
