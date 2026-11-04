// Guest test program for the NVN-HLE draw path (P4.1 part 2): what a game
// does to draw, built for AArch64 with the host clang, packed as NRO and
// recompiled with lgp_lift like nvn_clear.cpp. Own code, no NVN header:
// function types from the public C API shape, enum values and object sizes
// as the HLE assumes them (gpu/nvn/include/lgp/gpu/nvn/abi.hpp, UNVERIFIED
// there). Own Maxwell test shaders (tests/gpu/shaders: vs_transform +
// fs_textured, translated by the HLE at nvnProgramSetShaders).
//
// One 64x64 frame, NDC +y up, lower-left window origin:
//   top-left     green quad, z 0.5 (depth test LESS, writes depth)
//   top middle   red quad, z 0.75, half behind the green one
//   bottom-left  blue quad wound clockwise: culled (back faces, CCW front)
//   bottom-right 2x2 checker texture, nearest, indexed draw (16-bit indices),
//                texture uploaded with CopyBufferToTexture
//   top-right    white, alpha 0.5, blended over the clear colour
// The host (test_nvn_triangle.cpp) checks the pixels.
//
// Then compute: a second command list dispatches the compute test shaders
// cs_memory (2 workgroups of 64: 64-bit address chains, shared memory,
// barriers, atomics) and cs_lockstep (barriers after a divergent exit, local
// memory, byte stores) on storage buffers in the pool; after
// nvnQueueFinish the program checks the results in its own memory. The
// control section of a compute stage uses NVN's layout (gpu/nvn abi.hpp,
// kNvnControlComputeWorkgroup), read by the HLE's built-in reader; the code
// starts at the control section's entry offset 0x100 behind filler bytes,
// as glslc lays compute stages out.
//
// guest_main(bootstrap): x0 = nvnBootstrapLoader. Returns 0 on success,
// otherwise the number of the first failed step.
#include <stddef.h>
#include <stdint.h>

#include "nvn_test_shaders.h"

// Freestanding: the compiler may still emit calls to these.
extern "C" void* memcpy(void* dst, const void* src, size_t n) {
    auto* d = static_cast<volatile uint8_t*>(dst);
    const auto* s = static_cast<const uint8_t*>(src);
    for (size_t i = 0; i < n; ++i) d[i] = s[i];
    return dst;
}
extern "C" void* memset(void* dst, int v, size_t n) {
    auto* d = static_cast<volatile uint8_t*>(dst);
    for (size_t i = 0; i < n; ++i) d[i] = static_cast<uint8_t>(v);
    return dst;
}

typedef uint8_t NVNboolean;
typedef uint64_t NVNcommandHandle;
typedef uint64_t NVNbufferAddress;
typedef uint64_t NVNtextureHandle;

// Opaque storage, sizes >= what the HLE needs.
struct alignas(8) NVNdeviceBuilder { char r[64]; };
struct alignas(8) NVNdevice { char r[0x3000]; };
struct alignas(8) NVNqueueBuilder { char r[64]; };
struct alignas(8) NVNqueue { char r[0x2000]; };
struct alignas(8) NVNmemoryPoolBuilder { char r[64]; };
struct alignas(8) NVNmemoryPool { char r[256]; };
struct alignas(8) NVNtextureBuilder { char r[128]; };
struct alignas(8) NVNtexture { char r[192]; };
struct alignas(8) NVNtextureView { char r[40]; };
struct alignas(8) NVNwindowBuilder { char r[64]; };
struct alignas(8) NVNwindow { char r[384]; };
struct alignas(8) NVNsync { char r[64]; };
struct alignas(8) NVNcommandBuffer { char r[160]; };
struct alignas(8) NVNprogram { char r[192]; };
struct alignas(8) NVNsamplerBuilder { char r[96]; };
struct alignas(8) NVNsampler { char r[96]; };
struct alignas(8) NVNtexturePool { char r[32]; };
struct alignas(8) NVNsamplerPool { char r[32]; };
// State objects at the sizes the HLE assumes for arrays (abi.hpp).
struct alignas(4) NVNvertexAttribState { char r[4]; };
struct alignas(8) NVNvertexStreamState { char r[8]; };
struct alignas(8) NVNblendState { char r[8]; };
struct alignas(4) NVNcolorState { char r[4]; };
struct alignas(4) NVNchannelMaskState { char r[4]; };
struct alignas(8) NVNdepthStencilState { char r[8]; };
struct alignas(4) NVNpolygonState { char r[4]; };
struct alignas(8) NVNmultisampleState { char r[24]; };
struct NVNshaderData {
    NVNbufferAddress data;
    const void* control;
};
struct NVNbufferRange {
    NVNbufferAddress address;
    uint64_t size;
};
struct NVNcopyRegion {
    int xoffset, yoffset, zoffset, width, height, depth;
};

typedef void (*PFNGENERIC)(void);
typedef PFNGENERIC (*PFNBOOTSTRAP)(const char*);
typedef PFNGENERIC (*PFNGETPROC)(const NVNdevice*, const char*);

