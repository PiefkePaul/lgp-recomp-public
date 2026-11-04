// Guest test program for the NVN-HLE (P4.1): what a game does to get a
// cleared frame on screen, built for AArch64 with the host clang, packed as
// NRO and recompiled with lgp_lift. Own code, no NVN header: the function
// types below are written from the public C API shape (names as the proc
// loader asks for them), object storage sizes as the HLE assumes them
// (gpu/nvn/include/lgp/gpu/nvn/abi.hpp).
//
// guest_main(bootstrap): x0 is the address of nvnBootstrapLoader (on the
// console the import resolved through the GOT). Returns 0 on success,
// otherwise the number of the first failed step.
#include <stddef.h>
#include <stdint.h>

typedef uint8_t NVNboolean;
typedef uint64_t NVNcommandHandle;
typedef uint64_t NVNbufferAddress;

// Opaque storage, sizes >= what the HLE needs inline.
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

typedef void (*PFNGENERIC)(void);
typedef PFNGENERIC (*PFNBOOTSTRAP)(const char*);
typedef PFNGENERIC (*PFNGETPROC)(const NVNdevice*, const char*);

// The slots a game's proc loader fills (one global per function).
#define NVN_PROCS(X)                                                                                         \
    X(DeviceBuilderSetDefaults, void, NVNdeviceBuilder*)                                                     \
    X(DeviceBuilderSetFlags, void, NVNdeviceBuilder*, int)                                                   \
    X(DeviceInitialize, NVNboolean, NVNdevice*, const NVNdeviceBuilder*)                                     \
    X(DeviceGetInteger, void, const NVNdevice*, int, int*)                                                   \
    X(DeviceGetTextureHandle, uint64_t, const NVNdevice*, int, int)                                          \
    X(DeviceFinalizeCommandHandle, void, NVNdevice*, NVNcommandHandle)                                       \
    X(QueueBuilderSetDefaults, void, NVNqueueBuilder*)                                                       \
    X(QueueBuilderSetDevice, void, NVNqueueBuilder*, NVNdevice*)                                             \
    X(QueueInitialize, NVNboolean, NVNqueue*, const NVNqueueBuilder*)                                        \
    X(QueueSubmitCommands, void, NVNqueue*, int, const NVNcommandHandle*)                                    \
    X(QueuePresentTexture, void, NVNqueue*, NVNwindow*, int)                                                 \
    X(QueueFinish, void, NVNqueue*)                                                                          \
    X(QueueFenceSync, void, NVNqueue*, NVNsync*, int, int)                                                   \
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
    X(TextureBuilderGetStorageAlignment, size_t, const NVNtextureBuilder*)                                   \
    X(TextureBuilderSetStorage, void, NVNtextureBuilder*, NVNmemoryPool*, ptrdiff_t)                         \
    X(TextureInitialize, NVNboolean, NVNtexture*, const NVNtextureBuilder*)                                  \
    X(TextureGetWidth, int, const NVNtexture*)                                                               \
    X(WindowBuilderSetDefaults, void, NVNwindowBuilder*)                                                     \
    X(WindowBuilderSetDevice, void, NVNwindowBuilder*, NVNdevice*)                                           \
    X(WindowBuilderSetTextures, void, NVNwindowBuilder*, int, NVNtexture* const*)                            \
    X(WindowInitialize, NVNboolean, NVNwindow*, const NVNwindowBuilder*)                                     \
    X(WindowAcquireTexture, int, NVNwindow*, NVNsync*, int*)                                                 \
    X(SyncInitialize, NVNboolean, NVNsync*, NVNdevice*)                                                      \
    X(SyncWait, int, const NVNsync*, uint64_t)                                                               \
    X(CommandBufferInitialize, NVNboolean, NVNcommandBuffer*, NVNdevice*)                                    \
    X(CommandBufferAddCommandMemory, void, NVNcommandBuffer*, const NVNmemoryPool*, ptrdiff_t, size_t)       \
    X(CommandBufferAddControlMemory, void, NVNcommandBuffer*, void*, size_t)                                 \
    X(CommandBufferBeginRecording, void, NVNcommandBuffer*)                                                  \
    X(CommandBufferEndRecording, NVNcommandHandle, NVNcommandBuffer*)                                        \
    X(CommandBufferSetRenderTargets, void, NVNcommandBuffer*, int, const NVNtexture* const*,                 \
      const NVNtextureView* const*, const NVNtexture*, const NVNtextureView*)                                \
    X(CommandBufferSetScissor, void, NVNcommandBuffer*, int, int, int, int)                                  \
    X(CommandBufferSetDepthRange, void, NVNcommandBuffer*, float, float)                                     \
    X(CommandBufferClearColor, void, NVNcommandBuffer*, int, const float*, int)                              \
    X(CommandBufferClearDepthStencil, void, NVNcommandBuffer*, float, NVNboolean, int, int)

#define DECLARE(name, ret, ...) static ret (*pfnc_nvn##name)(__VA_ARGS__);
NVN_PROCS(DECLARE)
#undef DECLARE

// Values as the HLE's abi.hpp has them (UNVERIFIED there).
enum { kFormatRGBA8 = 37, kTarget2D = 1, kClearMaskRGBA = 0xF, kTexDescriptorSize = 15 };
enum { kWidth = 64, kHeight = 32 };

