// NVN-HLE (P4.1) driven through the guest ABI: every NVN call goes through
// the dispatch table to a thunk with arguments in CpuState registers and
// guest memory, the way a BLR from lifted code reaches it. The guest program
// test (test_nvn_guest.cpp) does the same from real lifted AArch64 code.
// Runs natively and in Node.
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <set>
#include <string>

#include "../runtime/check.hpp"
#include "lgp/gpu/nvn/hle.hpp"
#include "lgp/lifted/cpu_state.hpp"
#include "lgp/platform/null_backends.hpp"
#include "lgp/runtime/guest.hpp"
#include "lgp/runtime/memory.hpp"

namespace nvn = lgp::gpu::nvn;
using namespace lgp::rt;

namespace {

constexpr uint64_t kMiB = 0x10'0000;
constexpr uint64_t kBase = 0x0800'0000;
constexpr uint64_t kThunks = kBase + 0x8'0000;  // inside the code region, nothing mapped there

struct Guest {
    GuestMemory mem{Layout::packed(kBase, 1 * kMiB, 1 * kMiB, 1 * kMiB, 1 * kMiB, 8 * kMiB)};
    uint64_t next = 0;
    uint32_t last_halt = 0;
    uint64_t last_halt_info = 0;

    Guest() {
        const Region heap = mem.layout().heap;
        CHECK(mem.map(heap.base, heap.size, MemState::Normal, kPermRW).ok());
        next = heap.base;
    }

    uint64_t alloc(uint64_t size, uint64_t align = 16) {
        next = (next + align - 1) & ~(align - 1);
        const uint64_t va = next;
        next += size;
        return va;
    }
    uint64_t str(const char* s) {
        const uint64_t va = alloc(std::strlen(s) + 1);
        std::memcpy(mem.host_ptr(va, std::strlen(s) + 1), s, std::strlen(s) + 1);
        return va;
    }
    template <typename T>
    uint64_t put(const T& v) {
        const uint64_t va = alloc(sizeof(T), 8);
        std::memcpy(mem.host_ptr(va, sizeof(T)), &v, sizeof(T));
        return va;
    }
    template <typename T>
    void set(uint64_t va, const T& v) {
        std::memcpy(mem.host_ptr(va, sizeof(T)), &v, sizeof(T));
    }
    template <typename T>
    T get(uint64_t va) {
        T v;
        std::memcpy(&v, mem.host_ptr(va, sizeof(T)), sizeof(T));
        return v;
    }

    // BLR to `fn` with integer arguments in x0.. and float arguments in s0..
    uint64_t call(uint64_t fn, std::initializer_list<uint64_t> x, std::initializer_list<float> s = {}) {
        lgp::CpuState cpu{};
        int i = 0;
        for (uint64_t v : x) cpu.x[i++] = v;
        i = 0;
        for (float f : s) {
            uint32_t bits;
            std::memcpy(&bits, &f, 4);
            cpu.vreg[i++].lo = bits;
        }
        const lgp::LiftedFn target = lookup_function(fn);
        CHECK(target != nullptr);
        if (target == nullptr) return 0;
        run_lifted(&cpu, target);
        last_halt = cpu.halt;
        last_halt_info = cpu.halt_info;
        return cpu.x[0];
    }
};

// What a game does first: bootstrap -> nvnDeviceGetProcAddress -> procs.
struct Procs {
    Guest& g;
    uint64_t get_proc = 0;
    explicit Procs(Guest& guest, nvn::Hle& hle) : g(guest) {
        get_proc = g.call(hle.bootstrap_loader_va(), {g.str("nvnDeviceGetProcAddress")});
    }
    uint64_t operator()(const char* name) { return g.call(get_proc, {0, g.str(name)}); }
};

constexpr uint32_t kRGBA8 = static_cast<uint32_t>(nvn::Format::RGBA8);
constexpr uint32_t kRGBA8_SRGB = static_cast<uint32_t>(nvn::Format::RGBA8_SRGB);
constexpr uint32_t kTarget2D = static_cast<uint32_t>(nvn::TextureTarget::Target2D);

void test_proc_lookup() {
    Guest g;
    lgp::platform::NullRenderer renderer;
    nvn::SoftBackend backend(renderer);
    nvn::Hle hle({kThunks}, backend);
    hle.install();
    CHECK_EQ(hle.bootstrap_loader_va(), kThunks);
    Procs proc(g, hle);
    CHECK(proc.get_proc != 0);
    CHECK_EQ(proc.get_proc, hle.proc_address("nvnDeviceGetProcAddress"));
    const uint64_t init = proc("nvnDeviceInitialize");
    CHECK(init != 0);
    CHECK_EQ(proc("nvnDeviceInitialize"), init);  // same name, same slot
    CHECK(proc("nvnCommandBufferInsertDebugMarker") != 0);  // unimplemented still gets a thunk
    CHECK_EQ(proc("glClear"), 0u);                    // not an NVN name
    CHECK_EQ(g.call(hle.bootstrap_loader_va(), {g.str("")}), 0u);
    // Bootstrap answers other NVN names, too.
    CHECK_EQ(g.call(hle.bootstrap_loader_va(), {g.str("nvnDeviceInitialize")}), init);

    // sdk's export can be redirected to the bootstrap thunk.
    hle.replace_bootstrap_loader(kBase + 0x1000);
    CHECK_EQ(g.call(kBase + 0x1000, {g.str("nvnDeviceGetProcAddress")}), proc.get_proc);

    bool seen_draw = false;
    for (const nvn::ProcStats& s : hle.stats())
        if (s.name == "nvnCommandBufferInsertDebugMarker") seen_draw = !s.implemented && s.calls == 0;
    CHECK(seen_draw);
    CHECK(nvn::Hle::is_implemented("nvnCommandBufferClearColor"));
    CHECK(!nvn::Hle::is_implemented("nvnCommandBufferInsertDebugMarker"));
    CHECK(nvn::Hle::implemented_count() >= 100);
    reset_dispatch();
}

// The clear path of ADR-0003's abort criterion: Device, Queue, MemoryPool,
// TextureBuilder/Texture, Window and command-buffer recording.
struct ClearSetup {
    Guest& g;
    Procs p;
    uint64_t device = 0, queue = 0, pool = 0, window = 0, cmd = 0;
    uint64_t tex[2] = {};
    uint64_t tex_builder = 0;

    ClearSetup(Guest& guest, nvn::Hle& hle, uint32_t format, uint32_t width, uint32_t height)
        : g(guest), p(guest, hle) {
        const uint64_t db = g.alloc(nvn::kDeviceBuilderSize);
        device = g.alloc(0x3000);
        g.call(p("nvnDeviceBuilderSetDefaults"), {db});
        g.call(p("nvnDeviceBuilderSetFlags"), {db, 0});
        CHECK_EQ(g.call(p("nvnDeviceInitialize"), {device, db}), 1u);

        const uint64_t qb = g.alloc(nvn::kQueueBuilderSize);
        queue = g.alloc(0x2000);
        g.call(p("nvnQueueBuilderSetDefaults"), {qb});
        g.call(p("nvnQueueBuilderSetDevice"), {qb, device});
        CHECK_EQ(g.call(p("nvnQueueInitialize"), {queue, qb}), 1u);

        // Texture storage first: the game asks for size and alignment.
        tex_builder = g.alloc(nvn::kTextureBuilderSize);
        g.call(p("nvnTextureBuilderSetDefaults"), {tex_builder});
        g.call(p("nvnTextureBuilderSetDevice"), {tex_builder, device});
        g.call(p("nvnTextureBuilderSetTarget"), {tex_builder, kTarget2D});
        g.call(p("nvnTextureBuilderSetFormat"), {tex_builder, format});
        g.call(p("nvnTextureBuilderSetSize2D"), {tex_builder, width, height});
        const uint64_t size = g.call(p("nvnTextureBuilderGetStorageSize"), {tex_builder});
        const uint64_t align = g.call(p("nvnTextureBuilderGetStorageAlignment"), {tex_builder});
        CHECK(size >= uint64_t{width} * height * 4);
        CHECK(align != 0 && size % align == 0);

        const uint64_t pool_size = (2 * size + 0xFFF) & ~uint64_t{0xFFF};
        const uint64_t storage = g.alloc(pool_size, 0x1000);
        const uint64_t mb = g.alloc(nvn::kMemoryPoolBuilderSize);
        pool = g.alloc(0x100);
        g.call(p("nvnMemoryPoolBuilderSetDefaults"), {mb});
        g.call(p("nvnMemoryPoolBuilderSetDevice"), {mb, device});
        g.call(p("nvnMemoryPoolBuilderSetFlags"), {mb, 0x8});
        g.call(p("nvnMemoryPoolBuilderSetStorage"), {mb, storage, pool_size});
        CHECK_EQ(g.call(p("nvnMemoryPoolInitialize"), {pool, mb}), 1u);
        CHECK_EQ(g.call(p("nvnMemoryPoolGetBufferAddress"), {pool}), storage);  // identity GPU address
        CHECK_EQ(g.call(p("nvnMemoryPoolMap"), {pool}), storage);

        for (int i = 0; i < 2; ++i) {
            tex[i] = g.alloc(0xC0);
            g.call(p("nvnTextureBuilderSetStorage"), {tex_builder, pool, i * size});
            CHECK_EQ(g.call(p("nvnTextureInitialize"), {tex[i], tex_builder}), 1u);
            CHECK_EQ(g.call(p("nvnTextureGetWidth"), {tex[i]}), width);
            CHECK_EQ(g.call(p("nvnTextureGetHeight"), {tex[i]}), height);
            CHECK_EQ(g.call(p("nvnTextureGetFormat"), {tex[i]}), format);
        }

        const uint64_t wb = g.alloc(nvn::kWindowBuilderSize);
        window = g.alloc(0x180);
        const uint64_t list = g.alloc(16, 8);
        std::memcpy(g.mem.host_ptr(list, 16), tex, 16);
        g.call(p("nvnWindowBuilderSetDefaults"), {wb});
        g.call(p("nvnWindowBuilderSetDevice"), {wb, device});
        g.call(p("nvnWindowBuilderSetNativeWindow"), {wb, 0x1234});
        g.call(p("nvnWindowBuilderSetTextures"), {wb, 2, list});
        CHECK_EQ(g.call(p("nvnWindowInitialize"), {window, wb}), 1u);
        CHECK_EQ(g.call(p("nvnWindowGetNativeWindow"), {window}), 0x1234u);

        cmd = g.alloc(0xA0);
        CHECK_EQ(g.call(p("nvnCommandBufferInitialize"), {cmd, device}), 1u);
        g.call(p("nvnCommandBufferAddCommandMemory"), {cmd, pool, 0, 0x1000});
        g.call(p("nvnCommandBufferAddControlMemory"), {cmd, g.alloc(0x400), 0x400});
    }