#define NVN_PROCS(X)                                                                                         \
    X(DeviceBuilderSetDefaults, void, NVNdeviceBuilder*)                                                     \
    X(DeviceInitialize, NVNboolean, NVNdevice*, const NVNdeviceBuilder*)                                     \
    X(DeviceGetTextureHandle, NVNtextureHandle, const NVNdevice*, int, int)                                  \
    X(QueueBuilderSetDefaults, void, NVNqueueBuilder*)                                                       \
    X(QueueBuilderSetDevice, void, NVNqueueBuilder*, NVNdevice*)                                             \
    X(QueueInitialize, NVNboolean, NVNqueue*, const NVNqueueBuilder*)                                        \
    X(QueueSubmitCommands, void, NVNqueue*, int, const NVNcommandHandle*)                                    \
    X(QueuePresentTexture, void, NVNqueue*, NVNwindow*, int)                                                 \
    X(QueueFinish, void, NVNqueue*)                                                                          \
    X(MemoryPoolBuilderSetDefaults, void, NVNmemoryPoolBuilder*)                                             \
    X(MemoryPoolBuilderSetDevice, void, NVNmemoryPoolBuilder*, NVNdevice*)                                   \
    X(MemoryPoolBuilderSetFlags, void, NVNmemoryPoolBuilder*, int)                                           \
    X(MemoryPoolBuilderSetStorage, void, NVNmemoryPoolBuilder*, void*, size_t)                               \
    X(MemoryPoolInitialize, NVNboolean, NVNmemoryPool*, const NVNmemoryPoolBuilder*)                         \
    X(MemoryPoolGetBufferAddress, NVNbufferAddress, const NVNmemoryPool*)                                    \
    X(TextureBuilderSetDefaults, void, NVNtextureBuilder*)                                                   \
    X(TextureBuilderSetDevice, void, NVNtextureBuilder*, NVNdevice*)                                         \
    X(TextureBuilderSetTarget, void, NVNtextureBuilder*, int)                                                \
    X(TextureBuilderSetFormat, void, NVNtextureBuilder*, int)                                                \
    X(TextureBuilderSetSize2D, void, NVNtextureBuilder*, int, int)                                           \
    X(TextureBuilderGetStorageSize, size_t, const NVNtextureBuilder*)                                        \
    X(TextureBuilderSetStorage, void, NVNtextureBuilder*, NVNmemoryPool*, ptrdiff_t)                         \
    X(TextureInitialize, NVNboolean, NVNtexture*, const NVNtextureBuilder*)                                  \
    X(TextureWriteTexels, void, const NVNtexture*, const NVNtextureView*, const NVNcopyRegion*, const void*) \
    X(WindowBuilderSetDefaults, void, NVNwindowBuilder*)                                                     \
    X(WindowBuilderSetDevice, void, NVNwindowBuilder*, NVNdevice*)                                           \
    X(WindowBuilderSetTextures, void, NVNwindowBuilder*, int, NVNtexture* const*)                            \
    X(WindowInitialize, NVNboolean, NVNwindow*, const NVNwindowBuilder*)                                     \
    X(WindowAcquireTexture, int, NVNwindow*, NVNsync*, int*)                                                 \
    X(SyncInitialize, NVNboolean, NVNsync*, NVNdevice*)                                                      \
    X(ProgramInitialize, NVNboolean, NVNprogram*, NVNdevice*)                                                \
    X(ProgramSetShaders, NVNboolean, NVNprogram*, int, const NVNshaderData*)                                 \
    X(SamplerBuilderSetDefaults, void, NVNsamplerBuilder*)                                                   \
    X(SamplerBuilderSetDevice, void, NVNsamplerBuilder*, NVNdevice*)                                         \
    X(SamplerBuilderSetMinMagFilter, void, NVNsamplerBuilder*, int, int)                                     \
    X(SamplerBuilderSetWrapMode, void, NVNsamplerBuilder*, int, int, int)                                    \
    X(SamplerInitialize, NVNboolean, NVNsampler*, const NVNsamplerBuilder*)                                  \
    X(TexturePoolInitialize, NVNboolean, NVNtexturePool*, const NVNmemoryPool*, ptrdiff_t, int)              \
    X(TexturePoolRegisterTexture, void, const NVNtexturePool*, int, const NVNtexture*, const NVNtextureView*) \
    X(SamplerPoolInitialize, NVNboolean, NVNsamplerPool*, const NVNmemoryPool*, ptrdiff_t, int)              \
    X(SamplerPoolRegisterSampler, void, const NVNsamplerPool*, int, const NVNsampler*)                       \
    X(VertexAttribStateSetDefaults, void, NVNvertexAttribState*)                                             \
    X(VertexAttribStateSetFormat, void, NVNvertexAttribState*, int, ptrdiff_t)                               \
    X(VertexAttribStateSetStreamIndex, void, NVNvertexAttribState*, int)                                     \
    X(VertexStreamStateSetDefaults, void, NVNvertexStreamState*)                                             \
    X(VertexStreamStateSetStride, void, NVNvertexStreamState*, ptrdiff_t)                                    \
    X(BlendStateSetDefaults, void, NVNblendState*)                                                           \
    X(BlendStateSetBlendTarget, void, NVNblendState*, int)                                                   \
    X(BlendStateSetBlendFunc, void, NVNblendState*, int, int, int, int)                                      \
    X(ColorStateSetDefaults, void, NVNcolorState*)                                                           \
    X(ColorStateSetBlendEnable, void, NVNcolorState*, int, NVNboolean)                                       \
    X(DepthStencilStateSetDefaults, void, NVNdepthStencilState*)                                             \
    X(DepthStencilStateSetDepthTestEnable, void, NVNdepthStencilState*, NVNboolean)                          \
    X(DepthStencilStateSetDepthWriteEnable, void, NVNdepthStencilState*, NVNboolean)                         \
    X(DepthStencilStateSetDepthFunc, void, NVNdepthStencilState*, int)                                       \
    X(PolygonStateSetDefaults, void, NVNpolygonState*)                                                       \
    X(PolygonStateSetCullFace, void, NVNpolygonState*, int)                                                  \
    X(PolygonStateSetFrontFace, void, NVNpolygonState*, int)                                                 \
    X(ChannelMaskStateSetDefaults, void, NVNchannelMaskState*)                                               \
    X(MultisampleStateSetDefaults, void, NVNmultisampleState*)                                               \
    X(CommandBufferInitialize, NVNboolean, NVNcommandBuffer*, NVNdevice*)                                    \
    X(CommandBufferBeginRecording, void, NVNcommandBuffer*)                                                  \
    X(CommandBufferEndRecording, NVNcommandHandle, NVNcommandBuffer*)                                        \
    X(CommandBufferSetRenderTargets, void, NVNcommandBuffer*, int, const NVNtexture* const*,                 \
      const NVNtextureView* const*, const NVNtexture*, const NVNtextureView*)                                \
    X(CommandBufferClearColor, void, NVNcommandBuffer*, int, const float*, int)                              \
    X(CommandBufferClearDepthStencil, void, NVNcommandBuffer*, float, NVNboolean, int, int)                  \
    X(CommandBufferBindProgram, void, NVNcommandBuffer*, const NVNprogram*, int)                             \
    X(CommandBufferBindVertexAttribState, void, NVNcommandBuffer*, int, const NVNvertexAttribState*)         \
    X(CommandBufferBindVertexStreamState, void, NVNcommandBuffer*, int, const NVNvertexStreamState*)         \
    X(CommandBufferBindVertexBuffer, void, NVNcommandBuffer*, int, NVNbufferAddress, size_t)                 \
    X(CommandBufferBindUniformBuffer, void, NVNcommandBuffer*, int, int, NVNbufferAddress, size_t)           \
    X(CommandBufferSetTexturePool, void, NVNcommandBuffer*, const NVNtexturePool*)                           \
    X(CommandBufferSetSamplerPool, void, NVNcommandBuffer*, const NVNsamplerPool*)                           \
    X(CommandBufferBindTexture, void, NVNcommandBuffer*, int, int, NVNtextureHandle)                         \
    X(CommandBufferBindBlendState, void, NVNcommandBuffer*, const NVNblendState*)                            \
    X(CommandBufferBindColorState, void, NVNcommandBuffer*, const NVNcolorState*)                            \
    X(CommandBufferBindChannelMaskState, void, NVNcommandBuffer*, const NVNchannelMaskState*)                \
    X(CommandBufferBindDepthStencilState, void, NVNcommandBuffer*, const NVNdepthStencilState*)              \
    X(CommandBufferBindPolygonState, void, NVNcommandBuffer*, const NVNpolygonState*)                        \
    X(CommandBufferBindMultisampleState, void, NVNcommandBuffer*, const NVNmultisampleState*)                \
    X(CommandBufferCopyBufferToTexture, void, NVNcommandBuffer*, NVNbufferAddress, const NVNtexture*,        \
      const NVNtextureView*, const NVNcopyRegion*, int)                                                      \
    X(CommandBufferDrawArrays, void, NVNcommandBuffer*, int, int, int)                                       \
    X(CommandBufferDrawElements, void, NVNcommandBuffer*, int, int, int, NVNbufferAddress)                   \
    X(CommandBufferBindStorageBuffer, void, NVNcommandBuffer*, int, int, NVNbufferAddress, size_t)          \
    X(CommandBufferBindStorageBuffers, void, NVNcommandBuffer*, int, int, int, const NVNbufferRange*)       \
    X(CommandBufferDispatchCompute, void, NVNcommandBuffer*, int, int, int)

