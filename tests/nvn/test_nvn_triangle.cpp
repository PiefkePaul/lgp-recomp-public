// P4.1 part 2 end to end: the guest program guest/nvn_triangle.cpp
// (AArch64, packed as NRO, recompiled with lgp_lift) sets up an NVN program
// from own Maxwell shaders, state objects, a texture and sampler pool, draws
// with depth test, culling, an indexed textured quad and blending, and
// presents. The HLE records it, the WebGPU backend renders it; the presented
// texture is read back and checked pixel by pixel. Then two compute
// dispatches on storage buffers in guest memory, which the guest program
// checks itself after nvnQueueFinish (g_step stays non-zero otherwise).
//
// usage: lgp_test_nvn_triangle <program.nro> <program.elf>
//        lgp_test_nvn_triangle <program.img>
//        lgp_test_nvn_triangle --dump-image <program.nro> <program.elf> <out.img>
// The .img form is the linked image (loader + relocations already applied)
// for the WASM build, which runs without the recompiler's loader; the native
// build writes it with --dump-image. Own test program, no game code.
// Prints "nvn triangle: ok" on success (the browser test waits for it).
// Exit code 77 = skipped (no WebGPU adapter).
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "../runtime/check.hpp"
#include "lgp/gpu/nvn/async_backend.hpp"
#include "lgp/gpu/nvn/hle.hpp"
#include "lgp/gpu/webgpu/webgpu_backend.hpp"
#include "lgp/lifted/lifted.hpp"
#include "lgp/platform/webgpu/webgpu_renderer.hpp"
#if !defined(LGP_TRIANGLE_IMAGE_ONLY)
#include "lgp/recomp/elf_symbols.hpp"
#include "lgp/recomp/link.hpp"
#include "lgp/recomp/loader.hpp"
#endif
#include "lgp/runtime/dispatch.hpp"
#include "lgp/runtime/kernel.hpp"

extern const lgp::LiftedEntry lgp_lifted_entries[];
extern const size_t lgp_lifted_entries_count;

namespace {

using namespace lgp::rt;
namespace nvn = lgp::gpu::nvn;

constexpr uint64_t kMiB = 0x10'0000;
constexpr uint64_t kBase = 0x0800'0000;  // lgp_lift's default --base
constexpr uint64_t kStackSize = 256 * 1024;
constexpr uint32_t kSentinelBrk = 0x4C47;  // crt0.S

// The linked program: what the loader produces, or what an .img holds.
struct Image {
    struct Segment {
        uint64_t offset = 0, size = 0;
        uint8_t perm = kPermRW;
    };
    uint64_t entry_offset = 0;
    uint64_t step_offset = 0;  // g_step
    std::vector<Segment> segments;
    std::vector<uint8_t> bytes;
};
constexpr char kImageMagic[8] = {'L', 'G', 'P', 'I', 'M', 'G', '1', 0};

bool read_image(const char* path, Image* img) {
    std::FILE* f = std::fopen(path, "rb");
    if (f == nullptr) return false;
    char magic[8];
    uint64_t header[4];  // entry, step, segment count, image size
    bool ok = std::fread(magic, 8, 1, f) == 1 && std::memcmp(magic, kImageMagic, 8) == 0 &&
              std::fread(header, sizeof(header), 1, f) == 1 && header[2] < 16 && header[3] < (64u << 20);
    if (ok) {
        img->entry_offset = header[0];
        img->step_offset = header[1];
        img->segments.resize(header[2]);
        for (Image::Segment& s : img->segments) {
            uint64_t v[3];
            ok = ok && std::fread(v, sizeof(v), 1, f) == 1;
            s = {v[0], v[1], uint8_t(v[2])};
        }
        img->bytes.resize(header[3]);
        ok = ok && std::fread(img->bytes.data(), img->bytes.size(), 1, f) == 1;
    }
    std::fclose(f);
    return ok;
}

#if !defined(LGP_TRIANGLE_IMAGE_ONLY)
namespace rc = lgp::recomp;

uint8_t seg_perm(rc::SegmentKind k) {
    switch (k) {
    case rc::SegmentKind::Text: return kPermRX;
    case rc::SegmentKind::Ro: return kPermR;
    default: return kPermRW;
    }
}

Image load_program(const char* nro, const char* elf) {
    std::vector<rc::Module> mods;
    mods.push_back(rc::load_module_file(nro));
    rc::LinkOptions lo;
    lo.first_base = kBase;
    rc::LinkedImage linked = rc::link(std::move(mods), lo);
    const rc::Module& m = linked.modules.front();
    Image img;
    img.entry_offset = m.base + m.entry_offset - kBase;
    for (const rc::Symbol& s : rc::read_elf_symbols_file(elf).symbols)
        if (s.name == "g_step") img.step_offset = m.base + s.value - kBase;
    for (const rc::Segment& s : m.segments)
        img.segments.push_back({s.offset, s.size, seg_perm(s.kind)});
    img.bytes = m.image;
    return img;
}

bool write_image(const Image& img, const char* path) {
    std::FILE* f = std::fopen(path, "wb");
    if (f == nullptr) return false;
    const uint64_t header[4] = {img.entry_offset, img.step_offset, img.segments.size(), img.bytes.size()};
    bool ok = std::fwrite(kImageMagic, 8, 1, f) == 1 && std::fwrite(header, sizeof(header), 1, f) == 1;
    for (const Image::Segment& s : img.segments) {
        const uint64_t v[3] = {s.offset, s.size, s.perm};
        ok = ok && std::fwrite(v, sizeof(v), 1, f) == 1;
    }
    ok = ok && std::fwrite(img.bytes.data(), img.bytes.size(), 1, f) == 1;
    return std::fclose(f) == 0 && ok;
}
#endif

// Exact match, or within `tol` per channel where rounding of a blend or a
// float colour may differ between GPUs.
bool pixel_is(const std::vector<uint8_t>& img, uint32_t width, uint32_t x, uint32_t y, int r, int g, int b, int a,
              int tol = 0) {
    const uint8_t* p = img.data() + (size_t(y) * width + x) * 4;
    const bool ok = std::abs(p[0] - r) <= tol && std::abs(p[1] - g) <= tol && std::abs(p[2] - b) <= tol &&
                    std::abs(p[3] - a) <= tol;
    if (!ok)
        std::printf("  pixel (%u,%u) = %u %u %u %u, want %d %d %d %d\n", x, y, p[0], p[1], p[2], p[3], r, g, b, a);
    return ok;
}

// Guest memory for the backend: the kernel's view.
class KernelMemory final : public nvn::GuestMemoryView {
public:
    explicit KernelMemory(Kernel& k) : k_(k) {}
    bool read(uint64_t va, void* dst, uint64_t size) const override {
        const uint8_t* p = k_.memory().host_ptr(va, size);
        if (p == nullptr) return false;
        std::memcpy(dst, p, size);
        return true;
    }
    bool write(uint64_t va, const void* src, uint64_t size) const override {
        uint8_t* p = k_.memory().host_ptr(va, size);
        if (p == nullptr) return false;
        std::memcpy(p, src, size);
        return true;
    }

private:
    Kernel& k_;
};

}  // namespace