    // Acquire, record clears, submit, present. Returns the acquired index.
    int frame(const float rgba[4], const nvn::Rect* scissor, const float* second_rgba) {
        const uint64_t sync = g.alloc(0x40);
        CHECK_EQ(g.call(p("nvnSyncInitialize"), {sync, device}), 1u);
        const uint64_t index_va = g.alloc(4);
        CHECK_EQ(g.call(p("nvnWindowAcquireTexture"), {window, sync, index_va}), 0u);
        const int index = g.get<int32_t>(index_va);
        CHECK_EQ(g.call(p("nvnSyncWait"), {sync, ~0ull}), static_cast<uint64_t>(nvn::SyncWaitResult::AlreadySignaled));

        g.call(p("nvnCommandBufferBeginRecording"), {cmd});
        CHECK_EQ(g.call(p("nvnCommandBufferIsRecording"), {cmd}), 1u);
        const uint64_t targets = g.put(tex[index]);
        g.call(p("nvnCommandBufferSetRenderTargets"), {cmd, 1, targets, 0, 0, 0});
        g.call(p("nvnCommandBufferSetScissor"), {cmd, 0, 0, 1u << 20, 1u << 20});
        const uint64_t color = g.alloc(16);
        std::memcpy(g.mem.host_ptr(color, 16), rgba, 16);
        g.call(p("nvnCommandBufferClearColor"), {cmd, 0, color, nvn::kClearColorMaskRGBA});
        g.call(p("nvnCommandBufferClearDepthStencil"), {cmd, 1, 0, 0xFF}, {1.0f});
        if (scissor != nullptr) {
            g.call(p("nvnCommandBufferSetScissor"),
                   {cmd, uint64_t(scissor->x), uint64_t(scissor->y), uint64_t(scissor->width), uint64_t(scissor->height)});
            const uint64_t c2 = g.alloc(16);
            std::memcpy(g.mem.host_ptr(c2, 16), second_rgba, 16);
            g.call(p("nvnCommandBufferClearColor"), {cmd, 0, c2, nvn::kClearColorMaskRGBA});
        }
        const uint64_t handle = g.call(p("nvnCommandBufferEndRecording"), {cmd});
        CHECK(handle != 0);
        CHECK_EQ(g.call(p("nvnCommandBufferIsRecording"), {cmd}), 0u);
        g.call(p("nvnQueueSubmitCommands"), {queue, 1, g.put(handle)});
        g.call(p("nvnQueuePresentTexture"), {queue, window, uint64_t(index)});
        g.call(p("nvnQueueFinish"), {queue});
        g.call(p("nvnDeviceFinalizeCommandHandle"), {device, handle});
        g.call(p("nvnSyncFinalize"), {sync});
        return index;
    }
};

bool pixel_is(const std::vector<uint8_t>& img, uint32_t width, uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b,
              uint8_t a) {
    const uint8_t* p = img.data() + (size_t(y) * width + x) * 4;
    return p[0] == r && p[1] == g && p[2] == b && p[3] == a;
}

void test_clear_and_present() {
    Guest g;
    lgp::platform::NullRenderer renderer({1280, 720});
    nvn::SoftBackend backend(renderer);
    nvn::Hle hle({kThunks}, backend);
    hle.install();
    ClearSetup s(g, hle, kRGBA8, 64, 32);
    CHECK_EQ(g.last_halt, 0u);

    const float blue[4] = {0.25f, 0.5f, 1.0f, 1.0f};
    CHECK_EQ(s.frame(blue, nullptr, nullptr), 0);
    lgp::platform::SurfaceSize size;
    std::vector<uint8_t> img = renderer.last_image(&size);
    CHECK_EQ(size.width, 64u);
    CHECK_EQ(size.height, 32u);
    CHECK_EQ(renderer.frames(), 1u);
    CHECK(pixel_is(img, 64, 0, 0, 64, 128, 255, 255));
    CHECK(pixel_is(img, 64, 63, 31, 64, 128, 255, 255));

    // Second frame goes to the other texture; a scissored clear in NVN's
    // default lower-left origin lands in the bottom rows of the image.
    const float black[4] = {0, 0, 0, 1};
    const float red[4] = {1, 0, 0, 1};
    const nvn::Rect bottom_left{0, 0, 16, 8};
    CHECK_EQ(s.frame(black, &bottom_left, red), 1);
    img = renderer.last_image(&size);
    CHECK(pixel_is(img, 64, 0, 31, 255, 0, 0, 255));   // bottom-left corner: red
    CHECK(pixel_is(img, 64, 15, 24, 255, 0, 0, 255));
    CHECK(pixel_is(img, 64, 16, 31, 0, 0, 0, 255));    // right of the scissor
    CHECK(pixel_is(img, 64, 0, 23, 0, 0, 0, 255));     // above it
    CHECK(pixel_is(img, 64, 0, 0, 0, 0, 0, 255));

    // The crop is in window coordinates too: with the lower-left origin,
    // (0, 0, 16, 8) shows the bottom-left corner.
    g.call(s.p("nvnWindowSetCrop"), {s.window, 0, 0, 16, 8});
    s.frame(black, &bottom_left, red);
    img = renderer.last_image(&size);
    CHECK_EQ(size.width, 16u);
    CHECK_EQ(size.height, 8u);
    CHECK(pixel_is(img, 16, 0, 0, 255, 0, 0, 255));
    CHECK(pixel_is(img, 16, 15, 7, 255, 0, 0, 255));
    g.call(s.p("nvnWindowSetCrop"), {s.window, 0, 0, 0, 0});

    // Upper-left origin: the same scissor is the top-left corner.
    g.call(s.p("nvnDeviceSetWindowOriginMode"), {s.device, uint64_t(nvn::WindowOriginMode::UpperLeft)});
    CHECK_EQ(g.call(s.p("nvnDeviceGetWindowOriginMode"), {s.device}), uint64_t(nvn::WindowOriginMode::UpperLeft));
    CHECK_EQ(s.frame(black, &bottom_left, red), 1);
    img = renderer.last_image(&size);
    CHECK(pixel_is(img, 64, 0, 0, 255, 0, 0, 255));
    CHECK(pixel_is(img, 64, 0, 31, 0, 0, 0, 255));

    // Crop: present only a part of the texture.
    g.call(s.p("nvnWindowSetCrop"), {s.window, 0, 0, 16, 8});
    s.frame(black, &bottom_left, red);
    img = renderer.last_image(&size);
    CHECK_EQ(size.width, 16u);
    CHECK_EQ(size.height, 8u);
    CHECK(pixel_is(img, 16, 15, 7, 255, 0, 0, 255));

    const nvn::SoftBackend::Stats st = backend.stats();
    CHECK_EQ(st.presents, 5u);
    CHECK_EQ(st.clears, 9u);
    CHECK_EQ(st.skipped_commands, 5u);  // the depth clears (no depth buffer on the CPU path)
    CHECK(hle.unimplemented_calls().empty());
    reset_dispatch();
}

void test_srgb_and_mask() {
    Guest g;
    lgp::platform::NullRenderer renderer;
    nvn::SoftBackend backend(renderer);
    nvn::Hle hle({kThunks}, backend);
    hle.install();
    ClearSetup s(g, hle, kRGBA8_SRGB, 8, 8);
    const float grey[4] = {0.5f, 0.5f, 0.5f, 0.5f};
    s.frame(grey, nullptr, nullptr);
    lgp::platform::SurfaceSize size;
    std::vector<uint8_t> img = renderer.last_image(&size);
    // linear 0.5 -> sRGB 0.7354 -> 188; alpha stays linear (128).
    CHECK(pixel_is(img, 8, 3, 3, 188, 188, 188, 128));
    reset_dispatch();
}

void test_texture_queries_and_builder_copy() {
    Guest g;
    lgp::platform::NullRenderer renderer;
    nvn::SoftBackend backend(renderer);
    nvn::Hle hle({kThunks}, backend);
    hle.install();
    Procs p(g, hle);
    const uint64_t tb = g.alloc(nvn::kTextureBuilderSize);
    g.call(p("nvnTextureBuilderSetDefaults"), {tb});
    g.call(p("nvnTextureBuilderSetTarget"), {tb, kTarget2D});
    g.call(p("nvnTextureBuilderSetFormat"), {tb, kRGBA8});
    g.call(p("nvnTextureBuilderSetWidth"), {tb, 100});
    g.call(p("nvnTextureBuilderSetHeight"), {tb, 50});
    g.call(p("nvnTextureBuilderSetLevels"), {tb, 3});
    // The game copies builders by value; the copy must keep the state.
    const uint64_t copy = g.alloc(nvn::kTextureBuilderSize);
    std::memcpy(g.mem.host_ptr(copy, nvn::kTextureBuilderSize), g.mem.host_ptr(tb, nvn::kTextureBuilderSize),
                nvn::kTextureBuilderSize);
    const uint64_t tex = g.alloc(0xC0);
    CHECK_EQ(g.call(p("nvnTextureInitialize"), {tex, copy}), 1u);
    CHECK_EQ(g.call(p("nvnTextureGetWidth"), {tex}), 100u);
    CHECK_EQ(g.call(p("nvnTextureGetHeight"), {tex}), 50u);
    CHECK_EQ(g.call(p("nvnTextureGetLevels"), {tex}), 3u);
    CHECK_EQ(g.call(p("nvnTextureGetTarget"), {tex}), kTarget2D);
    // Block-linear estimate: level 0 = 448 B rows x 64 rows, level 1 = 256 x 32, level 2 = 128 x 16 (16-row blocks)
    CHECK_EQ(g.call(p("nvnTextureBuilderGetStorageSize"), {copy}), 448u * 64 + 256u * 32 + 128u * 16);

    // TextureView: setters/getters with the "was set" flag.
    const uint64_t view = g.alloc(nvn::kTextureViewSize);
    g.call(p("nvnTextureViewSetDefaults"), {view});
    const uint64_t a = g.alloc(4), b = g.alloc(4);
    CHECK_EQ(g.call(p("nvnTextureViewGetLevels"), {view, a, b}), 0u);
    g.call(p("nvnTextureViewSetLevels"), {view, 1, 2});
    CHECK_EQ(g.call(p("nvnTextureViewGetLevels"), {view, a, b}), 1u);
    CHECK_EQ(g.get<int32_t>(a), 1);
    CHECK_EQ(g.get<int32_t>(b), 2);

    // Device: bindless handles and integer queries.
    const uint64_t db = g.alloc(nvn::kDeviceBuilderSize), dev = g.alloc(0x3000);
    g.call(p("nvnDeviceBuilderSetDefaults"), {db});
    CHECK_EQ(g.call(p("nvnDeviceInitialize"), {dev, db}), 1u);
    CHECK_EQ(g.call(p("nvnDeviceGetTextureHandle"), {dev, 0x123, 0x45}), (uint64_t{0x45} << 20) | 0x123);
    CHECK_EQ(g.call(p("nvnDeviceGetTexelFetchHandle"), {dev, 7}), 7u);
    const uint64_t out = g.alloc(4);
    g.call(p("nvnDeviceGetInteger"), {dev, 15, out});  // TEXTURE_DESCRIPTOR_SIZE
    CHECK_EQ(g.get<int32_t>(out), 32);
    g.call(p("nvnDeviceGetInteger"), {dev, 9999, out});  // unknown key -> 0, logged
    CHECK_EQ(g.get<int32_t>(out), 0);
    CHECK_EQ(g.last_halt, 0u);
    reset_dispatch();
}

void test_overrides() {
    Guest g;
    lgp::platform::NullRenderer renderer;
    nvn::SoftBackend backend(renderer);
    nvn::HleOptions o{kThunks};
    o.device_integers.push_back({15, 64, "TEXTURE_DESCRIPTOR_SIZE"});
    nvn::Hle hle(o, backend);
    hle.install();
    Procs p(g, hle);
    const uint64_t out = g.alloc(4);
    g.call(p("nvnDeviceGetInteger"), {0, 15, out});
    CHECK_EQ(g.get<int32_t>(out), 64);  // the option wins over the built-in table
    g.call(p("nvnDeviceGetInteger"), {0, 0, out});
    CHECK_EQ(g.get<int32_t>(out), 53);  // API major version of the measured driver
    reset_dispatch();
}

void test_unimplemented() {
    {
        Guest g;
        lgp::platform::NullRenderer renderer;
        nvn::SoftBackend backend(renderer);
        nvn::Hle hle({kThunks}, backend);
        hle.install();
        Procs p(g, hle);
        const uint64_t draw = p("nvnCommandBufferInsertDebugMarker");
        g.call(draw, {0x1000, 4, 0, 3});
        CHECK_EQ(g.last_halt, static_cast<uint32_t>(lgp::kHaltUndefined));
        CHECK_EQ(g.last_halt_info, draw);
        CHECK(hle.unimplemented_calls() == std::vector<std::string>{"nvnCommandBufferInsertDebugMarker"});
        reset_dispatch();
    }
    {
        // Continue mode: returns 0, and recording commands keep a placeholder.
        Guest g;
        lgp::platform::NullRenderer renderer;
        nvn::SoftBackend backend(renderer);
        nvn::HleOptions o{kThunks};
        o.unimplemented = nvn::UnimplementedMode::Continue;
        nvn::Hle hle(o, backend);
        hle.install();
        ClearSetup s(g, hle, kRGBA8, 4, 4);
        g.call(s.p("nvnCommandBufferBeginRecording"), {s.cmd});
        g.call(s.p("nvnCommandBufferInsertDebugMarker"), {s.cmd, 4, 0, 3});
        CHECK_EQ(g.last_halt, 0u);
        CHECK_EQ(g.call(s.p("nvnDeviceInstallDebugCallback"), {0x1000}), 0u);
        CHECK_EQ(g.last_halt, 0u);
        const uint64_t handle = g.call(s.p("nvnCommandBufferEndRecording"), {s.cmd});
        g.call(s.p("nvnQueueSubmitCommands"), {s.queue, 1, g.put(handle)});
        CHECK_EQ(backend.stats().skipped_commands, 1u);
        CHECK_EQ(hle.unimplemented_calls().size(), 2u);
        reset_dispatch();
    }
}

// Records what the HLE hands to a backend (P4.1 part 2).
class CaptureBackend final : public nvn::Backend {
public:
    std::vector<nvn::Command> commands;
    std::vector<nvn::ProgramDesc> programs;
    std::vector<std::pair<nvn::TextureId, std::vector<uint8_t>>> uploads;
    nvn::Rect upload_region;
    uint32_t upload_stride = 0;
    nvn::TexelSlices upload_slices;
    void create_program(nvn::ProgramId, const nvn::ProgramDesc& desc) override { programs.push_back(desc); }
    std::set<nvn::TextureId> live_textures;
    void create_texture(nvn::TextureId id, const nvn::TextureDesc&) override { live_textures.insert(id); }
    void destroy_texture(nvn::TextureId id) override { live_textures.erase(id); }
    void write_texture(nvn::TextureId id, uint32_t, const nvn::Rect& region, const void* data,
                       uint32_t row_stride, const nvn::TexelSlices& slices = {}) override {
        upload_slices = slices;
        const auto* p = static_cast<const uint8_t*>(data);
        uploads.emplace_back(id, std::vector<uint8_t>(p, p + size_t(row_stride) * (region.height - 1) + 8));
        upload_region = region;
        upload_stride = row_stride;
    }
    void execute(const nvn::CommandList& list) override {
        commands.insert(commands.end(), list.commands.begin(), list.commands.end());
    }
    bool present(const nvn::PresentInfo&) override { return true; }