#define DECLARE(name, ret, ...) static ret (*pfnc_nvn##name)(__VA_ARGS__);
NVN_PROCS(DECLARE)
#undef DECLARE

// Values as the HLE's abi.hpp has them (UNVERIFIED there).
enum {
    kFormatRGBA8 = 37, kFormatRGBA32F = 46, kFormatRG32F = 22, kFormatDepth24Stencil8 = 53, kTarget2D = 1,
    kClearMaskRGBA = 0xF,
    kStageVertex = 0, kStageFragment = 1, kStageCompute = 5, kStageBitsGraphics = 0x1F, kStageBitCompute = 0x20,
    kPrimTriangles = 4, kIndexUnsignedShort = 1,
    kFuncLess = 2, kFaceBack = 2, kFrontFaceCcw = 1,
    kBlendZero = 1, kBlendOne = 2, kBlendSrcAlpha = 5, kBlendOneMinusSrcAlpha = 6,
    kMinNearest = 0, kMagNearest = 0, kWrapClampToEdge = 7,
};
enum { kSize = 64 };

static NVNdevice g_device;
static NVNqueue g_queue;
static NVNmemoryPool g_pool;
static NVNtexture g_target;
static NVNtexture g_depth;
static NVNtexture g_white;
static NVNtexture g_checker;
static NVNwindow g_window;
static NVNcommandBuffer g_cmd;
static NVNsync g_sync;
static NVNprogram g_program;
static NVNprogram g_cs_memory;
static NVNprogram g_cs_lockstep;
static NVNsampler g_sampler;
static NVNtexturePool g_texture_pool;
static NVNsamplerPool g_sampler_pool;
// One pool for everything the "GPU" reads: textures, shader code, vertex,
// index and uniform data (GPU address = guest address in the HLE).
alignas(4096) static uint8_t g_pool_storage[0x48000];

