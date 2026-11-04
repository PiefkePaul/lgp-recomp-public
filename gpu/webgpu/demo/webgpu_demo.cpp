// WebGPU backend demo (P4.4): a render thread drives WebGpuBackend the way
// the NVN-HLE does (textures, command lists with clears, present) and shows
// the result in a desktop window (SDL3) or the page's canvas (browser).
//
//   lgp_webgpu_demo [--frames N] [--headless] [--gpu-thread]
//
// Desktop: the main thread pumps window events, a second thread renders, as
// guest threads will. Web: main() already runs in a worker
// (PROXY_TO_PTHREAD) and renders; WebGPU itself lives on the browser's main
// thread (webgpu_renderer.hpp). Exits 0 when all frames were presented
// without WebGPU validation errors.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include "lgp/gpu/webgpu/webgpu_backend.hpp"
#include "lgp/platform/webgpu/webgpu_renderer.hpp"
#if defined(LGP_DEMO_SDL)
#include "lgp/platform/desktop/sdl_window.hpp"
#endif

namespace nvn = lgp::gpu::nvn;
namespace pw = lgp::platform::webgpu;

namespace {

constexpr uint32_t kWidth = 1280, kHeight = 720;  // the game's handheld resolution

// One frame: background colour cycling through hues, eight bars growing from
// the bottom (lower-left window origin, NVN's default) with channel masks.
nvn::CommandList frame_commands(uint64_t frame) {
    const float t = float(frame) / 60.0f;
    nvn::CommandList list;
    list.commands.push_back(nvn::CmdSetRenderTargets{{1}, 0});
    nvn::CmdClearColor bg;
    bg.rgba[0] = 0.5f + 0.5f * std::sin(t);
    bg.rgba[1] = 0.5f + 0.5f * std::sin(t + 2.094f);
    bg.rgba[2] = 0.5f + 0.5f * std::sin(t + 4.189f);
    bg.rgba[3] = 1.0f;
    list.commands.push_back(bg);
    for (int i = 0; i < 8; ++i) {
        const float phase = t * 2.0f + float(i) * 0.6f;
        const int32_t h = int32_t((0.5f + 0.45f * std::sin(phase)) * kHeight);
        list.commands.push_back(nvn::CmdSetScissors{0, {{int32_t(80 + i * 140), 0, 100, h}}});
        nvn::CmdClearColor bar{0, {1.0f, 1.0f, 1.0f, 1.0f}, nvn::kClearColorMaskRGBA};
        if (i % 2 == 1)
            bar.mask = 0x1 | 0x8;  // red channel only: tinted bar
        list.commands.push_back(bar);
    }
    return list;
}

// Renders `frames` frames (0 = until the window closes).
uint64_t render(pw::WebGpuRenderer& renderer, uint64_t frames, bool pace) {
    lgp::gpu::webgpu::WebGpuBackend backend(renderer);
    nvn::TextureDesc desc;
    desc.target = uint32_t(nvn::TextureTarget::Target2D);
    desc.format = uint32_t(nvn::Format::RGBA8_SRGB);
    desc.width = kWidth;
    desc.height = kHeight;
    backend.create_texture(1, desc);
    uint64_t n = 0;
    auto next = std::chrono::steady_clock::now();
    while (frames == 0 || n < frames) {
        backend.execute(frame_commands(n));
        ++n;
        if (!backend.present({1, {}}))
            break;
        if (pace) {  // the canvas shows at most one frame per display refresh
            next += std::chrono::microseconds(16667);
            std::this_thread::sleep_until(next);
        }
    }
    return n;
}

}  // namespace

// LGP_MAIN_NAME: shared library with an entry point the platform's glue
// calls (Android app, cmake/LgpAndroidApk.cmake).
#if defined(LGP_MAIN_NAME)
extern "C" int LGP_MAIN_NAME(int argc, char** argv) {
#else
int main(int argc, char** argv) {
#endif
    uint64_t frames = 0;
    bool headless = false;
    bool gpu_thread = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
            frames = std::strtoull(argv[++i], nullptr, 10);
        else if (std::strcmp(argv[i], "--headless") == 0)
            headless = true;
        else if (std::strcmp(argv[i], "--gpu-thread") == 0)
            gpu_thread = true;  // web: WebGPU on a pthread of its own
        else {
            std::fprintf(stderr, "usage: %s [--frames N] [--headless] [--gpu-thread]\n", argv[0]);
            return 2;
        }
    }

    (void)gpu_thread;  // desktop: no such thread
#if defined(__EMSCRIPTEN__)
    pw::RendererOptions options;
    options.size = {kWidth, kHeight};
    if (!headless)
        options.canvas_selector = "#canvas";
    options.gpu_thread = gpu_thread;
    pw::WebGpuRenderer renderer(options);
    if (!renderer.ok()) {
        std::printf("no WebGPU: %s\n", renderer.error().c_str());
        return 77;
    }
    std::printf("adapter: %s\n", renderer.adapter_name().c_str());
    std::printf("webgpu thread: %s\n", renderer.on_gpu_thread() ? "own GPU thread" : "browser main thread");
    // The browser takes every submit at once. Without a limit a slow GPU
    // (lavapipe on a busy CI runner) fell seconds behind: Firefox showed no
    // frame or crashed when the device was released at the end
    // (docs/knowledge/firefox-als-zielbrowser.md). At most two frames in
    // flight, unless LGP_WEBGPU_FRAMES_IN_FLIGHT says otherwise.
    setenv("LGP_WEBGPU_FRAMES_IN_FLIGHT", "2", /*overwrite=*/0);
    const uint64_t n = render(renderer, frames, !headless);
#else
    std::unique_ptr<pw::WebGpuRenderer> headless_renderer;
    pw::WebGpuRenderer* renderer_ptr = nullptr;
#if defined(LGP_DEMO_SDL)
    std::unique_ptr<lgp::platform::desktop::SdlWindow> window;
    if (!headless) {
        lgp::platform::desktop::WindowOptions wo;
        wo.title = "lgp WebGPU demo";
        wo.width = kWidth;
        wo.height = kHeight;
        std::string error;
        window = lgp::platform::desktop::SdlWindow::create(wo, &error);
        if (!window) {
            std::printf("no window: %s\n", error.c_str());
            return 77;
        }
        renderer_ptr = &window->renderer();
        std::printf("video driver: %s\n", window->video_driver().c_str());
    }
#endif
    if (renderer_ptr == nullptr) {
        pw::RendererOptions options;
        options.size = {kWidth, kHeight};
        headless_renderer = std::make_unique<pw::WebGpuRenderer>(options);
        if (!headless_renderer->ok()) {
            std::printf("no WebGPU: %s\n", headless_renderer->error().c_str());
            return 77;
        }
        renderer_ptr = headless_renderer.get();
    }
    pw::WebGpuRenderer& renderer = *renderer_ptr;
    std::printf("adapter: %s\n", renderer.adapter_name().c_str());

    std::atomic<uint64_t> rendered{0};
    std::atomic<bool> done{false};
    std::thread render_thread([&] {
        rendered = render(renderer, frames, false);
        done = true;
    });
#if defined(LGP_DEMO_SDL)
    while (!done.load()) {
        if (window && !window->pump_events(10))
            break;  // closed: the render thread ends at its next present
        if (!window)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
#endif
    render_thread.join();
    const uint64_t n = rendered.load();
#endif
    std::printf("frames: %llu, WebGPU errors: %llu\n", static_cast<unsigned long long>(n),
                static_cast<unsigned long long>(renderer.error_count()));
    return renderer.error_count() == 0 && (frames == 0 || n == frames) ? 0 : 1;
}