static NVNdevice g_device;
static NVNqueue g_queue;
static NVNmemoryPool g_pool;
static NVNtexture g_textures[2];
static NVNwindow g_window;
static NVNcommandBuffer g_cmd;
static NVNsync g_sync;
alignas(4096) static uint8_t g_pool_storage[0x10000];
alignas(8) static uint8_t g_control[0x400];

// Read by the host after the run.
extern "C" {
__attribute__((used)) int g_acquired[3];
__attribute__((used)) int g_descriptor_size;
__attribute__((used)) uint64_t g_handle;
}

static const float kBlue[4] = {0.25f, 0.5f, 1.0f, 1.0f};
static const float kRed[4] = {1.0f, 0.0f, 0.0f, 1.0f};

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

// One frame: acquire, clear (blue, then a red scissored corner), present.
static int frame(int n) {
    int index = -1;
    if (pfnc_nvnWindowAcquireTexture(&g_window, &g_sync, &index) != 0) return 20;
    if (pfnc_nvnSyncWait(&g_sync, ~0ull) != 0) return 21;
    g_acquired[n] = index;

    pfnc_nvnCommandBufferBeginRecording(&g_cmd);
    const NVNtexture* target = &g_textures[index];
    pfnc_nvnCommandBufferSetRenderTargets(&g_cmd, 1, &target, nullptr, nullptr, nullptr);
    pfnc_nvnCommandBufferSetDepthRange(&g_cmd, 0.0f, 1.0f);
    pfnc_nvnCommandBufferSetScissor(&g_cmd, 0, 0, kWidth, kHeight);
    pfnc_nvnCommandBufferClearColor(&g_cmd, 0, kBlue, kClearMaskRGBA);
    pfnc_nvnCommandBufferClearDepthStencil(&g_cmd, 1.0f, 1, 0, 0xFF);
    pfnc_nvnCommandBufferSetScissor(&g_cmd, 0, 0, 8 << n, 4);  // grows per frame
    pfnc_nvnCommandBufferClearColor(&g_cmd, 0, kRed, kClearMaskRGBA);
    const NVNcommandHandle handle = pfnc_nvnCommandBufferEndRecording(&g_cmd);
    if (handle == 0) return 22;
    g_handle = handle;

    pfnc_nvnQueueSubmitCommands(&g_queue, 1, &handle);
    pfnc_nvnQueueFenceSync(&g_queue, &g_sync, 0, 0);
    pfnc_nvnQueuePresentTexture(&g_queue, &g_window, index);
    pfnc_nvnQueueFinish(&g_queue);
    pfnc_nvnDeviceFinalizeCommandHandle(&g_device, handle);
    return 0;
}

extern "C" int guest_main(PFNBOOTSTRAP bootstrap) {
    if (int r = load_procs(bootstrap)) return r;

    NVNdeviceBuilder db;
    pfnc_nvnDeviceBuilderSetDefaults(&db);
    pfnc_nvnDeviceBuilderSetFlags(&db, 0);
    if (!pfnc_nvnDeviceInitialize(&g_device, &db)) return 3;
    pfnc_nvnDeviceGetInteger(&g_device, kTexDescriptorSize, &g_descriptor_size);

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
    if (pfnc_nvnMemoryPoolGetBufferAddress(&g_pool) == 0) return 6;

    NVNtextureBuilder tb;
    pfnc_nvnTextureBuilderSetDefaults(&tb);
    pfnc_nvnTextureBuilderSetDevice(&tb, &g_device);
    pfnc_nvnTextureBuilderSetTarget(&tb, kTarget2D);
    pfnc_nvnTextureBuilderSetFormat(&tb, kFormatRGBA8);
    pfnc_nvnTextureBuilderSetSize2D(&tb, kWidth, kHeight);
    const size_t align = pfnc_nvnTextureBuilderGetStorageAlignment(&tb);
    size_t size = pfnc_nvnTextureBuilderGetStorageSize(&tb);
    size = (size + align - 1) / align * align;
    if (2 * size > sizeof(g_pool_storage)) return 7;
    for (int i = 0; i < 2; ++i) {
        pfnc_nvnTextureBuilderSetStorage(&tb, &g_pool, static_cast<ptrdiff_t>(i * size));
        if (!pfnc_nvnTextureInitialize(&g_textures[i], &tb)) return 8;
        if (pfnc_nvnTextureGetWidth(&g_textures[i]) != kWidth) return 9;
    }

    NVNwindowBuilder wb;
    NVNtexture* const window_textures[2] = {&g_textures[0], &g_textures[1]};
    pfnc_nvnWindowBuilderSetDefaults(&wb);
    pfnc_nvnWindowBuilderSetDevice(&wb, &g_device);
    pfnc_nvnWindowBuilderSetTextures(&wb, 2, window_textures);
    if (!pfnc_nvnWindowInitialize(&g_window, &wb)) return 10;
    if (!pfnc_nvnSyncInitialize(&g_sync, &g_device)) return 11;

    if (!pfnc_nvnCommandBufferInitialize(&g_cmd, &g_device)) return 12;
    pfnc_nvnCommandBufferAddCommandMemory(&g_cmd, &g_pool, static_cast<ptrdiff_t>(2 * size), 0x1000);
    pfnc_nvnCommandBufferAddControlMemory(&g_cmd, g_control, sizeof(g_control));

    for (int n = 0; n < 3; ++n)
        if (int r = frame(n)) return r;
    return 0;
}