extern "C" {
__attribute__((used)) int g_step;
}

// Pool layout.
enum : uint32_t {
    kOffTextures = 0,           // targets and textures, laid out by GetStorageSize
    kOffShaders = 0x30000,      // vertex at +0, fragment at +0x1000
    kOffUniforms = 0x32000,     // c0 of the vertex shader
    kOffVertices = 0x33000,
    kOffIndices = 0x36000,
    kOffChecker = 0x37000,      // linear source of the checker texture
    kOffDescriptors = 0x38000,  // texture and sampler pool memory
    kOffCompute = 0x40000,      // cs_memory at +0, cs_lockstep at +0x1000
    kOffStorage = 0x42000,      // storage buffers of the dispatches
};
// Storage buffers (offsets from kOffStorage).
enum : uint32_t {
    kMemIn = 0, kMemOut = 0x200, kMemCnt = 0x600, kMemOut2 = 0x800,   // cs_memory, 128 threads
    kLsOut = 0x1800, kLsBytes = 0x1A00, kLsCnt = 0x1A40,             // cs_lockstep, 32 threads
};
enum { kThreads = 128 };

// Control section of a compute stage, NVN's layout (gpu/nvn abi.hpp): byte
// offsets of the fields the HLE reads; everything else stays 0.
enum : uint32_t {
    kControlMagic = 0x98761234,
    kCtlBlobSize = 0x704, kCtlEntryOffset = 0x708, kCtlStage = 0x714, kCtlWorkgroup = 0x748,
    kCtlSharedMemory = 0x754, kCtlEnd = 0x768,
    kCtlStageCompute = 5, kComputeEntry = 0x100,
};
struct TestControl {
    uint32_t words[kCtlEnd / 4];
    void set(uint32_t offset, uint32_t value) { words[offset / 4] = value; }
};
static TestControl g_control_memory, g_control_lockstep;

static uint8_t* pool(uint32_t off) { return g_pool_storage + off; }
static NVNbufferAddress gpu(uint32_t off) { return pfnc_nvnMemoryPoolGetBufferAddress(&g_pool) + off; }

static uint32_t* storage(uint32_t off) { return reinterpret_cast<uint32_t*>(pool(kOffStorage + off)); }
static uint32_t input(uint32_t i) { return (i * 2654435761u) ^ (i << 13); }
static int32_t s32(uint32_t v) { return static_cast<int32_t>(v); }

static int set_compute(NVNprogram* program, TestControl* control, const unsigned info[4], uint32_t off,
                       const unsigned char* code, size_t size) {
    // Filler the HLE must skip (no valid Maxwell code), then the code.
    memset(pool(off), 0xFF, kComputeEntry);
    memcpy(pool(off + kComputeEntry), code, size);
    control->set(0, kControlMagic);
    control->set(kCtlBlobSize, kComputeEntry + uint32_t(size));
    control->set(kCtlEntryOffset, kComputeEntry);
    control->set(kCtlStage, kCtlStageCompute);
    for (int i = 0; i < 3; ++i) control->set(kCtlWorkgroup + 4 * i, info[i]);
    control->set(kCtlSharedMemory, info[3]);
    if (!pfnc_nvnProgramInitialize(program, &g_device)) return 0;
    const NVNshaderData stage = {gpu(off), control};
    return pfnc_nvnProgramSetShaders(program, 1, &stage);
}

