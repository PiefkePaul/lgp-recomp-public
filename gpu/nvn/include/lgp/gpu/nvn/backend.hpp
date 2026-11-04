// What the NVN-HLE hands to a renderer: textures and recorded command lists.
//
// The HLE (hle.hpp) turns NVN calls into this small host-side model; a
// Backend executes it. SoftBackend is the CPU reference (clears and present
// through IRenderer::present_rgba8, used by tests and for bring-up without a
// GPU); the WebGPU backend (P4.4) implements the same interface.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "lgp/gpu/nvn/abi.hpp"
#include "lgp/platform/renderer.hpp"

namespace lgp::gpu::nvn {

using TextureId = uint32_t;  // 0 = none

struct TextureDesc {
    uint32_t target = 0;  // TextureTarget
    uint32_t format = 0;  // Format
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t depth = 1;
    uint32_t levels = 1;
    uint32_t samples = 0;
    uint64_t storage = 0;  // guest VA of the texture's pool storage (identity GPU address)
    // Level-0 block height (log2 GOBs) from nvnTextureBuilderSetPackagedTextureLayout,
    // -1 = NVN's default rule (texture_layout.hpp).
    int32_t gob_height_log2 = -1;
    // Pitch-linear texture (NVN_TEXTURE_FLAGS_LINEAR): bytes per row in the
    // pool, 0 = block-linear. Level 0 only.
    uint32_t linear_stride = 0;
};

struct Rect {
    int32_t x = 0, y = 0, width = 0, height = 0;
};
// Slices of a texel upload (Backend::write_texture): first slice z, count
// depth, image_stride bytes between slices (0 = row_stride * height).
struct TexelSlices {
    int32_t z = 0;
    int32_t depth = 1;
    uint64_t image_stride = 0;
};

// Recorded commands. Coordinates are in NVN window space; origin_lower_left
// says whether y counts from the last row in memory (NVN's default, like
// OpenGL) or from the first.
struct CmdSetRenderTargets {
    std::vector<TextureId> colors;  // index = color target, 0 = unbound
    TextureId depth_stencil = 0;
};
struct CmdClearColor {
    uint32_t index = 0;
    float rgba[4] = {};
    uint32_t mask = kClearColorMaskRGBA;
};
struct CmdClearDepthStencil {
    float depth = 1.0f;
    bool depth_mask = true;
    uint32_t stencil = 0;
    uint32_t stencil_mask = 0;
};
struct CmdSetScissors {
    uint32_t first = 0;
    std::vector<Rect> rects;
};
struct CmdSetViewports {
    uint32_t first = 0;
    std::vector<float> xywh;  // 4 per viewport
};
struct CmdSetDepthRanges {
    uint32_t first = 0;
    std::vector<float> near_far;  // 2 per viewport
};
struct CmdBarrier {
    uint32_t bits = 0;
};
// A command the HLE does not translate yet (lenient bring-up mode): kept so
// the backend can count what a frame skipped.
struct CmdUnsupported {
    uint32_t proc = 0;           // index into Hle's proc table
    const char* name = nullptr;  // its NVN name (lives as long as the Hle)
};

// --- Draws. Buffers are guest addresses (GPU address = guest VA, hle.hpp);
// the backend reads them through GuestMemoryView at execution time.

using ProgramId = uint32_t;  // 0 = none

// A linked vertex + fragment program, already translated to WGSL by
// gpu/shader (offline or at nvnProgramSetShaders, ADR-0003) with the
// backend's binding convention: entry points "main", vertex constant buffers
// in group 0, fragment constant buffers in group 1, vertex textures in group
// 2, fragment textures in group 3 (wgsl::Options::cbuf_group/texture_group;
// texture k at binding 2k, its sampler at 2k+1).
struct ProgramDesc {
    std::string vertex_wgsl;
    // Empty: a vertex-only program, drawn without fragment shader (color
    // targets are not written, depth and stencil are).
    std::string fragment_wgsl;
    std::vector<uint32_t> vertex_cbufs;    // constant buffer banks the stage reads
    std::vector<uint32_t> fragment_cbufs;
    // Bound texture slot (nvnCommandBufferBindTexture index) of the k-th
    // texture binding of each stage.
    std::vector<uint32_t> vertex_textures;
    std::vector<uint32_t> fragment_textures;
    // The stage loads global memory (wgsl::Options::graphics_global_memory):
    // read-only storage slots in its texture group from
    // wgsl::kGraphicsGlobalBindingBase, filled from the stage's storage buffers.
    bool vertex_global_memory = false;
    bool fragment_global_memory = false;
    // Per texture binding k: sampled as a 2D array (texture_2d_array).
    std::vector<bool> vertex_texture_arrays, fragment_texture_arrays, compute_texture_arrays;
    // Dimension of each texture binding (2 = texture_2d, 3 = texture_3d;
    // wgsl::Bindings::texture_dims). Empty = all 2D.
    std::vector<uint32_t> vertex_texture_dims;
    std::vector<uint32_t> fragment_texture_dims;
    // Per texture binding k: a depth texture with a comparison sampler
    // (texture_depth_2d(_array) + sampler_comparison, wgsl::Bindings::texture_depth).
    std::vector<bool> vertex_texture_depth, fragment_texture_depth, compute_texture_depth;

