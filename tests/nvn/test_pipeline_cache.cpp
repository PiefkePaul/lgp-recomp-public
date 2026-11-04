// Pipeline cache of the WebGPU backend (lgp/gpu/webgpu/pipeline_cache.hpp).
//
//   lgp_test_pipeline_cache file <source dir>   the file alone (no GPU)
//   lgp_test_pipeline_cache gpu                 record in one backend, create
//                                               ahead in the next (worker
//                                               thread and createRenderPipelineAsync)
//
// The files go to the system's temporary directory and are removed again.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "../runtime/check.hpp"
#include "lgp/gpu/webgpu/pipeline_cache.hpp"
#include "lgp/gpu/webgpu/webgpu_backend.hpp"
#if defined(LGP_WEBGPU_WGPU_NATIVE)
#include <webgpu/wgpu.h>  // WGPU_LGP_PIPELINE_CACHE
#endif

namespace fs = std::filesystem;
namespace nvn = lgp::gpu::nvn;
namespace pw = lgp::platform::webgpu;
using lgp::gpu::webgpu::kPipelineCacheFileLimit;
using lgp::gpu::webgpu::PipelineCacheFile;
using lgp::gpu::webgpu::WebGpuBackend;

namespace {

constexpr int kSkip = 77;

// A fresh directory per call: ctest runs the modes in parallel, and a name
// from the clock alone collided under Windows (both runs then shared one
// cache file). create_directory() only succeeds for the caller that made it.
fs::path temp_dir() {
    std::random_device rd;
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    for (;;) {
        const fs::path d = fs::temp_directory_path() /
                           ("lgp-pipeline-cache-" + std::to_string(tick) + "-" + std::to_string(rd()));
        if (fs::create_directory(d))
            return d;
    }
}

void set_env(const char* name, const char* value) {
#if defined(_MSC_VER)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

int test_file(const std::string& source_dir) {
    const fs::path dir = temp_dir();
    const std::string path = (dir / "sub" / "p.lgppipes").string();
    std::string error;
    {
        PipelineCacheFile f;
        CHECK(!f.open((dir / "p.bin").string(), "fp", &error));  // wrong extension
        CHECK(!f.open(source_dir + "/build/p.lgppipes", "fp", &error));  // inside the repository
        CHECK(error.find("source tree") != std::string::npos);
        CHECK(lgp::gpu::webgpu::path_inside(source_dir + "/a/../b/x", source_dir));
        CHECK(!lgp::gpu::webgpu::path_inside(source_dir + "-other/x", source_dir));
    }
    {
        PipelineCacheFile f;
        CHECK(f.open(path, "fp1", &error));  // creates the directory and the file
        CHECK_EQ(f.loaded(), 0u);
        f.add(7, "recipe a");
        f.add(7, "recipe b");
        f.add(7, "recipe a");  // duplicate
        f.add(9, "recipe a");
        f.add_program(7, "program seven");
        f.add_program(7, "program seven");  // duplicate
        f.flush();
        CHECK_EQ(f.written(), 4u);
    }
    {
        PipelineCacheFile f;
        CHECK(f.open(path, "fp1", &error));
        CHECK_EQ(f.loaded(), 4u);
        CHECK_EQ(f.recipes().at(7).size(), 2u);
        CHECK_EQ(f.program_count(), 1u);
        std::string text;
        CHECK(f.program(7, &text) && text == "program seven");
        CHECK_EQ(f.program_order().size(), 1u);
        CHECK(f.recipes().at(9).at(0) == "recipe a");
        // Positions in the order written: 7a, 7b, 9a, program 7.
        CHECK(f.recipe_positions().at(7) == std::vector<uint32_t>({0, 1}));
        CHECK(f.recipe_positions().at(9) == std::vector<uint32_t>({2}));
        CHECK_EQ(f.program_positions().at(7), 3u);
        f.add(7, "recipe a");  // known from the file
        f.flush();
        CHECK_EQ(f.written(), 0u);
        // After warming the program texts go; recipes and known records stay.
        CHECK(f.held().programs > 0);
        f.release_programs();
        CHECK(!f.program(7, &text));
        CHECK(f.program_order().empty());
        CHECK_EQ(f.held().programs, 0u);
        CHECK_EQ(f.recipes().at(7).size(), 2u);
        f.add_program(7, "program seven");  // still known: not written again
        f.flush();
        CHECK_EQ(f.written(), 0u);
    }
    // A damaged tail (crash while appending) is cut off, the rest kept.
    {
        FILE* out = std::fopen(path.c_str(), "ab");
        CHECK(out != nullptr);
        if (out != nullptr) {
            std::fwrite("\x01\x02\x03", 1, 3, out);
            std::fclose(out);
        }
        PipelineCacheFile f;
        CHECK(f.open(path, "fp1", &error));
        CHECK_EQ(f.loaded(), 4u);
        f.add(11, "recipe c");
        f.flush();
        PipelineCacheFile g;
        CHECK(g.open(path, "fp1", &error));
        CHECK_EQ(g.loaded(), 5u);
    }
    // Another GPU or driver: starts over.
    {
        PipelineCacheFile f;
        CHECK(f.open(path, "fp2", &error));
        CHECK_EQ(f.loaded(), 0u);
        PipelineCacheFile g;
        CHECK(g.open(path, "fp1", &error));
        CHECK_EQ(g.loaded(), 0u);
    }
    // A failed append keeps its records for the next flush.
    {
        const std::string other = (dir / "other.lgppipes").string();
        PipelineCacheFile f;
        CHECK(f.open(other, "fp1", &error));
        f.add(3, "recipe d");
        std::error_code ec;
        fs::remove(other, ec);
        fs::create_directory(other, ec);  // the append fails
        f.flush();
        CHECK_EQ(f.written(), 0u);
        // Moved away rather than removed: Emscripten's remove() tries unlink
        // first and only falls back to rmdir on EISDIR, but macOS reports
        // EPERM for a directory, so the directory would stay (Node on macOS).
        fs::rename(other, other + ".dir", ec);
        CHECK(!ec);
        f.flush();
        CHECK_EQ(f.written(), 1u);
    }
    // A seed from another device: taken over whatever its fingerprint, read
    // again at every start and never copied into this file; known records
    // only once.
    {
        const std::string seed = (dir / "seed.lgppipes").string();
        {
            PipelineCacheFile f;
            CHECK(f.open(seed, "other device", &error));
            f.add(21, "recipe s");
            f.add(22, "recipe t");
            f.add_program(21, "program twenty-one");
            f.add_program(23, std::string(4, '\0') + "compute program");  // empty vertex WGSL: compute
            f.add_program(24, "graphics program nobody drew with");
            f.flush();
        }
        const std::string mine = (dir / "mine.lgppipes").string();
        {
            PipelineCacheFile f;
            CHECK(f.open(mine, "fp1", &error));
            f.add(22, "recipe t");  // already known here
            f.flush();
        }
        std::error_code size_ec;
        const uint64_t mine_size = fs::file_size(mine, size_ec);
        for (int run = 0; run < 2; ++run) {
            PipelineCacheFile f;
            CHECK(f.open(mine, "fp1", &error));
            CHECK_EQ(f.loaded(), 1u);
            error.clear();
            CHECK_EQ(f.import_records(seed, &error), 4u);  // recipe s and three programs
            CHECK(error.empty());
            CHECK_EQ(f.loaded(), 5u);
            CHECK_EQ(f.program_count(), 3u);
            std::string text;
            CHECK(f.program(21, &text) && text == "program twenty-one");
            CHECK(f.program(24, &text) && text == "graphics program nobody drew with");
            CHECK(!f.program(25, &text));
            // Created ahead: the one with a recipe and the compute one, in file order.
            CHECK(f.program_order() == std::vector<uint64_t>({21, 23}));
            CHECK(f.recipes().at(21).at(0) == "recipe s");
            CHECK(f.recipe_positions().at(22) == std::vector<uint32_t>({0}));  // the file's own first
            CHECK(f.recipe_positions().at(21).at(0) >= 1u);
            CHECK(f.import_records(seed, &error) == 0 && !error.empty());  // one seed per run
            f.add_program(21, "program twenty-one");  // in the seed: not copied
            f.add(21, "recipe s");
            f.flush();
            CHECK_EQ(f.written(), 0u);
            CHECK(fs::file_size(mine, size_ec) == mine_size);
            f.release_programs();
            CHECK(!f.program(21, &text));
            f.add_program(21, "program twenty-one");  // still known after the texts went
            f.flush();
            CHECK_EQ(f.written(), 0u);
        }
        {
            PipelineCacheFile f;
            CHECK(f.open(mine, "fp1", &error));
            CHECK(f.import_records((dir / "missing.lgppipes").string(), &error) == 0 && !error.empty());
            error.clear();
            CHECK(f.import_records((dir / "sub").string(), &error) == 0 && !error.empty());  // a directory
        }
        {
            // A seed above 32 MiB (a whole catalog):
            // taken whole, the own file stays small and keeps its records.
            const std::string big_seed = (dir / "big_seed.lgppipes").string();
            const std::string full = (dir / "full.lgppipes").string();
            const std::string blob(size_t(17) << 20, 'x');
            {
                PipelineCacheFile f;
                CHECK(f.open(big_seed, "other device", &error));
                f.add(31, blob + "seed");
                f.add(32, "small seed recipe");
                f.add_program(32, blob + "seed program");
                f.flush();
            }
            {
                PipelineCacheFile f;
                CHECK(f.open(full, "fp1", &error));
                f.add(33, blob + "mine");
                f.flush();
            }
            {
                PipelineCacheFile f;
                CHECK(f.open(full, "fp1", &error));
                CHECK_EQ(f.loaded(), 1u);
                error.clear();
                CHECK_EQ(f.import_records(big_seed, &error), 3u);
                CHECK(error.empty());
                std::string text;
                CHECK(f.program(32, &text) && text == blob + "seed program");
                CHECK(f.held().programs < (size_t(1) << 20));  // the seed's text stays in its file
                f.flush();
            }
            CHECK(fs::file_size(full, size_ec) <= kPipelineCacheFileLimit);
            PipelineCacheFile f;
            CHECK(f.open(full, "fp1", &error));
            CHECK_EQ(f.loaded(), 1u);
            CHECK(f.recipes().count(33) == 1);
        }
        {
            // One run's own records above 32 MiB (LGP on Android wrote ~33 MiB):
            // the next run loads them. Above the limit the file starts over.
            const std::string own = (dir / "own.lgppipes").string();
            const std::string part(size_t(20) << 20, 'y');
            {
                PipelineCacheFile f;
                CHECK(f.open(own, "fp1", &error));
                f.add(41, part + "a");
                f.add_program(41, part + "b");
                f.flush();
            }
            CHECK(fs::file_size(own, size_ec) > (uint64_t(32) << 20));
            {
                PipelineCacheFile f;
                CHECK(f.open(own, "fp1", &error));
                CHECK_EQ(f.loaded(), 2u);
                f.add(42, part + "c");
                f.add(43, part + "d");
                f.flush();
            }
            CHECK(fs::file_size(own, size_ec) > kPipelineCacheFileLimit);
            PipelineCacheFile f;
            CHECK(f.open(own, "fp1", &error));
            CHECK_EQ(f.loaded(), 0u);
            CHECK(fs::file_size(own, size_ec) < (uint64_t(1) << 20));
        }
        {
            // Not a pipeline cache.
            const std::string junk = (dir / "junk.lgppipes").string();
            FILE* out = std::fopen(junk.c_str(), "wb");
            CHECK(out != nullptr);
            if (out != nullptr) {
                std::fwrite("NOTPIPE1\0\0\0\0", 1, 12, out);
                std::fclose(out);
            }
            PipelineCacheFile f;
            CHECK(f.open(mine, "fp1", &error));
            error.clear();
            CHECK_EQ(f.import_records(junk, &error), 0u);
            CHECK(error.find("no pipeline cache") != std::string::npos);
        }
    }
    // The magic the pre-commit guard looks for.
    {
        char head[8] = {};
        FILE* in = std::fopen(path.c_str(), "rb");
        CHECK(in != nullptr);
        if (in != nullptr) {
            CHECK_EQ(std::fread(head, 1, 8, in), 8u);
            std::fclose(in);
        }
        CHECK(std::memcmp(head, "LGPPIPE1", 8) == 0);
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
    return finish();
}

// Full-screen triangle from one vertex stream, constant colour.
// Draws only with the depth mode override set: a pipeline created ahead from
// a recipe that lost the list's depth mode draws nothing.
const char* kVertexWgsl =
    "override lgp_depth_minus_w: bool = false;\n"
    "@vertex fn main(@location(0) a0: vec4<f32>) -> @builtin(position) vec4<f32> {\n"
    "  if (lgp_depth_minus_w) { return a0; }\n"
    "  return vec4<f32>(0.0);\n"
    "}\n";
// Draws red only with the list's alpha test constants (Greater, 0.5): a
// pipeline created ahead without them discards everything.
const char* kRedWgsl =
    "override lgp_alpha_func: u32 = 7u;\n"
    "override lgp_alpha_ref: f32 = 0.0;\n"
    "@fragment fn main() -> @location(0) vec4<f32> {\n"
    "  if (lgp_alpha_func != 4u || lgp_alpha_ref != 0.5) { discard; }\n"
    "  return vec4<f32>(1.0, 0.0, 0.0, 1.0);\n"
    "}\n";
const char* kComputeWgsl = "@compute @workgroup_size(1) fn main() {}\n";
const char* kGreenWgsl = "@fragment fn main() -> @location(0) vec4<f32> { return vec4<f32>(0.0, 1.0, 0.0, 1.0); }\n";

class BlockMemory final : public nvn::GuestMemoryView {
public:
    static constexpr uint64_t kBase = 0x1000'0000;
    std::vector<uint8_t> bytes = std::vector<uint8_t>(0x1000, 0);
    bool read(uint64_t va, void* dst, uint64_t size) const override {
        if (va < kBase || va - kBase + size > bytes.size())
            return false;
        std::memcpy(dst, bytes.data() + (va - kBase), size);
        return true;
    }
    bool write(uint64_t, const void*, uint64_t) const override { return false; }
};

struct Run {
    WebGpuBackend::Stats stats;
    bool red = false;  // both targets drawn red
};

// One "game run": a backend with the cache, program 1, two draws into
// targets of different formats (two pipelines), and a dispatch of compute program 2. `presents`
// presents before the program is created (cached programs are created ahead after presents);
// without `wait`, the list runs while prebuilds may still be under way.
Run run(pw::WebGpuRenderer& renderer, const std::string& cache, const char* fragment, bool recreate_program,
        int presents = 0, bool wait = true, const std::string& seed = {}, bool held = false) {
    // Steps on stdout: a hang (browser test timeout) shows where it stopped.
    auto step = [](const char* what) {
        std::printf("  step: %s\n", what);
        std::fflush(stdout);
    };
    BlockMemory memory;
    const float tri[12] = {-1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1};
    std::memcpy(memory.bytes.data(), tri, sizeof(tri));
    WebGpuBackend backend(renderer);
    backend.set_guest_memory(&memory);
    if (held)  // app in the background before the cache is set
        CHECK_EQ(backend.hold_prebuilds(true), 0u);
    std::string error;
    CHECK(backend.set_pipeline_cache(cache, &error, seed));
    nvn::TextureDesc td;
    td.target = uint32_t(nvn::TextureTarget::Target2D);
    td.width = td.height = 4;
    td.format = uint32_t(nvn::Format::RGBA8);
    backend.create_texture(1, td);
    td.format = uint32_t(nvn::Format::BGRA8);
    backend.create_texture(2, td);
    for (int i = 0; i < presents; ++i) {
        nvn::PresentInfo info;
        info.texture = 1;
        backend.present(info);
    }
    if (presents < 0) {  // the app's graphics screen: all programs in slices, then their pipelines
        step("warm in slices");
        CHECK(backend.prebuild_progress().total == 0);
        for (int slices = 0; backend.warm_cached_programs(1) > 0; ++slices)
            CHECK(slices < 10);
        CHECK_EQ(backend.warm_cached_programs(1), 0u);
        backend.wait_prebuilds();
        const auto p = backend.prebuild_progress();
        CHECK(p.total >= 4u);  // two programs checked, two draw pipelines and one compute pipeline
        CHECK_EQ(p.done, p.total);
    }
    if (wait) {
        step("wait for warm programs");
        backend.wait_prebuilds();
    }
    step("create programs");
    nvn::ProgramDesc desc{kVertexWgsl, fragment};
    backend.create_program(1, desc);
    if (recreate_program) {  // prebuilds of the first one are stale
        backend.destroy_program(1);
        backend.create_program(1, desc);
    }
    step("create compute program");
    nvn::ProgramDesc compute;
    compute.compute_wgsl = kComputeWgsl;
    backend.create_program(2, compute);
    if (wait) {
        step("wait for prebuilds");
        backend.wait_prebuilds();
    }
    nvn::CommandList list;
    list.depth_minus_w = true;  // part of the recipe
    nvn::CmdSetVertexLayout layout;
    layout.attribs = {{0, nvn::VertexFormat::Float32x4, 0, 0}};
    layout.strides = {16};
    list.commands.push_back(nvn::CmdBindProgram{1});
    list.commands.push_back(nvn::CmdSetAlphaTest{nvn::CompareFunc::Greater});  // part of the recipe
    list.commands.push_back(nvn::CmdSetAlphaRef{0.5f});
    list.commands.push_back(layout);
    list.commands.push_back(nvn::CmdBindVertexBuffer{0, BlockMemory::kBase, sizeof(tri)});
    for (nvn::TextureId t : {1u, 2u}) {
        list.commands.push_back(nvn::CmdSetRenderTargets{{t}, 0});
        list.commands.push_back(nvn::CmdSetViewports{0, {0, 0, 4, 4}});
        list.commands.push_back(nvn::CmdClearColor{0, {0, 0, 1, 1}, nvn::kClearColorMaskRGBA});
        list.commands.push_back(nvn::CmdDraw{nvn::Topology::Triangles, 0, 3, 0, 1});
    }
    list.commands.push_back(nvn::CmdBindProgram{2, true});
    list.commands.push_back(nvn::CmdDispatchCompute{});
    step("execute");
    backend.execute(list);
    if (held) {  // back in the foreground: whatever is left runs
        backend.hold_prebuilds(false);
        backend.wait_prebuilds();
    }
    step("read back");
    Run r;
    r.red = true;
    for (nvn::TextureId t : {1u, 2u}) {
        std::vector<uint8_t> px;
        CHECK(backend.read_texture(t, &px, nullptr, nullptr));
        const uint8_t want_r = t == 1 ? 255 : 0, want_b = t == 1 ? 0 : 255;  // BGRA bytes in memory
        r.red &= px.size() == 4 * 4 * 4 && px[0] == want_r && px[2] == want_b;
    }
    r.stats = backend.stats();
    step("tear down");
    return r;
}

int test_gpu() {
    pw::RendererOptions options;
    options.size = {64, 64};
    pw::WebGpuRenderer renderer(options);
    if (!renderer.ok()) {
        std::printf("skipped: %s\n", renderer.error().c_str());
        return kSkip;
    }
    const fs::path dir = temp_dir();
    for (const char* mode : {"thread", "async"}) {
        std::printf("prebuilds: %s\n", mode);
        set_env("LGP_WEBGPU_PREBUILD", mode);
        // Cached programs warmed after presents; the warm-all start (browser
        // default) is checked below.
        set_env("LGP_WEBGPU_WARM_ALL", "0");
        const std::string cache = (dir / (std::string(mode) + ".lgppipes")).string();
        // First run: nothing cached, both pipelines created on use and recorded.
        Run first = run(renderer, cache, kRedWgsl, false);
        CHECK(first.red);
        CHECK_EQ(first.stats.pipeline_cache_records, 0u);
        CHECK_EQ(first.stats.draw_pipelines_created, 2u);
        CHECK_EQ(first.stats.pipelines_prebuilt, 0u);
        CHECK_EQ(first.stats.dispatches, 1u);
#if defined(WGPU_LGP_PIPELINE_CACHE)
        // The driver's pipeline cache (Vulkan only): empty at first, written
        // at the end and loaded by the next run.
        const bool driver_cache = first.stats.driver_cache_loaded >= 0;
        std::printf("driver pipeline cache: %s\n", driver_cache ? "yes" : "no");
        if (driver_cache)
            CHECK_EQ(first.stats.driver_cache_loaded, 0);
#endif
        // Second run, no present before the program: its pipelines are
        // created ahead once it exists, none on use.
        Run second = run(renderer, cache, kRedWgsl, false);
        CHECK(second.red);
        CHECK_EQ(second.stats.pipeline_cache_records, 2u);
        CHECK_EQ(second.stats.pipeline_cache_programs, 2u);  // with the compute program
        CHECK_EQ(second.stats.programs_adopted, 0u);
        CHECK_EQ(second.stats.draw_pipelines_created, 0u);
        CHECK_EQ(second.stats.pipelines_prebuilt, 2u);
        CHECK_EQ(second.stats.prebuilds_failed, 0u);
#if !defined(__EMSCRIPTEN__)
        // The browser settles the error scope around the prebuilds' preparation
        // after the call, wgpu-native inside it; LGP_WEBGPU_PREBUILD_POP
        // settles it as the browser does, once the next list waits.
        const char* async_pipelines = std::getenv("LGP_WEBGPU_ASYNC_PIPELINES");
        const char* nowait = std::getenv("LGP_WEBGPU_PIPELINE_NOWAIT");
        if (std::string(mode) == "async" && async_pipelines != nullptr && std::string(async_pipelines) == "1" &&
            (nowait == nullptr || std::string(nowait) == "0")) {
            // Scheduled, not yet handed on: the list waits for them instead of
            // creating the same pipelines itself.
            set_env("LGP_WEBGPU_PREBUILD_POP", "late");
            Run late = run(renderer, cache, kRedWgsl, false, 0, false);
            CHECK(late.red);
            CHECK_EQ(late.stats.draw_pipelines_created, 0u);
            CHECK_EQ(late.stats.pipelines_prebuilt, 2u);
            CHECK_EQ(late.stats.invalid_pipelines, 0u);
            // Given up (scope invalid) while the list waits for them: it
            // creates them itself and draws, as if none had been scheduled.
            set_env("LGP_WEBGPU_PREBUILD_POP", "fail");
            Run failed = run(renderer, cache, kRedWgsl, false, 0, false);
            CHECK(failed.red);
            CHECK_EQ(failed.stats.draw_pipelines_created, 2u);
            CHECK_EQ(failed.stats.pipelines_prebuilt, 0u);
            CHECK_EQ(failed.stats.invalid_pipelines, 0u);
            set_env("LGP_WEBGPU_PREBUILD_POP", "");
        }
#endif
#if defined(WGPU_LGP_PIPELINE_CACHE)
        if (driver_cache) {
            std::error_code ec;
            const auto bytes = fs::file_size(cache + ".driver", ec);
            CHECK(!ec);
            CHECK(bytes > 0);
            CHECK(second.stats.driver_cache_loaded > 0);
            // A damaged file (last byte flipped) fails the checksum: the
            // cache starts empty instead of handing it to the driver, and
            // the end of the run writes a good one again.
            if (FILE* f = std::fopen((cache + ".driver").c_str(), "r+b")) {
                std::fseek(f, -1, SEEK_END);
                const int c = std::fgetc(f);
                std::fseek(f, -1, SEEK_END);
                std::fputc(c ^ 0xff, f);
                std::fclose(f);
            }
            Run damaged = run(renderer, cache, kRedWgsl, false);
            CHECK(damaged.red);
            CHECK_EQ(damaged.stats.driver_cache_loaded, 0);
            Run repaired = run(renderer, cache, kRedWgsl, false);
            CHECK(repaired.stats.driver_cache_loaded > 0);
        }
#endif
        // A fresh cache seeded from the first one as if another device wrote
        // it (other fingerprint): the same pipelines created ahead; the seed
        // is read again in the next run and not copied into the fresh cache.
        {
            const std::string seed = (dir / (std::string(mode) + "-seed.lgppipes")).string();
            const std::string fresh = (dir / (std::string(mode) + "-fresh.lgppipes")).string();
            std::error_code ec;
            fs::copy_file(cache, seed, fs::copy_options::overwrite_existing, ec);
            CHECK(!ec);
            if (FILE* f = std::fopen(seed.c_str(), "r+b")) {
                std::fseek(f, 12, SEEK_SET);  // first fingerprint byte
                std::fputc('R', f);
                std::fclose(f);
            }
#if defined(WGPU_LGP_PIPELINE_CACHE)
            // The driver's cache beside the seed starts the fresh one.
            if (driver_cache) {
                fs::copy_file(cache + ".driver", seed + ".driver", fs::copy_options::overwrite_existing, ec);
                CHECK(!ec);
            }
#endif
            Run seeded = run(renderer, fresh, kRedWgsl, false, 0, true, seed);
            CHECK(seeded.red);
#if defined(WGPU_LGP_PIPELINE_CACHE)
            if (driver_cache) {
                CHECK(seeded.stats.driver_cache_loaded > 0);
                CHECK(fs::is_regular_file(fresh + ".driver", ec));  // written to its own file
            }
#endif
            CHECK_EQ(seeded.stats.pipeline_cache_seeded, 4u);  // two recipes, two programs
            CHECK_EQ(seeded.stats.draw_pipelines_created, 0u);
            CHECK_EQ(seeded.stats.pipelines_prebuilt, 2u);
            Run again = run(renderer, fresh, kRedWgsl, false, 0, true, seed);
            CHECK(again.red);
            CHECK_EQ(again.stats.pipeline_cache_seeded, 4u);  // from the seed again
            CHECK_EQ(again.stats.pipeline_cache_records, 2u);
            std::error_code size_ec;
            CHECK(fs::file_size(fresh, size_ec) < fs::file_size(seed, size_ec));  // own records only
            CHECK_EQ(again.stats.pipelines_prebuilt, 2u);
        }
        // With presents first, the programs themselves are created ahead
        // (the compute one too) and taken over with their pipelines.
        Run warm = run(renderer, cache, kRedWgsl, false, 2);
        CHECK(warm.red);
        CHECK_EQ(warm.stats.programs_warmed, 2u);
        CHECK_EQ(warm.stats.programs_adopted, 2u);
        CHECK_EQ(warm.stats.draw_pipelines_created, 0u);
        CHECK_EQ(warm.stats.pipelines_prebuilt, 2u);
        // The warm compute program's pipeline came from the background.
        CHECK_EQ(warm.stats.compute_pipelines_prebuilt, 1u);
        CHECK_EQ(warm.stats.dispatches, 1u);
        // As the app's graphics screen does before the game: the same, without presents.
        Run sliced = run(renderer, cache, kRedWgsl, false, -1);
        CHECK(sliced.red);
        CHECK_EQ(sliced.stats.programs_warmed, 2u);
        CHECK_EQ(sliced.stats.programs_adopted, 2u);
        CHECK_EQ(sliced.stats.draw_pipelines_created, 0u);
        CHECK_EQ(sliced.stats.pipelines_prebuilt, 2u);
        // Without waiting: whatever is not done yet is waited for, taken
        // back or created on use, each pipeline exactly once.
        Run hurried = run(renderer, cache, kRedWgsl, false, 2, false);
        CHECK(hurried.red);
        // (In the browser a warm program not yet validated is not taken over.)
        CHECK(hurried.stats.programs_adopted <= 2u);
        CHECK_EQ(hurried.stats.draw_pipelines_created + hurried.stats.pipelines_prebuilt, 2u);
        CHECK_EQ(hurried.stats.dispatches, 1u);
        CHECK_EQ(hurried.stats.invalid_pipelines, 0u);
        // The program recreated before use: the first one's pipelines go
        // with it, the new program's are created ahead again.
        Run third = run(renderer, cache, kRedWgsl, true, 2);
        CHECK(third.red);
        CHECK_EQ(third.stats.programs_adopted, 2u);  // the first program 1 and the compute program
        CHECK_EQ(third.stats.draw_pipelines_created, 0u);
        CHECK_EQ(third.stats.pipelines_prebuilt, 4u);
        // All cached programs warmed when the cache is set: taken over
        // without any present.
        set_env("LGP_WEBGPU_WARM_ALL", "1");
        Run all = run(renderer, cache, kRedWgsl, false);
        CHECK(all.red);
        CHECK_EQ(all.stats.programs_warmed, 2u);
        CHECK_EQ(all.stats.programs_adopted, 2u);
        CHECK_EQ(all.stats.draw_pipelines_created, 0u);
        CHECK_EQ(all.stats.pipelines_prebuilt, 2u);
        CHECK_EQ(all.stats.compute_pipelines_prebuilt, 1u);
        CHECK_EQ(all.stats.dispatches, 1u);
        Run all_hurried = run(renderer, cache, kRedWgsl, false, 0, false);
        CHECK(all_hurried.red);
        CHECK(all_hurried.stats.programs_adopted <= 2u);
        CHECK_EQ(all_hurried.stats.draw_pipelines_created + all_hurried.stats.pipelines_prebuilt, 2u);
        CHECK_EQ(all_hurried.stats.dispatches, 1u);
        CHECK_EQ(all_hurried.stats.invalid_pipelines, 0u);
#if !defined(__EMSCRIPTEN__)  // worker threads only on the desktop
        const char* async_env = std::getenv("LGP_WEBGPU_ASYNC_PIPELINES");
        if (std::string(mode) == "thread" && (async_env == nullptr || std::string(async_env) == "0")) {
            // Held workers (app in the background) start nothing: the list
            // creates its pipelines itself.
            Run held = run(renderer, cache, kRedWgsl, false, 0, false, {}, true);
            CHECK(held.red);
            CHECK_EQ(held.stats.pipelines_prebuilt, 0u);
            CHECK_EQ(held.stats.compute_pipelines_prebuilt, 0u);
            CHECK_EQ(held.stats.invalid_pipelines, 0u);
            CHECK_EQ(held.stats.programs_warmed, 0u);  // warm-all stopped too
            // Released: the next present warms what the start left out.
            BlockMemory memory;
            WebGpuBackend backend(renderer);
            backend.set_guest_memory(&memory);
            backend.hold_prebuilds(true);
            CHECK(backend.set_pipeline_cache(cache));
            CHECK_EQ(backend.stats().programs_warmed, 0u);
            nvn::TextureDesc td;
            td.target = uint32_t(nvn::TextureTarget::Target2D);
            td.width = td.height = 4;
            td.format = uint32_t(nvn::Format::RGBA8);
            backend.create_texture(1, td);
            backend.hold_prebuilds(false);
            nvn::PresentInfo info;
            info.texture = 1;
            backend.present(info);
            backend.wait_prebuilds();
            CHECK_EQ(backend.stats().programs_warmed, 2u);
        }
#endif
        set_env("LGP_WEBGPU_WARM_ALL", "0");
        // Other WGSL: another program hash, nothing from the cache.
        Run other = run(renderer, cache, kGreenWgsl, false);
        CHECK(!other.red);
        CHECK_EQ(other.stats.draw_pipelines_created, 2u);
        CHECK_EQ(other.stats.pipelines_prebuilt, 0u);
        CHECK_EQ(other.stats.pipeline_cache_records, 2u);
        CHECK_EQ(other.stats.programs_adopted, 0u);  // no present: nothing warm
    }
    CHECK_EQ(renderer.error_count(), 0u);
    std::error_code ec;
    fs::remove_all(dir, ec);
    return finish();
}

}  // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "file" && argc > 2)
        return test_file(argv[2]);
    if (mode == "gpu")
        return test_gpu();
    std::fprintf(stderr, "usage: lgp_test_pipeline_cache file <source dir> | gpu\n");
    return 2;
}