// Expected results, as tests/gpu/render/render_test.py has them.
static int check_compute() {
    const uint32_t* out = storage(kMemOut);
    for (uint32_t gid = 0; gid < kThreads; ++gid) {
        const uint32_t cta = gid / 64, tid = gid % 64;
        if (out[2 * gid] != input(cta * 64 + 63 - tid) || out[2 * gid + 1] != 64) return 40;
    }
    uint32_t sum = 0, umin = 0xFFFFFFFFu;
    int32_t smax = s32(0x80000000u), smin = 0x7FFFFFFF;
    for (uint32_t i = 0; i < kThreads; ++i) {
        const uint32_t v = input(i);
        sum += v;
        umin = v < umin ? v : umin;
        smax = s32(v) > smax ? s32(v) : smax;
        smin = s32(v) < smin ? s32(v) : smin;
    }
    const uint32_t* cnt = storage(kMemCnt);
    if (cnt[0] != sum || cnt[1] != kThreads || s32(cnt[2]) != smax || s32(cnt[3]) != smin || cnt[4] != umin)
        return 41;
    const uint32_t* out2 = storage(kMemOut2);
    for (uint32_t gid = 0; gid < kThreads; ++gid) {
        const uint32_t lo = input(gid & ~1u), hi = input(gid | 1u);
        const uint64_t v = (uint64_t{hi} << 32) | lo;
        const uint32_t want[4] = {uint32_t(v >> 4), uint32_t(s32(hi) >> 4), (hi << 4) | (lo >> 28),
                                  ((gid % 64) << 3) + gid};
        for (int k = 0; k < 4; ++k)
            if (out2[4 * gid + k] != want[k]) return 42;
    }
    const uint32_t* ls = storage(kLsOut);
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(storage(kLsBytes));
    for (uint32_t tid = 0; tid < 32; ++tid) {
        const uint8_t b = uint8_t(5 * tid + 0x70);
        const uint32_t want[4] = {6 * ((tid + 1) % 24 + 1), tid, uint32_t(int32_t(int8_t(b))), 13};
        for (int k = 0; k < 4; ++k)
            if (ls[4 * tid + k] != (tid < 24 ? want[k] : 0xEEEEEEEEu)) return 43;
        if (bytes[tid] != (tid < 24 ? b : 0xEE)) return 44;
    }
    const uint32_t* lc = storage(kLsCnt);
    if (lc[0] != 24 % 7 || lc[1] != uint32_t(-10)) return 45;
    return 0;
}