    template <typename T>
    std::vector<T> all() const {
        std::vector<T> out;
        for (const nvn::Command& c : commands)
            if (auto* v = std::get_if<T>(&c)) out.push_back(*v);
        return out;
    }
};

// NVN enum values as abi.hpp has them (UNVERIFIED there).
template <typename E>
constexpr uint64_t e(E v) {
    return static_cast<uint64_t>(v);
}

// State objects keep a 4-byte reference in guest storage: the HLE must not
// touch bytes behind it, copies made by the game must stay valid, and the
// Bind* commands must turn them into the backend model.
void test_state_objects() {
    Guest g;
    CaptureBackend backend;
    nvn::Hle hle({kThunks}, backend);
    hle.install();
    ClearSetup s(g, hle, kRGBA8, 8, 8);
    auto& p = s.p;

    // Footprint: canary bytes behind the first four stay untouched.
    const char* types[] = {"BlendState", "ColorState", "ChannelMaskState", "DepthStencilState",
                           "MultisampleState", "PolygonState", "VertexAttribState", "VertexStreamState",
                           "SamplerBuilder"};
    for (const char* t : types) {
        const uint64_t obj = g.alloc(16, 8);
        std::memset(g.mem.host_ptr(obj, 16), 0xCD, 16);
        g.call(p((std::string("nvn") + t + "SetDefaults").c_str()), {obj});
        bool untouched = true;
        for (int i = 4; i < 16; ++i) untouched &= g.get<uint8_t>(obj + i) == 0xCD;
        CHECK(untouched);
        CHECK(g.get<uint32_t>(obj) != 0xCDCDCDCDu);
    }

    // Depth/stencil: set up, copy by value, change the original.
    const uint64_t ds = g.alloc(8, 8), ds_copy = g.alloc(8, 8);
    g.call(p("nvnDepthStencilStateSetDefaults"), {ds});
    g.call(p("nvnDepthStencilStateSetDepthTestEnable"), {ds, 1});
    g.call(p("nvnDepthStencilStateSetDepthFunc"), {ds, e(nvn::CompareFuncNvn::Greater)});
    g.call(p("nvnDepthStencilStateSetStencilTestEnable"), {ds, 1});
    g.call(p("nvnDepthStencilStateSetStencilOp"),
           {ds, e(nvn::FaceNvn::Back), e(nvn::StencilOpNvn::Zero), e(nvn::StencilOpNvn::Invert),
            e(nvn::StencilOpNvn::Replace)});
    std::memcpy(g.mem.host_ptr(ds_copy, 8), g.mem.host_ptr(ds, 8), 8);
    g.call(p("nvnDepthStencilStateSetDepthWriteEnable"), {ds, 0});

    const uint64_t blend = g.alloc(8, 8), color = g.alloc(8, 8), mask = g.alloc(8, 8), poly = g.alloc(8, 8);
    g.call(p("nvnBlendStateSetDefaults"), {blend});
    g.call(p("nvnBlendStateSetBlendTarget"), {blend, 1});
    g.call(p("nvnBlendStateSetBlendFunc"),
           {blend, e(nvn::BlendFuncNvn::SrcAlpha), e(nvn::BlendFuncNvn::OneMinusSrcAlpha), e(nvn::BlendFuncNvn::One),
            e(nvn::BlendFuncNvn::Zero)});
    g.call(p("nvnBlendStateSetBlendEquation"), {blend, e(nvn::BlendEquationNvn::Add), e(nvn::BlendEquationNvn::Max)});
    g.call(p("nvnColorStateSetDefaults"), {color});
    g.call(p("nvnColorStateSetBlendEnable"), {color, 1, 1});
    g.call(p("nvnColorStateSetBlendEnable"), {color, 3, 0xFFFFFF01});  // NVNboolean: only the low byte counts
    g.call(p("nvnChannelMaskStateSetDefaults"), {mask});
    g.call(p("nvnChannelMaskStateSetChannelMask"), {mask, 0, 1, 0, 0, 1});
    g.call(p("nvnPolygonStateSetDefaults"), {poly});
    g.call(p("nvnPolygonStateSetCullFace"), {poly, e(nvn::FaceNvn::Back)});
    g.call(p("nvnPolygonStateSetFrontFace"), {poly, e(nvn::FrontFaceNvn::Cw)});

    // Vertex layout: an array of attribute states (stride = object size).
    const uint64_t attribs = g.alloc(5 * nvn::kVertexAttribStateSize, 8);
    for (int i = 0; i < 5; ++i) g.call(p("nvnVertexAttribStateSetDefaults"), {attribs + i * nvn::kVertexAttribStateSize});
    g.call(p("nvnVertexAttribStateSetFormat"), {attribs, nvn::kFormatRGB32F, 0});
    g.call(p("nvnVertexAttribStateSetFormat"), {attribs + 2 * nvn::kVertexAttribStateSize, kRGBA8, 12});
    g.call(p("nvnVertexAttribStateSetStreamIndex"), {attribs + 2 * nvn::kVertexAttribStateSize, 1});
    g.call(p("nvnVertexAttribStateSetFormat"), {attribs + 3 * nvn::kVertexAttribStateSize, 39, 12});  // RGBA8UI
    g.call(p("nvnVertexAttribStateSetFormat"), {attribs + 4 * nvn::kVertexAttribStateSize, 105, 0});  // RGB10A2SN
    const uint64_t streams = g.alloc(2 * nvn::kVertexStreamStateSize, 8);
    for (int i = 0; i < 2; ++i) {
        g.call(p("nvnVertexStreamStateSetDefaults"), {streams + i * nvn::kVertexStreamStateSize});
        g.call(p("nvnVertexStreamStateSetStride"), {streams + i * nvn::kVertexStreamStateSize, uint64_t(16 + 8 * i)});
    }

    // Sampler and texture pools.
    const uint64_t sb = g.alloc(8, 8), sampler = g.alloc(0x60), tpool = g.alloc(0x20), spool = g.alloc(0x20);
    g.call(p("nvnSamplerBuilderSetDefaults"), {sb});
    g.call(p("nvnSamplerBuilderSetMinMagFilter"), {sb, e(nvn::MinFilterNvn::Nearest), e(nvn::MagFilterNvn::Nearest)});
    g.call(p("nvnSamplerBuilderSetWrapMode"),
           {sb, e(nvn::WrapModeNvn::ClampToEdge), e(nvn::WrapModeNvn::MirroredRepeat), e(nvn::WrapModeNvn::Repeat)});
    CHECK_EQ(g.call(p("nvnSamplerInitialize"), {sampler, sb}), 1u);
    CHECK_EQ(g.call(p("nvnTexturePoolInitialize"), {tpool, s.pool, 0, 64}), 1u);
    CHECK_EQ(g.call(p("nvnSamplerPoolInitialize"), {spool, s.pool, 0x800, 16}), 1u);
    CHECK_EQ(g.call(p("nvnTexturePoolGetSize"), {tpool}), 64u);
    // The view's swizzle (here "white with alpha = red", as LGP uses for
    // single-channel UI textures) travels with the pool entry.
    const uint64_t view = g.alloc(nvn::kTextureViewSize, 8);
    g.call(p("nvnTextureViewSetDefaults"), {view});
    g.call(p("nvnTextureViewSetSwizzle"), {view, 1, 1, 1, 2});
    g.call(p("nvnTexturePoolRegisterTexture"), {tpool, 5, s.tex[1], view});
    g.call(p("nvnSamplerPoolRegisterSampler"), {spool, 2, sampler});
    const uint64_t handle = g.call(p("nvnDeviceGetTextureHandle"), {s.device, 5, 2});

    g.call(p("nvnCommandBufferBeginRecording"), {s.cmd});
    g.call(p("nvnCommandBufferBindDepthStencilState"), {s.cmd, ds_copy});
    g.call(p("nvnCommandBufferBindDepthStencilState"), {s.cmd, ds});
    g.call(p("nvnCommandBufferBindBlendState"), {s.cmd, blend});
    g.call(p("nvnCommandBufferBindColorState"), {s.cmd, color});
    g.call(p("nvnCommandBufferBindChannelMaskState"), {s.cmd, mask});
    g.call(p("nvnCommandBufferBindPolygonState"), {s.cmd, poly});
    g.call(p("nvnCommandBufferBindVertexAttribState"), {s.cmd, 5, attribs});
    g.call(p("nvnCommandBufferBindVertexStreamState"), {s.cmd, 2, streams});
    const uint64_t ranges = g.alloc(32, 8);
    const uint64_t r[4] = {0x1000, 0x40, 0x2000, 0x80};
    std::memcpy(g.mem.host_ptr(ranges, 32), r, 32);
    g.call(p("nvnCommandBufferBindVertexBuffers"), {s.cmd, 0, 2, ranges});
    g.call(p("nvnCommandBufferBindUniformBuffer"), {s.cmd, e(nvn::ShaderStageNvn::Fragment), 3, 0x3000, 0x100});
    g.call(p("nvnCommandBufferSetTexturePool"), {s.cmd, tpool});
    g.call(p("nvnCommandBufferSetSamplerPool"), {s.cmd, spool});
    g.call(p("nvnCommandBufferBindTexture"), {s.cmd, e(nvn::ShaderStageNvn::Fragment), 3, handle});
    g.call(p("nvnCommandBufferDrawArrays"), {s.cmd, e(nvn::DrawPrimitive::TriangleStrip), 4, 6});
    g.call(p("nvnCommandBufferDrawElementsInstanced"),
           {s.cmd, e(nvn::DrawPrimitive::Triangles), e(nvn::IndexTypeNvn::UnsignedShort), 36, 0x4000, uint64_t(-2), 1,
            5});
    g.call(p("nvnCommandBufferDrawArrays"), {s.cmd, e(nvn::DrawPrimitive::Quads), 0, 4});  // skipped
    // Compute recording: storage buffers (single and ranges), direct and indirect dispatch.
    const uint32_t cs = e(nvn::ShaderStageNvn::Compute);
    g.call(p("nvnCommandBufferBindStorageBuffer"), {s.cmd, cs, 1, 0x5000, 0x40});
    g.call(p("nvnCommandBufferBindStorageBuffers"), {s.cmd, cs, 2, 2, ranges});
    g.call(p("nvnCommandBufferSetShaderScratchMemory"), {s.cmd, s.pool, 0, 0x1000});
    g.call(p("nvnCommandBufferDispatchCompute"), {s.cmd, 4, 2, 1});
    g.call(p("nvnCommandBufferDispatchComputeIndirect"), {s.cmd, 0x6000});
    const uint64_t list = g.call(p("nvnCommandBufferEndRecording"), {s.cmd});
    g.call(p("nvnQueueSubmitCommands"), {s.queue, 1, g.put(list)});
    CHECK_EQ(g.last_halt, 0u);

    const auto dss = backend.all<nvn::CmdSetDepthStencil>();
    CHECK_EQ(dss.size(), 2u);
    if (dss.size() == 2) {
        CHECK(dss[0].depth_test && dss[0].depth_write);  // the copy keeps the old value
        CHECK(dss[1].depth_test && !dss[1].depth_write);
        CHECK(dss[1].depth_func == nvn::CompareFunc::Greater);
        CHECK(dss[1].stencil_test);
        CHECK(dss[1].back.fail == nvn::StencilOp::Zero && dss[1].back.depth_fail == nvn::StencilOp::Invert &&
              dss[1].back.pass == nvn::StencilOp::Replace);
        CHECK(dss[1].front.pass == nvn::StencilOp::Keep);
    }
    const auto blends = backend.all<nvn::CmdSetBlendEquation>();
    CHECK_EQ(blends.size(), 1u);
    if (!blends.empty()) {
        CHECK_EQ(blends[0].target, 1u);
        CHECK(blends[0].equation.src_color == nvn::BlendFactor::SrcAlpha);
        CHECK(blends[0].equation.dst_color == nvn::BlendFactor::OneMinusSrcAlpha);
        CHECK(blends[0].equation.alpha_op == nvn::BlendOp::Max);
    }
    const auto enables = backend.all<nvn::CmdSetBlendEnables>();
    CHECK(enables.size() == 1 && enables[0].enable_mask == 0x0A);
    const auto masks = backend.all<nvn::CmdSetChannelMasks>();
    CHECK(masks.size() == 1 && masks[0].masks[0] == 0x9 && masks[0].masks[1] == 0xF);
    const auto raster = backend.all<nvn::CmdSetRaster>();
    CHECK(raster.size() == 1 && raster[0].cull == nvn::CullMode::Back && raster[0].front_face == nvn::FrontFace::Cw);
    const auto layouts = backend.all<nvn::CmdSetVertexLayout>();
    CHECK_EQ(layouts.size(), 2u);
    if (layouts.size() == 2) {
        const nvn::CmdSetVertexLayout& l = layouts[1];
        CHECK_EQ(l.attribs.size(), 4u);  // attribute 1 has no format: disabled
        if (l.attribs.size() == 4) {
            CHECK(l.attribs[0].location == 0 && l.attribs[0].format == nvn::VertexFormat::Float32x3);
            CHECK(l.attribs[1].location == 2 && l.attribs[1].format == nvn::VertexFormat::Unorm8x4 &&
                  l.attribs[1].stream == 1 && l.attribs[1].offset == 12);
            // Integer formats stay integers (bone indices): RGBA8UI -> Uint8x4.
            CHECK(l.attribs[2].location == 3 && l.attribs[2].format == nvn::VertexFormat::Uint8x4 &&
                  l.attribs[2].stream == 0 && l.attribs[2].offset == 12);
            CHECK_EQ(nvn::vertex_format_integer(l.attribs[2].format), 1);
            // Vertex-only packed format (LGP, Route 1): RGB10A2SN, unpacked in the shader.
            CHECK(l.attribs[3].location == 4 && l.attribs[3].format == nvn::VertexFormat::Snorm10_10_10_2);
            CHECK(nvn::vertex_format_input(l.attribs[3].format) == nvn::VertexInput::PackedSnorm);
        }
        CHECK(l.strides == std::vector<uint32_t>({16, 24}));
    }
    const auto vbs = backend.all<nvn::CmdBindVertexBuffer>();
    CHECK(vbs.size() == 2 && vbs[1].stream == 1 && vbs[1].address == 0x2000 && vbs[1].size == 0x80);
    const auto ubs = backend.all<nvn::CmdBindUniformBuffer>();
    CHECK(ubs.size() == 1 && ubs[0].stage == nvn::ShaderStage::Fragment && ubs[0].bank == 3 + nvn::kUniformBufferBankBase &&
          ubs[0].address == 0x3000);
    const auto texs = backend.all<nvn::CmdBindTexture>();
    CHECK_EQ(texs.size(), 1u);
    if (!texs.empty()) {
        CHECK_EQ(texs[0].slot, 3u);
        CHECK_EQ(texs[0].texture, 2u);  // s.tex[1] is the second texture created
        CHECK(texs[0].sampler.min == nvn::Filter::Nearest && texs[0].sampler.mag == nvn::Filter::Nearest);
        CHECK(texs[0].swizzle ==
              nvn::SwizzleRgba({nvn::Swizzle::One, nvn::Swizzle::One, nvn::Swizzle::One, nvn::Swizzle::R}));
        CHECK(texs[0].sampler.wrap[0] == nvn::AddressMode::ClampToEdge &&
              texs[0].sampler.wrap[1] == nvn::AddressMode::MirrorRepeat);
    }
    const auto draws = backend.all<nvn::CmdDraw>();
    CHECK(draws.size() == 1 && draws[0].topology == nvn::Topology::TriangleStrip && draws[0].first_vertex == 4 &&
          draws[0].vertex_count == 6);
    const auto indexed = backend.all<nvn::CmdDrawIndexed>();
    CHECK_EQ(indexed.size(), 1u);
    if (!indexed.empty()) {
        CHECK(indexed[0].type == nvn::IndexType::U16 && indexed[0].index_count == 36);
        CHECK(indexed[0].index_address == 0x4000 && indexed[0].base_vertex == -2);
        CHECK(indexed[0].first_instance == 1 && indexed[0].instance_count == 5);
    }

    // CPU texel upload: region and tightly packed rows.
    const uint64_t region = g.alloc(24, 8);
    const int32_t reg[6] = {1, 2, 0, 3, 2, 1};
    std::memcpy(g.mem.host_ptr(region, 24), reg, 24);
    const uint64_t texels = g.alloc(64);
    for (int i = 0; i < 24; ++i) g.mem.host_ptr(texels + i, 1)[0] = uint8_t(i);
    g.call(p("nvnTextureWriteTexels"), {s.tex[0], 0, region, texels});
    CHECK_EQ(backend.uploads.size(), 1u);
    if (!backend.uploads.empty()) {
        CHECK_EQ(backend.uploads[0].first, 1u);
        CHECK_EQ(backend.upload_stride, 12u);
        CHECK(backend.upload_region.x == 1 && backend.upload_region.y == 2 && backend.upload_region.width == 3 &&
              backend.upload_region.height == 2);
        CHECK_EQ(backend.uploads[0].second[13], 13u);
        CHECK(backend.upload_slices.z == 0 && backend.upload_slices.depth == 1);
    }
    // Slices of a 3D texture: z from region[2], depth region[5], images packed.
    const int32_t reg3[6] = {0, 0, 1, 2, 1, 2};
    std::memcpy(g.mem.host_ptr(region, 24), reg3, 24);
    g.call(p("nvnTextureWriteTexels"), {s.tex[0], 0, region, texels});
    CHECK_EQ(backend.uploads.size(), 2u);
    CHECK(backend.upload_slices.z == 1 && backend.upload_slices.depth == 2 && backend.upload_slices.image_stride == 8);

    const auto ssbos = backend.all<nvn::CmdBindStorageBuffer>();
    CHECK_EQ(ssbos.size(), 3u);
    if (ssbos.size() == 3) {
        CHECK(ssbos[0].stage == nvn::ShaderStage::Compute && ssbos[0].index == 1 && ssbos[0].address == 0x5000 &&
              ssbos[0].size == 0x40);
        CHECK(ssbos[2].index == 3 && ssbos[2].address == 0x2000 && ssbos[2].size == 0x80);
    }
    const auto dispatches = backend.all<nvn::CmdDispatchCompute>();
    CHECK_EQ(dispatches.size(), 2u);
    if (dispatches.size() == 2) {
        CHECK(dispatches[0].groups[0] == 4 && dispatches[0].groups[1] == 2 && dispatches[0].groups[2] == 1 &&
              dispatches[0].indirect_address == 0);
        CHECK_EQ(dispatches[1].indirect_address, 0x6000u);
    }

    // A compute stage (one stage without NVN code header) needs its control
    // section; without one the program is rejected.
    {
        const uint64_t cs_program = g.alloc(0x100);
        CHECK_EQ(g.call(p("nvnProgramInitialize"), {cs_program, s.device}), 1u);
        const uint64_t code = g.call(p("nvnMemoryPoolGetBufferAddress"), {s.pool});
        const uint64_t stage = g.alloc(16, 8);
        const uint64_t sd1[2] = {code, 0};
        std::memcpy(g.mem.host_ptr(stage, 16), sd1, 16);
        CHECK_EQ(g.call(p("nvnProgramSetShaders"), {cs_program, 1, stage}), 0u);
        CHECK_EQ(g.last_halt, 0u);
        CHECK_EQ(hle.untranslated_programs(), 0u);
    }

    // A program without shaders in a memory pool is rejected like a bad binary.
    const uint64_t program = g.alloc(0x100);
    CHECK_EQ(g.call(p("nvnProgramInitialize"), {program, s.device}), 1u);
    const uint64_t stages = g.alloc(32, 8);
    const uint64_t sd[4] = {0x10, 0, 0x20, 0};
    std::memcpy(g.mem.host_ptr(stages, 32), sd, 32);
    CHECK_EQ(g.call(p("nvnProgramSetShaders"), {program, 2, stages}), 0u);
    CHECK_EQ(g.last_halt, 0u);
    CHECK(hle.unimplemented_calls().empty());
    reset_dispatch();
}

// Constant buffers as compiled shaders address them: BindUniformBuffer index i
// is bank 3 + i, and BindProgram binds each stage's compiler constant pool to
// bank 1, at the offset and size the control section gives (relative to the
// NVN code header). A pool that does not fit the code is not bound.
void test_program_constants() {
    Guest g;
    CaptureBackend backend;
    nvn::HleOptions o{kThunks};
    o.program_translator = [](const std::vector<uint8_t>&, const std::vector<uint8_t>&, uint32_t,
                              nvn::ProgramDesc* d, std::string*) {
        d->vertex_wgsl = "vs";
        d->fragment_wgsl = "fs";
        return true;
    };
    nvn::Hle hle(o, backend);
    hle.install();
    ClearSetup s(g, hle, kRGBA8, 8, 8);
    auto& p = s.p;
    const uint64_t pool = g.call(p("nvnMemoryPoolGetBufferAddress"), {s.pool});
    // Two stage blobs in the pool: NVN code header, SPH, then the constants.
    const uint32_t sph_words[2] = {0x461, 0x1462};  // vertex, fragment (SPH version 3)
    const uint32_t const_offset[2] = {0x100, 0x180}, const_size[2] = {0x10, 0x8};
    uint64_t stage_data[4] = {};
    for (int st = 0; st < 2; ++st) {
        const uint64_t blob = pool + 0x200 * st;
        const uint32_t blob_size = const_offset[st] + const_size[st];
        g.set<uint32_t>(blob, nvn::kNvnCodeHeaderMagic);
        g.set<uint32_t>(blob + nvn::kNvnCodeHeaderSize, sph_words[st]);
        const uint64_t control = g.alloc(0x800, 8);
        g.set<uint32_t>(control, nvn::kNvnControlMagic);
        g.set<uint32_t>(control + nvn::kNvnControlConstantsSize, const_size[st]);
        g.set<uint32_t>(control + nvn::kNvnControlConstantsOffset, const_offset[st]);
        g.set<uint32_t>(control + nvn::kNvnControlBlobSize, blob_size);
        stage_data[2 * st] = blob;
        stage_data[2 * st + 1] = control;
    }
    const uint64_t program = g.alloc(0x100), stages = g.alloc(32, 8);
    std::memcpy(g.mem.host_ptr(stages, 32), stage_data, 32);
    CHECK_EQ(g.call(p("nvnProgramInitialize"), {program, s.device}), 1u);
    CHECK_EQ(g.call(p("nvnProgramSetShaders"), {program, 2, stages}), 1u);

    // A broken control section (constants past the blob): no constants bound.
    const uint64_t program2 = g.alloc(0x100), stages2 = g.alloc(32, 8);
    std::memcpy(g.mem.host_ptr(stages2, 32), stage_data, 32);
    g.set<uint32_t>(stage_data[1] + nvn::kNvnControlBlobSize, 0x10);
    g.set<uint32_t>(stage_data[3] + nvn::kNvnControlBlobSize, 0x10);
    CHECK_EQ(g.call(p("nvnProgramInitialize"), {program2, s.device}), 1u);
    CHECK_EQ(g.call(p("nvnProgramSetShaders"), {program2, 2, stages2}), 1u);

    g.call(p("nvnCommandBufferBeginRecording"), {s.cmd});
    g.call(p("nvnCommandBufferBindProgram"), {s.cmd, program, nvn::kShaderStageBitVertex | nvn::kShaderStageBitFragment});
    g.call(p("nvnCommandBufferBindUniformBuffer"), {s.cmd, e(nvn::ShaderStageNvn::Vertex), 0, 0x3000, 0x40});
    g.call(p("nvnCommandBufferBindProgram"), {s.cmd, program2, nvn::kShaderStageBitVertex | nvn::kShaderStageBitFragment});
    const uint64_t list = g.call(p("nvnCommandBufferEndRecording"), {s.cmd});
    g.call(p("nvnQueueSubmitCommands"), {s.queue, 1, g.put(list)});

    const auto ubs = backend.all<nvn::CmdBindUniformBuffer>();
    CHECK_EQ(ubs.size(), 3u);
    if (ubs.size() == 3) {
        CHECK(ubs[0].stage == nvn::ShaderStage::Vertex && ubs[0].bank == nvn::kCompilerConstantBank &&
              ubs[0].address == stage_data[0] + const_offset[0] && ubs[0].size == const_size[0]);
        CHECK(ubs[1].stage == nvn::ShaderStage::Fragment && ubs[1].bank == nvn::kCompilerConstantBank &&
              ubs[1].address == stage_data[2] + const_offset[1] && ubs[1].size == const_size[1]);
        CHECK(ubs[2].stage == nvn::ShaderStage::Vertex && ubs[2].bank == nvn::kUniformBufferBankBase &&
              ubs[2].address == 0x3000);
    }
    CHECK_EQ(g.last_halt, 0u);
    reset_dispatch();
}

// Command handles are released when the memory they were recorded into is
// given to the command buffer again (games reuse command memory every frame
// and rarely finalize handles); handles from other memory stay valid.
void test_command_memory_reuse() {
    Guest g;
    CaptureBackend backend;
    nvn::Hle hle(nvn::HleOptions{kThunks}, backend);
    hle.install();
    ClearSetup s(g, hle, kRGBA8, 8, 8);
    auto& p = s.p;
    const uint64_t ctrl_a = g.alloc(0x100), ctrl_b = g.alloc(0x100);
    auto record = [&] {
        g.call(p("nvnCommandBufferBeginRecording"), {s.cmd});
        g.call(p("nvnCommandBufferBarrier"), {s.cmd, 0});
        return g.call(p("nvnCommandBufferEndRecording"), {s.cmd});
    };
    auto barriers_after_submit = [&](uint64_t handle) {
        const size_t before = backend.all<nvn::CmdBarrier>().size();
        g.call(p("nvnQueueSubmitCommands"), {s.queue, 1, g.put(handle)});
        return backend.all<nvn::CmdBarrier>().size() - before;
    };
    // Every recording also uses the current command memory: give h1/h2 their own.
    g.call(p("nvnCommandBufferAddCommandMemory"), {s.cmd, s.pool, 0x800, 0x100});
    g.call(p("nvnCommandBufferAddControlMemory"), {s.cmd, ctrl_a, 0x100});
    const uint64_t h1 = record();
    g.call(p("nvnCommandBufferAddControlMemory"), {s.cmd, ctrl_b, 0x100});
    const uint64_t h2 = record();
    // Command memory in the pool: the third list also uses it.
    g.call(p("nvnCommandBufferAddCommandMemory"), {s.cmd, s.pool, 0x40, 0x80});
    const uint64_t h3 = record();
    CHECK_EQ(barriers_after_submit(h1), 1u);
    g.call(p("nvnCommandBufferAddControlMemory"), {s.cmd, ctrl_a + 0x80, 0x80});  // overlaps A
    CHECK_EQ(barriers_after_submit(h1), 0u);  // released
    CHECK_EQ(barriers_after_submit(h2), 1u);
    CHECK_EQ(barriers_after_submit(h3), 1u);
    g.call(p("nvnCommandBufferAddCommandMemory"), {s.cmd, s.pool, 0, 0x40});  // next to it: kept
    CHECK_EQ(barriers_after_submit(h3), 1u);
    g.call(p("nvnCommandBufferAddCommandMemory"), {s.cmd, s.pool, 0x80, 0x40});  // inside it
    CHECK_EQ(barriers_after_submit(h3), 0u);
    CHECK_EQ(barriers_after_submit(h2), 1u);  // only control memory B
}

// Vertex-only drawing (depth/shadow passes): BindProgram(NULL, FRAGMENT) and
// a separable vertex program bound with the vertex bit leave no fragment
// stage; the next draw binds a program translated with an empty fragment
// stage. A fragment stage bound before the draw makes the normal pair (no
// vertex-only translation), and the vertex-only program is translated once.
void test_vertex_only_program() {
    Guest g;
    CaptureBackend backend;
    nvn::HleOptions o{kThunks};
    int pairs = 0, vertex_only = 0;
    o.program_translator = [&](const std::vector<uint8_t>&, const std::vector<uint8_t>& fs, uint32_t,
                               nvn::ProgramDesc* d, std::string*) {
        ++(fs.empty() ? vertex_only : pairs);
        d->vertex_wgsl = "vs";
        d->fragment_wgsl = fs.empty() ? "" : "fs";
        return true;
    };
    nvn::Hle hle(o, backend);
    hle.install();
    ClearSetup s(g, hle, kRGBA8, 8, 8);
    auto& p = s.p;
    const uint64_t pool = g.call(p("nvnMemoryPoolGetBufferAddress"), {s.pool});
    // Two separable programs with one stage each: NVN code header + SPH.
    const uint32_t sph_words[2] = {0x461, 0x1462};  // vertex, fragment (SPH version 3)
    uint64_t programs[2];
    for (int st = 0; st < 2; ++st) {
        const uint64_t blob = pool + 0x200 * st;
        g.set<uint32_t>(blob, nvn::kNvnCodeHeaderMagic);
        g.set<uint32_t>(blob + nvn::kNvnCodeHeaderSize, sph_words[st]);
        g.set<uint32_t>(blob + nvn::kNvnCodeHeaderSize + 0x60, 0x1234 + st);  // distinct code
        programs[st] = g.alloc(0x100);
        const uint64_t stage = g.alloc(16, 8);
        g.set<uint64_t>(stage, blob);
        g.set<uint64_t>(stage + 8, 0);
        CHECK_EQ(g.call(p("nvnProgramInitialize"), {programs[st], s.device}), 1u);
        CHECK_EQ(g.call(p("nvnProgramSetShaders"), {programs[st], 1, stage}), 1u);
    }
    const uint64_t vs = programs[0], fs = programs[1];
    const uint64_t strip = e(nvn::DrawPrimitive::TriangleStrip);

    g.call(p("nvnCommandBufferBeginRecording"), {s.cmd});
    // Normal pair: vertex, then fragment.
    g.call(p("nvnCommandBufferBindProgram"), {s.cmd, vs, nvn::kShaderStageBitVertex});
    g.call(p("nvnCommandBufferBindProgram"), {s.cmd, fs, nvn::kShaderStageBitFragment});
    g.call(p("nvnCommandBufferDrawArrays"), {s.cmd, strip, 0, 4});
    // Depth pass: no fragment program, as LGP binds it.
    g.call(p("nvnCommandBufferBindProgram"), {s.cmd, 0, nvn::kShaderStageBitFragment});
    g.call(p("nvnCommandBufferBindProgram"), {s.cmd, vs, nvn::kShaderStageBitVertex});
    g.call(p("nvnCommandBufferDrawArrays"), {s.cmd, strip, 0, 4});
    g.call(p("nvnCommandBufferDrawArrays"), {s.cmd, strip, 0, 4});
    // Again, with an indexed draw.
    g.call(p("nvnCommandBufferBindProgram"), {s.cmd, fs, nvn::kShaderStageBitFragment});
    g.call(p("nvnCommandBufferBindProgram"), {s.cmd, 0, nvn::kShaderStageBitFragment});
    g.call(p("nvnCommandBufferDrawElements"),
           {s.cmd, strip, e(nvn::IndexTypeNvn::UnsignedShort), 4, pool + 0x400});
    const uint64_t list = g.call(p("nvnCommandBufferEndRecording"), {s.cmd});
    g.call(p("nvnQueueSubmitCommands"), {s.queue, 1, g.put(list)});

    CHECK_EQ(pairs, 1);
    CHECK_EQ(vertex_only, 1);
    // Binds and draws (D): vs alone (0, vertex-only pending), pair, D,
    // fragment unbound (0), vs (0), vertex-only at the draw, D, D, pair, 0,
    // vertex-only, D.
    std::vector<nvn::ProgramId> ids;
    for (const nvn::Command& c : backend.commands) {
        if (auto* b = std::get_if<nvn::CmdBindProgram>(&c))
            ids.push_back(b->program);
        else if (std::holds_alternative<nvn::CmdDraw>(c) || std::holds_alternative<nvn::CmdDrawIndexed>(c))
            ids.push_back(~0u);  // draw marker
    }
    CHECK_EQ(ids.size(), 12u);
    if (ids.size() == 12) {
        const nvn::ProgramId pair = ids[1], vonly = ids[5], draw = ~0u;
        CHECK(pair != 0 && vonly != 0 && pair != vonly);
        const std::vector<nvn::ProgramId> want = {0, pair, draw, 0, 0, vonly, draw, draw, pair, 0, vonly, draw};
        CHECK(want == ids);
    }
    CHECK(hle.unimplemented_calls().empty());
    CHECK_EQ(g.last_halt, 0u);
    reset_dispatch();
}

// CopyBufferToBuffer and ClearTexture become backend commands; the soft
// backend clears the texel region (texture space, no window flip).
void test_copy_and_clear_texture() {
    Guest g;
    CaptureBackend backend;
    nvn::Hle hle({kThunks}, backend);
    hle.install();
    ClearSetup s(g, hle, kRGBA8, 8, 8);
    auto& p = s.p;
    const int32_t region[6] = {2, 1, 0, 3, 4, 1};
    const float rgba[4] = {1.0f, 0.5f, 0.0f, 1.0f};
    g.call(p("nvnCommandBufferBeginRecording"), {s.cmd});
    g.call(p("nvnCommandBufferCopyBufferToBuffer"), {s.cmd, 0x1000, 0x2000, 0x70, 0});
    g.call(p("nvnCommandBufferClearTexture"), {s.cmd, s.tex[0], 0, g.put(region), g.put(rgba), 0x7});
    const uint64_t list = g.call(p("nvnCommandBufferEndRecording"), {s.cmd});
    g.call(p("nvnQueueSubmitCommands"), {s.queue, 1, g.put(list)});
    const auto copies = backend.all<nvn::CmdCopyBuffer>();
    CHECK(copies.size() == 1 && copies[0].src == 0x1000 && copies[0].dst == 0x2000 && copies[0].size == 0x70);
    const auto clears = backend.all<nvn::CmdClearTexture>();
    CHECK_EQ(clears.size(), 1u);
    if (!clears.empty()) {
        CHECK(clears[0].region.x == 2 && clears[0].region.y == 1 && clears[0].region.width == 3 &&
              clears[0].region.height == 4);
        CHECK(clears[0].mask == 0x7 && clears[0].rgba[1] == 0.5f);
    }
    CHECK(hle.unimplemented_calls().empty());
    reset_dispatch();
}

// nvnCommandBufferCallCommands: a finished recording (here a red clear) runs as
// part of another one. LGP records most of its frame that way.
void test_call_commands() {
    Guest g;
    lgp::platform::NullRenderer renderer({1280, 720});
    nvn::SoftBackend backend(renderer);
    nvn::Hle hle({kThunks}, backend);
    hle.install();
    ClearSetup s(g, hle, kRGBA8, 8, 8);
    auto& p = s.p;
    const uint64_t sync = g.alloc(0x40);
    CHECK_EQ(g.call(p("nvnSyncInitialize"), {sync, s.device}), 1u);
    const uint64_t index_va = g.alloc(4);
    g.call(p("nvnWindowAcquireTexture"), {s.window, sync, index_va});
    const int index = g.get<int32_t>(index_va);

    g.call(p("nvnCommandBufferBeginRecording"), {s.cmd});
    g.call(p("nvnCommandBufferSetRenderTargets"), {s.cmd, 1, g.put(s.tex[index]), 0, 0, 0});
    g.call(p("nvnCommandBufferSetScissor"), {s.cmd, 0, 0, 1u << 20, 1u << 20});
    const float red[4] = {1, 0, 0, 1};
    const uint64_t color = g.alloc(16);
    std::memcpy(g.mem.host_ptr(color, 16), red, 16);
    g.call(p("nvnCommandBufferClearColor"), {s.cmd, 0, color, nvn::kClearColorMaskRGBA});
    const uint64_t inner = g.call(p("nvnCommandBufferEndRecording"), {s.cmd});

    g.call(p("nvnCommandBufferBeginRecording"), {s.cmd});
    g.call(p("nvnCommandBufferCallCommands"), {s.cmd, 1, g.put(inner)});
    const uint64_t outer = g.call(p("nvnCommandBufferEndRecording"), {s.cmd});
    g.call(p("nvnQueueSubmitCommands"), {s.queue, 1, g.put(outer)});
    g.call(p("nvnQueuePresentTexture"), {s.queue, s.window, uint64_t(index)});
    lgp::platform::SurfaceSize size;
    const std::vector<uint8_t> img = renderer.last_image(&size);
    CHECK(size.width == 8 && pixel_is(img, 8, 3, 3, 255, 0, 0, 255));
    CHECK_EQ(backend.stats().clears, 1u);  // only through the call: the inner list was never submitted
    CHECK(hle.unimplemented_calls().empty());
    CHECK_EQ(g.last_halt, 0u);
    reset_dispatch();
}

// nvnCommandBufferCopyTextureToTexture records both regions, the levels and
// layers of the views and the flags.
void test_copy_texture() {
    Guest g;
    CaptureBackend backend;
    nvn::Hle hle({kThunks}, backend);
    hle.install();
    ClearSetup s(g, hle, kRGBA8, 8, 8);
    auto& p = s.p;
    const uint64_t src_view = g.alloc(nvn::kTextureViewSize, 8);
    g.call(p("nvnTextureViewSetDefaults"), {src_view});
    g.call(p("nvnTextureViewSetLevels"), {src_view, 1, 1});
    const uint64_t dst_view = g.alloc(nvn::kTextureViewSize, 8);
    g.call(p("nvnTextureViewSetDefaults"), {dst_view});
    g.call(p("nvnTextureViewSetLayers"), {dst_view, 2, 1});
    const int32_t src_region[6] = {1, 2, 0, 3, 4, 1}, dst_region[6] = {5, 6, 0, 6, 8, 1};
    const uint64_t sr = g.alloc(24, 8), dr = g.alloc(24, 8);
    std::memcpy(g.mem.host_ptr(sr, 24), src_region, 24);
    std::memcpy(g.mem.host_ptr(dr, 24), dst_region, 24);
    g.call(p("nvnCommandBufferBeginRecording"), {s.cmd});
    g.call(p("nvnCommandBufferCopyTextureToTexture"),
           {s.cmd, s.tex[0], src_view, sr, s.tex[1], dst_view, dr, nvn::kCopyFlagLinearFilter});
    g.call(p("nvnCommandBufferCopyTextureToTexture"), {s.cmd, s.tex[1], 0, dr, s.tex[0], 0, sr, 0});
    g.call(p("nvnCommandBufferCopyTextureToTexture"), {s.cmd, 0x1234, 0, dr, s.tex[0], 0, sr, 0});  // unknown: dropped
    const uint64_t handle = g.call(p("nvnCommandBufferEndRecording"), {s.cmd});
    g.call(p("nvnQueueSubmitCommands"), {s.queue, 1, g.put(handle)});
    const auto copies = backend.all<nvn::CmdCopyTextureToTexture>();
    CHECK_EQ(copies.size(), 2u);
    if (copies.size() == 2) {
        const auto& c = copies[0];
        CHECK(c.src.texture != 0 && c.dst.texture != 0 && c.src.texture != c.dst.texture);
        CHECK_EQ(c.src.level, 1u);
        CHECK_EQ(c.src.layer, 0u);
        CHECK_EQ(c.dst.level, 0u);
        CHECK_EQ(c.dst.layer, 2u);
        CHECK(c.src.x == 1 && c.src.y == 2 && c.src.width == 3 && c.src.height == 4 && c.src.depth == 1);
        CHECK(c.dst.x == 5 && c.dst.y == 6 && c.dst.width == 6 && c.dst.height == 8);
        CHECK_EQ(c.flags, nvn::kCopyFlagLinearFilter);
        CHECK_EQ(copies[1].src.texture, c.dst.texture);
        CHECK_EQ(copies[1].src.level, 0u);
    }
    CHECK(nvn::Hle::is_implemented("nvnCommandBufferCopyTextureToTexture"));
    CHECK_EQ(g.last_halt, 0u);
    reset_dispatch();
}

// nvnCommandBufferCopyTextureToBuffer is accepted without halting the guest;
// the contents are not copied yet (no readback), so nothing is recorded.
void test_copy_texture_to_buffer() {
    Guest g;
    CaptureBackend backend;
    nvn::Hle hle({kThunks}, backend);
    hle.install();
    ClearSetup s(g, hle, kRGBA8, 8, 8);
    auto& p = s.p;
    const int32_t region[6] = {0, 0, 0, 8, 8, 1};
    const uint64_t r = g.alloc(24, 8), dst = g.alloc(8 * 8 * 4, 8);
    std::memcpy(g.mem.host_ptr(r, 24), region, 24);
    g.call(p("nvnCommandBufferBeginRecording"), {s.cmd});
    g.call(p("nvnCommandBufferCopyTextureToBuffer"), {s.cmd, s.tex[0], 0, r, dst, 0});
    const uint64_t handle = g.call(p("nvnCommandBufferEndRecording"), {s.cmd});
    g.call(p("nvnQueueSubmitCommands"), {s.queue, 1, g.put(handle)});
    CHECK(nvn::Hle::is_implemented("nvnCommandBufferCopyTextureToBuffer"));
    CHECK(backend.all<nvn::CmdUnsupported>().empty());
    CHECK_EQ(g.last_halt, 0u);
    reset_dispatch();
}

// A control section in NVN's layout (abi.hpp): magic, entry offset, stage,
// compute workgroup and shared memory; `blob` = code blob size.
std::vector<uint8_t> compute_control(uint32_t stage, uint32_t x, uint32_t y, uint32_t z, uint32_t shared,
                                     uint32_t entry = 0x100, uint32_t blob = 0) {
    std::vector<uint8_t> c(0x800);
    const auto set = [&c](uint64_t offset, uint32_t v) { std::memcpy(c.data() + offset, &v, 4); };
    set(0, nvn::kNvnControlMagic);
    set(nvn::kNvnControlBlobSize, blob);
    set(nvn::kNvnControlEntryOffset, entry);
    set(nvn::kNvnControlStage, stage);
    set(nvn::kNvnControlComputeWorkgroup, x);
    set(nvn::kNvnControlComputeWorkgroup + 4, y);
    set(nvn::kNvnControlComputeWorkgroup + 8, z);
    set(nvn::kNvnControlComputeSharedMemory, shared);
    set(nvn::kNvnControlComputeLocalMemoryLow, 0x40);
    return c;
}

// The built-in reader of a compute stage's control section: offsets as
// public NVN tools document them (abi.hpp), plausibility checks.
void test_compute_control_parse() {
    nvn::ComputeInfo info;
    std::string error;
    auto c = compute_control(nvn::kNvnStageCompute, 16, 8, 2, 0x1100);
    CHECK(nvn::parse_compute_control(c.data(), c.size(), &info, &error));
    CHECK(info.workgroup[0] == 16 && info.workgroup[1] == 8 && info.workgroup[2] == 2);
    CHECK_EQ(info.shared_memory_bytes, 0x1100u);
    CHECK_EQ(info.local_memory_bytes, 0x40u);
    // Exactly the bytes the reader needs are enough.
    CHECK(nvn::parse_compute_control(c.data(), nvn::kNvnControlComputeEnd, &info, &error));
    CHECK(!nvn::parse_compute_control(c.data(), nvn::kNvnControlComputeEnd - 4, &info, &error));
    // Not compute, no Maxwell CTA, too much shared memory, wrong magic.
    c = compute_control(1, 16, 8, 2, 0);
    CHECK(!nvn::parse_compute_control(c.data(), c.size(), &info, &error));
    CHECK(error.find("not compute") != std::string::npos);
    const uint32_t bad_dims[3][3] = {{0, 1, 1}, {64, 32, 1}, {1, 1, 65}};
    for (const auto& dims : bad_dims) {
        c = compute_control(nvn::kNvnStageCompute, dims[0], dims[1], dims[2], 0);
        CHECK(!nvn::parse_compute_control(c.data(), c.size(), &info, &error));
    }
    c = compute_control(nvn::kNvnStageCompute, 1024, 1, 1, 48 * 1024);
    CHECK(nvn::parse_compute_control(c.data(), c.size(), &info, &error));
    c = compute_control(nvn::kNvnStageCompute, 32, 1, 1, 48 * 1024 + 4);
    CHECK(!nvn::parse_compute_control(c.data(), c.size(), &info, &error));
    c = compute_control(nvn::kNvnStageCompute, 32, 1, 1, 0);
    c[0] ^= 1;
    CHECK(!nvn::parse_compute_control(c.data(), c.size(), &info, &error));
    // High (negative) local memory has no layout in the translator.
    c = compute_control(nvn::kNvnStageCompute, 32, 1, 1, 0);
    const uint32_t high = 0x20;
    std::memcpy(c.data() + nvn::kNvnControlComputeLocalMemoryHigh, &high, sizeof(high));
    CHECK(!nvn::parse_compute_control(c.data(), c.size(), &info, &error));
    CHECK(error.find("high local memory") != std::string::npos);
}

// nvnProgramSetShaders with a compute stage and the built-in reader: the code
// starts at the control section's entry offset (filler before it would not
// translate), the workgroup size reaches the WGSL, and the blob size bounds
// what is read.
void test_compute_program() {
    Guest g;
    CaptureBackend backend;
    nvn::Hle hle({kThunks}, backend);
    hle.install();
    ClearSetup s(g, hle, kRGBA8, 4, 4);
    auto& p = s.p;
    const uint64_t pool = g.call(p("nvnMemoryPoolGetBufferAddress"), {s.pool});
    // 0x100 bytes of filler, then sched + EXIT + NOP + NOP.
    const uint64_t code = pool;  // texture storage, unused here
    std::memset(g.mem.host_ptr(code, 0x100), 0xFF, 0x100);
    const uint64_t bundle[4] = {0x001f8000fc0007e0, 0xe30000000007000f, 0x50b0000000070f00, 0x50b0000000070f00};
    std::memcpy(g.mem.host_ptr(code + 0x100, sizeof(bundle)), bundle, sizeof(bundle));
    const auto run = [&](const std::vector<uint8_t>& control_bytes) {
        const uint64_t control = g.alloc(control_bytes.size(), 8);
        std::memcpy(g.mem.host_ptr(control, control_bytes.size()), control_bytes.data(), control_bytes.size());
        const uint64_t program = g.alloc(0x100), stage = g.alloc(16, 8);
        CHECK_EQ(g.call(p("nvnProgramInitialize"), {program, s.device}), 1u);
        const uint64_t sd[2] = {code, control};
        std::memcpy(g.mem.host_ptr(stage, 16), sd, 16);
        return g.call(p("nvnProgramSetShaders"), {program, 1, stage});
    };
    CHECK_EQ(run(compute_control(nvn::kNvnStageCompute, 8, 4, 1, 0x200, 0x100, 0x120)), 1u);
    CHECK_EQ(backend.programs.size(), 1u);
    if (backend.programs.size() == 1) {
        const std::string& wgsl = backend.programs[0].compute_wgsl;
        CHECK(wgsl.find("wg_size_x: u32 = 8u") != std::string::npos);
        CHECK(wgsl.find("wg_size_y: u32 = 4u") != std::string::npos);
    }
    // Entry offset 0: the filler is read as code and does not translate.
    CHECK_EQ(run(compute_control(nvn::kNvnStageCompute, 8, 4, 1, 0x200, 0, 0x120)), 0u);
    // A control section of another stage or a broken workgroup: rejected.
    CHECK_EQ(run(compute_control(nvn::kNvnStageCompute, 0, 4, 1, 0x200)), 0u);
    CHECK_EQ(backend.programs.size(), 1u);
    CHECK_EQ(g.last_halt, 0u);
    reset_dispatch();
}

// accept_untranslated_programs: a binary the translator cannot handle (here a
// compute stage without HleOptions::compute_info) succeeds like on the console,
// is counted, and a broken binary (code outside every pool) still fails.
void test_accept_untranslated() {
    Guest g;
    lgp::platform::NullRenderer renderer;
    nvn::SoftBackend backend(renderer);
    nvn::HleOptions o{kThunks};
    o.accept_untranslated_programs = true;
    nvn::Hle hle(o, backend);
    hle.install();
    ClearSetup s(g, hle, kRGBA8, 4, 4);
    auto& p = s.p;
    const uint64_t cs_program = g.alloc(0x100);
    CHECK_EQ(g.call(p("nvnProgramInitialize"), {cs_program, s.device}), 1u);
    const uint64_t code = g.call(p("nvnMemoryPoolGetBufferAddress"), {s.pool});
    const uint64_t stage = g.alloc(16, 8);
    const uint64_t sd1[2] = {code, 0};
    std::memcpy(g.mem.host_ptr(stage, 16), sd1, 16);
    CHECK_EQ(g.call(p("nvnProgramSetShaders"), {cs_program, 1, stage}), 1u);
    CHECK_EQ(hle.untranslated_programs(), 1u);
    const auto reasons = hle.untranslated_reasons();
    CHECK(reasons.size() == 1 && reasons[0].rfind("1x ", 0) == 0);

    const uint64_t program = g.alloc(0x100);
    CHECK_EQ(g.call(p("nvnProgramInitialize"), {program, s.device}), 1u);
    const uint64_t stages = g.alloc(32, 8);
    const uint64_t sd[4] = {0x10, 0, 0x20, 0};
    std::memcpy(g.mem.host_ptr(stages, 32), sd, 32);
    CHECK_EQ(g.call(p("nvnProgramSetShaders"), {program, 2, stages}), 0u);
    CHECK_EQ(hle.untranslated_programs(), 1u);
    CHECK_EQ(g.last_halt, 0u);
    reset_dispatch();
}

void test_bad_pointer() {
    Guest g;
    lgp::platform::NullRenderer renderer;
    nvn::SoftBackend backend(renderer);
    nvn::Hle hle({kThunks}, backend);
    hle.install();
    ClearSetup s(g, hle, kRGBA8, 4, 4);
    g.call(s.p("nvnCommandBufferBeginRecording"), {s.cmd});
    g.call(s.p("nvnCommandBufferClearColor"), {s.cmd, 0, 0x10, 0xF});  // null page
    CHECK_EQ(g.last_halt, static_cast<uint32_t>(lgp::kHaltMemFault));
    CHECK_EQ(g.last_halt_info, 0x10u);
    // The HLE is still usable (no lock left held, no longjmp through it).
    CHECK_EQ(g.call(s.p("nvnCommandBufferIsRecording"), {s.cmd}), 1u);
    CHECK_EQ(g.last_halt, 0u);
    reset_dispatch();
}

}  // namespace