    // A compute program instead (compute_wgsl not empty, the graphics fields
    // stay empty): constant buffers in group 0, textures in group 1, global
    // memory (storage buffers, wgsl.h) in group 2. The workgroup size and
    // shared memory size are baked into the WGSL.
    std::string compute_wgsl;
    std::vector<uint32_t> compute_cbufs;
    std::vector<uint32_t> compute_textures;
    std::vector<uint32_t> compute_texture_dims;
    bool compute_global_memory = false;  // group 2 is used
    bool compute_gmem_probe = false;     // group 2 also has wgsl::Options::gmem_probe's counters
    // The WGSL applies texture view swizzles (wgsl::Options::texture_swizzle):
    // each texture group has a swizzle uniform after its texture bindings.
    bool texture_swizzle = false;
    // FNV-1a 64 of the guest code the program was translated from (the
    // translation cache key: vertex|fragment code, or compute code and its
    // ComputeInfo). Unlike the backend's program hash it stays the same when
    // the translator changes; not part of the pipeline cache's program hash.
    // 0: unknown (programs built without guest code, e.g. in tests).
    uint64_t guest_hash = 0;

    bool is_compute() const { return !compute_wgsl.empty(); }
};

// Vertex formats. Float-readable ones first; the integer formats (Uint*,
// Sint*) reach the shader as raw integers (the translated vertex shader
// declares those inputs vec4<u32>/vec4<i32>, see vertex_format_input below).
// UScaled*/SScaled* are integers converted to float without normalising
// (NVN *_UI2F/*_I2F); *10_10_10_2 other than Unorm are packed 32-bit words.
// WebGPU has neither, so the vertex shader converts (vertex_format_input).
enum class VertexFormat : uint8_t {
    Float32, Float32x2, Float32x3, Float32x4, Unorm8x4,
    Unorm8, Unorm8x2, Snorm8, Snorm8x2, Snorm8x4, Unorm8x4Bgra,
    Float16, Float16x2, Float16x4, Unorm16, Unorm16x2, Unorm16x4, Snorm16, Snorm16x2, Snorm16x4,
    Unorm10_10_10_2,
    Uint8, Uint8x2, Uint8x4, Sint8, Sint8x2, Sint8x4,
    Uint16, Uint16x2, Uint16x4, Sint16, Sint16x2, Sint16x4,
    Uint32, Uint32x2, Uint32x3, Uint32x4, Sint32, Sint32x2, Sint32x3, Sint32x4,
    UScaled8, UScaled8x2, UScaled8x4, SScaled8, SScaled8x2, SScaled8x4,
    UScaled16, UScaled16x2, UScaled16x4, SScaled16, SScaled16x2, SScaled16x4,
    UScaled32, UScaled32x2, UScaled32x3, UScaled32x4, SScaled32, SScaled32x2, SScaled32x3, SScaled32x4,
    Uint10_10_10_2, Sint10_10_10_2, Snorm10_10_10_2, UScaled10_10_10_2, SScaled10_10_10_2,
};
// How the vertex shader takes an attribute of this format: as the float the
// fetch delivers, as raw integers (Uint/Sint), as integers it converts to
// float (UScaled/SScaled), or as a packed 10:10:10:2 word it unpacks
// (Packed*, x in bits 0-9 ... w in bits 30-31).
enum class VertexInput : uint8_t {
    Float, Uint, Sint, UScaled, SScaled,
    PackedUint, PackedSint, PackedSnorm, PackedUScaled, PackedSScaled,
};
constexpr VertexInput vertex_format_input(VertexFormat f) {
    using F = VertexFormat;
    using I = VertexInput;
    if (f >= F::Uint8 && f <= F::Uint8x4) return I::Uint;
    if (f >= F::Sint8 && f <= F::Sint8x4) return I::Sint;
    if (f >= F::Uint16 && f <= F::Uint16x4) return I::Uint;
    if (f >= F::Sint16 && f <= F::Sint16x4) return I::Sint;
    if (f >= F::Uint32 && f <= F::Uint32x4) return I::Uint;
    if (f >= F::Sint32 && f <= F::Sint32x4) return I::Sint;
    if (f >= F::UScaled8 && f <= F::UScaled8x4) return I::UScaled;
    if (f >= F::SScaled8 && f <= F::SScaled8x4) return I::SScaled;
    if (f >= F::UScaled16 && f <= F::UScaled16x4) return I::UScaled;
    if (f >= F::SScaled16 && f <= F::SScaled16x4) return I::SScaled;
    if (f >= F::UScaled32 && f <= F::UScaled32x4) return I::UScaled;
    if (f >= F::SScaled32 && f <= F::SScaled32x4) return I::SScaled;
    switch (f) {
    case F::Uint10_10_10_2: return I::PackedUint;
    case F::Sint10_10_10_2: return I::PackedSint;
    case F::Snorm10_10_10_2: return I::PackedSnorm;
    case F::UScaled10_10_10_2: return I::PackedUScaled;
    case F::SScaled10_10_10_2: return I::PackedSScaled;
    default: return I::Float;
    }
}
// 0 = read as float, 1 = unsigned integer, 2 = signed integer (raw integer formats only).
constexpr int vertex_format_integer(VertexFormat f) {
    const VertexInput i = vertex_format_input(f);
    return i == VertexInput::Uint ? 1 : i == VertexInput::Sint ? 2 : 0;
}
struct VertexAttrib {
    uint32_t location = 0;  // shader input (generic attribute) index
    VertexFormat format = VertexFormat::Float32x4;
    uint32_t stream = 0;    // vertex buffer binding
    uint32_t offset = 0;    // bytes into the vertex
};
enum class Topology : uint8_t { Points, Lines, LineStrip, Triangles, TriangleStrip };
enum class ShaderStage : uint8_t { Vertex, Fragment, Compute };
enum class IndexType : uint8_t { U8, U16, U32 };
// Base of the storage buffer descriptors of a stage in the driver constant buffer (abi.hpp).
inline uint32_t storage_desc_offset(ShaderStage stage) {
    return stage == ShaderStage::Compute    ? kStorageBufferDescOffsetCompute
           : stage == ShaderStage::Fragment ? kStorageBufferDescOffsetFragment
                                            : kStorageBufferDescOffset;
}

// Graphics and compute programs are bound independently (NVN binds per
// stage): a compute program replaces only the compute binding.
struct CmdBindProgram {
    ProgramId program = 0;
    bool compute = false;
};
struct CmdSetVertexLayout {
    std::vector<VertexAttrib> attribs;
    std::vector<uint32_t> strides;  // per stream, bytes
    // Per stream: 0 = one element per vertex, n = one element per n instances
    // (nvnVertexStreamStateSetDivisor; element = base instance + instance / n).
    // Empty or shorter than strides = 0.
    std::vector<uint32_t> divisors;
};
struct CmdBindVertexBuffer {
    uint32_t stream = 0;
    uint64_t address = 0;
    uint64_t size = 0;
};
struct CmdBindUniformBuffer {
    ShaderStage stage = ShaderStage::Vertex;
    uint32_t bank = 0;  // c[bank]
    uint64_t address = 0;
    uint64_t size = 0;
};
struct CmdDraw {
    Topology topology = Topology::Triangles;
    uint32_t first_vertex = 0;
    uint32_t vertex_count = 0;
    uint32_t first_instance = 0;
    uint32_t instance_count = 1;
};
struct CmdDrawIndexed {
    Topology topology = Topology::Triangles;
    IndexType type = IndexType::U16;
    uint32_t index_count = 0;
    uint64_t index_address = 0;  // guest VA of the first index
    int32_t base_vertex = 0;
    uint32_t first_instance = 0;
    uint32_t instance_count = 1;
};

// --- Compute. Storage buffers are guest memory ranges a compute shader
// reaches through 64-bit addresses: the driver constant buffer (bank 0)
// carries (address, size) of storage buffer k at storage_desc_offset(stage)
// + k * 16 (abi.hpp), the backend fills it in and maps the ranges to the
// shader's global memory slots.
struct CmdBindStorageBuffer {
    ShaderStage stage = ShaderStage::Compute;
    uint32_t index = 0;
    uint64_t address = 0;
    uint64_t size = 0;  // 0 = unbind
};
// nvnCommandBufferDispatchCompute; indirect_address != 0: the three group
// counts are read from guest memory when the list executes
// (nvnCommandBufferDispatchComputeIndirect).
struct CmdDispatchCompute {
    uint32_t groups[3] = {1, 1, 1};
    uint64_t indirect_address = 0;
};

// --- Textures and samplers for shaders. The HLE resolves NVN's bindless
// handles (texture pool id + sampler pool id) when the command is recorded.

enum class Filter : uint8_t { Nearest, Linear };
// ClampToBorder: outside [0, 1] the lookup returns the border colour
// (border[]); the WebGPU backend emulates it for depth-compare lookups only.
enum class AddressMode : uint8_t { Repeat, MirrorRepeat, ClampToEdge, ClampToBorder };
enum class CompareFunc : uint8_t { Never, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always };
struct SamplerDesc {
    Filter min = Filter::Linear;
    Filter mag = Filter::Linear;
    Filter mip = Filter::Nearest;
    bool mipmapped = false;  // min filter uses mip levels
    AddressMode wrap[3] = {AddressMode::Repeat, AddressMode::Repeat, AddressMode::Repeat};
    float min_lod = 0.0f, max_lod = 1000.0f;
    uint16_t max_anisotropy = 1;
    // Added to the level of detail of every filtered lookup (implicit,
    // explicit and shader-biased) before the clamps. WebGPU samplers have no
    // bias, so the backend passes it to the shader (wgsl::Options::sampler_lod_bias).
    float lod_bias = 0.0f;
    // Depth compare (NVN compare mode R_TO_TEXTURE): shadow lookups compare
    // the reference with the texel using compare_func.
    bool compare = false;
    CompareFunc compare_func = CompareFunc::LessEqual;
    float border[4] = {0.0f, 0.0f, 0.0f, 0.0f};  // RGBA, used with ClampToBorder
    bool operator==(const SamplerDesc& o) const {
        return min == o.min && mag == o.mag && mip == o.mip && mipmapped == o.mipmapped && wrap[0] == o.wrap[0] &&
               wrap[1] == o.wrap[1] && wrap[2] == o.wrap[2] && min_lod == o.min_lod && max_lod == o.max_lod &&
               max_anisotropy == o.max_anisotropy && lod_bias == o.lod_bias && compare == o.compare &&
               compare_func == o.compare_func && border[0] == o.border[0] && border[1] == o.border[1] &&
               border[2] == o.border[2] && border[3] == o.border[3];
    }
};
// Component a shader sees for each of r, g, b, a (texture view swizzle).
enum class Swizzle : uint8_t { Zero, One, R, G, B, A };
using SwizzleRgba = std::array<Swizzle, 4>;
inline constexpr SwizzleRgba kIdentitySwizzle = {Swizzle::R, Swizzle::G, Swizzle::B, Swizzle::A};

struct CmdBindTexture {
    ShaderStage stage = ShaderStage::Fragment;
    uint32_t slot = 0;        // nvnCommandBufferBindTexture index
    TextureId texture = 0;    // 0 = unbound (the backend binds a dummy)
    SamplerDesc sampler;
    SwizzleRgba swizzle = kIdentitySwizzle;  // from the texture view registered in the pool
    // NVNtextureHandle as recorded; the HLE resolves it through the texture
    // and sampler pools current when the list is submitted (as the GPU does
    // at draw time), so lists recorded without pools still bind.
    uint64_t handle = 0;
};
// nvnCommandBufferSetTexturePool / SetSamplerPool (0 = keep). Used by the
// HLE to resolve handles at submit time; backends ignore it.
struct CmdSetTexturePools {
    uint64_t texture_pool = 0;
    uint64_t sampler_pool = 0;
};
// Linear texel data from guest memory into a texture region
// (nvnCommandBufferCopyBufferToTexture). Rows are row_stride bytes apart
// (0 = tightly packed), slices z .. z + depth - 1 (3D slices or array
// layers) image_stride bytes apart (0 = row stride * height;
// nvnCommandBufferSetCopyImageStride).
struct CmdCopyBufferToTexture {
    uint64_t address = 0;
    TextureId texture = 0;
    uint32_t level = 0;
    int32_t x = 0, y = 0, width = 0, height = 0;
    uint32_t row_stride = 0;
    int32_t z = 0, depth = 1;
    uint64_t image_stride = 0;
};

// nvnCommandBufferCopyBufferToBuffer: guest bytes copied in command order
// (GPU addresses are guest addresses, so later draws read the copy).
struct CmdCopyBuffer {
    uint64_t src = 0, dst = 0, size = 0;
};
// nvnCommandBufferClearTexture: a region of one texture, independent of the
// bound render targets and the scissor. Texel coordinates, row 0 = first
// memory row (texture space, not window space).
struct CmdClearTexture {
    TextureId texture = 0;
    Rect region;
    float rgba[4] = {};
    uint32_t mask = 0xF;  // bit c = channel c written
};
// Texture to texture (nvnCommandBufferCopyTextureToTexture): region of a
// source level/layer into a region of a destination level/layer, in texels
// (row 0 = first row in memory, like CmdCopyBufferToTexture). Regions of
// different sizes scale (flags: kCopyFlagLinearFilter, kCopyFlagMirror*).
// z/depth select slices of a 3D texture or layers of an array.
struct CmdCopyTextureToTexture {
    struct Side {
        TextureId texture = 0;
        uint32_t level = 0;
        uint32_t layer = 0;  // base layer of the view
        int32_t x = 0, y = 0, z = 0, width = 0, height = 0, depth = 1;
    };
    Side src, dst;
    uint32_t flags = 0;
};

// --- Fixed-function state, in WebGPU's terms. Each command replaces its part
// of the queue's state; the state persists across command lists like GPU
// state persists across submits.
enum class BlendFactor : uint8_t {
    Zero, One, SrcColor, OneMinusSrcColor, SrcAlpha, OneMinusSrcAlpha, DstColor, OneMinusDstColor, DstAlpha,
    OneMinusDstAlpha, SrcAlphaSaturated, Constant, OneMinusConstant
};
enum class BlendOp : uint8_t { Add, Subtract, ReverseSubtract, Min, Max };
enum class StencilOp : uint8_t { Keep, Zero, Replace, IncrementClamp, DecrementClamp, Invert, IncrementWrap, DecrementWrap };
enum class CullMode : uint8_t { None, Front, Back };
enum class FrontFace : uint8_t { Ccw, Cw };

inline constexpr uint32_t kMaxColorTargets = 8;

struct BlendEquation {
    BlendFactor src_color = BlendFactor::One, dst_color = BlendFactor::Zero;
    BlendFactor src_alpha = BlendFactor::One, dst_alpha = BlendFactor::Zero;
    BlendOp color_op = BlendOp::Add, alpha_op = BlendOp::Add;
};
// nvnCommandBufferBindBlendState: the equation of one colour target.
struct CmdSetBlendEquation {
    uint32_t target = 0;
    BlendEquation equation;
};
// nvnCommandBufferBindColorState: which targets blend.
struct CmdSetBlendEnables {
    uint8_t enable_mask = 0;  // bit t = target t
};
// nvnCommandBufferBindChannelMaskState: RGBA write mask per target (bit 0 = R).
struct CmdSetChannelMasks {
    uint8_t masks[kMaxColorTargets] = {0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF};
};
struct StencilFace {
    CompareFunc func = CompareFunc::Always;
    StencilOp fail = StencilOp::Keep, depth_fail = StencilOp::Keep, pass = StencilOp::Keep;
};
// nvnCommandBufferBindDepthStencilState.
struct CmdSetDepthStencil {
    bool depth_test = false;
    bool depth_write = true;
    CompareFunc depth_func = CompareFunc::Less;
    bool stencil_test = false;
    StencilFace front, back;
};
// Primitive kinds a depth bias applies to (CmdSetRaster::depth_bias).
inline constexpr uint8_t kDepthBiasPoints = 1, kDepthBiasLines = 2, kDepthBiasTriangles = 4;
// nvnCommandBufferBindPolygonState.
struct CmdSetRaster {
    CullMode cull = CullMode::None;
    FrontFace front_face = FrontFace::Ccw;
    uint8_t depth_bias = 0;  // kDepthBias* bits: polygon offset enables
};
// nvnCommandBufferBindMultisampleState.
struct CmdSetMultisample {
    bool alpha_to_coverage = false;
};
// nvnCommandBufferSetPolygonOffsetClamp: depth bias (OpenGL semantics:
// factor * max depth slope + units * minimum resolvable difference, clamped
// to +-clamp when clamp != 0) for the primitive kinds CmdSetRaster enables.
struct CmdSetDepthBias {
    float factor = 0.0f, units = 0.0f, clamp = 0.0f;
};
// nvnCommandBufferSetDepthClamp: no near/far clipping, depth clamped to the
// depth range instead.
struct CmdSetDepthClamp {
    bool enable = false;
};
// nvnCommandBufferSetRasterizerDiscard: draws produce no fragments (they
// still count as input to the primitive counters).
struct CmdSetRasterizerDiscard {
    bool enable = false;
};
// nvnCommandBufferSetSampleMask: bit s = sample s may be written
// (multisampled targets only).
struct CmdSetSampleMask {
    uint32_t mask = 0xFFFFFFFFu;
};
// nvnCommandBufferReportCounter / ResetCounter. The HLE writes the 16-byte
// NVNcounterData (counter value, timestamp) to `address` itself when the
// list is submitted (hle.hpp, Hle::Impl::counters); backends ignore both.
struct CmdReportCounter {
    uint32_t counter = 0;  // NVNcounterType (CounterTypeNvn)
    uint64_t address = 0;
};
struct CmdResetCounter {
    uint32_t counter = 0;
};
struct CmdSetBlendColor {
    float rgba[4] = {};
};
// Fixed-function alpha test: nvnColorStateSetAlphaTest (through
// nvnCommandBufferBindColorState) and nvnCommandBufferSetAlphaRef. A fragment
// is discarded unless "alpha of color target 0 <func> ref" holds.
struct CmdSetAlphaTest {
    CompareFunc func = CompareFunc::Always;
};
struct CmdSetAlphaRef {
    float ref = 0.0f;
};
// nvnCommandBufferSetStencilRef/ValueMask/Mask. WebGPU has one reference and
// one pair of masks for both faces; the backend uses the front values.
struct CmdSetStencilValues {
    enum Kind : uint8_t { Reference, ValueMask, WriteMask } kind = Reference;
    uint8_t faces = 3;  // bit 0 front, bit 1 back
    uint32_t value = 0;
};

using Command =
    std::variant<CmdSetRenderTargets, CmdClearColor, CmdClearDepthStencil, CmdSetScissors, CmdSetViewports,
                 CmdSetDepthRanges, CmdBarrier, CmdUnsupported, CmdBindProgram, CmdSetVertexLayout,
                 CmdBindVertexBuffer, CmdBindUniformBuffer, CmdDraw, CmdDrawIndexed, CmdBindTexture,
                 CmdCopyBufferToTexture, CmdCopyTextureToTexture, CmdSetBlendEquation, CmdSetBlendEnables, CmdSetChannelMasks,
                 CmdSetDepthStencil, CmdSetRaster, CmdSetMultisample, CmdSetBlendColor, CmdSetStencilValues,
                 CmdBindStorageBuffer, CmdDispatchCompute, CmdSetTexturePools, CmdCopyBuffer, CmdClearTexture,
                 CmdSetDepthBias, CmdSetDepthClamp, CmdSetRasterizerDiscard, CmdSetSampleMask, CmdReportCounter,
                 CmdResetCounter, CmdSetAlphaTest, CmdSetAlphaRef>;

struct CommandList {
    bool origin_lower_left = true;
    // NVN depth mode NEAR_IS_MINUS_W (the NVN default, OpenGL clip space:
    // z in [-w, w], window depth (z / w + 1) / 2). False: NEAR_IS_ZERO,
    // z in [0, w] as in WebGPU.
    bool depth_minus_w = false;
    std::vector<Command> commands;
};

// The display reads texture memory top row first whatever the device's
// window origin mode (that mode only flips rasterization), so presenting
// never flips. UNVERIFIED for NVN; a scissored clear compared with Ryujinx
// on the PC settles it.
struct PresentInfo {
    TextureId texture = 0;
    Rect crop;  // memory rows; width/height 0 = whole texture
    // Lower-left window origin: memory row 0 is the bottom of the picture, so
    // the display shows the (cropped) texture upside down.
    bool flip_y = false;
};

// Access to guest memory for buffer contents: reads for vertex, index and
// uniform data, writes for what compute shaders stored into storage buffers.
class GuestMemoryView {
public:
    virtual ~GuestMemoryView() = default;
    // Copies [va, va+size) to dst; false if any byte is unmapped.
    virtual bool read(uint64_t va, void* dst, uint64_t size) const = 0;
    // True if [va, va+size) is mapped and equals data. Views with direct
    // access compare in place; the default reads in small chunks.
    virtual bool equal(uint64_t va, const void* data, uint64_t size) const {
        uint8_t chunk[4096];  // small: Emscripten threads have 64 KiB of stack
        const auto* p = static_cast<const uint8_t*>(data);
        for (uint64_t done = 0; done < size;) {
            const uint64_t n = std::min<uint64_t>(sizeof(chunk), size - done);
            if (!read(va + done, chunk, n) || std::memcmp(chunk, p + done, size_t(n)) != 0)
                return false;
            done += n;
        }
        return true;
    }
    // Read-only host pointer to [va, va+size) if the view has direct access
    // and the range is mapped, else nullptr (callers fall back to read()).
    virtual const uint8_t* view(uint64_t va, uint64_t size) const {
        (void)va;
        (void)size;
        return nullptr;
    }
    // Copies src to [va, va+size); false if any byte is unmapped (nothing is
    // written then). Read-only views keep the default.
    virtual bool write(uint64_t va, const void* src, uint64_t size) const {
        (void)va;
        (void)src;
        (void)size;
        return false;
    }
};

class Backend {
public:
    virtual ~Backend() = default;
    // Programs for CmdBindProgram; backends that cannot draw ignore them.
    virtual void create_program(ProgramId id, const ProgramDesc& desc) {
        (void)id;
        (void)desc;
    }
    virtual void destroy_program(ProgramId id) { (void)id; }
    virtual void create_texture(TextureId id, const TextureDesc& desc) = 0;
    virtual void destroy_texture(TextureId id) = 0;
    // nvnTextureWriteTexels: CPU upload of linear texel data (row_stride bytes
    // per row) into a region of one level, outside any command list. slices:
    // z range of a 3D texture (or layers of an array), image_stride bytes apart.
    virtual void write_texture(TextureId id, uint32_t level, const Rect& region, const void* data,
                               uint32_t row_stride, const TexelSlices& slices = {}) {
        (void)id;
        (void)level;
        (void)region;
        (void)data;
        (void)row_stride;
        (void)slices;
    }
    // Runs one submitted command list (nvnQueueSubmitCommands, in order).
    virtual void execute(const CommandList& list) = 0;
    // The same for a list the caller no longer needs: backends that keep it
    // (AsyncBackend) take it over instead of copying.
    virtual void execute_owned(CommandList&& list) { execute(list); }
    // nvnQueuePresentTexture: shows the texture and ends the frame. Returns
    // IRenderer::end_frame() (false = window closed).
    virtual bool present(const PresentInfo& info) = 0;
    // nvnQueueFinish, nvnSyncWait: everything submitted so far is visible in
    // guest memory afterwards (compute results written back). Backends that
    // finish each list inside execute() keep the default.
    virtual void finish() {}
    // nvnQueueFenceSync: a mark for what was submitted so far; nvnSyncWait on
    // that fence then only needs what came before it (finish_until). Backends
    // without marks wait for everything.
    virtual uint64_t fence_mark() { return 0; }
    virtual void finish_until(uint64_t mark) {
        (void)mark;
        finish();
    }
    // Runs fn once the lists submitted so far have executed: at once for
    // backends that execute inside execute(), after them on the worker for
    // AsyncBackend (counter reports, nvnCommandBufferReportCounter).
    virtual void after_queued(std::function<void()> fn) { fn(); }
};

// CPU reference renderer: colour textures in host memory (RGBA8 storage for
// the formats FormatInfo::soft_renderable marks), clears with scissor and
// channel mask, present through IRenderer::present_rgba8. No draws (counted
// as skipped).
class SoftBackend final : public Backend {
public:
    explicit SoftBackend(platform::IRenderer& renderer) : renderer_(renderer) {}

