// P4.4: the WebGPU backend against the CPU reference. The same command lists
// (clears with scissors in both window origin modes, channel masks, sRGB
// targets) run on SoftBackend and on WebGpuBackend; the textures read back
// from the GPU must match the reference texel by texel (±1 for rounding).
// Depth/stencil clears and present are checked for validation errors.
//
// Needs a WebGPU adapter (any GPU, or Mesa lavapipe in CI); without one the
// test reports itself skipped (exit code 77).
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "../runtime/check.hpp"
#include "lgp/gpu/nvn/backend.hpp"
#include "lgp/gpu/webgpu/webgpu_backend.hpp"
#include "lgp/platform/null_backends.hpp"
#include "lgp/platform/webgpu/webgpu_renderer.hpp"

#if defined(LGP_WEBGPU_WGPU_NATIVE)
#include <webgpu/wgpu.h>  // wgpuQueueSubmitForIndex
#endif

namespace nvn = lgp::gpu::nvn;
namespace pw = lgp::platform::webgpu;
using lgp::gpu::webgpu::WebGpuBackend;

namespace {

constexpr int kSkip = 77;

pw::WebGpuRenderer* g_renderer = nullptr;

nvn::TextureDesc color_desc(nvn::Format format, uint32_t w, uint32_t h) {
    nvn::TextureDesc d;
    d.target = uint32_t(nvn::TextureTarget::Target2D);
    d.format = uint32_t(format);
    d.width = w;
    d.height = h;
    return d;
}

// Small deterministic generator (xorshift), so failures reproduce.
struct Rng {
    uint32_t s;
    uint32_t next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    int32_t range(int32_t lo, int32_t hi) { return lo + int32_t(next() % uint32_t(hi - lo + 1)); }
    float unit() { return float(next() % 1001) / 1000.0f; }
};

// Compares texture `id` of both backends; returns the number of texels that
// differ by more than one step in any channel.
int compare(nvn::SoftBackend& soft, WebGpuBackend& gpu, nvn::TextureId id, uint32_t w, uint32_t h) {
    std::vector<uint8_t> pixels;
    uint32_t gw = 0, gh = 0;
    CHECK(gpu.read_texture(id, &pixels, &gw, &gh));
    CHECK_EQ(gw, w);
    CHECK_EQ(gh, h);
    if (gw != w || gh != h)
        return -1;
    int bad = 0;
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t ref[4];
            CHECK(soft.read_texel(id, x, y, ref));
            const uint8_t* p = pixels.data() + (size_t(y) * w + x) * 4;
            bool same = true;
            for (int c = 0; c < 4; ++c)
                same &= std::abs(int(p[c]) - int(ref[c])) <= 1;
            if (!same && bad++ < 5)
                std::printf("  texel (%u,%u): gpu %u %u %u %u, reference %u %u %u %u\n", x, y, p[0], p[1], p[2], p[3],
                            ref[0], ref[1], ref[2], ref[3]);
        }
    }
    return bad;
}

void test_device() {
    std::printf("  adapter: %s\n", g_renderer->adapter_name().c_str());
    const lgp::platform::GpuHandles h = g_renderer->gpu();
    CHECK(h.valid());
    CHECK(h.queue != nullptr);
    CHECK(h.surface == nullptr);  // headless
}

#if !defined(__EMSCRIPTEN__)
// Android's UI thread drops the surface in surfaceDestroyed (P7.3): that must
// not wait for the GPU thread, which may spend seconds on a pipeline.
void test_surface_release_while_busy() {
    std::mutex m;
    std::condition_variable cv;
    bool started = false, release = false;
    std::thread busy([&] {
        g_renderer->run_sync([&] {
            std::unique_lock lock(m);
            started = true;
            cv.notify_all();
            cv.wait_for(lock, std::chrono::seconds(10), [&] { return release; });
        });
    });
    {
        std::unique_lock lock(m);
        cv.wait(lock, [&] { return started; });
    }
    auto dropped = std::async(std::launch::async, [] { return g_renderer->replace_surface(nullptr); });
    const bool in_time = dropped.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    CHECK(in_time);  // the GPU thread is still busy
    {
        std::lock_guard lock(m);
        release = true;
    }
    cv.notify_all();
    busy.join();
    CHECK(dropped.get());
    CHECK(!g_renderer->has_surface());
}
#endif

