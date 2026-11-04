// NVN ABI facts the HLE depends on (ADR-0003, P4.1).
//
// NVN is not publicly documented. Everything in this file is either derived
// from how a game uses the API (docs/knowledge/nvn-bootstrap-und-treiber.md)
// or remembered from public homebrew/modding code, and is marked as such.
// UNVERIFIED values must be checked against the NVN driver in the game's own
// `sdk` module (getter/setter implementations, nvnDeviceGetInteger) on the PC
// before P4.1 is ticked; docs/knowledge/nvn-hle.md lists how.
//
// Only names, numbers and sizes live here, no driver code.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "lgp/gpu/nvn/texture_layout.hpp"

namespace lgp::gpu::nvn {

// ---------------------------------------------------------------------------
// Guest storage the game provides per object (bytes).
//
// The HLE keeps the state of real objects (Device, Queue, Texture, ...) on
// the host, keyed by the object's guest address, and writes nothing into
// their storage. Builders and small state objects (TextureView, ...) are
// passed and copied by value by the game, so their HLE state lives inline in
// the guest storage and must fit. The sizes below are the *smallest* storage
// we assume; every inline state static_asserts against them. UNVERIFIED
// (remembered NVN 5.x header sizes); a real size smaller than ours would let
// the HLE overwrite neighbouring game data, so this is the first thing the
// PC check confirms.
inline constexpr size_t kDeviceBuilderSize = 64;
inline constexpr size_t kQueueBuilderSize = 64;
inline constexpr size_t kMemoryPoolBuilderSize = 64;
inline constexpr size_t kBufferBuilderSize = 64;
inline constexpr size_t kTextureBuilderSize = 128;
inline constexpr size_t kTextureViewSize = 40;
inline constexpr size_t kWindowBuilderSize = 64;

// The small state objects (BlendState, ColorState, ChannelMaskState,
// DepthStencilState, MultisampleState, PolygonState, VertexAttribState,
// VertexStreamState) and SamplerBuilder hold their HLE state on the host; the
// guest storage only carries a 32-bit reference to an interned value
// (procs_draw.cpp). The only size assumption for them is ">= 4 bytes".
inline constexpr size_t kStateReferenceSize = 4;
// Except for arrays: nvnCommandBufferBindVertexAttribState/StreamState take
// an array of states, so the element size is the array stride. UNVERIFIED
// (remembered NVN header sizes); a wrong stride reads attribute i from the
// wrong place. checklist ("Prüfliste") item 7 in docs/knowledge/nvn-hle.md.
inline constexpr size_t kVertexAttribStateSize = 4;
inline constexpr size_t kVertexStreamStateSize = 8;
// NVNshaderData {NVNbufferAddress data; const void* control;} and
// NVNbufferRange {NVNbufferAddress address; uint64_t size;}: two 64-bit
// fields each, layout follows from the types.
inline constexpr size_t kShaderDataSize = 16;
inline constexpr size_t kBufferRangeSize = 16;
// NVNcopyRegion {int xoffset, yoffset, zoffset, width, height, depth;}.
inline constexpr size_t kCopyRegionSize = 24;
// NVNcopyFlags (nvnCommandBufferCopyTextureToTexture). UNVERIFIED (remembered
// public header order: NONE, LINEAR_FILTER, ENGINE_2D, MIRROR_X/Y/Z as bits);
// LINEAR_FILTER only matters for scaled copies, ENGINE_2D only picks the
// hardware unit.
inline constexpr uint32_t kCopyFlagLinearFilter = 0x1;
inline constexpr uint32_t kCopyFlagEngine2D = 0x2;
inline constexpr uint32_t kCopyFlagMirrorX = 0x4;
inline constexpr uint32_t kCopyFlagMirrorY = 0x8;
inline constexpr uint32_t kCopyFlagMirrorZ = 0x10;

// ---------------------------------------------------------------------------
// Enumerations. Numeric values UNVERIFIED unless noted.

enum class Boolean : uint32_t { False = 0, True = 1 };

// NVNtextureTarget.
enum class TextureTarget : uint32_t {
    Target1D = 0,
    Target2D = 1,
    Target3D = 2,
    Target1DArray = 3,
    Target2DArray = 4,
    Target2DMultisample = 5,
    Target2DMultisampleArray = 6,
    TargetRectangle = 7,
    TargetCubemap = 8,
    TargetCubemapArray = 9,
    TargetBuffer = 10,
};

// NVNwindowOriginMode (nvnDeviceSetWindowOriginMode). NVN defaults to
// LOWER_LEFT like OpenGL: scissor/viewport y counts from the bottom row.
enum class WindowOriginMode : uint32_t { LowerLeft = 0, UpperLeft = 1 };
// NVNdepthMode (nvnDeviceSetDepthMode).
enum class DepthMode : uint32_t { NearIsMinusW = 0, NearIsZero = 1 };

// NVNqueueAcquireTextureResult / NVNwindowAcquireTextureResult.
inline constexpr uint32_t kAcquireTextureSuccess = 0;
// NVNsyncWaitResult.
enum class SyncWaitResult : uint32_t { AlreadySignaled = 0, ConditionSatisfied = 1, TimeoutExpired = 2, Failed = 3 };
// NVNqueueGetErrorResult: no error.
inline constexpr uint32_t kQueueGetErrorResultGpuNoError = 0;

// ClearColor mask: one bit per channel, R = bit 0 ... A = bit 3 (UNVERIFIED).
inline constexpr uint32_t kClearColorMaskRGBA = 0xF;

// Texture and sampler handles as the shader reads them from uniform data:
// Maxwell's bindless convention (texture descriptor index in bits 0-19,
// sampler descriptor index in bits 20-31), which the shader translator
// (gpu/shader) decodes the same way. Verified for Maxwell (Ryujinx'
// TextureHandle); that NVN returns exactly this value is UNVERIFIED.
inline constexpr uint64_t texture_handle(uint32_t texture_id, uint32_t sampler_id) {
    return (uint64_t{sampler_id & 0xFFFu} << 20) | (texture_id & 0xFFFFFu);
}

// NVNformat. Values checked against the driver's format table in LGP's own
// sdk (texel size, block size, sRGB flag per entry; analysis 11); the full
// table with all names is in abi.cpp (format_info). Only the formats code
// refers to by name are listed here.
enum class Format : uint32_t {
    None = 0,
    R8 = 1,
    RG8 = 13,
    RGBA8 = 37,
    RGBA16F = 41,
    RGBA32F = 46,
    Stencil8 = 49,
    Depth16 = 50,
    Depth24 = 51,
    Depth32F = 52,
    Depth24Stencil8 = 53,
    Depth32FStencil8 = 54,
    RGBX8_SRGB = 55,
    RGBA8_SRGB = 56,
    RGBA4 = 57,
    RGB5 = 58,
    RGB5A1 = 59,
    RGB565 = 60,
    RGB10A2 = 61,
    R11G11B10F = 63,
    RGB_DXT1 = 65,
    RGBA_DXT1 = 66,
    RGBA_DXT3 = 67,
    RGBA_DXT5 = 68,
    RGB_DXT1_SRGB = 69,
    RGBA_DXT1_SRGB = 70,
    RGBA_DXT3_SRGB = 71,
    RGBA_DXT5_SRGB = 72,
    RGTC1_UNORM = 73,
    RGTC1_SNORM = 74,
    RGTC2_UNORM = 75,
    RGTC2_SNORM = 76,
    BPTC_UNORM = 77,
    BPTC_UNORM_SRGB = 78,
    BPTC_SFLOAT = 79,
    BPTC_UFLOAT = 80,
    ASTC_4x4 = 121,
    ASTC_12x12_SRGB = 148,
    BGR565 = 149,
    BGRX8 = 153,
    BGRA8 = 154,
    BGRX8_SRGB = 155,
    BGRA8_SRGB = 156,
};

enum class FormatKind : uint8_t { Color, Depth, DepthStencil, Stencil };

struct FormatInfo {
    std::string_view name;
    uint32_t bytes_per_pixel = 0;  // bytes per block (per texel for 1x1 blocks)
    uint32_t block_width = 1;      // texels per block (4 for BCn, 4..12 for ASTC)
    uint32_t block_height = 1;
    FormatKind kind = FormatKind::Color;
    bool srgb = false;
    // The CPU reference backend can clear and present this format.
    bool soft_renderable = false;
};

std::optional<FormatInfo> format_info(uint32_t format);

// Vertex attribute formats use NVNformat too. Values continue the remembered
// order above (R8=1 ... R32F=10, RG32F=22, RGB32F=34, RGBA8=37, RGBA32F=46),
// UNVERIFIED like the rest of the enum.
inline constexpr uint32_t kFormatR32F = 10;
inline constexpr uint32_t kFormatRG32F = 22;
inline constexpr uint32_t kFormatRGB32F = 34;

// ---------------------------------------------------------------------------
// Draw-related enumerations, all UNVERIFIED (remembered from the public NVN
// header shape; most follow OpenGL's order, compare functions and blend
// factors start at 1 like the Maxwell register values). The PC check reads
// them from the driver (checklist ("Prüfliste") item 8).

// NVNdrawPrimitive.
enum class DrawPrimitive : uint32_t {
    Points = 0, Lines = 1, LineLoop = 2, LineStrip = 3, Triangles = 4, TriangleStrip = 5, TriangleFan = 6,
    Quads = 7, QuadStrip = 8, Polygon = 9, LinesAdjacency = 10, LineStripAdjacency = 11,
    TrianglesAdjacency = 12, TriangleStripAdjacency = 13, Patches = 14,
};
// NVNindexType.
enum class IndexTypeNvn : uint32_t { UnsignedByte = 0, UnsignedShort = 1, UnsignedInt = 2 };
// NVNshaderStage (BindUniformBuffer, BindTexture) and NVNshaderStageBits (BindProgram).
enum class ShaderStageNvn : uint32_t { Vertex = 0, Fragment = 1, Geometry = 2, TessControl = 3, TessEvaluation = 4, Compute = 5 };
inline constexpr uint32_t kShaderStageBitVertex = 0x1;
inline constexpr uint32_t kShaderStageBitFragment = 0x2;
inline constexpr uint32_t kShaderStageBitCompute = 0x20;  // UNVERIFIED (ALL_GRAPHICS = 0x1F below it)
// NVNdepthFunc / NVNstencilFunc / NVNalphaFunc.
enum class CompareFuncNvn : uint32_t { Never = 1, Less = 2, Equal = 3, LessEqual = 4, Greater = 5, NotEqual = 6, GreaterEqual = 7, Always = 8 };
// NVNstencilOp.
enum class StencilOpNvn : uint32_t { Keep = 1, Zero = 2, Replace = 3, Increment = 4, Decrement = 5, Invert = 6, IncrementWrap = 7, DecrementWrap = 8 };
// NVNface (cull face, stencil faces).
enum class FaceNvn : uint32_t { None = 0, Front = 1, Back = 2, FrontAndBack = 3 };
// NVNfrontFace.
enum class FrontFaceNvn : uint32_t { Cw = 0, Ccw = 1 };
// NVNpolygonMode.
enum class PolygonModeNvn : uint32_t { Point = 0, Line = 1, Fill = 2 };
// NVNpolygonOffsetEnable bits (PolygonStateSetPolygonOffsetEnables).
inline constexpr uint32_t kPolygonOffsetPoint = 0x1, kPolygonOffsetLine = 0x2, kPolygonOffsetFill = 0x4;
// NVNcounterType (ReportCounter, ResetCounter).
enum class CounterTypeNvn : uint32_t {
    Timestamp = 0, SamplesPassed = 1, InputVertices = 2, InputPrimitives = 3, VertexShaderInvocations = 4,
    TessControlShaderInvocations = 5, TessEvaluationShaderInvocations = 6, GeometryShaderInvocations = 7,
    FragmentShaderInvocations = 8, TessEvaluationShaderPrimitives = 9, GeometryShaderPrimitives = 10,
    ClipperInputPrimitives = 11, ClipperOutputPrimitives = 12, PrimitivesGenerated = 13,
    TransformFeedbackPrimitivesWritten = 14, ZcullStats = 15, TimestampTop = 16,
};
// NVNcounterData: 64-bit counter value, then a 64-bit GPU timestamp. GPU
// timestamps count ns * 384 / 625 (what nvnDeviceGetTimestampInNanoseconds
// undoes; the factor other Switch GPU emulators use). ZCULL_STATS writes a
// different, larger record (not supported).
inline constexpr uint64_t kCounterDataSize = 16;
inline constexpr uint64_t kGpuTimestampNumerator = 384, kGpuTimestampDenominator = 625;
// NVNblendFunc.
enum class BlendFuncNvn : uint32_t {
    Zero = 1, One = 2, SrcColor = 3, OneMinusSrcColor = 4, SrcAlpha = 5, OneMinusSrcAlpha = 6, DstAlpha = 7,
    OneMinusDstAlpha = 8, DstColor = 9, OneMinusDstColor = 10, SrcAlphaSaturate = 11, ConstantColor = 17,
    OneMinusConstantColor = 18,
};
// NVNblendEquation.
enum class BlendEquationNvn : uint32_t { Add = 1, Subtract = 2, ReverseSubtract = 3, Min = 4, Max = 5 };
// NVNlogicOp: COPY is the only one WebGPU can do.
inline constexpr uint32_t kLogicOpCopy = 3;
// NVNminFilter / NVNmagFilter.
enum class MinFilterNvn : uint32_t {
    Nearest = 0, Linear = 1, NearestMipmapNearest = 2, LinearMipmapNearest = 3, NearestMipmapLinear = 4,
    LinearMipmapLinear = 5,
};
enum class MagFilterNvn : uint32_t { Nearest = 0, Linear = 1 };
// NVNwrapMode.
enum class WrapModeNvn : uint32_t {
    Clamp = 0, Repeat = 1, MirrorClamp = 2, MirrorClampToEdge = 3, MirrorClampToBorder = 4, ClampToBorder = 5,
    MirroredRepeat = 6, ClampToEdge = 7,
};

// Shader code as NVN takes it (NVNshaderData::data). Graphics stages in BNSH
// start with a 0x30-byte NVN code header (magic 0x12345678) before the
// 0x50-byte SPH (docs/knowledge/bnsh-format.md, measured on a real game's
// shaders); the HLE accepts code with and without that header.
inline constexpr uint32_t kNvnCodeHeaderMagic = 0x12345678;
inline constexpr uint64_t kNvnCodeHeaderSize = 0x30;

// Constant buffer banks as compiled NVN shaders address them. Measured in
// LGP's driver: nvnCommandBufferBindUniformBuffer(stage, index, ...) binds
// the buffer to Maxwell constant buffer slot index + 3 (CB_BIND), so user
// uniform block `index` is c[index + 3]. c[1] holds the compiler's constant
// pool (literals the code reads from memory); the driver binds it from the
// stage's code: kNvnControlConstantsSize / kNvnControlConstantsOffset in the
// control section give its size and its offset from the start of the NVN
// code header (measured on all BNSH shaders of the title: the offset is the
// end of the code, offset + size the end of the code blob, and the constants
// the assembly text in the control section names sit there). c[0] is the
// driver's own constant buffer (texture handles, storage buffers).
inline constexpr uint32_t kUniformBufferBankBase = 3;
inline constexpr uint32_t kCompilerConstantBank = 1;
inline constexpr uint64_t kNvnControlConstantsSize = 0x6FC;
inline constexpr uint64_t kNvnControlConstantsOffset = 0x700;
inline constexpr uint64_t kNvnControlBlobSize = 0x704;

// The control section (NVNshaderData::control, CPU memory, magic 0x98761234)
// of every stage. Program fields after the constant pool ones above, as
// public tools document them (THIRD_PARTY.md: uam-nvn nvn_control.h,
// KillzXGaming ShaderLibrary ControlShader.cs, masagrator's glslc output
// dumper; all three agree, and the constant pool offsets they give are the
// ones measured on LGP's shaders):
//   +0x708 entry offset of the stage's code from NVNshaderData::data
//          (graphics 0x30 = behind the NVN code header, at the SPH)
//   +0x714 NVNshaderStage (0 vertex, 1 fragment, 2 geometry, 3 tess control,
//          4 tess evaluation, 5 compute)
// and for compute stages (QMD inputs, same order as deko3d's DKSH):
//   +0x748 workgroup size x, y, z   +0x754 shared memory bytes
//   +0x758 local memory bytes per thread (positive, "low")
//   +0x75C local memory negative part ("high")   +0x760 CRS stack bytes
//   +0x764 barrier count
// Checked against LGP's compute stages (checklist ("Prüfliste") item 11 in
// docs/knowledge/nvn-hle.md): entry offset 0 (glslc's newer output uses
// 0x100), workgroup 32x1x1 in all of them, and their code indexes with
// ctaid.x * 32 + tid.x; shared/local memory and barriers are 0 there and the
// code uses none, so those offsets are consistent but not proven by a
// non-zero value.
inline constexpr uint32_t kNvnControlMagic = 0x98761234;
inline constexpr uint64_t kNvnControlEntryOffset = 0x708;
inline constexpr uint64_t kNvnControlStage = 0x714;
inline constexpr uint64_t kNvnControlComputeWorkgroup = 0x748;
inline constexpr uint64_t kNvnControlComputeSharedMemory = 0x754;
inline constexpr uint64_t kNvnControlComputeLocalMemoryLow = 0x758;
inline constexpr uint64_t kNvnControlComputeLocalMemoryHigh = 0x75C;
inline constexpr uint64_t kNvnControlComputeCrsSize = 0x760;
inline constexpr uint64_t kNvnControlComputeBarriers = 0x764;
// Bytes of the control section the compute reader needs.
inline constexpr uint64_t kNvnControlComputeEnd = 0x768;
inline constexpr uint32_t kNvnStageCompute = 5;

// Storage buffers: the compiled shader takes the 64-bit address and size of
// storage buffer k from the driver constant buffer c[kDriverConstantBank] at
// a per-stage base + k * kStorageBufferDescStride (address low, high, size),
// one 0x100-byte block of 16 descriptors per stage from 0x110 on, in the
// order vertex, tess control, tess evaluation / compute, geometry, fragment
// (as Ryujinx's shader translator lays NVN's driver constant buffer out;
// recalled, not checked against its source here). Checked on LGP
// (checklist ("Prüfliste") item 12): its 24 compute stages read their LDG/STG addresses
// from c0[0x310 + 16 k], k = 0..7; vertex 0x110 matches its glyph vertex
// shaders. Fragment (0x510) UNVERIFIED.
inline constexpr uint32_t kDriverConstantBank = 0;
inline constexpr uint32_t kStorageBufferDescOffset = 0x110;  // vertex
inline constexpr uint32_t kStorageBufferDescOffsetCompute = 0x310;
inline constexpr uint32_t kStorageBufferDescOffsetFragment = 0x510;  // UNVERIFIED
inline constexpr uint32_t kStorageBufferDescStride = 0x10;
// Compute shaders read constant buffers through global memory: c0 at
// kComputeUniformDescOffset + s * 16 holds (address low, high, size) of NVN
// uniform block s (bank kUniformBufferBankBase + s), s < 16; the block of
// descriptors ends where the compute storage descriptors start (0x310).
// Measured on LGP (docs/knowledge/nvn-hle.md, checklist ("Prüfliste") item 12): its compute
// dispatches bind uniform blocks 6, 7 and 8, and its 24 compute stages read
// addresses from c0[0x270], [0x280] and [0x290]. Other blocks UNVERIFIED.
inline constexpr uint32_t kComputeUniformDescOffset = 0x210;
inline constexpr uint32_t kComputeUniformDescCount = 16;

// Bound textures: nvnCommandBufferBindTexture(stage, index, handle) makes the
// handle visible to the stage's shaders at texture slot `index`. Measured in
// LGP's driver: it writes the 64-bit handle with CB_POS/CB_DATA to byte
// 0x20 + 8 * index of the stage's driver constant buffer, so a compiled
// shader addresses slot `index` as TEX handle (word offset)
// `kBoundTextureHandleBase + kBoundTextureHandleStride * index`. The base is
// overridable through HleOptions::bound_texture_handle_base.
inline constexpr uint32_t kBoundTextureHandleBase = 8;
inline constexpr uint32_t kBoundTextureHandleStride = 2;

// nvnDeviceGetInteger. The game asks for alignments and limits; the answer
// must match what its NVN version expects. Keys are NVNdeviceInfo values.
// UNVERIFIED: the defaults come from memory; the PC check reads the real
// switch in sdk's nvnDeviceGetInteger. Unknown keys answer 0 and are logged.
struct DeviceInteger {
    uint32_t key;
    int32_t value;
    std::string_view name;
};
// Built-in defaults (may be overridden through HleOptions::device_integers).
const DeviceInteger* default_device_integers(size_t* count);

// Storage of a texture in a memory pool. The HLE renders into host
// textures, so the guest storage only needs to be large enough for what the
// game does with it (CPU uploads go through API calls). This approximates
// Maxwell's block-linear size (64-byte x 8-row GOBs, block height up to 16
// GOBs) so pool layouts look like on hardware; UNVERIFIED against the driver.
uint64_t texture_storage_size(uint32_t format, uint32_t target, uint32_t width, uint32_t height, uint32_t depth,
                              uint32_t levels);
// Block-linear layout of a texture (texture_layout.hpp) with layers and depth
// taken from the target the way NVN interprets the builder's depth.
// gob_height_log2: level-0 block height from a packaged texture layout
// (TextureDesc::gob_height_log2), -1 = NVN's default rule.
TextureLayout texture_layout(uint32_t format, uint32_t target, uint32_t width, uint32_t height, uint32_t depth,
                             uint32_t levels, int32_t gob_height_log2 = -1);
inline constexpr uint64_t kTextureStorageAlignment = 0x200;
// NVNtextureFlags bits the HLE looks at. Measured: the driver's storage
// alignment query returns 0x80 for bit 6 (linear render target) and 0x20 for
// bit 4 (linear), so bit 4 is LINEAR and linear rows are 32-byte aligned.
inline constexpr uint32_t kTextureFlagCompressible = 0x8;  // render targets (UNVERIFIED)
inline constexpr uint32_t kTextureFlagLinear = 0x10;
inline constexpr uint32_t kTextureFlagLinearRenderTarget = 0x40;
inline constexpr uint32_t kLinearTextureStrideAlignment = 0x20;  // one GOB block row (UNVERIFIED)

}  // namespace lgp::gpu::nvn