int main(int argc, char** argv) {
    Image program;
#if !defined(LGP_TRIANGLE_IMAGE_ONLY)
    if (argc == 5 && std::strcmp(argv[1], "--dump-image") == 0) {
        program = load_program(argv[2], argv[3]);
        if (program.step_offset == 0 || !write_image(program, argv[4])) {
            std::fprintf(stderr, "cannot write %s\n", argv[4]);
            return 1;
        }
        return 0;
    }
    if (argc == 3) {
        program = load_program(argv[1], argv[2]);
    } else
#endif
    if (argc != 2 || !read_image(argv[1], &program)) {
        std::fprintf(stderr, "usage: %s <program.nro> <program.elf> | <program.img>\n", argv[0]);
        return 2;
    }
    CHECK(program.step_offset != 0);

    lgp::platform::webgpu::RendererOptions renderer_options;
    renderer_options.size = {256, 256};
    lgp::platform::webgpu::WebGpuRenderer renderer(renderer_options);
    if (!renderer.ok()) {
        std::printf("skipped: %s\n", renderer.error().c_str());
        return 77;
    }
    std::printf("  adapter: %s\n", renderer.adapter_name().c_str());

    const uint64_t entry = kBase + program.entry_offset;
    const uint64_t image_size = (program.bytes.size() + 0xFFF) & ~uint64_t{0xFFF};
    const uint64_t sym_step = kBase + program.step_offset;

    ProcessConfig cfg;
    cfg.total_memory_size = 16 * kMiB;
    cfg.custom_layout = true;
    cfg.layout = Layout::packed(kBase, 2 * kMiB, 1 * kMiB, 1 * kMiB, 1 * kMiB, 8 * kMiB);
    Kernel k(cfg);
    set_active_kernel(&k);
    const Layout& l = k.memory().layout();
    CHECK(image_size + 0x2000 <= l.code.size);
    CHECK(k.map_owned(l.code.base, image_size, MemState::Code, kPermRW).ok());
    std::memcpy(k.memory().host_ptr(l.code.base, image_size), program.bytes.data(), program.bytes.size());
    for (const Image::Segment& s : program.segments) {
        if (s.size == 0) continue;
        const uint64_t lo_off = s.offset & ~uint64_t{0xFFF};
        const uint64_t hi_off = (s.offset + s.size + 0xFFF) & ~uint64_t{0xFFF};
        k.memory().set_perm(l.code.base + lo_off, hi_off - lo_off, s.perm);
    }
    CHECK(k.map_owned(l.stack.end() - kStackSize, kStackSize, MemState::Stack, kPermRW).ok());
    register_functions(lgp_lifted_entries, lgp_lifted_entries_count);

    KernelMemory memory(k);
    lgp::gpu::webgpu::WebGpuBackend backend(renderer);
    backend.set_guest_memory(&memory);
    nvn::HleOptions options;
    options.thunk_base = l.code.base + image_size;  // directly behind the module (hle.hpp)
    options.trace = std::getenv("LGP_NVN_TRACE") != nullptr;
    // LGP_WEBGPU_ASYNC_EXECUTE: the guest runs ahead of the backend.
    std::unique_ptr<nvn::AsyncBackend> async;
    if (const char* v = std::getenv("LGP_WEBGPU_ASYNC_EXECUTE"); v != nullptr && std::atoi(v) > 0)
        async = std::make_unique<nvn::AsyncBackend>(backend);
    nvn::Hle hle(options, async ? static_cast<nvn::Backend&>(*async) : backend);
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
    int32_t step = -1;
    std::memcpy(&step, k.memory().host_ptr(sym_step, 4), 4);
    CHECK_EQ(step, 0);

    if (async)
        async->drain();
    CHECK_EQ(renderer.frames(), 1u);
    std::vector<uint8_t> img;
    uint32_t w = 0, h = 0;
    CHECK(backend.read_presented(&img, &w, &h));
    CHECK_EQ(w, 64u);
    CHECK_EQ(h, 64u);
    if (w == 64 && h == 64) {
        // Memory row 0 = top (NDC +1). Top-left: green in front of red.
        CHECK(pixel_is(img, 64, 8, 8, 0, 255, 0, 255));
        CHECK(pixel_is(img, 64, 24, 24, 0, 255, 0, 255));  // red quad behind: depth test
        CHECK(pixel_is(img, 64, 40, 8, 255, 0, 0, 255));   // red where nothing is in front
        CHECK(pixel_is(img, 64, 56, 24, 51, 51, 51, 255)); // clear colour
        // Bottom-left: the clockwise blue quad is culled.
        CHECK(pixel_is(img, 64, 8, 48, 51, 51, 51, 255));
        CHECK(pixel_is(img, 64, 24, 40, 51, 51, 51, 255));
        // Bottom-right: 2x2 checker, nearest sampling, indexed draw.
        CHECK(pixel_is(img, 64, 36, 36, 255, 0, 0, 255));
        CHECK(pixel_is(img, 64, 60, 36, 0, 255, 0, 255));
        CHECK(pixel_is(img, 64, 36, 60, 0, 0, 255, 255));
        CHECK(pixel_is(img, 64, 60, 60, 255, 255, 255, 255));
        // Top-right: white at alpha 0.5 over grey 51: 0.5 * 255 + 0.5 * 51 = 153.
        CHECK(pixel_is(img, 64, 56, 4, 153, 153, 153, 128, 1));
    }
    const auto st = backend.stats();
    std::printf("  draws %" PRIu64 ", skipped %" PRIu64 ", copies %" PRIu64 ", uploads %" PRIu64
                ", unbound textures %" PRIu64 "\n",
                st.draws, st.skipped_commands, st.copies, st.uploads, st.unbound_textures);
    std::printf("  dispatches %" PRIu64 "\n", st.dispatches);
    for (const auto& [name, count] : st.diagnostics)  // LGP_WEBGPU_DEBUG=computeoverlap
        std::printf("  diag %" PRIu64 ": %s\n", count, name.c_str());
    CHECK_EQ(st.draws, 5u);
    CHECK_EQ(st.dispatches, 2u);
    // Synchronous write-back (the default): execute() waited for each one.
    CHECK_EQ(st.writeback_waits, st.writebacks);
    CHECK_EQ(st.skipped_commands, 0u);
    CHECK_EQ(st.copies, 1u);
    CHECK_EQ(st.uploads, 1u);
    CHECK_EQ(st.unbound_textures, 0u);
    CHECK_EQ(st.invalid_programs, 0u);
    CHECK_EQ(st.invalid_pipelines, 0u);
    CHECK_EQ(renderer.error_count(), 0u);
    CHECK(hle.unimplemented_calls().empty());
    set_active_kernel(nullptr);
    const int failed = finish();
    if (failed == 0)
        std::printf("nvn triangle: ok\n");
    return failed;
}