#if defined(LGP_WEBGPU_WGPU_NATIVE)
// Call::wait_for_submission (LGP_WEBGPU_POLL_SUBMIT_INDEX): a task that maps
// the result of a small dispatch queued behind heavy work and then submits
// more heavy work returns once the dispatch is done, not after the later work.
// The run without the index is only reported. Without it run() waits for the
// latest submission (docs/analysis/39-rueckschrieb-warten-strukturell.md).
void test_wait_for_submission() {
    const WGPUDevice device = g_renderer->device();
    const WGPUQueue queue = g_renderer->queue();
    WGPUShaderModule small_module = nullptr, heavy_module = nullptr;
    WGPUComputePipeline small = nullptr, heavy = nullptr;
    WGPUBuffer out = nullptr, readback = nullptr, scratch = nullptr, scratch2 = nullptr;
    WGPUBindGroup small_group = nullptr;
    auto pipeline = [&](WGPUShaderModule module) {
        WGPUComputePipelineDescriptor d = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
        d.compute.module = module;
        d.compute.entryPoint = pw::string_view("main");
        return wgpuDeviceCreateComputePipeline(device, &d);
    };
    auto buffer = [&](uint64_t size, WGPUBufferUsage usage) {
        WGPUBufferDescriptor d = WGPU_BUFFER_DESCRIPTOR_INIT;
        d.size = size;
        d.usage = usage;
        return wgpuDeviceCreateBuffer(device, &d);
    };
    auto group = [&](WGPUComputePipeline p, WGPUBuffer b) {
        WGPUBindGroupEntry e = WGPU_BIND_GROUP_ENTRY_INIT;
        e.binding = 0;
        e.buffer = b;
        e.size = wgpuBufferGetSize(b);
        WGPUBindGroupDescriptor d = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        WGPUBindGroupLayout layout = wgpuComputePipelineGetBindGroupLayout(p, 0);
        d.layout = layout;
        d.entryCount = 1;
        d.entries = &e;
        WGPUBindGroup g = wgpuDeviceCreateBindGroup(device, &d);
        wgpuBindGroupLayoutRelease(layout);
        return g;
    };
    constexpr uint32_t kMaxGroups = 4096;
    constexpr uint32_t kMaxTotalGroups = kMaxGroups * 1024;  // a fast desktop GPU
    // `groups` workgroups of the heavy shader (in dispatches of at most
    // kMaxGroups), or the small one
    // with its read-back copy.
    auto commands = [&](uint32_t groups, bool with_small, bool second = false) {
        WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(device, nullptr);
        WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(enc, nullptr);
        WGPUBindGroup g = nullptr;
        if (with_small) {
            wgpuComputePassEncoderSetPipeline(pass, small);
            wgpuComputePassEncoderSetBindGroup(pass, 0, small_group, 0, nullptr);
            wgpuComputePassEncoderDispatchWorkgroups(pass, 1, 1, 1);
        } else {
            g = group(heavy, second ? scratch2 : scratch);
            wgpuComputePassEncoderSetPipeline(pass, heavy);
            wgpuComputePassEncoderSetBindGroup(pass, 0, g, 0, nullptr);
            for (uint32_t left = groups; left > 0;) {
                const uint32_t n = std::min(left, kMaxGroups);
                wgpuComputePassEncoderDispatchWorkgroups(pass, n, 1, 1);
                left -= n;
            }
        }
        wgpuComputePassEncoderEnd(pass);
        wgpuComputePassEncoderRelease(pass);
        if (with_small) wgpuCommandEncoderCopyBufferToBuffer(enc, out, 0, readback, 0, 16);
        WGPUCommandBuffer cb = wgpuCommandEncoderFinish(enc, nullptr);
        wgpuCommandEncoderRelease(enc);
        if (g != nullptr) wgpuBindGroupRelease(g);
        return cb;
    };
    g_renderer->run_sync([&] {
        small_module = pw::create_wgsl_module(device, "small",
                                              "@group(0) @binding(0) var<storage, read_write> b: array<u32>;\n"
                                              "@compute @workgroup_size(1) fn main() { b[0] = 42u; }");
        heavy_module = pw::create_wgsl_module(
            device, "heavy",
            "@group(0) @binding(0) var<storage, read_write> b: array<u32>;\n"
            "@compute @workgroup_size(64) fn main(@builtin(global_invocation_id) id: vec3u) {\n"
            "  var x = id.x;\n"
            "  for (var i = 0u; i < 200000u; i++) { x = x * 1664525u + 1013904223u; }\n"
            "  b[id.x] = x;\n"
            "}");
        small = pipeline(small_module);
        heavy = pipeline(heavy_module);
        out = buffer(16, WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc);
        readback = buffer(16, WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst);
        scratch = buffer(uint64_t(kMaxGroups) * 64 * 4, WGPUBufferUsage_Storage);
        scratch2 = buffer(uint64_t(kMaxGroups) * 64 * 4, WGPUBufferUsage_Storage);
        small_group = group(small, out);
    });
    CHECK(small != nullptr && heavy != nullptr);
    if (small == nullptr || heavy == nullptr) return;
    // Heavy work of at least ~150 ms on this adapter (a fast GPU gets more
    // workgroups), so "returned before it" cannot be luck.
    uint32_t groups = 1;
    double heavy_ms = 0;
    // Warm-up: the first dispatch also compiles the pipeline (lavapipe in CI:
    // 151 ms for one workgroup, which then ran in a fraction of that).
    g_renderer->run_sync([&] {
        WGPUCommandBuffer cb = commands(1, false);
        wgpuQueueSubmit(queue, 1, &cb);
        wgpuCommandBufferRelease(cb);
        wgpuDevicePoll(device, true, nullptr);
    });
    for (;;) {
        double ms = 0;
        g_renderer->run_sync([&] {
            WGPUCommandBuffer cb = commands(groups, false);
            const auto t0 = std::chrono::steady_clock::now();
            wgpuQueueSubmit(queue, 1, &cb);
            wgpuCommandBufferRelease(cb);
            wgpuDevicePoll(device, true, nullptr);
            ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        });
        if (ms >= 150 || groups >= kMaxTotalGroups) {
            heavy_ms = ms;
            std::printf("  heavy work: %u workgroups, %.0f ms\n", groups, ms);
            break;
        }
        groups = uint32_t(std::min<double>(kMaxTotalGroups, ms < 1 ? groups * 16.0 : groups * (200 / ms) + 1));
    }
    for (const bool by_index : {true, false}) {
        std::atomic<bool> heavy_done{false};
        uint32_t value = 0;
        double submit_ms = 0;  // the submits after `ahead` (lavapipe blocks them while the queue is busy)
        const auto t_run = std::chrono::steady_clock::now();
        g_renderer->run([&](pw::WebGpuRenderer::Call& call) {
            // Work ahead of the dispatch (the previous frame's draws): without
            // it the dispatch is done before the later submit, which then
            // runs the map callback itself.
            WGPUCommandBuffer ahead = commands(groups, false);
            wgpuQueueSubmit(queue, 1, &ahead);
            wgpuCommandBufferRelease(ahead);
            const auto t_submit = std::chrono::steady_clock::now();
            WGPUCommandBuffer cb = commands(0, true);
            const WGPUSubmissionIndex index = wgpuQueueSubmitForIndex(queue, 1, &cb);
            wgpuCommandBufferRelease(cb);
            WGPUBufferMapCallbackInfo map = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
            map.mode = WGPUCallbackMode_AllowSpontaneous;
            map.userdata1 = &call;
            map.callback = [](WGPUMapAsyncStatus, WGPUStringView, void* u1, void*) {
                static_cast<pw::WebGpuRenderer::Call*>(u1)->finish();
            };
            wgpuBufferMapAsync(readback, WGPUMapMode_Read, 0, 16, map);
            WGPUCommandBuffer later = commands(groups, false, true);
            wgpuQueueSubmit(queue, 1, &later);
            submit_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_submit).count();
            wgpuCommandBufferRelease(later);
            WGPUQueueWorkDoneCallbackInfo done = WGPU_QUEUE_WORK_DONE_CALLBACK_INFO_INIT;
            done.mode = WGPUCallbackMode_AllowSpontaneous;
            done.userdata1 = &heavy_done;
            done.callback = [](WGPUQueueWorkDoneStatus, WGPUStringView, void* u1, void*) {
                static_cast<std::atomic<bool>*>(u1)->store(true);
            };
            wgpuQueueOnSubmittedWorkDone(queue, done);
            if (by_index) call.wait_for_submission(index);
        });
        const bool returned_before_heavy = !heavy_done.load();
        const double run_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_run).count();
        g_renderer->run_sync([&] {
            const void* data = wgpuBufferGetConstMappedRange(readback, 0, 16);
            if (data != nullptr) std::memcpy(&value, data, sizeof(value));
            wgpuBufferUnmap(readback);
            while (!heavy_done.load()) wgpuDevicePoll(device, true, nullptr);
        });
        // A driver that runs one submission at a time (Mesa lavapipe) blocks
        // the submits after `ahead` until the queue is free; the map callback then
        // finishes the call before run() polls, with or without the index.
        // Heavy work too short to tell (calibration capped) counts the same,
        // and so does LGP_GPU_SPIN_US: its non-blocking polls may run the
        // map callback before the later work is done, with or without index.
        const char* spin = std::getenv("LGP_GPU_SPIN_US");
        const bool serialized =
            submit_ms > heavy_ms / 2 || heavy_ms < 50 || (spin != nullptr && *spin != '\0' && std::atol(spin) > 0);
        std::printf("  %s: returned after %.0f ms, %s the later work%s\n", by_index ? "by index" : "latest", run_ms,
                    returned_before_heavy ? "before" : "after",
                    serialized ? " (driver blocked the submits or work too short: order not checked)" : "");
        CHECK_EQ(value, 42u);
        // Only the index is checked: without it, when run() returns depends
        // on the driver (in CI lavapipe ran the later work alongside).
        if (by_index && !serialized) CHECK(returned_before_heavy);
    }
    g_renderer->run_sync([&] {
        wgpuBindGroupRelease(small_group);
        for (WGPUBuffer b : {out, readback, scratch, scratch2}) {
            wgpuBufferDestroy(b);
            wgpuBufferRelease(b);
        }
        wgpuComputePipelineRelease(small);
        wgpuComputePipelineRelease(heavy);
        wgpuShaderModuleRelease(small_module);
        wgpuShaderModuleRelease(heavy_module);
    });
}
#endif