// Rasterizer state set on the command buffer, instanced streams, sampler LOD
// bias, slice copies with an image stride, and the statistics counters the
// HLE writes itself at submit (ReportCounter/ResetCounter).
void test_raster_state_and_counters() {
    Guest g;
    CaptureBackend backend;
    nvn::Hle hle({kThunks}, backend);
    hle.install();
    ClearSetup s(g, hle, kRGBA8, 8, 8);
    auto& p = s.p;

    const uint64_t poly = g.alloc(8, 8);
    g.call(p("nvnPolygonStateSetDefaults"), {poly});
    g.call(p("nvnPolygonStateSetPolygonOffsetEnables"), {poly, nvn::kPolygonOffsetFill | nvn::kPolygonOffsetLine});
    const uint64_t streams = g.alloc(3 * nvn::kVertexStreamStateSize, 8);
    for (uint64_t i = 0; i < 3; ++i) {
        g.call(p("nvnVertexStreamStateSetDefaults"), {streams + i * nvn::kVertexStreamStateSize});
        g.call(p("nvnVertexStreamStateSetStride"), {streams + i * nvn::kVertexStreamStateSize, 16});
        g.call(p("nvnVertexStreamStateSetDivisor"), {streams + i * nvn::kVertexStreamStateSize, i == 2 ? 3u : i});
    }
    const uint64_t sb = g.alloc(8, 8), sampler = g.alloc(0x60), tpool = g.alloc(0x20), spool = g.alloc(0x20);
    g.call(p("nvnSamplerBuilderSetDefaults"), {sb});
    g.call(p("nvnSamplerBuilderSetLodBias"), {sb}, {-1.5f});
    CHECK_EQ(g.call(p("nvnSamplerInitialize"), {sampler, sb}), 1u);
    CHECK_EQ(g.call(p("nvnTexturePoolInitialize"), {tpool, s.pool, 0, 64}), 1u);
    CHECK_EQ(g.call(p("nvnSamplerPoolInitialize"), {spool, s.pool, 0x800, 16}), 1u);
    g.call(p("nvnTexturePoolRegisterTexture"), {tpool, 1, s.tex[0], 0});
    g.call(p("nvnSamplerPoolRegisterSampler"), {spool, 1, sampler});
    const uint64_t handle = g.call(p("nvnDeviceGetTextureHandle"), {s.device, 1, 1});

    const uint64_t reports = g.alloc(8 * nvn::kCounterDataSize, 16);
    std::memset(g.mem.host_ptr(reports, 8 * nvn::kCounterDataSize), 0xEE, 8 * nvn::kCounterDataSize);
    auto report = [&](nvn::CounterTypeNvn type, int slot) {
        g.call(p("nvnCommandBufferReportCounter"), {s.cmd, e(type), reports + slot * nvn::kCounterDataSize});
    };
    const int32_t region[6] = {1, 2, 1, 3, 2, 2};
    g.call(p("nvnCommandBufferBeginRecording"), {s.cmd});
    g.call(p("nvnCommandBufferSetPolygonOffsetClamp"), {s.cmd}, {1.5f, 2.0f, 0.25f});
    g.call(p("nvnCommandBufferSetDepthClamp"), {s.cmd, 1});
    g.call(p("nvnCommandBufferSetSampleMask"), {s.cmd, 0x5});
    // No WebGPU equivalent: accepted, logged, no command.
    g.call(p("nvnCommandBufferSetDepthBounds"), {s.cmd, 1}, {0.25f, 0.75f});
    g.call(p("nvnCommandBufferSetConservativeRasterEnable"), {s.cmd, 1});
    g.call(p("nvnCommandBufferSetLineWidth"), {s.cmd}, {2.0f});
    g.call(p("nvnCommandBufferBindPolygonState"), {s.cmd, poly});
    g.call(p("nvnCommandBufferBindVertexStreamState"), {s.cmd, 3, streams});
    g.call(p("nvnCommandBufferSetTexturePool"), {s.cmd, tpool});
    g.call(p("nvnCommandBufferSetSamplerPool"), {s.cmd, spool});
    g.call(p("nvnCommandBufferBindTexture"), {s.cmd, e(nvn::ShaderStageNvn::Fragment), 0, handle});
    g.call(p("nvnCommandBufferSetCopyRowStride"), {s.cmd, 64});
    g.call(p("nvnCommandBufferSetCopyImageStride"), {s.cmd, 512});
    g.call(p("nvnCommandBufferCopyBufferToTexture"), {s.cmd, 0x7000, s.tex[0], 0, g.put(region), 0});
    // Counters: reset, nothing drawn, a draw, a discarded draw, a timestamp.
    g.call(p("nvnCommandBufferResetCounter"), {s.cmd, e(nvn::CounterTypeNvn::SamplesPassed)});
    report(nvn::CounterTypeNvn::SamplesPassed, 0);
    g.call(p("nvnCommandBufferDrawArraysInstanced"), {s.cmd, e(nvn::DrawPrimitive::Triangles), 0, 6, 0, 2});
    report(nvn::CounterTypeNvn::SamplesPassed, 1);
    report(nvn::CounterTypeNvn::InputVertices, 2);
    g.call(p("nvnCommandBufferSetRasterizerDiscard"), {s.cmd, 1});
    g.call(p("nvnCommandBufferDrawArrays"), {s.cmd, e(nvn::DrawPrimitive::TriangleStrip), 0, 5});
    report(nvn::CounterTypeNvn::SamplesPassed, 3);
    report(nvn::CounterTypeNvn::PrimitivesGenerated, 4);
    report(nvn::CounterTypeNvn::Timestamp, 5);
    const uint64_t list = g.call(p("nvnCommandBufferEndRecording"), {s.cmd});
    // Nothing is written before the submit.
    CHECK_EQ(g.get<uint64_t>(reports), 0xEEEEEEEEEEEEEEEEull);
    g.call(p("nvnQueueSubmitCommands"), {s.queue, 1, g.put(list)});
    CHECK_EQ(g.last_halt, 0u);
    CHECK(hle.unimplemented_calls().empty());

    const auto bias = backend.all<nvn::CmdSetDepthBias>();
    CHECK(bias.size() == 1 && bias[0].factor == 1.5f && bias[0].units == 2.0f && bias[0].clamp == 0.25f);
    const auto clamp = backend.all<nvn::CmdSetDepthClamp>();
    CHECK(clamp.size() == 1 && clamp[0].enable);
    const auto masks = backend.all<nvn::CmdSetSampleMask>();
    CHECK(masks.size() == 1 && masks[0].mask == 0x5u);
    const auto discard = backend.all<nvn::CmdSetRasterizerDiscard>();
    CHECK(discard.size() == 1 && discard[0].enable);
    const auto raster = backend.all<nvn::CmdSetRaster>();
    CHECK(raster.size() == 1 && raster[0].depth_bias == (nvn::kDepthBiasTriangles | nvn::kDepthBiasLines));
    const auto layouts = backend.all<nvn::CmdSetVertexLayout>();
    CHECK(layouts.size() == 1 && layouts[0].divisors == std::vector<uint32_t>({0, 1, 3}));
    const auto texs = backend.all<nvn::CmdBindTexture>();
    CHECK(texs.size() == 1 && texs[0].sampler.lod_bias == -1.5f);
    const auto copies = backend.all<nvn::CmdCopyBufferToTexture>();
    CHECK_EQ(copies.size(), 1u);
    if (!copies.empty()) {
        CHECK(copies[0].x == 1 && copies[0].y == 2 && copies[0].z == 1 && copies[0].width == 3 &&
              copies[0].height == 2 && copies[0].depth == 2);
        CHECK(copies[0].row_stride == 64 && copies[0].image_stride == 512 && copies[0].level == 0);
    }

    auto counter = [&](int slot) { return g.get<uint64_t>(reports + slot * nvn::kCounterDataSize); };
    auto stamp = [&](int slot) { return g.get<uint64_t>(reports + slot * nvn::kCounterDataSize + 8); };
    CHECK_EQ(counter(0), 0u);  // reset, no draw yet
    CHECK_EQ(counter(1), 4u);  // 2 triangles x 2 instances
    CHECK_EQ(counter(2), 12u);
    CHECK_EQ(counter(3), 4u);   // the discarded strip adds nothing
    CHECK_EQ(counter(4), 7u);   // ... but its 3 primitives were generated
    CHECK_EQ(counter(5), 0u);   // a timestamp report carries no counter
    CHECK(stamp(5) != 0 && stamp(5) >= stamp(0));
    CHECK_EQ(g.get<uint64_t>(reports + 6 * nvn::kCounterDataSize), 0xEEEEEEEEEEEEEEEEull);  // untouched
    reset_dispatch();
}