static int load_procs(PFNBOOTSTRAP bootstrap) {
    PFNGETPROC get_proc = reinterpret_cast<PFNGETPROC>(bootstrap("nvnDeviceGetProcAddress"));
    if (get_proc == nullptr) return 1;
#define LOAD(name, ret, ...) \
    pfnc_nvn##name = reinterpret_cast<ret (*)(__VA_ARGS__)>(get_proc(nullptr, "nvn" #name));
    NVN_PROCS(LOAD)
#undef LOAD
#define CHECK_LOADED(name, ret, ...) \
    if (pfnc_nvn##name == nullptr) return 2;
    NVN_PROCS(CHECK_LOADED)
#undef CHECK_LOADED
    return 0;
}

static size_t g_tex_offset = kOffTextures;
static int make_texture(NVNtexture* t, int format, int w, int h) {
    NVNtextureBuilder tb;
    pfnc_nvnTextureBuilderSetDefaults(&tb);
    pfnc_nvnTextureBuilderSetDevice(&tb, &g_device);
    pfnc_nvnTextureBuilderSetTarget(&tb, kTarget2D);
    pfnc_nvnTextureBuilderSetFormat(&tb, format);
    pfnc_nvnTextureBuilderSetSize2D(&tb, w, h);
    const size_t size = (pfnc_nvnTextureBuilderGetStorageSize(&tb) + 0xFFF) & ~size_t{0xFFF};
    if (g_tex_offset + size > kOffShaders) return 0;
    pfnc_nvnTextureBuilderSetStorage(&tb, &g_pool, static_cast<ptrdiff_t>(g_tex_offset));
    g_tex_offset += size;
    return pfnc_nvnTextureInitialize(t, &tb);
}

// Six vertices (two CCW triangles, or CW with cw = true): position, colour, uv.
static float* quad(float* v, float x0, float y0, float x1, float y1, float z, const float rgba[4], bool cw) {
    const float corners[6][4] = {{x0, y0, 0, 1}, {x1, y0, 1, 1}, {x0, y1, 0, 0},
                                 {x0, y1, 0, 0}, {x1, y0, 1, 1}, {x1, y1, 1, 0}};
    static const int ccw_order[6] = {0, 1, 2, 3, 4, 5};
    static const int cw_order[6] = {0, 2, 1, 3, 5, 4};
    for (int i = 0; i < 6; ++i) {
        const float* c = corners[cw ? cw_order[i] : ccw_order[i]];
        *v++ = c[0], *v++ = c[1], *v++ = z, *v++ = 1.0f;
        for (int k = 0; k < 4; ++k) *v++ = rgba[k];
        *v++ = c[2], *v++ = c[3];
    }
    return v;
}

extern "C" int guest_main(PFNBOOTSTRAP bootstrap) {
    g_step = 1;
    if (int r = load_procs(bootstrap)) return r;

    NVNdeviceBuilder db;
    pfnc_nvnDeviceBuilderSetDefaults(&db);
    if (!pfnc_nvnDeviceInitialize(&g_device, &db)) return 3;
    NVNqueueBuilder qb;
    pfnc_nvnQueueBuilderSetDefaults(&qb);
    pfnc_nvnQueueBuilderSetDevice(&qb, &g_device);
    if (!pfnc_nvnQueueInitialize(&g_queue, &qb)) return 4;
    NVNmemoryPoolBuilder mb;
    pfnc_nvnMemoryPoolBuilderSetDefaults(&mb);
    pfnc_nvnMemoryPoolBuilderSetDevice(&mb, &g_device);
    pfnc_nvnMemoryPoolBuilderSetFlags(&mb, 0x8);
    pfnc_nvnMemoryPoolBuilderSetStorage(&mb, g_pool_storage, sizeof(g_pool_storage));
    if (!pfnc_nvnMemoryPoolInitialize(&g_pool, &mb)) return 5;

    // Render target, depth buffer, a white 1x1 and a 2x2 checker texture.
    if (!make_texture(&g_target, kFormatRGBA8, kSize, kSize)) return 6;
    if (!make_texture(&g_depth, kFormatDepth24Stencil8, kSize, kSize)) return 7;
    if (!make_texture(&g_white, kFormatRGBA8, 1, 1)) return 8;
    if (!make_texture(&g_checker, kFormatRGBA8, 2, 2)) return 9;
    static const uint8_t white[4] = {255, 255, 255, 255};
    const NVNcopyRegion one = {0, 0, 0, 1, 1, 1};
    pfnc_nvnTextureWriteTexels(&g_white, nullptr, &one, white);
    static const uint8_t checker[16] = {255, 0, 0, 255, 0, 255, 0, 255,      // row 0: red, green
                                        0, 0, 255, 255, 255, 255, 255, 255};  // row 1: blue, white
    memcpy(pool(kOffChecker), checker, sizeof(checker));

    NVNwindowBuilder wb;
    NVNtexture* const window_textures[1] = {&g_target};
    pfnc_nvnWindowBuilderSetDefaults(&wb);
    pfnc_nvnWindowBuilderSetDevice(&wb, &g_device);
    pfnc_nvnWindowBuilderSetTextures(&wb, 1, window_textures);
    if (!pfnc_nvnWindowInitialize(&g_window, &wb)) return 10;
    if (!pfnc_nvnSyncInitialize(&g_sync, &g_device)) return 11;

    // Program: the vertex stage carries BNSH's NVN code header, the fragment
    // stage starts directly with its SPH.
    g_step = 12;
    memcpy(pool(kOffShaders), kVsTransform, sizeof(kVsTransform));
    memcpy(pool(kOffShaders + 0x1000), kFsTextured, sizeof(kFsTextured));
    if (!pfnc_nvnProgramInitialize(&g_program, &g_device)) return 12;
    const NVNshaderData stages[2] = {{gpu(kOffShaders), nullptr}, {gpu(kOffShaders + 0x1000), nullptr}};
    if (!pfnc_nvnProgramSetShaders(&g_program, 2, stages)) return 13;

    // Sampler and pools: texture ids 256/257, sampler id 300 (past the
    // reserved descriptors, as games do).
    NVNsamplerBuilder sb;
    pfnc_nvnSamplerBuilderSetDefaults(&sb);
    pfnc_nvnSamplerBuilderSetDevice(&sb, &g_device);
    pfnc_nvnSamplerBuilderSetMinMagFilter(&sb, kMinNearest, kMagNearest);
    pfnc_nvnSamplerBuilderSetWrapMode(&sb, kWrapClampToEdge, kWrapClampToEdge, kWrapClampToEdge);
    if (!pfnc_nvnSamplerInitialize(&g_sampler, &sb)) return 14;
    if (!pfnc_nvnTexturePoolInitialize(&g_texture_pool, &g_pool, kOffDescriptors, 512)) return 15;
    if (!pfnc_nvnSamplerPoolInitialize(&g_sampler_pool, &g_pool, kOffDescriptors + 0x4000, 512)) return 16;
    pfnc_nvnTexturePoolRegisterTexture(&g_texture_pool, 256, &g_white, nullptr);
    pfnc_nvnTexturePoolRegisterTexture(&g_texture_pool, 257, &g_checker, nullptr);
    pfnc_nvnSamplerPoolRegisterSampler(&g_sampler_pool, 300, &g_sampler);
    const NVNtextureHandle h_white = pfnc_nvnDeviceGetTextureHandle(&g_device, 256, 300);
    const NVNtextureHandle h_checker = pfnc_nvnDeviceGetTextureHandle(&g_device, 257, 300);

    // Vertex format: position RGBA32F, colour RGBA32F, uv RG32F, stride 40.
    NVNvertexAttribState attribs[3];
    const int formats[3] = {kFormatRGBA32F, kFormatRGBA32F, kFormatRG32F};
    const int offsets[3] = {0, 16, 32};
    for (int i = 0; i < 3; ++i) {
        pfnc_nvnVertexAttribStateSetDefaults(&attribs[i]);
        pfnc_nvnVertexAttribStateSetFormat(&attribs[i], formats[i], offsets[i]);
        pfnc_nvnVertexAttribStateSetStreamIndex(&attribs[i], 0);
    }
    NVNvertexStreamState stream;
    pfnc_nvnVertexStreamStateSetDefaults(&stream);
    pfnc_nvnVertexStreamStateSetStride(&stream, 40);

    // c0: identity matrix (column-major).
    float* m = reinterpret_cast<float*>(pool(kOffUniforms));
    for (int i = 0; i < 16; ++i) m[i] = i % 5 == 0 ? 1.0f : 0.0f;

    static const float green[4] = {0, 1, 0, 1}, red[4] = {1, 0, 0, 1}, blue[4] = {0, 0, 1, 1};
    static const float half[4] = {1, 1, 1, 0.5f};
    float* v = reinterpret_cast<float*>(pool(kOffVertices));
    v = quad(v, -1.0f, 0.0f, 0.0f, 1.0f, 0.5f, green, false);  // vertices 0..5
    v = quad(v, -0.5f, 0.0f, 0.5f, 1.0f, 0.75f, red, false);   // 6..11
    v = quad(v, -1.0f, -1.0f, 0.0f, 0.0f, 0.25f, blue, true);  // 12..17, clockwise
    v = quad(v, 0.5f, 0.5f, 1.0f, 1.0f, 0.0f, half, false);    // 18..23
    // Bottom-right textured quad: four corners (24..27), indexed.
    const float tq[4][10] = {{0, -1, 0.5f, 1, 1, 1, 1, 1, 0, 1},
                             {1, -1, 0.5f, 1, 1, 1, 1, 1, 1, 1},
                             {0, 0, 0.5f, 1, 1, 1, 1, 1, 0, 0},
                             {1, 0, 0.5f, 1, 1, 1, 1, 1, 1, 0}};
    memcpy(v, tq, sizeof(tq));
    uint16_t* idx = reinterpret_cast<uint16_t*>(pool(kOffIndices));
    const uint16_t indices[6] = {0, 1, 2, 2, 1, 3};
    memcpy(idx, indices, sizeof(indices));

    // Fixed-function state objects.
    NVNdepthStencilState depth_on, depth_off;
    pfnc_nvnDepthStencilStateSetDefaults(&depth_on);
    pfnc_nvnDepthStencilStateSetDepthTestEnable(&depth_on, 1);
    pfnc_nvnDepthStencilStateSetDepthWriteEnable(&depth_on, 1);
    pfnc_nvnDepthStencilStateSetDepthFunc(&depth_on, kFuncLess);
    pfnc_nvnDepthStencilStateSetDefaults(&depth_off);
    NVNpolygonState cull_back;
    pfnc_nvnPolygonStateSetDefaults(&cull_back);
    pfnc_nvnPolygonStateSetCullFace(&cull_back, kFaceBack);
    pfnc_nvnPolygonStateSetFrontFace(&cull_back, kFrontFaceCcw);
    NVNblendState alpha_blend;
    pfnc_nvnBlendStateSetDefaults(&alpha_blend);
    pfnc_nvnBlendStateSetBlendTarget(&alpha_blend, 0);
    pfnc_nvnBlendStateSetBlendFunc(&alpha_blend, kBlendSrcAlpha, kBlendOneMinusSrcAlpha, kBlendOne, kBlendZero);
    NVNcolorState no_blend, blend_on;
    pfnc_nvnColorStateSetDefaults(&no_blend);
    pfnc_nvnColorStateSetDefaults(&blend_on);
    pfnc_nvnColorStateSetBlendEnable(&blend_on, 0, 1);
    NVNchannelMaskState all_channels;
    pfnc_nvnChannelMaskStateSetDefaults(&all_channels);
    NVNmultisampleState ms;
    pfnc_nvnMultisampleStateSetDefaults(&ms);

    // Record the frame.
    g_step = 20;
    if (!pfnc_nvnCommandBufferInitialize(&g_cmd, &g_device)) return 20;
    int index = -1;
    if (pfnc_nvnWindowAcquireTexture(&g_window, &g_sync, &index) != 0 || index != 0) return 21;

    pfnc_nvnCommandBufferBeginRecording(&g_cmd);
    const NVNcopyRegion checker_region = {0, 0, 0, 2, 2, 1};
    pfnc_nvnCommandBufferCopyBufferToTexture(&g_cmd, gpu(kOffChecker), &g_checker, nullptr, &checker_region, 0);
    const NVNtexture* target = &g_target;
    pfnc_nvnCommandBufferSetRenderTargets(&g_cmd, 1, &target, nullptr, &g_depth, nullptr);
    static const float grey[4] = {0.2f, 0.2f, 0.2f, 1.0f};
    pfnc_nvnCommandBufferClearColor(&g_cmd, 0, grey, kClearMaskRGBA);
    pfnc_nvnCommandBufferClearDepthStencil(&g_cmd, 1.0f, 1, 0, 0xFF);

    pfnc_nvnCommandBufferBindProgram(&g_cmd, &g_program, kStageBitsGraphics);
    pfnc_nvnCommandBufferBindVertexAttribState(&g_cmd, 3, attribs);
    pfnc_nvnCommandBufferBindVertexStreamState(&g_cmd, 1, &stream);
    pfnc_nvnCommandBufferBindVertexBuffer(&g_cmd, 0, gpu(kOffVertices), 28 * 40);
    pfnc_nvnCommandBufferBindUniformBuffer(&g_cmd, kStageVertex, 0, gpu(kOffUniforms), 64);
    pfnc_nvnCommandBufferSetTexturePool(&g_cmd, &g_texture_pool);
    pfnc_nvnCommandBufferSetSamplerPool(&g_cmd, &g_sampler_pool);
    pfnc_nvnCommandBufferBindTexture(&g_cmd, kStageFragment, 0, h_white);
    pfnc_nvnCommandBufferBindChannelMaskState(&g_cmd, &all_channels);
    pfnc_nvnCommandBufferBindMultisampleState(&g_cmd, &ms);
    pfnc_nvnCommandBufferBindColorState(&g_cmd, &no_blend);
    pfnc_nvnCommandBufferBindDepthStencilState(&g_cmd, &depth_on);
    pfnc_nvnCommandBufferBindPolygonState(&g_cmd, &cull_back);
    pfnc_nvnCommandBufferDrawArrays(&g_cmd, kPrimTriangles, 0, 6);    // green, z 0.5
    pfnc_nvnCommandBufferDrawArrays(&g_cmd, kPrimTriangles, 6, 6);    // red, z 0.75: half hidden
    pfnc_nvnCommandBufferDrawArrays(&g_cmd, kPrimTriangles, 12, 6);   // blue, clockwise: culled
    pfnc_nvnCommandBufferBindTexture(&g_cmd, kStageFragment, 0, h_checker);
    pfnc_nvnCommandBufferBindVertexBuffer(&g_cmd, 0, gpu(kOffVertices) + 24 * 40, 4 * 40);
    pfnc_nvnCommandBufferDrawElements(&g_cmd, kPrimTriangles, kIndexUnsignedShort, 6, gpu(kOffIndices));
    pfnc_nvnCommandBufferBindVertexBuffer(&g_cmd, 0, gpu(kOffVertices), 28 * 40);
    pfnc_nvnCommandBufferBindTexture(&g_cmd, kStageFragment, 0, h_white);
    pfnc_nvnCommandBufferBindDepthStencilState(&g_cmd, &depth_off);
    pfnc_nvnCommandBufferBindBlendState(&g_cmd, &alpha_blend);
    pfnc_nvnCommandBufferBindColorState(&g_cmd, &blend_on);
    pfnc_nvnCommandBufferDrawArrays(&g_cmd, kPrimTriangles, 18, 6);   // white, alpha 0.5, blended
    const NVNcommandHandle handle = pfnc_nvnCommandBufferEndRecording(&g_cmd);
    if (handle == 0) return 22;

    pfnc_nvnQueueSubmitCommands(&g_queue, 1, &handle);
    pfnc_nvnQueuePresentTexture(&g_queue, &g_window, index);
    pfnc_nvnQueueFinish(&g_queue);

    // Compute.
    g_step = 30;
    if (!set_compute(&g_cs_memory, &g_control_memory, kCsMemoryInfo, kOffCompute, kCsMemory, sizeof(kCsMemory)))
        return 30;
    if (!set_compute(&g_cs_lockstep, &g_control_lockstep, kCsLockstepInfo, kOffCompute + 0x1000, kCsLockstep,
                     sizeof(kCsLockstep)))
        return 31;
    // Inputs; the counters start where min/max can only move one way.
    for (uint32_t i = 0; i < kThreads; ++i) storage(kMemIn)[i] = input(i);
    const uint32_t counters[5] = {0, 0, 0x80000000u, 0x7FFFFFFFu, 0xFFFFFFFFu};
    memcpy(storage(kMemCnt), counters, sizeof(counters));
    memset(storage(kLsOut), 0xEE, 0x200);
    memset(storage(kLsBytes), 0xEE, 32);
    const uint32_t ls_counters[2] = {0, 0x7FFFFFFFu};
    memcpy(storage(kLsCnt), ls_counters, sizeof(ls_counters));

    pfnc_nvnCommandBufferBeginRecording(&g_cmd);
    // cs_memory: storage buffers 0..3 = input, output, counters, output 2.
    const NVNbufferRange ranges[4] = {{gpu(kOffStorage + kMemIn), kThreads * 4},
                                      {gpu(kOffStorage + kMemOut), kThreads * 8},
                                      {gpu(kOffStorage + kMemCnt), 20},
                                      {gpu(kOffStorage + kMemOut2), kThreads * 16}};
    pfnc_nvnCommandBufferBindProgram(&g_cmd, &g_cs_memory, kStageBitCompute);
    pfnc_nvnCommandBufferBindStorageBuffers(&g_cmd, kStageCompute, 0, 4, ranges);
    pfnc_nvnCommandBufferDispatchCompute(&g_cmd, 2, 1, 1);
    // cs_lockstep: output, bytes, counters.
    pfnc_nvnCommandBufferBindProgram(&g_cmd, &g_cs_lockstep, kStageBitCompute);
    pfnc_nvnCommandBufferBindStorageBuffer(&g_cmd, kStageCompute, 0, gpu(kOffStorage + kLsOut), 0x200);
    pfnc_nvnCommandBufferBindStorageBuffer(&g_cmd, kStageCompute, 1, gpu(kOffStorage + kLsBytes), 32);
    pfnc_nvnCommandBufferBindStorageBuffer(&g_cmd, kStageCompute, 2, gpu(kOffStorage + kLsCnt), 8);
    pfnc_nvnCommandBufferBindStorageBuffer(&g_cmd, kStageCompute, 3, 0, 0);
    pfnc_nvnCommandBufferDispatchCompute(&g_cmd, 1, 1, 1);
    const NVNcommandHandle compute = pfnc_nvnCommandBufferEndRecording(&g_cmd);
    if (compute == 0) return 32;
    pfnc_nvnQueueSubmitCommands(&g_queue, 1, &compute);
    pfnc_nvnQueueFinish(&g_queue);
    g_step = 40;
    if (int r = check_compute()) return r;

    g_step = 0;
    return 0;
}