void test_fixed_clears() {
    lgp::platform::NullRenderer null_renderer;
    nvn::SoftBackend soft(null_renderer);
    WebGpuBackend gpu(*g_renderer);
    for (nvn::Backend* b : {static_cast<nvn::Backend*>(&soft), static_cast<nvn::Backend*>(&gpu)}) {
        b->create_texture(1, color_desc(nvn::Format::RGBA8, 64, 32));
        b->create_texture(2, color_desc(nvn::Format::RGBA8_SRGB, 64, 32));
        nvn::CommandList list;  // lower-left origin (NVN default)
        list.commands.push_back(nvn::CmdSetRenderTargets{{1, 2}, 0});
        list.commands.push_back(nvn::CmdClearColor{0, {0.25f, 0.5f, 1.0f, 1.0f}, nvn::kClearColorMaskRGBA});
        list.commands.push_back(nvn::CmdClearColor{1, {0.25f, 0.5f, 1.0f, 1.0f}, nvn::kClearColorMaskRGBA});
        list.commands.push_back(nvn::CmdSetScissors{0, {{0, 0, 32, 4}}});
        list.commands.push_back(nvn::CmdClearColor{0, {1, 0, 0, 1}, nvn::kClearColorMaskRGBA});
        list.commands.push_back(nvn::CmdClearColor{1, {0, 1, 0, 0}, 0x2 | 0x8});  // G and A only
        b->execute(list);
    }
    CHECK_EQ(compare(soft, gpu, 1, 64, 32), 0);
    CHECK_EQ(compare(soft, gpu, 2, 64, 32), 0);
    // Spot checks against hand-computed values: the red scissor covers the
    // first four memory rows (window rows are memory rows; the lower-left
    // origin only flips the picture on the display).
    std::vector<uint8_t> px;
    CHECK(gpu.read_texture(1, &px, nullptr, nullptr));
    if (px.size() == 64 * 32 * 4) {
        CHECK_EQ(px[(0 * 64 + 0) * 4 + 0], 255u);
        CHECK_EQ(px[(4 * 64 + 0) * 4 + 0], 64u);
        CHECK_EQ(px[(0 * 64 + 32) * 4 + 2], 255u);
    }
    const WebGpuBackend::Stats st = gpu.stats();
    CHECK_EQ(st.clears, 4u);
    CHECK_EQ(st.skipped_commands, 0u);
}