// LGP finalizes and initializes the same NVNtexture again (a larger version
// streamed in after a capture) without registering it in its pool slot
// anew: the slot must name the new texture, not the destroyed one. A slot
// registered to another texture since stays with that one.
void test_texture_rebuilt_in_place() {
    Guest g;
    CaptureBackend backend;
    nvn::Hle hle({kThunks}, backend);
    hle.install();
    ClearSetup s(g, hle, kRGBA8, 8, 8);
    auto& p = s.p;
    const uint64_t sb = g.alloc(8, 8), sampler = g.alloc(0x60), tpool = g.alloc(0x20), spool = g.alloc(0x20);
    g.call(p("nvnSamplerBuilderSetDefaults"), {sb});
    CHECK_EQ(g.call(p("nvnSamplerInitialize"), {sampler, sb}), 1u);
    CHECK_EQ(g.call(p("nvnTexturePoolInitialize"), {tpool, s.pool, 0, 64}), 1u);
    CHECK_EQ(g.call(p("nvnSamplerPoolInitialize"), {spool, s.pool, 0x800, 16}), 1u);
    g.call(p("nvnTexturePoolRegisterTexture"), {tpool, 1, s.tex[0], 0});
    g.call(p("nvnTexturePoolRegisterTexture"), {tpool, 2, s.tex[0], 0});
    g.call(p("nvnTexturePoolRegisterTexture"), {tpool, 2, s.tex[1], 0});  // slot 2 now names tex[1]
    g.call(p("nvnSamplerPoolRegisterSampler"), {spool, 1, sampler});
    auto bound = [&](int slot) {
        backend.commands.clear();
        g.call(p("nvnCommandBufferBeginRecording"), {s.cmd});
        g.call(p("nvnCommandBufferSetTexturePool"), {s.cmd, tpool});
        g.call(p("nvnCommandBufferSetSamplerPool"), {s.cmd, spool});
        const uint64_t handle = g.call(p("nvnDeviceGetTextureHandle"), {s.device, uint64_t(slot), 1});
        g.call(p("nvnCommandBufferBindTexture"), {s.cmd, e(nvn::ShaderStageNvn::Fragment), 0, handle});
        const uint64_t list = g.call(p("nvnCommandBufferEndRecording"), {s.cmd});
        g.call(p("nvnQueueSubmitCommands"), {s.queue, 1, g.put(list)});
        const auto texs = backend.all<nvn::CmdBindTexture>();
        return texs.size() == 1 ? texs[0].texture : nvn::TextureId(~0u);
    };
    const nvn::TextureId first = bound(1), other = bound(2);
    CHECK(first != 0 && other != 0 && first != other);
    g.call(p("nvnTextureFinalize"), {s.tex[0]});
    g.call(p("nvnTextureBuilderSetSize2D"), {s.tex_builder, 4, 4});
    g.call(p("nvnTextureBuilderSetStorage"), {s.tex_builder, s.pool, 0});
    CHECK_EQ(g.call(p("nvnTextureInitialize"), {s.tex[0], s.tex_builder}), 1u);
    const nvn::TextureId rebuilt = bound(1);
    CHECK(rebuilt != 0 && rebuilt != first);
    CHECK_EQ(backend.live_textures.count(rebuilt), 1u);
    CHECK_EQ(backend.live_textures.count(first), 0u);
    CHECK_EQ(bound(2), other);
    CHECK_EQ(g.last_halt, 0u);
    reset_dispatch();
}

