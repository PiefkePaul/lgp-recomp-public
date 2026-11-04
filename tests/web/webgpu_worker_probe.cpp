// Feasibility probe (P6, docs/analysis/21-internet-recherche-werkzeuge.md,
// proposal A1): WebGPU device and canvas owned by a dedicated pthread through
// an OffscreenCanvas, instead of the browser's main thread.
//
// main() runs in a worker (PROXY_TO_PTHREAD) and owns the canvas
// (OFFSCREENCANVASES_TO_PTHREAD). It hands the canvas on to a GPU thread,
// which creates instance, surface, adapter and device with emdawnwebgpu,
// returns to its event loop (emscripten_exit_with_live_runtime) and presents
// from a requestAnimationFrame loop of its own. main() then measures the
// round trip of emscripten_proxy_sync to the browser main thread and to the
// GPU thread, without and with simulated load on the browser main thread
// (a JavaScript busy loop), plus real WebGPU work (writeBuffer, clear pass,
// submit) proxied synchronously and asynchronously to the GPU thread.
//
// Prints "webgpu worker probe: ok" and exits 0 when the GPU thread presented
// frames without WebGPU errors.
#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>
#include <pthread.h>
#include <webgpu/webgpu.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

WGPUStringView sv(const char* s) { return WGPUStringView{s, std::strlen(s)}; }

struct Gpu {
    WGPUInstance instance = nullptr;
    WGPUSurface surface = nullptr;
    WGPUAdapter adapter = nullptr;
    WGPUDevice device = nullptr;
    WGPUQueue queue = nullptr;
    WGPUTextureFormat format = WGPUTextureFormat_BGRA8Unorm;
    WGPUBuffer upload = nullptr;
    WGPUTexture target = nullptr;
    WGPUTextureView target_view = nullptr;
};

Gpu g_gpu;  // touched only on the GPU thread
pthread_t g_gpu_thread;
std::atomic<int> g_state{0};  // 0 starting, 1 ready, -1 failed
std::atomic<int> g_errors{0};
std::atomic<uint64_t> g_frames{0};
std::atomic<uint64_t> g_work_done{0};
std::atomic<bool> g_stop{false};
char g_fail[256];
// Own queue, not emscripten_proxy_get_system_queue(): the system queue is for
// Emscripten's internals and may also be run from inside system calls, which
// could re-enter the GPU thread in the middle of a frame.
em_proxying_queue* g_queue = nullptr;

constexpr size_t kUploadBytes = 1 << 20;
std::vector<uint8_t> g_upload_data(kUploadBytes, 0x5a);

void fail(const char* what) {
    std::snprintf(g_fail, sizeof g_fail, "%s", what);
    g_state.store(-1);
}

void clear_pass(WGPUCommandEncoder enc, WGPUTextureView view, double r, double g, double b) {
    WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    color.view = view;
    color.loadOp = WGPULoadOp_Clear;
    color.storeOp = WGPUStoreOp_Store;
    color.clearValue = WGPUColor{r, g, b, 1.0};
    WGPURenderPassDescriptor pass_desc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    pass_desc.colorAttachmentCount = 1;
    pass_desc.colorAttachments = &color;
    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &pass_desc);
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);
}

void submit(WGPUCommandEncoder enc) {
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
    wgpuQueueSubmit(g_gpu.queue, 1, &cmd);
    wgpuCommandBufferRelease(cmd);
    wgpuCommandEncoderRelease(enc);
}

// One unit of "guest" GPU work: 1 MiB upload, a clear pass, a submit.
void gpu_work() {
    wgpuQueueWriteBuffer(g_gpu.queue, g_gpu.upload, 0, g_upload_data.data(), kUploadBytes);
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g_gpu.device, nullptr);
    clear_pass(enc, g_gpu.target_view, 0.2, 0.4, 0.6);
    submit(enc);
    g_work_done.fetch_add(1);
}

bool frame(double, void*) {
    if (g_stop.load())
        return false;
    WGPUSurfaceTexture st = WGPU_SURFACE_TEXTURE_INIT;
    wgpuSurfaceGetCurrentTexture(g_gpu.surface, &st);
    if (st.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
        st.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) {
        g_errors.fetch_add(1);
        return false;
    }
    WGPUTextureView view = wgpuTextureCreateView(st.texture, nullptr);
    const uint64_t n = g_frames.load();
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g_gpu.device, nullptr);
    clear_pass(enc, view, 0.3 + 0.3 * double(n % 60) / 60.0, 0.6, 0.3);
    submit(enc);
    wgpuTextureViewRelease(view);
    wgpuTextureRelease(st.texture);
    g_frames.fetch_add(1);
    // No wgpuSurfacePresent: the canvas presents when this task returns.
    return true;
}