void test_random_clears() {
    lgp::platform::NullRenderer null_renderer;
    nvn::SoftBackend soft(null_renderer);
    WebGpuBackend gpu(*g_renderer);
    const uint32_t w = 37, h = 23;  // odd sizes catch off-by-one flips
    for (nvn::Backend* b : {static_cast<nvn::Backend*>(&soft), static_cast<nvn::Backend*>(&gpu)}) {
        b->create_texture(7, color_desc(nvn::Format::RGBA8, w, h));
        b->create_texture(8, color_desc(nvn::Format::RGBX8_SRGB, w, h));
    }
    Rng rng{0x1234567u};
    for (int round = 0; round < 12; ++round) {
        nvn::CommandList list;
        list.origin_lower_left = (round % 3) != 0;
        list.commands.push_back(nvn::CmdSetRenderTargets{{7, 8}, 0});
        for (int i = 0; i < 6; ++i) {
            if (rng.next() % 3 != 0) {
                const nvn::Rect r{rng.range(-8, int32_t(w)), rng.range(-8, int32_t(h)), rng.range(0, int32_t(w) + 8),
                                  rng.range(0, int32_t(h) + 8)};
                list.commands.push_back(nvn::CmdSetScissors{0, {r}});
            }
            nvn::CmdClearColor c;
            c.index = rng.next() % 2;
            for (float& v : c.rgba)
                v = rng.unit();
            c.mask = (rng.next() % 4 == 0) ? (rng.next() & 0xF) : nvn::kClearColorMaskRGBA;
            list.commands.push_back(c);
        }
        soft.execute(list);
        gpu.execute(list);
    }
    CHECK_EQ(compare(soft, gpu, 7, w, h), 0);
    CHECK_EQ(compare(soft, gpu, 8, w, h), 0);
    CHECK_EQ(gpu.stats().clears, soft.stats().clears);
}

