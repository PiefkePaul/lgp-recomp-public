// P4.1 end to end: the guest program guest/nvn_clear.cpp (AArch64, packed as
// NRO, recompiled with lgp_lift) loads NVN through the HLE's bootstrap
// loader, sets up device, queue, pool, textures and window, clears and
// presents three frames. The CPU backend's image is checked here; built with
// LGP_TEST_WEBGPU the same program runs on the WebGPU backend (P4.4, headless
// device) and the presented texture is read back from the GPU.
//
// usage: lgp_test_nvn_guest <program.nro> <program.elf>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../runtime/check.hpp"
#include "lgp/gpu/nvn/hle.hpp"
#include "lgp/lifted/lifted.hpp"
#include "lgp/platform/null_backends.hpp"
#include "lgp/recomp/elf_symbols.hpp"
#include "lgp/recomp/link.hpp"
#include "lgp/recomp/loader.hpp"
#include "lgp/runtime/dispatch.hpp"
#include "lgp/runtime/kernel.hpp"
#if defined(LGP_TEST_WEBGPU)
#include "lgp/gpu/webgpu/webgpu_backend.hpp"
#include "lgp/platform/webgpu/webgpu_renderer.hpp"
#endif

extern const lgp::LiftedEntry lgp_lifted_entries[];
extern const size_t lgp_lifted_entries_count;

namespace {

using namespace lgp::rt;
namespace rc = lgp::recomp;
namespace nvn = lgp::gpu::nvn;

constexpr uint64_t kMiB = 0x10'0000;
constexpr uint64_t kBase = 0x0800'0000;  // lgp_lift's default --base
constexpr uint64_t kStackSize = 256 * 1024;
constexpr uint32_t kSentinelBrk = 0x4C47;  // crt0.S

uint8_t seg_perm(rc::SegmentKind k) {
    switch (k) {
    case rc::SegmentKind::Text: return kPermRX;
    case rc::SegmentKind::Ro: return kPermR;
    default: return kPermRW;
    }
}

bool pixel_is(const std::vector<uint8_t>& img, uint32_t width, uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b,
              uint8_t a) {
    const uint8_t* p = img.data() + (size_t(y) * width + x) * 4;
    // +-1: a clear colour of 0.5 may land on 127 or 128 (lavapipe rounds up,
    // NVIDIA's Vulkan driver down); both are within the UNORM conversion rules.
    const auto near = [](uint8_t v, uint8_t want) { return v + 1 >= want && v <= want + 1; };
    if (near(p[0], r) && near(p[1], g) && near(p[2], b) && near(p[3], a))
        return true;
    std::printf("  pixel (%u,%u) = %u %u %u %u\n", x, y, p[0], p[1], p[2], p[3]);
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <program.nro> <program.elf>\n", argv[0]);
        return 2;
    }
    std::vector<rc::Module> mods;
    mods.push_back(rc::load_module_file(argv[1]));
    rc::LinkOptions lo;
    lo.first_base = kBase;
    const rc::LinkedImage linked = rc::link(std::move(mods), lo);
    const rc::Module& m = linked.modules.front();
    const uint64_t entry = m.base + m.entry_offset;
    const uint64_t image_size = (m.image.size() + 0xFFF) & ~uint64_t{0xFFF};
    uint64_t sym_acquired = 0, sym_descriptor = 0, sym_handle = 0;
    for (const rc::Symbol& s : rc::read_elf_symbols_file(argv[2]).symbols) {
        if (s.name == "g_acquired") sym_acquired = m.base + s.value;
        if (s.name == "g_descriptor_size") sym_descriptor = m.base + s.value;
        if (s.name == "g_handle") sym_handle = m.base + s.value;
    }
    CHECK(sym_acquired != 0 && sym_descriptor != 0 && sym_handle != 0);

    ProcessConfig cfg;
    cfg.total_memory_size = 16 * kMiB;
    cfg.custom_layout = true;
    cfg.layout = Layout::packed(kBase, 1 * kMiB, 1 * kMiB, 1 * kMiB, 1 * kMiB, 8 * kMiB);
    Kernel k(cfg);
    set_active_kernel(&k);
    const Layout& l = k.memory().layout();
    CHECK(image_size + 0x2000 <= l.code.size);
    CHECK(k.map_owned(l.code.base, image_size, MemState::Code, kPermRW).ok());
    std::memcpy(k.memory().host_ptr(l.code.base, image_size), m.image.data(), m.image.size());
    for (const rc::Segment& s : m.segments) {
        if (s.size == 0) continue;
        const uint64_t lo_off = s.offset & ~uint64_t{0xFFF};
        const uint64_t hi_off = (s.offset + s.size + 0xFFF) & ~uint64_t{0xFFF};
        k.memory().set_perm(l.code.base + lo_off, hi_off - lo_off, seg_perm(s.kind));
    }
    CHECK(k.map_owned(l.stack.end() - kStackSize, kStackSize, MemState::Stack, kPermRW).ok());