void on_device(WGPURequestDeviceStatus status, WGPUDevice device, WGPUStringView, void*, void*) {
    if (status != WGPURequestDeviceStatus_Success || device == nullptr)
        return fail("no device");
    g_gpu.device = device;
    g_gpu.queue = wgpuDeviceGetQueue(device);

    WGPUSurfaceCapabilities caps = WGPU_SURFACE_CAPABILITIES_INIT;
    if (wgpuSurfaceGetCapabilities(g_gpu.surface, g_gpu.adapter, &caps) == WGPUStatus_Success &&
        caps.formatCount > 0)
        g_gpu.format = caps.formats[0];
    wgpuSurfaceCapabilitiesFreeMembers(caps);
    WGPUSurfaceConfiguration config = WGPU_SURFACE_CONFIGURATION_INIT;
    config.device = device;
    config.format = g_gpu.format;
    config.usage = WGPUTextureUsage_RenderAttachment;
    config.width = 1280;
    config.height = 720;
    config.presentMode = WGPUPresentMode_Fifo;
    config.alphaMode = WGPUCompositeAlphaMode_Opaque;
    wgpuSurfaceConfigure(g_gpu.surface, &config);

    WGPUBufferDescriptor buf = WGPU_BUFFER_DESCRIPTOR_INIT;
    buf.size = kUploadBytes;
    buf.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_Uniform;
    g_gpu.upload = wgpuDeviceCreateBuffer(device, &buf);
    WGPUTextureDescriptor tex = WGPU_TEXTURE_DESCRIPTOR_INIT;
    tex.size = WGPUExtent3D{256, 256, 1};
    tex.format = WGPUTextureFormat_RGBA8Unorm;
    tex.usage = WGPUTextureUsage_RenderAttachment;
    g_gpu.target = wgpuDeviceCreateTexture(device, &tex);
    g_gpu.target_view = wgpuTextureCreateView(g_gpu.target, nullptr);

    emscripten_request_animation_frame_loop(frame, nullptr);
    g_state.store(1);
}

void on_adapter(WGPURequestAdapterStatus status, WGPUAdapter adapter, WGPUStringView, void*, void*) {
    if (status != WGPURequestAdapterStatus_Success || adapter == nullptr)
        return fail("no adapter");
    g_gpu.adapter = adapter;
    WGPUDeviceDescriptor desc = WGPU_DEVICE_DESCRIPTOR_INIT;
    desc.uncapturedErrorCallbackInfo.callback = [](const WGPUDevice*, WGPUErrorType, WGPUStringView msg, void*,
                                                   void*) {
        std::fprintf(stderr, "webgpu error: %.*s\n", int(msg.length), msg.data);
        g_errors.fetch_add(1);
    };
    WGPURequestDeviceCallbackInfo cb = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
    cb.mode = WGPUCallbackMode_AllowSpontaneous;
    cb.callback = on_device;
    wgpuAdapterRequestDevice(adapter, &desc, cb);
}

void* gpu_thread_main(void*) {
    g_gpu.instance = wgpuCreateInstance(nullptr);
    WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvas = WGPU_EMSCRIPTEN_SURFACE_SOURCE_CANVAS_HTML_SELECTOR_INIT;
    canvas.selector = sv("#canvas");
    WGPUSurfaceDescriptor surface_desc = WGPU_SURFACE_DESCRIPTOR_INIT;
    surface_desc.nextInChain = &canvas.chain;
    g_gpu.surface = wgpuInstanceCreateSurface(g_gpu.instance, &surface_desc);
    if (g_gpu.surface == nullptr) {
        fail("no surface from the OffscreenCanvas");
        return nullptr;
    }
    WGPURequestAdapterCallbackInfo cb = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
    cb.mode = WGPUCallbackMode_AllowSpontaneous;
    cb.callback = on_adapter;
    wgpuInstanceRequestAdapter(g_gpu.instance, nullptr, cb);
    // Back to this worker's event loop: WebGPU callbacks, rAF and proxied
    // tasks run there.
    emscripten_exit_with_live_runtime();
    return nullptr;
}

// --- measurements (on main(), a worker) -------------------------------------

struct Stats {
    double min, median, p95, max, mean;
};

Stats stats(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    double sum = 0;
    for (double x : v) sum += x;
    return {v.front(), v[v.size() / 2], v[v.size() * 95 / 100], v.back(), sum / double(v.size())};
}

void print(const char* label, const Stats& s) {
    std::printf("probe: %-34s min %6.3f  median %6.3f  p95 %6.3f  max %7.3f  mean %6.3f ms\n", label, s.min,
                s.median, s.p95, s.max, s.mean);
}

Stats measure_sync(pthread_t target, void (*fn)(void*), int n) {
    std::vector<double> t;
    t.reserve(size_t(n));
    for (int i = 0; i < n; ++i) {
        const double t0 = emscripten_get_now();
        if (!emscripten_proxy_sync(g_queue, target, fn, nullptr)) {
            std::printf("probe: proxying failed\n");
            std::exit(1);
        }
        t.push_back(emscripten_get_now() - t0);
        emscripten_thread_sleep(1);  // like a guest thread doing other work in between
    }
    return stats(std::move(t));
}