void test_depth_stencil_and_formats() {
    WebGpuBackend gpu(*g_renderer);
    nvn::TextureDesc ds = color_desc(nvn::Format::Depth24Stencil8, 64, 32);
    gpu.create_texture(1, color_desc(nvn::Format::RGBA16F, 64, 32));
    gpu.create_texture(2, ds);
    ds.format = uint32_t(nvn::Format::Depth32F);
    gpu.create_texture(3, ds);
    gpu.create_texture(4, color_desc(nvn::Format::RGBA32F, 16, 16));
    nvn::TextureDesc cube = color_desc(nvn::Format::RGBA8, 16, 16);
    cube.target = uint32_t(nvn::TextureTarget::TargetCubemap);
    gpu.create_texture(5, cube);  // not supported yet: kept, but skipped

    nvn::CommandList list;
    list.commands.push_back(nvn::CmdSetRenderTargets{{1, 4}, 2});
    list.commands.push_back(nvn::CmdClearColor{0, {0.5f, 0.25f, 2.0f, 1.0f}, nvn::kClearColorMaskRGBA});
    list.commands.push_back(nvn::CmdClearColor{1, {1, 2, 3, 4}, 0x5});
    list.commands.push_back(nvn::CmdClearDepthStencil{1.0f, true, 0x80, 0xFF});  // full: load op
    list.commands.push_back(nvn::CmdSetScissors{0, {{8, 8, 16, 8}}});
    list.commands.push_back(nvn::CmdClearDepthStencil{0.5f, true, 0x3, 0x0F});  // drawn
    list.commands.push_back(nvn::CmdSetRenderTargets{{5}, 3});
    list.commands.push_back(nvn::CmdClearDepthStencil{0.25f, true, 0, 0xFF});  // depth only format
    list.commands.push_back(nvn::CmdClearColor{0, {1, 1, 1, 1}, nvn::kClearColorMaskRGBA});  // cube: skipped
    list.commands.push_back(nvn::CmdUnsupported{42});
    gpu.execute(list);
    const WebGpuBackend::Stats st = gpu.stats();
    CHECK_EQ(st.clears, 5u);
    CHECK_EQ(st.skipped_commands, 2u);
    gpu.destroy_texture(2);
    gpu.destroy_texture(5);
}