    register_functions(lgp_lifted_entries, lgp_lifted_entries_count);
#if defined(LGP_TEST_WEBGPU)
    lgp::platform::webgpu::RendererOptions renderer_options;
    renderer_options.size = {1280, 720};
    lgp::platform::webgpu::WebGpuRenderer renderer(renderer_options);
    if (!renderer.ok()) {
        std::printf("skipped: %s\n", renderer.error().c_str());
        set_active_kernel(nullptr);
        return 77;
    }
    std::printf("  adapter: %s\n", renderer.adapter_name().c_str());
    lgp::gpu::webgpu::WebGpuBackend backend(renderer);
#else
    lgp::platform::NullRenderer renderer({1280, 720});
    nvn::SoftBackend backend(renderer);
#endif
    nvn::HleOptions options;
    options.thunk_base = l.code.base + image_size;  // directly behind the module (hle.hpp)
    options.trace = std::getenv("LGP_NVN_TRACE") != nullptr;
    nvn::Hle hle(options, backend);
    hle.install();

    lgp::CpuState cpu{};
    cpu.x[0] = hle.bootstrap_loader_va();
    cpu.sp = l.stack.end();
    cpu.pc = entry;
    const lgp::LiftedFn fn = lookup_function(entry);
    CHECK(fn != nullptr);
    if (fn != nullptr) run_lifted(&cpu, fn);

    const bool sentinel = cpu.halt == lgp::kHaltBrk && cpu.halt_info == kSentinelBrk;
    CHECK(sentinel);
    if (!sentinel)
        std::printf("  halt %u info 0x%" PRIx64 " pc 0x%" PRIx64 "\n", cpu.halt, cpu.halt_info, cpu.pc);
    std::printf("  guest_main returned %" PRIu64 "\n", cpu.x[0]);
    CHECK_EQ(cpu.x[0], 0u);

    int32_t acquired[3];
    std::memcpy(acquired, k.memory().host_ptr(sym_acquired, sizeof(acquired)), sizeof(acquired));
    CHECK_EQ(acquired[0], 0);
    CHECK_EQ(acquired[1], 1);
    CHECK_EQ(acquired[2], 0);
    int32_t descriptor = 0;
    std::memcpy(&descriptor, k.memory().host_ptr(sym_descriptor, 4), 4);
    CHECK_EQ(descriptor, 32);
    uint64_t handle = 0;
    std::memcpy(&handle, k.memory().host_ptr(sym_handle, 8), 8);
    CHECK(handle != 0);

    CHECK_EQ(renderer.frames(), 3u);
    lgp::platform::SurfaceSize size;
#if defined(LGP_TEST_WEBGPU)
    std::vector<uint8_t> img;
    CHECK(backend.read_presented(&img, &size.width, &size.height));
    CHECK_EQ(renderer.error_count(), 0u);
#else
    const std::vector<uint8_t> img = renderer.last_image(&size);
#endif
    CHECK_EQ(size.width, 64u);
    CHECK_EQ(size.height, 32u);
    if (size.width == 64 && size.height == 32) {
        // Frame 2: red scissor (0,0,32,4) in lower-left origin = bottom 4 rows.
        CHECK(pixel_is(img, 64, 0, 31, 255, 0, 0, 255));
        CHECK(pixel_is(img, 64, 31, 28, 255, 0, 0, 255));
        CHECK(pixel_is(img, 64, 32, 31, 64, 128, 255, 255));
        CHECK(pixel_is(img, 64, 0, 27, 64, 128, 255, 255));
        CHECK(pixel_is(img, 64, 63, 0, 64, 128, 255, 255));
    }
    const auto st = backend.stats();
    CHECK_EQ(st.clears, 6u);
    CHECK_EQ(st.presents, 3u);
    CHECK(hle.unimplemented_calls().empty());

    size_t handed_out = 0;
    for (const nvn::ProcStats& s : hle.stats())
        if (s.calls != 0) ++handed_out;
    std::printf("  NVN functions called: %zu\n", handed_out);
    set_active_kernel(nullptr);
    return finish();
}