void noop(void*) {}
void work(void*) { gpu_work(); }

}  // namespace

int main() {
    g_queue = em_proxying_queue_create();
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    emscripten_pthread_attr_settransferredcanvases(&attr, "#canvas");
    if (pthread_create(&g_gpu_thread, &attr, gpu_thread_main, nullptr) != 0) {
        std::printf("probe: could not start the GPU thread with the canvas\n");
        return 1;
    }
    const double start = emscripten_get_now();
    while (g_state.load() == 0) {
        if (emscripten_get_now() - start > 30000) {
            std::printf("probe: GPU thread not ready after 30 s\n");
            return 1;
        }
        emscripten_thread_sleep(5);
    }
    if (g_state.load() < 0) {
        std::printf("probe: GPU thread failed: %s\n", g_fail);
        return 1;
    }
    std::printf("probe: GPU thread ready after %.1f ms (format %d)\n", emscripten_get_now() - start,
                int(g_gpu.format));

    const pthread_t browser_main = emscripten_main_runtime_thread_id();
    constexpr int kN = 300;
    print("sync noop -> browser main", measure_sync(browser_main, noop, kN));
    print("sync noop -> GPU thread", measure_sync(g_gpu_thread, noop, kN));
    const uint64_t sync_done_before = g_work_done.load();
    print("sync work -> GPU thread", measure_sync(g_gpu_thread, work, kN));

    // Simulated load on the browser main thread (DOM, input, audio control):
    // 8 ms of JavaScript every 10 ms.
    MAIN_THREAD_EM_ASM({
        globalThis.lgpProbeLoad = setInterval(() => {
            const t = performance.now();
            while (performance.now() - t < 8) {}
        }, 10);
    });
    emscripten_thread_sleep(100);
    print("busy main: sync noop -> browser main", measure_sync(browser_main, noop, kN / 3));
    print("busy main: sync noop -> GPU thread", measure_sync(g_gpu_thread, noop, kN));
    print("busy main: sync work -> GPU thread", measure_sync(g_gpu_thread, work, kN));
    if (g_work_done.load() - sync_done_before != 2 * kN) {
        std::printf("probe: %llu of %d synchronous work tasks ran\n",
                    (unsigned long long)(g_work_done.load() - sync_done_before), 2 * kN);
        return 1;
    }
    const uint64_t frames_before = g_frames.load();
    const double f0 = emscripten_get_now();
    emscripten_thread_sleep(2000);
    std::printf("probe: busy main: GPU thread presented %.1f frames/s\n",
                double(g_frames.load() - frames_before) * 1000.0 / (emscripten_get_now() - f0));
    MAIN_THREAD_EM_ASM({ clearInterval(globalThis.lgpProbeLoad); });

    // Asynchronous hand-off: queue the work, wait once at the end (fence).
    const uint64_t done_before = g_work_done.load();
    const double a0 = emscripten_get_now();
    double max_enqueue = 0;
    for (int i = 0; i < kN; ++i) {
        const double e0 = emscripten_get_now();
        if (!emscripten_proxy_async(g_queue, g_gpu_thread, work, nullptr)) {
            std::printf("probe: asynchronous proxying failed\n");
            return 1;
        }
        max_enqueue = std::max(max_enqueue, emscripten_get_now() - e0);
    }
    const double enqueue_ms = emscripten_get_now() - a0;
    if (!emscripten_proxy_sync(g_queue, g_gpu_thread, noop, nullptr)) {
        std::printf("probe: proxying failed\n");
        return 1;
    }
    const double total_ms = emscripten_get_now() - a0;
    std::printf("probe: async work -> GPU thread: %d tasks enqueued in %.3f ms (max %.3f ms each), "
                "all done after %.3f ms (%.3f ms per task), done %llu\n",
                kN, enqueue_ms, max_enqueue, total_ms, total_ms / kN,
                (unsigned long long)(g_work_done.load() - done_before));

    if (g_work_done.load() - done_before != uint64_t(kN)) {
        std::printf("probe: %llu of %d asynchronous work tasks ran\n",
                    (unsigned long long)(g_work_done.load() - done_before), kN);
        return 1;
    }

    // frame() ends its loop on a surface error: stop waiting then, and give up
    // after 10 s instead of running into the ctest timeout.
    const double w0 = emscripten_get_now();
    while (g_frames.load() < 120 && g_errors.load() == 0 && emscripten_get_now() - w0 < 10000)
        emscripten_thread_sleep(10);
    g_stop.store(true);
    if (g_frames.load() < 120) {
        std::printf("probe: only %llu frames presented\n", (unsigned long long)g_frames.load());
        return 1;
    }
    const int errors = g_errors.load();
    std::printf("frames: %llu, WebGPU errors: %d\n", (unsigned long long)g_frames.load(), errors);
    if (errors != 0)
        return 1;
    std::printf("webgpu worker probe: ok\n");
    return 0;
}