void set_env(const char* name, const char* value) {
#if defined(_MSC_VER)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

void test_present() {
    set_env("LGP_WEBGPU_FRAMES_IN_FLIGHT", "2");  // backpressure is opt-in
    WebGpuBackend gpu(*g_renderer);
    gpu.create_texture(1, color_desc(nvn::Format::RGBA8_SRGB, 32, 32));
    nvn::CommandList list;
    list.commands.push_back(nvn::CmdSetRenderTargets{{1}, 0});
    list.commands.push_back(nvn::CmdClearColor{0, {0, 0, 1, 1}, nvn::kClearColorMaskRGBA});
    gpu.execute(list);
    const uint64_t before = g_renderer->frames();
    CHECK(gpu.present({1, {}}));
    CHECK(gpu.present({1, {4, 4, 8, 8}}));
    CHECK(gpu.present({99, {}}));  // unknown texture: warning, frame still ends
    CHECK_EQ(g_renderer->frames() - before, 3u);
    CHECK_EQ(gpu.stats().presents, 3u);
    // Backpressure: present returns with at most one earlier frame running.
    for (int i = 0; i < 8; ++i) {
        gpu.execute(list);
        CHECK(gpu.present({1, {}}));
        CHECK(gpu.stats().frames_in_flight <= 1u);
    }
    const auto st = gpu.stats();
    CHECK(st.frames_in_flight_max >= 1u && st.frames_in_flight_max <= 2u);
    // Per-list buffers are destroyed again, not only released.
    CHECK(st.buffers_destroyed > 0u);

    // The CPU path of the renderer.
    std::vector<uint8_t> image(16 * 8 * 4, 0x80);
    lgp::platform::FrameView view{image.data(), 12, 8, 16 * 4};
    g_renderer->present_rgba8(view);
}

// The app's pause (P7): end_frame() of the presenting thread waits while
// paused and goes on after set_paused(false); redraw() and set_ui() work
// meanwhile (headless: nothing is shown, nothing fails).
void test_pause() {
    std::atomic<bool> returned{false};
    g_renderer->set_paused(true);
    std::thread game([&] {
        g_renderer->end_frame();
        returned = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(!returned.load());
    auto ui = std::make_shared<lgp::platform::UiDrawData>();
    ui->width = 256, ui->height = 144;
    auto tex = std::make_shared<lgp::platform::UiTexture>();
    tex->width = tex->height = 1, tex->version = 1, tex->rgba = {255, 255, 255, 255};
    ui->texture = tex;
    ui->vertices = {{0, 0, 0, 0, 0xffffffffu}, {256, 0, 0, 0, 0xffffffffu}, {0, 144, 0, 0, 0xffffffffu}};
    ui->indices = {0, 1, 2};
    ui->commands = {{{0, 0, 256, 144}, 0, 3, 0}};
    g_renderer->set_ui(ui);
    g_renderer->redraw();
    CHECK(!returned.load());
    g_renderer->set_paused(false);
    game.join();
    CHECK(returned.load());
    g_renderer->set_ui(nullptr);
}

}  // namespace

int main() {
    pw::RendererOptions options;
    options.size = {256, 144};
    pw::WebGpuRenderer renderer(options);
    if (!renderer.ok()) {
        std::printf("skipped: %s\n", renderer.error().c_str());
        return kSkip;
    }
    g_renderer = &renderer;
    RUN(test_device);
#if !defined(__EMSCRIPTEN__)
    RUN(test_surface_release_while_busy);
#endif
#if defined(LGP_WEBGPU_WGPU_NATIVE)
    RUN(test_wait_for_submission);
#endif
    RUN(test_fixed_clears);
    RUN(test_random_clears);
    RUN(test_depth_stencil_and_formats);
    RUN(test_present);
    RUN(test_pause);
    CHECK_EQ(renderer.error_count(), 0u);
    g_renderer = nullptr;
    return finish();
}