    void create_texture(TextureId id, const TextureDesc& desc) override;
    void destroy_texture(TextureId id) override;
    void execute(const CommandList& list) override;
    bool present(const PresentInfo& info) override;

    struct Stats {
        uint64_t clears = 0;
        uint64_t skipped_commands = 0;  // CmdUnsupported, clears of formats it cannot render
        uint64_t presents = 0;
    };
    Stats stats() const;
    // Texel (x, y) of a texture in memory order (row 0 = first row in
    // memory), RGBA8; tests only.
    bool read_texel(TextureId id, uint32_t x, uint32_t y, uint8_t rgba[4]) const;

private:
    struct Texture {
        TextureDesc desc;
        bool renderable = false;
        bool srgb = false;
        std::vector<uint8_t> rgba;  // width * height * 4, level 0, layer 0
    };

    // Render targets and the scissor of viewport 0 (the one clears use; unset
    // = whole target). NVN keeps them across command lists.
    struct ExecState {
        std::vector<TextureId> colors;
        bool scissor_set = false;
        Rect scissor;
    };

    platform::IRenderer& renderer_;
    mutable std::mutex mutex_;
    std::unordered_map<TextureId, Texture> textures_;
    ExecState state_;
    Stats stats_;
};

}  // namespace lgp::gpu::nvn