// LGP_NVN_PROJECTION_SCAN (ctest nvn.projection_scan checks the log lines):
// one uniform buffer with a 21:9 projection in rows at +0x40, a 16:9 one in
// columns at +0x100, an orthographic matrix at +0x180, which is no hit, and a
// 4:3 view-projection (fovy 40, turned camera away from the origin) at +0x1c0.
void test_projection_scan() {
    Guest g;
    CaptureBackend backend;
    nvn::Hle hle(nvn::HleOptions{kThunks}, backend);
    hle.install();
    ClearSetup s(g, hle, kRGBA8, 8, 8);
    auto& p = s.p;
    const uint64_t ubo = g.alloc(0x200, 256);
    const float f = 1.f / std::tan(25.f * 3.14159265f / 180.f);  // fovy 50 deg
    const float near_far[2] = {-1.002f, -0.2002f};
    const float rows[16] = {f / (21.f / 9.f), 0, 0, 0, 0, f, 0, 0, 0, 0, near_far[0], near_far[1], 0, 0, -1, 0};
    const float columns[16] = {f / (16.f / 9.f), 0, 0, 0, 0, f, 0, 0, 0, 0, near_far[0], -1, 0, 0, near_far[1], 0};
    const float ortho[16] = {2.f / 1280, 0, 0, 0, 0, 2.f / 720, 0, 0, 0, 0, 1, 0, -1, -1, 0, 1};
    std::memcpy(g.mem.host_ptr(ubo + 0x40, 64), rows, 64);
    std::memcpy(g.mem.host_ptr(ubo + 0x100, 64), columns, 64);
    std::memcpy(g.mem.host_ptr(ubo + 0x180, 64), ortho, 64);
    {
        const float f43 = 1.f / std::tan(20.f * 3.14159265f / 180.f);
        const float proj[4][4] = {{f43 / (4.f / 3.f), 0, 0, 0}, {0, f43, 0, 0}, {0, 0, -1.002f, -0.2002f}, {0, 0, -1, 0}};
        const float cy = std::cos(0.5f), sy = std::sin(0.5f), cx = std::cos(0.2f), sx = std::sin(0.2f);
        // view = rotate x * rotate y, then translate (3, -2, -10)
        const float view[4][4] = {{cy, 0, sy, 3}, {sx * sy, cx, -sx * cy, -2}, {-cx * sy, sx, cx * cy, -10}, {0, 0, 0, 1}};
        float vp[16];
        for (int r = 0; r < 4; ++r)
            for (int col = 0; col < 4; ++col) {
                float sum = 0;
                for (int k = 0; k < 4; ++k)
                    sum += proj[r][k] * view[k][col];
                vp[4 * r + col] = sum;
            }
        std::memcpy(g.mem.host_ptr(ubo + 0x1c0, 64), vp, 64);
    }
    g.call(p("nvnCommandBufferBeginRecording"), {s.cmd});
    g.call(p("nvnCommandBufferBindUniformBuffer"), {s.cmd, e(nvn::ShaderStageNvn::Vertex), 0, ubo, 0x200});
    // LGP_NVN_CBUF_FIND: a draw into the 8x8 target sees m11 of the 21:9 matrix at +0x54.
    g.call(p("nvnCommandBufferSetRenderTargets"), {s.cmd, 1, g.put(s.tex[0]), 0, 0, 0});
    g.call(p("nvnCommandBufferDrawArrays"), {s.cmd, e(nvn::DrawPrimitive::Triangles), 0, 3});
    const uint64_t list = g.call(p("nvnCommandBufferEndRecording"), {s.cmd});
    g.call(p("nvnQueueSubmitCommands"), {s.queue, 1, g.put(list)});  // scanned again
    g.call(p("nvnQueuePresentTexture"), {s.queue, s.window, 0});       // LGP_NVN_DRAW_STATS_FRAME=1
    CHECK_EQ(g.last_halt, 0u);
    reset_dispatch();
}

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--projection-scan") == 0) {
        // Set here: under Emscripten/Node getenv does not see the ctest environment.
#ifdef _WIN32
        _putenv_s("LGP_NVN_PROJECTION_SCAN", "1");
        _putenv_s("LGP_NVN_CBUF_FIND", "2.144507");
        _putenv_s("LGP_NVN_DRAW_STATS_FRAME", "1");
#else
        setenv("LGP_NVN_PROJECTION_SCAN", "1", 1);
        setenv("LGP_NVN_CBUF_FIND", "2.144507", 1);  // 1 / tan(25 deg)
        setenv("LGP_NVN_DRAW_STATS_FRAME", "1", 1);
#endif
        RUN(test_projection_scan);
        return finish();
    }
    RUN(test_proc_lookup);
    RUN(test_clear_and_present);
    RUN(test_srgb_and_mask);
    RUN(test_texture_queries_and_builder_copy);
    RUN(test_overrides);
    RUN(test_unimplemented);
    RUN(test_state_objects);
    RUN(test_program_constants);
    RUN(test_copy_and_clear_texture);
    RUN(test_call_commands);
    RUN(test_copy_texture);
    RUN(test_copy_texture_to_buffer);
    RUN(test_compute_control_parse);
    RUN(test_compute_program);
    RUN(test_accept_untranslated);
    RUN(test_bad_pointer);
    RUN(test_raster_state_and_counters);
    RUN(test_command_memory_reuse);
    RUN(test_vertex_only_program);
    RUN(test_texture_rebuilt_in_place);
    return finish();
}
