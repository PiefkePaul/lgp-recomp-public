#include "lgp/platform/webgpu/webgpu_renderer.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>

#if defined(LGP_WEBGPU_WGPU_NATIVE)
#include <webgpu/wgpu.h>
#endif
#if defined(__EMSCRIPTEN__)
#include <emscripten/proxying.h>
#include <emscripten/eventloop.h>
#include <emscripten/html5.h>
#include <emscripten/threading.h>
#endif

namespace lgp::platform::webgpu {

namespace {

void log_error(const char* what, const std::string& detail) {
    std::fprintf(stderr, "webgpu: %s: %s\n", what, detail.c_str());
}

// Draws a texel rectangle of a texture into a destination rectangle (NDC) of
// the target. textureLoad, not textureSample: works for unfilterable formats
// (RGBA32F) and keeps the bytes of sRGB textures when the view is the linear
// variant. Nearest-neighbour scaling; fs_linear and fs_fxaa filter
// (PresentFilter) and need a filterable format.
constexpr const char* kBlitWgsl = R"(
struct Params {
    src: vec4f,   // x0, y0, x1, y1 in texels
    dst: vec4f,   // x0, y0 (top left), x1, y1 in NDC
    clip: vec4f,  // filtered blits: the texel centres lookups stay within (x0, y0, x1, y1)
};
@group(0) @binding(0) var<uniform> p: Params;
@group(0) @binding(1) var t: texture_2d<f32>;
@group(0) @binding(2) var s: sampler;  // filtered blits only

struct VOut {
    @builtin(position) pos: vec4f,
    @location(0) texel: vec2f,
};

@vertex fn vs(@builtin(vertex_index) i: u32) -> VOut {
    let c = vec2f(f32(i & 1u), f32(i >> 1u));
    var o: VOut;
    o.pos = vec4f(mix(p.dst.xy, p.dst.zw, c), 0.0, 1.0);
    o.texel = mix(p.src.xy, p.src.zw, c);
    return o;
}

@fragment fn fs(v: VOut) -> @location(0) vec4f {
    let size = vec2i(textureDimensions(t));
    let xy = clamp(vec2i(floor(v.texel)), vec2i(0), size - 1);
    return vec4f(textureLoad(t, xy, 0).rgb, 1.0);
}

// Bilinear lookup at a point in texels, kept inside the source rectangle
// (a crop must not pull in its neighbours).
fn at(xy: vec2f) -> vec3f {
    let size = vec2f(textureDimensions(t));
    return textureSampleLevel(t, s, clamp(xy, p.clip.xy, p.clip.zw) / size, 0.0).rgb;
}

@fragment fn fs_linear(v: VOut) -> @location(0) vec4f {
    return vec4f(at(v.texel), 1.0);
}

// FXAA (after T. Lottes' FXAA 2 description): the luma of four diagonal
// neighbours gives the edge direction; two or four lookups along it replace
// the texel where they stay within the neighbourhood's luma range.
@fragment fn fs_fxaa(v: VOut) -> @location(0) vec4f {
    let c = v.texel;
    let k = vec3f(0.299, 0.587, 0.114);
    let rgb_m = at(c);
    let nw = dot(at(c + vec2f(-1.0, -1.0)), k);
    let ne = dot(at(c + vec2f(1.0, -1.0)), k);
    let sw = dot(at(c + vec2f(-1.0, 1.0)), k);
    let se = dot(at(c + vec2f(1.0, 1.0)), k);
    let m = dot(rgb_m, k);
    let lo = min(m, min(min(nw, ne), min(sw, se)));
    let hi = max(m, max(max(nw, ne), max(sw, se)));
    var dir = vec2f(-((nw + ne) - (sw + se)), (nw + sw) - (ne + se));
    let reduce = max((nw + ne + sw + se) * (0.25 / 8.0), 1.0 / 128.0);
    let scale = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);
    dir = clamp(dir * scale, vec2f(-8.0), vec2f(8.0));
    let a = 0.5 * (at(c + dir * (1.0 / 3.0 - 0.5)) + at(c + dir * (2.0 / 3.0 - 0.5)));
    let b = 0.5 * a + 0.25 * (at(c - dir * 0.5) + at(c + dir * 0.5));
    let lb = dot(b, k);
    return vec4f(select(b, a, lb < lo || lb > hi), 1.0);
}
)";

struct BlitParams {
    float src[4];
    float dst[4];
    float clip[4];
};

// Overlay circles (WebGpuRenderer::set_overlay): a quad over the whole
// target, each fragment shades the first circle it lies in, in the same steps
// as lgp::platform::overlay_shade (overlay.cpp, the reference for pictures
// without a GPU): buttons are a dark disc with a light ring and their name in
// light strokes, pressed a light disc with a dark name.
constexpr size_t kMaxOverlayStrokes = 64;
constexpr const char* kOverlayWgsl = R"(
struct Params {
    info: vec4f,              // target width, height, circle count
    c: array<vec4f, 24>,      // x, y (0..1), r (fraction of the height), first stroke
    k: array<vec4f, 24>,      // kind (0 button, 1 stick base, 2 knob), pressed, alpha, stroke count
    s: array<vec4f, 64>,      // label stroke from a.xy to b.xy, in radii around the centre
    t: array<vec4f, 64>,      // 1: arc (right half circle around a), its radius
    e: array<vec4f, 24>,      // half width, half height, corner (in r; 1, 1, 1 = circle)
};
@group(0) @binding(0) var<uniform> p: Params;

@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {
    return vec4f(f32(i & 1u) * 2.0 - 1.0, f32(i >> 1u) * 2.0 - 1.0, 0.0, 1.0);
}

// Straight-alpha "over".
fn over(dst: vec4f, rgb: vec3f, a: f32) -> vec4f {
    let oa = a + dst.a * (1.0 - a);
    if (oa <= 0.0) { return vec4f(0.0); }
    return vec4f((rgb * a + dst.rgb * dst.a * (1.0 - a)) / oa, oa);
}

fn stroke_distance(q: vec2f, j: u32) -> f32 {
    let a = p.s[j].xy;
    if (p.t[j].x > 0.5) {
        let r = p.t[j].y;
        if (q.x >= a.x) { return abs(length(q - a) - r); }
        return min(length(q - vec2f(a.x, a.y - r)), length(q - vec2f(a.x, a.y + r)));
    }
    let v = p.s[j].zw - a;
    let len2 = dot(v, v);
    var t = 0.0;
    if (len2 > 0.0) { t = clamp(dot(q - a, v) / len2, 0.0, 1.0); }
    return length(q - (a + t * v));
}

@fragment fn fs(@builtin(position) pos: vec4f) -> @location(0) vec4f {
    let size = p.info.xy;
    for (var i = 0u; i < u32(p.info.z); i = i + 1u) {
        let r = p.c[i].z * size.y;
        let rel = pos.xy - p.c[i].xy * size;
        let half = p.e[i].xy * r;
        let corner = min(p.e[i].z * r, min(half.x, half.y));
        let q = abs(rel) - (half - vec2f(corner));
        let sd = length(max(q, vec2f(0.0))) + min(max(q.x, q.y), 0.0) - corner;
        if (sd > 1.0) { continue; }
        let edge = 1.0 - smoothstep(-1.0, 1.0, sd);
        let rw = max(2.0, 0.07 * r);
        let ring = edge * smoothstep(-rw - 1.0, -rw, sd);
        let kind = p.k[i].x;
        let pressed = p.k[i].y > 0.5;
        let alpha = p.k[i].z;
        var px = vec4f(0.0);
        if (kind < 0.5) {
            let fv = select(0.0, 1.0, pressed);
            px = over(px, vec3f(fv), edge * select(0.12, 0.6, pressed) * alpha);
            px = over(px, vec3f(1.0), ring * 0.5 * alpha);
            var dist = 1e9;
            let first = u32(p.c[i].w);
            for (var j = first; j < first + u32(p.k[i].w); j = j + 1u) {
                dist = min(dist, stroke_distance(rel / r, j));
            }
            let stroke = 0.055;  // kOverlayStrokeWidth / 2
            let aa = 1.0 / max(r, 1.0);
            let ink = 1.0 - smoothstep(stroke - aa, stroke + aa, dist);
            px = over(px, vec3f(select(1.0, 0.0, pressed)), ink * select(0.6, 0.8, pressed) * alpha);
        } else {
            var fill = select(0.3, 0.5, pressed);
            var line = 0.0;
            if (kind < 1.5) { fill = select(0.05, 0.12, pressed); line = 0.35; }
            px = over(px, vec3f(1.0), max(ring * line, edge * fill) * alpha);
        }
        return px;
    }
    return vec4f(0.0);
}
)";

struct OverlayParams {
    float info[4];
    float c[WebGpuRenderer::kMaxOverlayCircles][4];
    float k[WebGpuRenderer::kMaxOverlayCircles][4];
    float s[kMaxOverlayStrokes][4];
    float t[kMaxOverlayStrokes][4];
    float e[WebGpuRenderer::kMaxOverlayCircles][4];
};

// Interface triangles (WebGpuRenderer::set_ui, lgp/platform/ui_draw.hpp):
// positions in UI units, straight-alpha sRGB colours times the atlas texel.
// On an sRGB surface the colours are linearized first (the surface encodes).
constexpr const char* kUiWgsl = R"(
struct Params {
    scale: vec4f,   // 2 / ui width, 2 / ui height, linearize (1 = yes)
};
@group(0) @binding(0) var<uniform> p: Params;
@group(0) @binding(1) var atlas: texture_2d<f32>;
@group(0) @binding(2) var smp: sampler;

struct Out {
    @builtin(position) pos: vec4f,
    @location(0) uv: vec2f,
    @location(1) color: vec4f,
};

@vertex fn vs(@location(0) pos: vec2f, @location(1) uv: vec2f, @location(2) color: vec4f) -> Out {
    var o: Out;
    o.pos = vec4f(pos.x * p.scale.x - 1.0, 1.0 - pos.y * p.scale.y, 0.0, 1.0);
    o.uv = uv;
    o.color = color;
    if (p.scale.z > 0.5) {
        o.color = vec4f(pow(color.rgb, vec3f(2.2)), color.a);
    }
    return o;
}

@fragment fn fs(in: Out) -> @location(0) vec4f {
    return in.color * textureSample(atlas, smp, in.uv);
}
)";

struct UiParams {
    float scale[4];
};

bool is_srgb(WGPUTextureFormat format) { return linear_variant(format) != format; }

}  // namespace

std::string to_string(WGPUStringView s) {
    if (s.data == nullptr)
        return {};
    if (s.length == WGPU_STRLEN)
        return std::string(s.data);
    return std::string(s.data, s.length);
}

WGPUShaderModule create_wgsl_module(WGPUDevice device, const char* label, const char* wgsl) {
    WGPUShaderSourceWGSL source = WGPU_SHADER_SOURCE_WGSL_INIT;
    source.code = string_view(wgsl);
    WGPUShaderModuleDescriptor desc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    desc.nextInChain = &source.chain;
    desc.label = string_view(label);
#if defined(LGP_WEBGPU_WGPU_NATIVE)
    // LGP_WEBGPU_BOUNDS_CHECKS=0: no bounds checks inserted by naga (loops
    // stay bounded). Diagnosis for drivers whose arrayLength() is wrong
    // (Verdacht on Adreno 640: every invocation read element 0, P7.3); an
    // out-of-range access is then undefined behaviour on the GPU.
    static const bool unchecked = [] {
        const char* v = std::getenv("LGP_WEBGPU_BOUNDS_CHECKS");
        return v != nullptr && std::string(v) == "0";
    }();
    WGPUShaderModule module = unchecked ? wgpuDeviceCreateShaderModuleTrusted(
                                              device, &desc, WGPUShaderRuntimeChecks_ForceLoopBounding)
                                        : wgpuDeviceCreateShaderModule(device, &desc);
#else
    WGPUShaderModule module = wgpuDeviceCreateShaderModule(device, &desc);
#endif
    if (module == nullptr)
        log_error("shader module", label);
    return module;
}

WGPUTextureFormat linear_variant(WGPUTextureFormat format) {
    switch (format) {
    case WGPUTextureFormat_RGBA8UnormSrgb: return WGPUTextureFormat_RGBA8Unorm;
    case WGPUTextureFormat_BGRA8UnormSrgb: return WGPUTextureFormat_BGRA8Unorm;
    default: return format;
    }
}

// --- construction ----------------------------------------------------------

WebGpuRenderer::WebGpuRenderer(const RendererOptions& options)
    : present_mode_(options.present_mode), size_(options.size), frame_limit_(options.frame_limit) {
    if (const char* e = std::getenv("LGP_GPU_SPIN_US"); e != nullptr && *e != '\0') {
        gpu_spin_ = std::chrono::microseconds(std::max(std::atol(e), 0L));
        std::printf("webgpu: GPU waits poll %lld us before blocking (LGP_GPU_SPIN_US)\n",
                    static_cast<long long>(gpu_spin_.count()));
    }
#if defined(__EMSCRIPTEN__)
    start_gpu_thread(options);
#endif
    create_device(options);
}

bool WebGpuRenderer::on_gpu_thread() const {
#if defined(__EMSCRIPTEN__)
    return gpu_thread_queue_ != nullptr;
#else
    return false;
#endif
}

#if defined(__EMSCRIPTEN__)
void WebGpuRenderer::start_gpu_thread(const RendererOptions& options) {
    bool want = options.gpu_thread;
    if (const char* e = std::getenv("LGP_WEBGPU_GPU_THREAD"); e != nullptr && *e != '\0')
        want = std::strcmp(e, "0") != 0;
    // From the browser's main thread itself (no PROXY_TO_PTHREAD) run() could
    // not wait for another thread: stay there.
    if (!want || emscripten_is_main_runtime_thread())
        return;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    // The canvas moves to the new thread as an OffscreenCanvas (from the
    // browser's main thread, where the page left it: lgp_platform_webgpu
    // links with an empty OFFSCREENCANVASES_TO_PTHREAD).
    // pthread_create special-cases "#canvas" as Module.canvas and, called
    // from a pthread (no Module.canvas there), logs an error before it hands
    // the call to the browser's main thread, which then finds the canvas.
    // The plain ID finds Module.canvas there too, without that noise.
    const std::string canvas =
        options.canvas_selector == "#canvas" ? std::string("canvas") : options.canvas_selector;
    if (!canvas.empty())
        emscripten_pthread_attr_settransferredcanvases(&attr, canvas.c_str());
    pthread_t thread{};
    const int rc = pthread_create(
        &thread, &attr,
        [](void*) -> void* {
            // Back to this worker's event loop: proxied tasks, WebGPU
            // callbacks and timers run there, and the canvas presents when a
            // task returns to it.
            emscripten_exit_with_live_runtime();
            return nullptr;
        },
        nullptr);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        std::fprintf(stderr, "webgpu: no GPU thread (error %d), using the browser main thread\n", rc);
        return;
    }
    emscripten_set_thread_name(thread, "lgp gpu");
    gpu_thread_ = thread;
    // Own queue, not the system queue: that one also runs from inside
    // Emscripten's system calls and could re-enter the GPU thread mid-task.
    gpu_thread_queue_ = em_proxying_queue_create();
}

void WebGpuRenderer::stop_gpu_thread() {
    if (gpu_thread_queue_ == nullptr)
        return;
    emscripten_proxy_async(gpu_thread_queue_, gpu_thread_, [](void*) { pthread_exit(nullptr); }, nullptr);
    pthread_join(gpu_thread_, nullptr);
    em_proxying_queue_destroy(gpu_thread_queue_);
    gpu_thread_queue_ = nullptr;
}
#endif

#if defined(__EMSCRIPTEN__)
namespace {
// Longest wait for the GPU to finish what was submitted before the device is
// released. The browser accepts submits without waiting, so a slow GPU
// (lavapipe on a busy CI runner) can be seconds behind the last present.
constexpr int kWorkDoneWaitMs = 20000;
// Longest wait for one more frame once the GPU is done.
constexpr int kFrameWaitMs = 2000;
}  // namespace
#endif

WebGpuRenderer::~WebGpuRenderer() {
    if (instance_ == nullptr) {
#if defined(__EMSCRIPTEN__)
        stop_gpu_thread();
#endif
        return;
    }
    bool keep_device = false;
#if defined(__EMSCRIPTEN__)
    // Firefox (157, Linux) crashed (the whole browser) when the device was
    // destroyed while the GPU still worked on earlier frames: let the queue
    // drain and the page show one more frame first
    // (docs/knowledge/firefox-als-zielbrowser.md). Waiting needs the
    // browser's event loop, so only from a pthread.
    if (surface_ != nullptr && queue_ != nullptr && !emscripten_is_main_runtime_thread()) {
        run([this, &keep_device](Call& call) {
            wgpuSurfaceUnconfigure(surface_);
            // Timeouts finish the call too: in Firefox (CI, xvfb, own GPU
            // thread) the frame or the work-done callback once never came
            // and the program never exited. Everything runs on this thread's
            // event loop. finish() clears the timers (a pending timer keeps
            // the runtime alive). The state is leaked (once, at shutdown): the
            // work-done callback or the frame may still come afterwards.
            struct Wait {
                Call* call;
                bool* keep_device;
                bool finished = false;
                bool work_done = false;
                int work_timer = 0, frame_timer = 0;  // 0: none pending
                void finish() {
                    if (finished) return;
                    finished = true;
                    if (work_timer != 0) emscripten_clear_timeout(work_timer);
                    if (frame_timer != 0) emscripten_clear_timeout(frame_timer);
                    call->finish();
                }
            };
            auto* wait = new Wait{&call, &keep_device};
            wait->work_timer = emscripten_set_timeout(
                [](void* w) {
                    auto* wait = static_cast<Wait*>(w);
                    wait->work_timer = 0;
                    if (wait->finished || wait->work_done)
                        return;
                    // Destroying a device with work in flight is what crashed
                    // Firefox: leave it to the page's end instead.
                    std::fprintf(stderr, "webgpu: GPU work not done within %d ms, device not released\n",
                                 kWorkDoneWaitMs);
                    *wait->keep_device = true;
                    wait->finish();
                },
                kWorkDoneWaitMs, wait);
            WGPUQueueWorkDoneCallbackInfo done = WGPU_QUEUE_WORK_DONE_CALLBACK_INFO_INIT;
            done.mode = WGPUCallbackMode_AllowSpontaneous;
            done.userdata1 = wait;
            done.callback = [](WGPUQueueWorkDoneStatus, WGPUStringView, void* userdata1, void*) {
                auto* wait = static_cast<Wait*>(userdata1);
                if (wait->finished)
                    return;
                wait->work_done = true;
                if (wait->work_timer != 0) {
                    emscripten_clear_timeout(wait->work_timer);
                    wait->work_timer = 0;
                }
                wait->frame_timer = emscripten_set_timeout(
                    [](void* w) {
                        auto* wait = static_cast<Wait*>(w);
                        wait->frame_timer = 0;
                        if (!wait->finished)
                            std::fprintf(stderr,
                                         "webgpu: no frame after the last present within %d ms, releasing anyway\n",
                                         kFrameWaitMs);
                        wait->finish();
                    },
                    kFrameWaitMs, wait);
                emscripten_request_animation_frame(
                    [](double, void* w) -> bool {
                        static_cast<Wait*>(w)->finish();
                        return false;
                    },
                    wait);
            };
            wgpuQueueOnSubmittedWorkDone(queue_, done);
        });
        surface_unconfigured_ = true;
    }
#endif
    run_sync([this, keep_device] {
        for (auto& [format, pipeline] : blit_pipelines_)
            wgpuRenderPipelineRelease(pipeline);
        for (auto& [format, pipeline] : overlay_pipelines_)
            wgpuRenderPipelineRelease(pipeline);
        if (overlay_bind_group_ != nullptr) wgpuBindGroupRelease(overlay_bind_group_);
        if (overlay_uniforms_ != nullptr) wgpuBufferRelease(overlay_uniforms_);
        if (overlay_shader_ != nullptr) wgpuShaderModuleRelease(overlay_shader_);
        if (overlay_pipeline_layout_ != nullptr) wgpuPipelineLayoutRelease(overlay_pipeline_layout_);
        if (overlay_layout_ != nullptr) wgpuBindGroupLayoutRelease(overlay_layout_);
        for (auto& [format, pipeline] : ui_pipelines_)
            wgpuRenderPipelineRelease(pipeline);
        release_ui_texture(ui_atlas_);
        for (UiGpuTexture& image : ui_images_)
            release_ui_texture(image);
        if (ui_sampler_ != nullptr) wgpuSamplerRelease(ui_sampler_);
        if (ui_vertices_ != nullptr) wgpuBufferRelease(ui_vertices_);
        if (ui_indices_ != nullptr) wgpuBufferRelease(ui_indices_);
        if (ui_uniforms_ != nullptr) wgpuBufferRelease(ui_uniforms_);
        if (ui_shader_ != nullptr) wgpuShaderModuleRelease(ui_shader_);
        if (ui_pipeline_layout_ != nullptr) wgpuPipelineLayoutRelease(ui_pipeline_layout_);
        if (ui_layout_ != nullptr) wgpuBindGroupLayoutRelease(ui_layout_);
        if (last_view_ != nullptr) wgpuTextureViewRelease(last_view_);
        if (staging_ != nullptr) wgpuTextureRelease(staging_);
        if (blit_uniforms_ != nullptr) wgpuBufferRelease(blit_uniforms_);
        if (blit_shader_ != nullptr) wgpuShaderModuleRelease(blit_shader_);
        if (blit_pipeline_layout_ != nullptr) wgpuPipelineLayoutRelease(blit_pipeline_layout_);
        if (blit_layout_ != nullptr) wgpuBindGroupLayoutRelease(blit_layout_);
        if (blit_sampler_ != nullptr) wgpuSamplerRelease(blit_sampler_);
        if (blit_filtered_pipeline_layout_ != nullptr) wgpuPipelineLayoutRelease(blit_filtered_pipeline_layout_);
        if (blit_filtered_layout_ != nullptr) wgpuBindGroupLayoutRelease(blit_filtered_layout_);
        if (surface_ != nullptr) {
            if (device_ != nullptr && !surface_unconfigured_) wgpuSurfaceUnconfigure(surface_);
            wgpuSurfaceRelease(surface_);
        }
        if (queue_ != nullptr) wgpuQueueRelease(queue_);
        if (device_ != nullptr && !keep_device) wgpuDeviceRelease(device_);
        if (adapter_ != nullptr) wgpuAdapterRelease(adapter_);
        wgpuInstanceRelease(instance_);
    });
#if defined(__EMSCRIPTEN__)
    stop_gpu_thread();
#endif
}

#if defined(__EMSCRIPTEN__)
namespace {

// Safari (iPadOS) sporadically returns null from getContext('webgpu') on the
// first start after a page load (about every second start, cause unknown);
// a later attempt succeeds. So the surface is tried again from the event
// loop for a while before the start fails.
constexpr int kSurfaceAttempts = 25;
constexpr double kSurfaceRetryMs = 200.0;

// Firefox (Linux, CI) sporadically answers requestAdapter with null and
// "WebGPU is disabled by blocklist" for one page while the pages before and
// after it get an adapter, also with gfx.webgpu.ignore-blocklist set. So a
// missing adapter is asked for again a few times before the start fails.
constexpr int kAdapterAttempts = 5;
constexpr double kAdapterRetryMs = 200.0;

struct SurfaceAttempt {
    WGPUInstance instance = nullptr;
    std::string selector;
    WGPUSurface* out = nullptr;
    std::string* error = nullptr;
    std::function<void()> next;
    WebGpuRenderer::Call* call = nullptr;
    int tries = 0;
};

void create_surface_with_retry(void* p) {
    auto* a = static_cast<SurfaceAttempt*>(p);
    WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvas = WGPU_EMSCRIPTEN_SURFACE_SOURCE_CANVAS_HTML_SELECTOR_INIT;
    canvas.selector = string_view(a->selector.c_str());
    WGPUSurfaceDescriptor desc = WGPU_SURFACE_DESCRIPTOR_INIT;
    desc.nextInChain = &canvas.chain;
    *a->out = wgpuInstanceCreateSurface(a->instance, &desc);
    ++a->tries;
    if (*a->out != nullptr) {
        if (a->tries > 1)
            std::fprintf(stderr, "webgpu: surface created on attempt %d\n", a->tries);
        std::function<void()> next = std::move(a->next);
        delete a;
        next();
        return;
    }
    if (a->tries >= kSurfaceAttempts) {
        *a->error = "could not create a surface for the window (" + std::to_string(a->tries) + " attempts)";
        WebGpuRenderer::Call* call = a->call;
        delete a;
        call->finish();
        return;
    }
    if (a->tries == 1)
        log_error("surface", "getContext('webgpu') failed, trying again");
    emscripten_set_timeout(create_surface_with_retry, kSurfaceRetryMs, a);
}

}  // namespace
#endif

void WebGpuRenderer::create_device(const RendererOptions& options) {
    // Callback state; lives until run() returns.
    struct Request {
        WebGpuRenderer* self;
        Call* call;
        std::function<void()> retry;  // the adapter request again (browser)
        int adapter_tries = 0;
    };
    Request request{this, nullptr};
    run([&](Call& call) {
        request.call = &call;
#if defined(LGP_WEBGPU_WGPU_NATIVE)
        // LGP_WEBGPU_BACKEND=vulkan|dx12|metal|gl picks the native backend
        // (pipeline creation cost differs a lot between them; docs/knowledge/
        // webgpu-backend-draw-kosten.md). Unset: wgpu's choice.
        WGPUInstanceExtras extras = {};
        extras.chain.sType = static_cast<WGPUSType>(WGPUSType_InstanceExtras);
        extras.flags = WGPUInstanceFlag_Default;
        // LGP_WEBGPU_DEBUG=passlabels names the passes for GPU profilers;
        // Vulkan passes the names on only with the Debug flag (debug utils).
        if (const char* debug = std::getenv("LGP_WEBGPU_DEBUG");
            debug != nullptr && std::strstr(debug, "passlabels") != nullptr)
            extras.flags = WGPUInstanceFlag_Debug | WGPUInstanceFlag_ValidationIndirectCall;
        if (const char* b = std::getenv("LGP_WEBGPU_BACKEND"); b != nullptr) {
            const std::string name = b;
            extras.backends = name == "vulkan" ? WGPUInstanceBackend_Vulkan
                              : name == "dx12" ? WGPUInstanceBackend_DX12
                              : name == "metal" ? WGPUInstanceBackend_Metal
                              : name == "gl"    ? WGPUInstanceBackend_GL
                                                : WGPUInstanceBackend_All;
        }
        WGPUInstanceDescriptor instance_desc = WGPU_INSTANCE_DESCRIPTOR_INIT;
        instance_desc.nextInChain = &extras.chain;
        instance_ = wgpuCreateInstance(&instance_desc);
#else
        instance_ = wgpuCreateInstance(nullptr);
#endif
        if (instance_ == nullptr) {
            error_ = "wgpuCreateInstance failed";
            call.finish();
            return;
        }
        // Runs once the surface (if any) exists; `request` and `options`
        // live until run() returns.
        auto request_adapter = [this, &request, &options] {
            WGPURequestAdapterOptions adapter_options = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
            adapter_options.compatibleSurface = surface_;
            adapter_options.forceFallbackAdapter = options.force_fallback_adapter;
            adapter_options.powerPreference =
                options.low_power ? WGPUPowerPreference_LowPower : WGPUPowerPreference_HighPerformance;
            WGPURequestAdapterCallbackInfo adapter_cb = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
            adapter_cb.mode = WGPUCallbackMode_AllowSpontaneous;
            adapter_cb.userdata1 = &request;
            adapter_cb.callback = [](WGPURequestAdapterStatus status, WGPUAdapter adapter, WGPUStringView message,
                                     void* userdata1, void*) {
                auto* req = static_cast<Request*>(userdata1);
                WebGpuRenderer* self = req->self;
                if (status != WGPURequestAdapterStatus_Success || adapter == nullptr) {
#if defined(__EMSCRIPTEN__)
                    if (++req->adapter_tries < kAdapterAttempts) {
                        log_error("adapter", "no WebGPU adapter (" + to_string(message) + "), trying again");
                        emscripten_set_timeout([](void* r) { static_cast<Request*>(r)->retry(); }, kAdapterRetryMs,
                                               req);
                        return;
                    }
#endif
                    self->error_ = "no WebGPU adapter: " + to_string(message);
                    req->call->finish();
                    return;
                }
                self->adapter_ = adapter;
                WGPUAdapterInfo info = WGPU_ADAPTER_INFO_INIT;
                if (wgpuAdapterGetInfo(adapter, &info) == WGPUStatus_Success) {
                    self->adapter_name_ = to_string(info.device);
                    const std::string description = to_string(info.description);
                    if (!description.empty())
                        self->adapter_name_ += " (" + description + ")";
                    const char* backend = info.backendType == WGPUBackendType_Vulkan   ? "Vulkan"
                                          : info.backendType == WGPUBackendType_D3D12  ? "D3D12"
                                          : info.backendType == WGPUBackendType_Metal  ? "Metal"
                                          : info.backendType == WGPUBackendType_OpenGL ? "OpenGL"
                                          : info.backendType == WGPUBackendType_WebGPU ? "WebGPU"
                                                                                       : nullptr;
                    if (backend != nullptr)
                        self->adapter_name_ += std::string(", ") + backend;
                    wgpuAdapterInfoFreeMembers(info);
                }

                WGPUDeviceDescriptor device_desc = WGPU_DEVICE_DESCRIPTOR_INIT;
                device_desc.label = string_view("lgp");
                // Compressed texture formats the game's textures use (BCn on
                // desktop GPUs, ASTC on mobile ones), when the adapter has them;
                // depth-clip-control for NVN's depth clamp (unclippedDepth).
                static std::vector<WGPUFeatureName> features;
                features.clear();
                // What the adapter offers, for the log (WebGpuBackend).
                self->adapter_features_.clear();
                for (WGPUFeatureName f :
                     {WGPUFeatureName_TextureCompressionBC, WGPUFeatureName_TextureCompressionASTC,
                      WGPUFeatureName_TextureCompressionETC2, WGPUFeatureName_RG11B10UfloatRenderable,
                      WGPUFeatureName_Float32Filterable, WGPUFeatureName_DepthClipControl,
                      WGPUFeatureName_TimestampQuery})
                    if (wgpuAdapterHasFeature(adapter, f))
                        self->adapter_features_.push_back(f);
                // float32-filterable: the game's 32-bit float textures go through
                // filtering samplers like every other colour texture.
                for (WGPUFeatureName f : {WGPUFeatureName_TextureCompressionBC, WGPUFeatureName_TextureCompressionASTC,
                                          WGPUFeatureName_RG11B10UfloatRenderable, WGPUFeatureName_Float32Filterable,
                                          WGPUFeatureName_DepthClipControl})
                    if (wgpuAdapterHasFeature(adapter, f))
                        features.push_back(f);
                // Diagnosis only (LGP_WEBGPU_DEBUG=gputime, GPU time per pass).
                if (const char* debug = std::getenv("LGP_WEBGPU_DEBUG");
                    debug != nullptr && std::strstr(debug, "gputime") != nullptr &&
                    wgpuAdapterHasFeature(adapter, WGPUFeatureName_TimestampQuery))
                    features.push_back(WGPUFeatureName_TimestampQuery);
#if defined(WGPU_LGP_PIPELINE_CACHE)
                // The driver's pipeline cache (Vulkan), with a wgpu-native
                // built with tools/webgpu/wgpu-native-pipeline-cache.patch.
                if (const auto f = static_cast<WGPUFeatureName>(WGPUNativeFeature_PipelineCache);
                    wgpuAdapterHasFeature(adapter, f))
                    features.push_back(f);
#endif
                device_desc.requiredFeatureCount = features.size();
                device_desc.requiredFeatures = features.data();
                // More vertex buffers than the guaranteed 8 when the adapter has
                // them (NVN has 16 vertex streams), and compute limits up to what
                // Maxwell compute programs use (1024 threads per workgroup,
                // z <= 64, 48 KiB shared memory), as far as the adapter allows;
                // the WebGPU defaults are 256 threads and 16 KiB. Everything else
                // stays at the defaults.
                static WGPULimits limits;
                limits = WGPU_LIMITS_INIT;
                if (WGPULimits adapter_limits = WGPU_LIMITS_INIT;
                    wgpuAdapterGetLimits(adapter, &adapter_limits) == WGPUStatus_Success) {
                    if (adapter_limits.maxVertexBuffers > 8)
                        limits.maxVertexBuffers = adapter_limits.maxVertexBuffers;
                    const auto raise = [](uint32_t* limit, uint32_t adapter_value, uint32_t want) {
                        const uint32_t v = std::min(adapter_value, want);
                        if (v > *limit || *limit == WGPU_LIMIT_U32_UNDEFINED)
                            *limit = v;
                    };
                    const auto raise64 = [](uint64_t* limit, uint64_t adapter_value, uint64_t want) {
                        const uint64_t v = std::min(adapter_value, want);
                        if (v > *limit || *limit == WGPU_LIMIT_U64_UNDEFINED)
                            *limit = v;
                    };
                    // At least the WebGPU spec defaults where the adapter has
                    // them. wgpu-native fills undefined limits from its
                    // downlevel defaults on adapters it does not count as fully
                    // WebGPU compliant (Metal on iOS): 16 KiB uniform bindings,
                    // 2048 texels per 2D dimension, 4 storage buffers. Guest
                    // constant buffers are bound with 64 KiB.
                    raise(&limits.maxTextureDimension1D, adapter_limits.maxTextureDimension1D, 8192);
                    raise(&limits.maxTextureDimension2D, adapter_limits.maxTextureDimension2D, 8192);
                    raise(&limits.maxTextureDimension3D, adapter_limits.maxTextureDimension3D, 2048);
                    raise(&limits.maxTextureArrayLayers, adapter_limits.maxTextureArrayLayers, 256);
                    raise(&limits.maxBindGroups, adapter_limits.maxBindGroups, 4);
                    raise(&limits.maxBindingsPerBindGroup, adapter_limits.maxBindingsPerBindGroup, 1000);
                    raise(&limits.maxDynamicUniformBuffersPerPipelineLayout,
                          adapter_limits.maxDynamicUniformBuffersPerPipelineLayout, 8);
                    raise(&limits.maxDynamicStorageBuffersPerPipelineLayout,
                          adapter_limits.maxDynamicStorageBuffersPerPipelineLayout, 4);
                    raise(&limits.maxSampledTexturesPerShaderStage, adapter_limits.maxSampledTexturesPerShaderStage, 16);
                    raise(&limits.maxSamplersPerShaderStage, adapter_limits.maxSamplersPerShaderStage, 16);
                    raise(&limits.maxStorageBuffersPerShaderStage, adapter_limits.maxStorageBuffersPerShaderStage, 8);
                    raise(&limits.maxStorageTexturesPerShaderStage, adapter_limits.maxStorageTexturesPerShaderStage, 4);
                    raise(&limits.maxUniformBuffersPerShaderStage, adapter_limits.maxUniformBuffersPerShaderStage, 12);
                    raise64(&limits.maxUniformBufferBindingSize, adapter_limits.maxUniformBufferBindingSize, 64 * 1024);
                    raise64(&limits.maxStorageBufferBindingSize, adapter_limits.maxStorageBufferBindingSize,
                            128ull << 20);
                    raise64(&limits.maxBufferSize, adapter_limits.maxBufferSize, 256ull << 20);
                    raise(&limits.maxVertexAttributes, adapter_limits.maxVertexAttributes, 16);
                    raise(&limits.maxVertexBufferArrayStride, adapter_limits.maxVertexBufferArrayStride, 2048);
                    raise(&limits.maxInterStageShaderVariables, adapter_limits.maxInterStageShaderVariables, 16);
                    raise(&limits.maxColorAttachments, adapter_limits.maxColorAttachments, 8);
                    raise(&limits.maxColorAttachmentBytesPerSample, adapter_limits.maxColorAttachmentBytesPerSample, 32);
                    raise(&limits.maxComputeWorkgroupsPerDimension, adapter_limits.maxComputeWorkgroupsPerDimension,
                          65535);
                    raise(&limits.maxComputeInvocationsPerWorkgroup, adapter_limits.maxComputeInvocationsPerWorkgroup,
                          1024);
                    raise(&limits.maxComputeWorkgroupSizeX, adapter_limits.maxComputeWorkgroupSizeX, 1024);
                    raise(&limits.maxComputeWorkgroupSizeY, adapter_limits.maxComputeWorkgroupSizeY, 1024);
                    raise(&limits.maxComputeWorkgroupSizeZ, adapter_limits.maxComputeWorkgroupSizeZ, 64);
                    raise(&limits.maxComputeWorkgroupStorageSize, adapter_limits.maxComputeWorkgroupStorageSize,
                          48 * 1024);
                    // 8 global memory slots plus LGP_WEBGPU_DEBUG=computeprobe's
                    // counters (wgsl::Options::gmem_probe).
                    raise(&limits.maxStorageBuffersPerShaderStage, adapter_limits.maxStorageBuffersPerShaderStage, 9);
                }
                device_desc.requiredLimits = &limits;
                device_desc.deviceLostCallbackInfo.mode = WGPUCallbackMode_AllowSpontaneous;
                device_desc.deviceLostCallbackInfo.userdata1 = self;
                device_desc.deviceLostCallbackInfo.callback = [](const WGPUDevice*, WGPUDeviceLostReason reason,
                                                                 WGPUStringView msg, void* u1, void*) {
                    if (reason == WGPUDeviceLostReason_Destroyed || reason == WGPUDeviceLostReason_CallbackCancelled)
                        return;
                    ++static_cast<WebGpuRenderer*>(u1)->errors_;
                    log_error("device lost", to_string(msg));
                };
                device_desc.uncapturedErrorCallbackInfo.userdata1 = self;
                device_desc.uncapturedErrorCallbackInfo.callback = [](const WGPUDevice*, WGPUErrorType,
                                                                      WGPUStringView msg, void* u1, void*) {
                    ++static_cast<WebGpuRenderer*>(u1)->errors_;
                    log_error("validation", to_string(msg));
                };
                WGPURequestDeviceCallbackInfo device_cb = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
                device_cb.mode = WGPUCallbackMode_AllowSpontaneous;
                device_cb.userdata1 = req;
                device_cb.callback = [](WGPURequestDeviceStatus status, WGPUDevice device, WGPUStringView msg,
                                        void* userdata1, void*) {
                    auto* r = static_cast<Request*>(userdata1);
                    if (status != WGPURequestDeviceStatus_Success || device == nullptr)
                        r->self->error_ = "no WebGPU device: " + to_string(msg);
                    else
                        r->self->device_ = device;
                    r->call->finish();
                };
                wgpuAdapterRequestDevice(adapter, &device_desc, device_cb);
            };
            wgpuInstanceRequestAdapter(instance_, &adapter_options, adapter_cb);
        };
        request.retry = request_adapter;
#if defined(__EMSCRIPTEN__)
        if (!options.canvas_selector.empty()) {
            auto* attempt = new SurfaceAttempt;
            attempt->instance = instance_;
            attempt->selector = options.canvas_selector;
            attempt->out = &surface_;
            attempt->error = &error_;
            attempt->next = request_adapter;
            attempt->call = &call;
            create_surface_with_retry(attempt);
            return;
        }
#else
        if (options.native_surface != nullptr) {
            WGPUSurfaceDescriptor surface_desc = WGPU_SURFACE_DESCRIPTOR_INIT;
            surface_desc.nextInChain = const_cast<WGPUChainedStruct*>(options.native_surface);
            surface_ = wgpuInstanceCreateSurface(instance_, &surface_desc);
            if (surface_ == nullptr) {
                error_ = "could not create a surface for the window";
                call.finish();
                return;
            }
        }
#endif
        request_adapter();
    });
    if (device_ == nullptr) {
        if (error_.empty())
            error_ = "no WebGPU device";
        return;
    }

    run_sync([&] {
        queue_ = wgpuDeviceGetQueue(device_);

        if (surface_ != nullptr) {
            WGPUSurfaceCapabilities caps = WGPU_SURFACE_CAPABILITIES_INIT;
            if (wgpuSurfaceGetCapabilities(surface_, adapter_, &caps) == WGPUStatus_Success && caps.formatCount > 0) {
                // The display shows memory bytes (sRGB-encoded by the game),
                // so present into a non-sRGB format.
                surface_format_ = caps.formats[0];
                for (size_t i = 0; i < caps.formatCount; ++i) {
                    if (caps.formats[i] == WGPUTextureFormat_BGRA8Unorm ||
                        caps.formats[i] == WGPUTextureFormat_RGBA8Unorm) {
                        surface_format_ = caps.formats[i];
                        break;
                    }
                }
                surface_format_ = linear_variant(surface_format_);
                alpha_mode_ = caps.alphaModeCount > 0 ? caps.alphaModes[0] : WGPUCompositeAlphaMode_Auto;
                for (size_t i = 0; i < caps.alphaModeCount; ++i)
                    if (caps.alphaModes[i] == WGPUCompositeAlphaMode_Opaque)
                        alpha_mode_ = WGPUCompositeAlphaMode_Opaque;
                auto supported = [&caps](WGPUPresentMode m) {
                    for (size_t i = 0; i < caps.presentModeCount; ++i)
                        if (caps.presentModes[i] == m) return true;
                    return false;
                };
                // Mailbox (no vsync wait, no tearing) falls back to Immediate,
                // everything to Fifo (always supported).
                if (!supported(present_mode_) && present_mode_ == WGPUPresentMode_Mailbox)
                    present_mode_ = WGPUPresentMode_Immediate;
                if (!supported(present_mode_))
                    present_mode_ = WGPUPresentMode_Fifo;
                wgpuSurfaceCapabilitiesFreeMembers(caps);
            } else {
                surface_format_ = WGPUTextureFormat_BGRA8Unorm;
            }
#if defined(__EMSCRIPTEN__)
            present_mode_ = WGPUPresentMode_Fifo;  // the only mode a canvas has
#endif
        }

        // Blit resources.
        WGPUBindGroupLayoutEntry entries[2] = {WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT, WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT};
        entries[0].binding = 0;
        entries[0].visibility = WGPUShaderStage_Vertex;
        entries[0].buffer.type = WGPUBufferBindingType_Uniform;
        entries[0].buffer.minBindingSize = sizeof(BlitParams);
        entries[1].binding = 1;
        entries[1].visibility = WGPUShaderStage_Fragment;
        entries[1].texture.sampleType = WGPUTextureSampleType_UnfilterableFloat;
        entries[1].texture.viewDimension = WGPUTextureViewDimension_2D;
        WGPUBindGroupLayoutDescriptor layout_desc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
        layout_desc.label = string_view("blit");
        layout_desc.entryCount = 2;
        layout_desc.entries = entries;
        blit_layout_ = wgpuDeviceCreateBindGroupLayout(device_, &layout_desc);
        WGPUPipelineLayoutDescriptor pl_desc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
        pl_desc.bindGroupLayoutCount = 1;
        pl_desc.bindGroupLayouts = &blit_layout_;
        blit_pipeline_layout_ = wgpuDeviceCreatePipelineLayout(device_, &pl_desc);
        // Filtered blits: the same uniforms (also read by the fragment
        // stage), a filterable texture and a linear sampler.
        WGPUBindGroupLayoutEntry filtered[3] = {entries[0], entries[1], WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT};
        filtered[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
        filtered[1].texture.sampleType = WGPUTextureSampleType_Float;
        filtered[2].binding = 2;
        filtered[2].visibility = WGPUShaderStage_Fragment;
        filtered[2].sampler.type = WGPUSamplerBindingType_Filtering;
        layout_desc.label = string_view("blit filtered");
        layout_desc.entryCount = 3;
        layout_desc.entries = filtered;
        blit_filtered_layout_ = wgpuDeviceCreateBindGroupLayout(device_, &layout_desc);
        pl_desc.bindGroupLayouts = &blit_filtered_layout_;
        blit_filtered_pipeline_layout_ = wgpuDeviceCreatePipelineLayout(device_, &pl_desc);
        WGPUSamplerDescriptor sampler_desc = WGPU_SAMPLER_DESCRIPTOR_INIT;
        sampler_desc.label = string_view("blit");
        sampler_desc.magFilter = WGPUFilterMode_Linear;
        sampler_desc.minFilter = WGPUFilterMode_Linear;
        blit_sampler_ = wgpuDeviceCreateSampler(device_, &sampler_desc);
        blit_shader_ = create_wgsl_module(device_, "blit", kBlitWgsl);
        WGPUBufferDescriptor buf_desc = WGPU_BUFFER_DESCRIPTOR_INIT;
        buf_desc.label = string_view("blit params");
        buf_desc.size = sizeof(BlitParams);
        buf_desc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
        blit_uniforms_ = wgpuDeviceCreateBuffer(device_, &buf_desc);
    });
}

// --- GPU thread ------------------------------------------------------------

namespace {

struct LocalCall final : WebGpuRenderer::Call {
    std::atomic<bool> done{false};
    void finish() override { done.store(true, std::memory_order_release); }
};

#if defined(__EMSCRIPTEN__)
struct ProxiedCall final : WebGpuRenderer::Call {
    em_proxying_ctx* ctx = nullptr;
    const WebGpuRenderer::Task* task = nullptr;
    std::atomic<uint64_t>* busy_ns = nullptr;
    void finish() override { emscripten_proxy_finish(ctx); }
};

// Runs a proxied task where it was proxied to.
void execute_proxied(ProxiedCall* c) {
    // `c` lives on the caller's stack and may be gone once the task called
    // finish(): take what is needed afterwards first.
    std::atomic<uint64_t>* busy = c->busy_ns;
    const auto t0 = std::chrono::steady_clock::now();
    (*c->task)(*c);
    busy->fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now() - t0)
                                 .count()),
                    std::memory_order_relaxed);
}
#endif

}  // namespace

namespace {

uint64_t nanos_since(std::chrono::steady_clock::time_point t0) {
    return uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
}

}  // namespace

void WebGpuRenderer::run(const Task& task) {
    std::unique_lock lock(run_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        const auto t0 = std::chrono::steady_clock::now();
        lock.lock();
        run_lock_waits_.fetch_add(1, std::memory_order_relaxed);
        run_lock_wait_ns_.fetch_add(nanos_since(t0), std::memory_order_relaxed);
    }
#if defined(__EMSCRIPTEN__)
    if (!emscripten_is_main_runtime_thread()) {
        ProxiedCall call;
        call.task = &task;
        call.busy_ns = &run_busy_ns_;
        const bool own = gpu_thread_queue_ != nullptr;
        const int ok = emscripten_proxy_sync_with_ctx(
            own ? gpu_thread_queue_ : emscripten_proxy_get_system_queue(),
            own ? gpu_thread_ : emscripten_main_runtime_thread_id(),
            own ? +[](em_proxying_ctx* ctx, void* arg) {
                auto* c = static_cast<ProxiedCall*>(arg);
                c->ctx = ctx;
                execute_proxied(c);
            }
                : +[](em_proxying_ctx* ctx, void* arg) {
                      auto* c = static_cast<ProxiedCall*>(arg);
                      c->ctx = ctx;
                      // The browser main thread also runs the system queue
                      // while it waits for any lock (malloc's included), so a
                      // task could start inside a WebGPU callback that holds
                      // one of the backend's mutexes and lock it again, a
                      // deadlock (browser.pipeline_cache, rare). The task runs
                      // on its own turn of the event loop instead.
                      emscripten_set_immediate([](void* a) { execute_proxied(static_cast<ProxiedCall*>(a)); }, c);
                  },
            &call);
        if (!ok) {
            std::fprintf(stderr, "webgpu: proxying to the GPU thread failed\n");
            std::abort();
        }
        run_calls_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // On the main thread itself (only without PROXY_TO_PTHREAD) nothing can
    // wait for the event loop: tasks must finish synchronously.
    LocalCall call;
    task(call);
    if (!call.done.load()) {
        std::fprintf(stderr, "webgpu: asynchronous GPU task on the browser main thread\n");
        std::abort();
    }
#else
    LocalCall call;
    const auto t0 = std::chrono::steady_clock::now();
    task(call);
    run_calls_.fetch_add(1, std::memory_order_relaxed);
    run_busy_ns_.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now() - t0)
                                        .count()),
                           std::memory_order_relaxed);
    // A wait that ends within gpu_spin_ never sleeps: the core stays out of
    // its idle states (P7.3, Android).
    const auto wait_start = std::chrono::steady_clock::now();
    const auto spin_until = wait_start + gpu_spin_;
    uint64_t polls = 0;
    while (!call.done.load(std::memory_order_acquire)) {
        ++polls;
        if (instance_ != nullptr)
            wgpuInstanceProcessEvents(instance_);
        if (call.done.load(std::memory_order_acquire))
            break;
#if defined(LGP_WEBGPU_WGPU_NATIVE)
        // Blocks until submitted work is done, so buffer mappings complete:
        // the latest submission, or the one the task named.
        if (device_ != nullptr) {
            const WGPUSubmissionIndex index = call.wait_submission();
            wgpuDevicePoll(device_, std::chrono::steady_clock::now() >= spin_until,
                           call.has_wait_submission() ? &index : nullptr);
        } else
            std::this_thread::yield();
#else
        std::this_thread::yield();
#endif
    }
    if (polls != 0) {
        run_wait_polls_.fetch_add(polls, std::memory_order_relaxed);
        run_wait_ns_.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                            std::chrono::steady_clock::now() - wait_start)
                                            .count()),
                               std::memory_order_relaxed);
    }
#endif
}

void WebGpuRenderer::run_sync(const std::function<void()>& fn) {
    run([&](Call& call) {
        fn();
        call.finish();
    });
}

void WebGpuRenderer::configure_surface_locked() {
    WGPUSurfaceConfiguration config = WGPU_SURFACE_CONFIGURATION_INIT;
    config.device = device_;
    config.format = surface_format_;
    config.usage = WGPUTextureUsage_RenderAttachment;
    config.width = std::max(size_.width, 1u);
    config.height = std::max(size_.height, 1u);
    config.presentMode = present_mode_;
    config.alphaMode = alpha_mode_;
    wgpuSurfaceConfigure(surface_, &config);
    surface_dirty_ = false;
    // Swapchain size in pixels (a 4K TV gets 3840x2160 with HIGH_PIXEL_DENSITY);
    // only on a change, an outdated surface is reconfigured with the same size.
    if (config.width != logged_size_.width || config.height != logged_size_.height) {
        std::fprintf(stderr, "webgpu: surface %ux%u px, format 0x%x, present mode %u\n", config.width, config.height,
                     unsigned(config.format), unsigned(config.presentMode));
        logged_size_ = {config.width, config.height};
    }
}

bool parse_present_filter(const std::string& name, PresentFilter* out) {
    if (name == "off") *out = PresentFilter::Nearest;
    else if (name == "linear") *out = PresentFilter::Linear;
    else if (name == "fxaa") *out = PresentFilter::Fxaa;
    else return false;
    return true;
}

const char* present_filter_name(PresentFilter f) {
    switch (f) {
    case PresentFilter::Linear: return "linear";
    case PresentFilter::Fxaa: return "fxaa";
    case PresentFilter::Nearest: break;
    }
    return "off";
}

WGPURenderPipeline WebGpuRenderer::blit_pipeline(WGPUTextureFormat format, PresentFilter filter) {
    const uint64_t key = uint64_t(format) << 8 | uint64_t(filter);
    auto it = blit_pipelines_.find(key);
    if (it != blit_pipelines_.end())
        return it->second;
    WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
    target.format = format;
    target.writeMask = WGPUColorWriteMask_All;
    WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
    fragment.module = blit_shader_;
    fragment.entryPoint = string_view(filter == PresentFilter::Fxaa     ? "fs_fxaa"
                                      : filter == PresentFilter::Linear ? "fs_linear"
                                                                        : "fs");
    fragment.targetCount = 1;
    fragment.targets = &target;
    WGPURenderPipelineDescriptor desc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    desc.label = string_view("blit");
    desc.layout = filter == PresentFilter::Nearest ? blit_pipeline_layout_ : blit_filtered_pipeline_layout_;
    desc.vertex.module = blit_shader_;
    desc.vertex.entryPoint = string_view("vs");
    desc.primitive.topology = WGPUPrimitiveTopology_TriangleStrip;
    desc.fragment = &fragment;
    WGPURenderPipeline pipeline = wgpuDeviceCreateRenderPipeline(device_, &desc);
    blit_pipelines_[key] = pipeline;
    return pipeline;
}

bool WebGpuRenderer::present_texture(WGPUTextureView view, uint32_t width, uint32_t height, const PresentRect& rect) {
    std::lock_guard surface_lock(surface_mutex_);
    if (surface_ == nullptr) {
        // In the background on iOS/Android (replace_surface(nullptr)) until
        // the window hands a new one back.
        if (no_surface_presents_++ % 300 == 0)
            std::fprintf(stderr, "webgpu: no surface, %llu presents in a row not shown\n",
                         (unsigned long long)no_surface_presents_);
        return true;
    }
    if (no_surface_presents_ != 0) {
        std::fprintf(stderr, "webgpu: surface back after %llu presents not shown\n",
                     (unsigned long long)no_surface_presents_);
        no_surface_presents_ = 0;
    }
    {
        std::lock_guard lock(size_mutex_);
        if (surface_dirty_)
            configure_surface_locked();
    }
    WGPUSurfaceTexture surface_texture = WGPU_SURFACE_TEXTURE_INIT;
    const auto t_acquire = std::chrono::steady_clock::now();
    wgpuSurfaceGetCurrentTexture(surface_, &surface_texture);
    surface_presents_.fetch_add(1, std::memory_order_relaxed);
    surface_acquire_ns_.fetch_add(nanos_since(t_acquire), std::memory_order_relaxed);
    if (surface_texture.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
        surface_texture.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) {
        if (surface_texture.texture != nullptr)
            wgpuTextureRelease(surface_texture.texture);
        // Nothing reaches the screen while this repeats (the window keeps
        // its last image); logged at the start of a run of failures and
        // every 300 after.
        if (acquire_failures_++ % 300 == 0)
            std::fprintf(stderr, "webgpu: no surface texture (status %d), %llu presents in a row not shown\n",
                         int(surface_texture.status), (unsigned long long)acquire_failures_);
        std::lock_guard lock(size_mutex_);
        surface_dirty_ = true;  // outdated or lost: reconfigure next time
        return false;
    }
    if (acquire_failures_ != 0) {
        std::fprintf(stderr, "webgpu: surface texture again after %llu presents not shown\n",
                     (unsigned long long)acquire_failures_);
        acquire_failures_ = 0;
    }
    const uint32_t tw = wgpuTextureGetWidth(surface_texture.texture);
    const uint32_t th = wgpuTextureGetHeight(surface_texture.texture);
    if (view != last_view_) {  // for redraw()
        if (view != nullptr)
            wgpuTextureViewAddRef(view);
        if (last_view_ != nullptr)
            wgpuTextureViewRelease(last_view_);
        last_view_ = view;
    }
    last_width_ = width, last_height_ = height, last_rect_ = rect;
    if (view == nullptr)
        width = height = 0;

    // Source rectangle, clamped to the texture.
    int64_t sx = std::clamp<int64_t>(rect.x, 0, width), sy = std::clamp<int64_t>(rect.y, 0, height);
    int64_t sw = rect.width > 0 ? rect.width : int64_t{width} - sx;
    int64_t sh = rect.height > 0 ? rect.height : int64_t{height} - sy;
    sw = std::clamp<int64_t>(sw, 0, int64_t{width} - sx);
    sh = std::clamp<int64_t>(sh, 0, int64_t{height} - sy);
    // Placed by the aspect mode: fit (letterbox / pillarbox), fill (the
    // quad reaches beyond the surface, the rasterizer cuts it) or stretch.
    float fx = 1.0f, fy = 1.0f;
    if (sw > 0 && sh > 0 && tw > 0 && th > 0) {
        const auto image_aspect = image_aspect_.load(std::memory_order_relaxed);
        const float aspect = image_aspect != nullptr ? image_aspect() : 0.0f;
        const float iw = aspect > 0 ? float(sh) * aspect : float(sw);
        const ImageRect placed = place_image(aspect_mode(), iw, float(sh), float(tw), float(th));
        fx = placed.width / float(tw);
        fy = placed.height / float(th);
        std::lock_guard lock(placed_mutex_);
        placed_ = placed;
        placed_w_ = float(tw), placed_h_ = float(th);
    }
    const float y0 = float(rect.flip_y ? sy + sh : sy), y1 = float(rect.flip_y ? sy : sy + sh);
    const BlitParams params = {
        {float(sx), y0, float(sx + sw), y1},
        {-fx, fy, fx, -fy},
        {float(sx) + 0.5f, float(sy) + 0.5f, float(sx + sw) - 0.5f, float(sy + sh) - 0.5f},
    };
    wgpuQueueWriteBuffer(queue_, blit_uniforms_, 0, &params, sizeof(params));
    const PresentFilter filter = rect.filterable ? present_filter() : PresentFilter::Nearest;

    WGPUBindGroupEntry entries[3] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT,
                                     WGPU_BIND_GROUP_ENTRY_INIT};
    entries[0].binding = 0;
    entries[0].buffer = blit_uniforms_;
    entries[0].size = sizeof(BlitParams);
    entries[1].binding = 1;
    entries[1].textureView = view;
    entries[2].binding = 2;
    entries[2].sampler = blit_sampler_;
    WGPUBindGroupDescriptor bg_desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bg_desc.layout = filter == PresentFilter::Nearest ? blit_layout_ : blit_filtered_layout_;
    bg_desc.entryCount = filter == PresentFilter::Nearest ? 2 : 3;
    bg_desc.entries = entries;
    WGPUBindGroup bind_group = view != nullptr ? wgpuDeviceCreateBindGroup(device_, &bg_desc) : nullptr;

    WGPUTextureView target = wgpuTextureCreateView(surface_texture.texture, nullptr);
    WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(device_, nullptr);
    WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    color.view = target;
    color.loadOp = WGPULoadOp_Clear;
    color.storeOp = WGPUStoreOp_Store;
    color.clearValue = WGPUColor{0.0, 0.0, 0.0, 1.0};
    WGPURenderPassDescriptor pass_desc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    pass_desc.colorAttachmentCount = 1;
    pass_desc.colorAttachments = &color;
    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &pass_desc);
    if (sw > 0 && sh > 0) {
        wgpuRenderPassEncoderSetPipeline(pass, blit_pipeline(surface_format_, filter));
        wgpuRenderPassEncoderSetBindGroup(pass, 0, bind_group, 0, nullptr);
        wgpuRenderPassEncoderDraw(pass, 4, 1, 0, 0);
    }
    draw_overlay(pass, tw, th);
    draw_ui(pass, tw, th);
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);
    WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder, nullptr);
    const auto t_submit = std::chrono::steady_clock::now();
    wgpuQueueSubmit(queue_, 1, &commands);
    surface_submit_ns_.fetch_add(nanos_since(t_submit), std::memory_order_relaxed);
    wgpuCommandBufferRelease(commands);
    wgpuCommandEncoderRelease(encoder);
    if (bind_group != nullptr)
        wgpuBindGroupRelease(bind_group);
    wgpuTextureViewRelease(target);
#if !defined(__EMSCRIPTEN__)
    // In the browser the canvas presents when the GPU thread's task ends.
    const auto t_present = std::chrono::steady_clock::now();
    wgpuSurfacePresent(surface_);
    surface_present_ns_.fetch_add(nanos_since(t_present), std::memory_order_relaxed);
#endif
    wgpuTextureRelease(surface_texture.texture);
    return true;
}

bool WebGpuRenderer::window_to_image(float wx, float wy, float* ix, float* iy) const {
    std::lock_guard lock(placed_mutex_);
    return lgp::platform::window_to_image(placed_, placed_w_, placed_h_, wx, wy, ix, iy);
}

void WebGpuRenderer::set_overlay(std::vector<OverlayCircle> circles) {
    if (circles.size() > kMaxOverlayCircles)
        circles.resize(kMaxOverlayCircles);
    std::lock_guard lock(overlay_mutex_);
    overlay_ = std::move(circles);
}

void WebGpuRenderer::draw_overlay(WGPURenderPassEncoder pass, uint32_t width, uint32_t height) {
    OverlayParams params{};
    {
        std::lock_guard lock(overlay_mutex_);
        if (overlay_.empty())
            return;
        params.info[0] = float(width);
        params.info[1] = float(height);
        params.info[2] = float(overlay_.size());
        size_t strokes = 0;
        for (size_t i = 0; i < overlay_.size(); ++i) {
            const OverlayCircle& c = overlay_[i];
            params.c[i][0] = c.x, params.c[i][1] = c.y, params.c[i][2] = c.r;
            params.k[i][0] = float(c.kind), params.k[i][1] = c.pressed ? 1.0f : 0.0f;
            params.k[i][2] = std::clamp(c.alpha, 0.0f, 1.0f);
            params.e[i][0] = c.half_w, params.e[i][1] = c.half_h, params.e[i][2] = c.corner;
            params.c[i][3] = float(strokes);
            for (const OverlayStroke& st : overlay_label_strokes(c.label)) {
                if (strokes == kMaxOverlayStrokes)
                    break;
                params.s[strokes][0] = st.ax, params.s[strokes][1] = st.ay;
                params.s[strokes][2] = st.bx, params.s[strokes][3] = st.by;
                params.t[strokes][0] = st.arc ? 1.0f : 0.0f, params.t[strokes][1] = st.r;
                ++strokes;
            }
            params.k[i][3] = float(strokes) - params.c[i][3];
        }
    }
    if (overlay_shader_ == nullptr) {
        WGPUBindGroupLayoutEntry entry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        entry.binding = 0;
        entry.visibility = WGPUShaderStage_Fragment;
        entry.buffer.type = WGPUBufferBindingType_Uniform;
        entry.buffer.minBindingSize = sizeof(OverlayParams);
        WGPUBindGroupLayoutDescriptor layout_desc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
        layout_desc.label = string_view("overlay");
        layout_desc.entryCount = 1;
        layout_desc.entries = &entry;
        overlay_layout_ = wgpuDeviceCreateBindGroupLayout(device_, &layout_desc);
        WGPUPipelineLayoutDescriptor pl_desc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
        pl_desc.bindGroupLayoutCount = 1;
        pl_desc.bindGroupLayouts = &overlay_layout_;
        overlay_pipeline_layout_ = wgpuDeviceCreatePipelineLayout(device_, &pl_desc);
        overlay_shader_ = create_wgsl_module(device_, "overlay", kOverlayWgsl);
        WGPUBufferDescriptor buf_desc = WGPU_BUFFER_DESCRIPTOR_INIT;
        buf_desc.label = string_view("overlay params");
        buf_desc.size = sizeof(OverlayParams);
        buf_desc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
        overlay_uniforms_ = wgpuDeviceCreateBuffer(device_, &buf_desc);
        WGPUBindGroupEntry bg_entry = WGPU_BIND_GROUP_ENTRY_INIT;
        bg_entry.binding = 0;
        bg_entry.buffer = overlay_uniforms_;
        bg_entry.size = sizeof(OverlayParams);
        WGPUBindGroupDescriptor bg_desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        bg_desc.layout = overlay_layout_;
        bg_desc.entryCount = 1;
        bg_desc.entries = &bg_entry;
        overlay_bind_group_ = wgpuDeviceCreateBindGroup(device_, &bg_desc);
    }
    auto it = overlay_pipelines_.find(static_cast<uint32_t>(surface_format_));
    if (it == overlay_pipelines_.end()) {
        WGPUBlendState blend = WGPU_BLEND_STATE_INIT;
        blend.color.operation = WGPUBlendOperation_Add;
        blend.color.srcFactor = WGPUBlendFactor_SrcAlpha;
        blend.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
        blend.alpha.operation = WGPUBlendOperation_Add;
        blend.alpha.srcFactor = WGPUBlendFactor_Zero;
        blend.alpha.dstFactor = WGPUBlendFactor_One;
        WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
        target.format = surface_format_;
        target.blend = &blend;
        target.writeMask = WGPUColorWriteMask_All;
        WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
        fragment.module = overlay_shader_;
        fragment.entryPoint = string_view("fs");
        fragment.targetCount = 1;
        fragment.targets = &target;
        WGPURenderPipelineDescriptor desc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
        desc.label = string_view("overlay");
        desc.layout = overlay_pipeline_layout_;
        desc.vertex.module = overlay_shader_;
        desc.vertex.entryPoint = string_view("vs");
        desc.primitive.topology = WGPUPrimitiveTopology_TriangleStrip;
        desc.fragment = &fragment;
        it = overlay_pipelines_.emplace(static_cast<uint32_t>(surface_format_),
                                        wgpuDeviceCreateRenderPipeline(device_, &desc)).first;
    }
    wgpuQueueWriteBuffer(queue_, overlay_uniforms_, 0, &params, sizeof(params));
    wgpuRenderPassEncoderSetPipeline(pass, it->second);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, overlay_bind_group_, 0, nullptr);
    wgpuRenderPassEncoderDraw(pass, 4, 1, 0, 0);
}

void WebGpuRenderer::set_ui(std::shared_ptr<const UiDrawData> ui) {
    std::lock_guard lock(ui_mutex_);
    ui_ = std::move(ui);
}

void WebGpuRenderer::release_ui_texture(UiGpuTexture& slot) {
    if (slot.bind_group != nullptr)
        wgpuBindGroupRelease(slot.bind_group);
    if (slot.texture != nullptr)
        wgpuTextureRelease(slot.texture);
    slot = {};
}

// Uploads tex into slot unless it holds this version of it already.
bool WebGpuRenderer::sync_ui_texture(UiGpuTexture& slot, const UiTexture& tex) {
    if (tex.width == 0 || tex.height == 0 || tex.rgba.size() < size_t{tex.width} * tex.height * 4)
        return false;
    if (slot.source == &tex && slot.texture != nullptr && slot.version == tex.version)
        return true;
    if (slot.bind_group != nullptr)
        wgpuBindGroupRelease(slot.bind_group);
    if (slot.texture != nullptr)
        wgpuTextureRelease(slot.texture);
    WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    desc.label = string_view("ui texture");
    desc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    desc.size = {tex.width, tex.height, 1};
    desc.format = WGPUTextureFormat_RGBA8Unorm;
    slot.texture = wgpuDeviceCreateTexture(device_, &desc);
    WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    dst.texture = slot.texture;
    WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
    layout.bytesPerRow = tex.width * 4;
    layout.rowsPerImage = tex.height;
    const WGPUExtent3D extent = {tex.width, tex.height, 1};
    wgpuQueueWriteTexture(queue_, &dst, tex.rgba.data(), size_t{tex.width} * tex.height * 4, &layout, &extent);
    WGPUTextureView view = wgpuTextureCreateView(slot.texture, nullptr);
    WGPUBindGroupEntry bg[3] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
    bg[0].binding = 0;
    bg[0].buffer = ui_uniforms_;
    bg[0].size = sizeof(UiParams);
    bg[1].binding = 1;
    bg[1].textureView = view;
    bg[2].binding = 2;
    bg[2].sampler = ui_sampler_;
    WGPUBindGroupDescriptor bg_desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bg_desc.layout = ui_layout_;
    bg_desc.entryCount = 3;
    bg_desc.entries = bg;
    slot.bind_group = wgpuDeviceCreateBindGroup(device_, &bg_desc);
    wgpuTextureViewRelease(view);
    slot.version = tex.version;
    slot.source = &tex;
    return true;
}

void WebGpuRenderer::draw_ui(WGPURenderPassEncoder pass, uint32_t width, uint32_t height) {
    std::shared_ptr<const UiDrawData> ui;
    {
        std::lock_guard lock(ui_mutex_);
        ui = ui_;
    }
    if (!ui || ui->commands.empty() || ui->indices.empty() || !ui->texture || ui->texture->width == 0 ||
        ui->width <= 0 || ui->height <= 0 || width == 0 || height == 0)
        return;
    const UiTexture& tex = *ui->texture;
    if (tex.rgba.size() < size_t{tex.width} * tex.height * 4)
        return;
    if (ui_shader_ == nullptr) {
        WGPUBindGroupLayoutEntry entries[3] = {WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT, WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT,
                                               WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT};
        entries[0].binding = 0;
        entries[0].visibility = WGPUShaderStage_Vertex;
        entries[0].buffer.type = WGPUBufferBindingType_Uniform;
        entries[0].buffer.minBindingSize = sizeof(UiParams);
        entries[1].binding = 1;
        entries[1].visibility = WGPUShaderStage_Fragment;
        entries[1].texture.sampleType = WGPUTextureSampleType_Float;
        entries[1].texture.viewDimension = WGPUTextureViewDimension_2D;
        entries[2].binding = 2;
        entries[2].visibility = WGPUShaderStage_Fragment;
        entries[2].sampler.type = WGPUSamplerBindingType_Filtering;
        WGPUBindGroupLayoutDescriptor layout_desc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
        layout_desc.label = string_view("ui");
        layout_desc.entryCount = 3;
        layout_desc.entries = entries;
        ui_layout_ = wgpuDeviceCreateBindGroupLayout(device_, &layout_desc);
        WGPUPipelineLayoutDescriptor pl_desc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
        pl_desc.bindGroupLayoutCount = 1;
        pl_desc.bindGroupLayouts = &ui_layout_;
        ui_pipeline_layout_ = wgpuDeviceCreatePipelineLayout(device_, &pl_desc);
        ui_shader_ = create_wgsl_module(device_, "ui", kUiWgsl);
        WGPUBufferDescriptor buf_desc = WGPU_BUFFER_DESCRIPTOR_INIT;
        buf_desc.label = string_view("ui params");
        buf_desc.size = sizeof(UiParams);
        buf_desc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
        ui_uniforms_ = wgpuDeviceCreateBuffer(device_, &buf_desc);
        WGPUSamplerDescriptor sampler_desc = WGPU_SAMPLER_DESCRIPTOR_INIT;
        sampler_desc.magFilter = WGPUFilterMode_Linear;
        sampler_desc.minFilter = WGPUFilterMode_Linear;
        ui_sampler_ = wgpuDeviceCreateSampler(device_, &sampler_desc);
    }
    if (!sync_ui_texture(ui_atlas_, tex))
        return;
    // Pictures stay uploaded while the same objects come back.
    for (size_t i = ui->images.size(); i < ui_images_.size(); ++i)
        release_ui_texture(ui_images_[i]);
    ui_images_.resize(ui->images.size());
    for (size_t i = 0; i < ui->images.size(); ++i)
        if (ui->images[i] == nullptr || !sync_ui_texture(ui_images_[i], *ui->images[i]))
            release_ui_texture(ui_images_[i]);
    // One pipeline per surface format and blend (UiCommand::blend).
    const auto pipeline_for = [&](uint8_t mode) {
    const uint32_t key = static_cast<uint32_t>(surface_format_) * 2u + (mode == 1 ? 1u : 0u);
    auto it = ui_pipelines_.find(key);
    if (it == ui_pipelines_.end()) {
        WGPUVertexAttribute attributes[3] = {WGPU_VERTEX_ATTRIBUTE_INIT, WGPU_VERTEX_ATTRIBUTE_INIT,
                                             WGPU_VERTEX_ATTRIBUTE_INIT};
        attributes[0].format = WGPUVertexFormat_Float32x2;
        attributes[0].offset = offsetof(UiVertex, x);
        attributes[0].shaderLocation = 0;
        attributes[1].format = WGPUVertexFormat_Float32x2;
        attributes[1].offset = offsetof(UiVertex, u);
        attributes[1].shaderLocation = 1;
        attributes[2].format = WGPUVertexFormat_Unorm8x4;
        attributes[2].offset = offsetof(UiVertex, rgba);
        attributes[2].shaderLocation = 2;
        WGPUVertexBufferLayout vb = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
        vb.arrayStride = sizeof(UiVertex);
        vb.stepMode = WGPUVertexStepMode_Vertex;
        vb.attributeCount = 3;
        vb.attributes = attributes;
        WGPUBlendState blend = WGPU_BLEND_STATE_INIT;
        blend.color.operation = WGPUBlendOperation_Add;
        blend.color.srcFactor = WGPUBlendFactor_SrcAlpha;
        blend.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
        blend.alpha.operation = WGPUBlendOperation_Add;
        blend.alpha.srcFactor = WGPUBlendFactor_Zero;
        blend.alpha.dstFactor = WGPUBlendFactor_One;
        if (mode == 1) {  // destination × source colour
            blend.color.srcFactor = WGPUBlendFactor_Zero;
            blend.color.dstFactor = WGPUBlendFactor_Src;
        }
        WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
        target.format = surface_format_;
        target.blend = &blend;
        target.writeMask = WGPUColorWriteMask_All;
        WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
        fragment.module = ui_shader_;
        fragment.entryPoint = string_view("fs");
        fragment.targetCount = 1;
        fragment.targets = &target;
        WGPURenderPipelineDescriptor desc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
        desc.label = string_view("ui");
        desc.layout = ui_pipeline_layout_;
        desc.vertex.module = ui_shader_;
        desc.vertex.entryPoint = string_view("vs");
        desc.vertex.bufferCount = 1;
        desc.vertex.buffers = &vb;
        desc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
        desc.fragment = &fragment;
        it = ui_pipelines_.emplace(key, wgpuDeviceCreateRenderPipeline(device_, &desc)).first;
    }
    return it->second;
    };
    const WGPURenderPipeline alpha_pipeline = pipeline_for(0);
    // Buffers grow to the largest list seen (sizes rounded up to 4 bytes for writeBuffer).
    const auto upload = [&](WGPUBuffer& buffer, uint64_t& capacity, WGPUBufferUsage usage, const void* data,
                            uint64_t bytes, const char* label) {
        const uint64_t padded = (bytes + 3) & ~uint64_t{3};
        if (padded > capacity) {
            if (buffer != nullptr)
                wgpuBufferRelease(buffer);
            capacity = std::max<uint64_t>(padded * 2, 64 * 1024);
            WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
            desc.label = string_view(label);
            desc.size = capacity;
            desc.usage = usage | WGPUBufferUsage_CopyDst;
            buffer = wgpuDeviceCreateBuffer(device_, &desc);
        }
        if (padded == bytes) {
            wgpuQueueWriteBuffer(queue_, buffer, 0, data, bytes);
        } else {
            std::vector<uint8_t> copy(padded, 0);
            std::memcpy(copy.data(), data, bytes);
            wgpuQueueWriteBuffer(queue_, buffer, 0, copy.data(), padded);
        }
    };
    const uint64_t vertex_bytes = ui->vertices.size() * sizeof(UiVertex);
    const uint64_t index_bytes = ui->indices.size() * sizeof(uint32_t);
    upload(ui_vertices_, ui_vertex_bytes_, WGPUBufferUsage_Vertex, ui->vertices.data(), vertex_bytes, "ui vertices");
    upload(ui_indices_, ui_index_bytes_, WGPUBufferUsage_Index, ui->indices.data(), index_bytes, "ui indices");
    const UiParams params = {{2.0f / ui->width, 2.0f / ui->height, is_srgb(surface_format_) ? 1.0f : 0.0f, 0.0f}};
    wgpuQueueWriteBuffer(queue_, ui_uniforms_, 0, &params, sizeof(params));

    wgpuRenderPassEncoderSetPipeline(pass, alpha_pipeline);
    WGPURenderPipeline current = alpha_pipeline;
    wgpuRenderPassEncoderSetVertexBuffer(pass, 0, ui_vertices_, 0, vertex_bytes);
    wgpuRenderPassEncoderSetIndexBuffer(pass, ui_indices_, WGPUIndexFormat_Uint32, 0, index_bytes);
    const float sx = float(width) / ui->width, sy = float(height) / ui->height;
    const size_t vertex_count = ui->vertices.size();
    WGPUBindGroup bound = nullptr;
    for (const UiCommand& c : ui->commands) {
        if (c.index_count == 0 || uint64_t{c.first_index} + c.index_count > ui->indices.size() ||
            c.base_vertex >= vertex_count)
            continue;
        const float x0 = std::clamp(c.clip[0] * sx, 0.0f, float(width));
        const float y0 = std::clamp(c.clip[1] * sy, 0.0f, float(height));
        const float x1 = std::clamp(c.clip[2] * sx, 0.0f, float(width));
        const float y1 = std::clamp(c.clip[3] * sy, 0.0f, float(height));
        const uint32_t ix0 = uint32_t(x0), iy0 = uint32_t(y0);
        const uint32_t ix1 = uint32_t(std::ceil(x1)), iy1 = uint32_t(std::ceil(y1));
        if (ix1 <= ix0 || iy1 <= iy0)
            continue;
        const UiGpuTexture& t = c.image == 0 ? ui_atlas_
                                : c.image <= ui_images_.size() ? ui_images_[c.image - 1]
                                                               : UiGpuTexture{};
        if (t.bind_group == nullptr)
            continue;
        if (const WGPURenderPipeline p = c.blend == 1 ? pipeline_for(1) : alpha_pipeline; p != current) {
            wgpuRenderPassEncoderSetPipeline(pass, p);
            current = p;
        }
        if (t.bind_group != bound) {
            wgpuRenderPassEncoderSetBindGroup(pass, 0, t.bind_group, 0, nullptr);
            bound = t.bind_group;
        }
        wgpuRenderPassEncoderSetScissorRect(pass, ix0, iy0, std::min(ix1, width) - ix0, std::min(iy1, height) - iy0);
        wgpuRenderPassEncoderDrawIndexed(pass, c.index_count, 1, c.first_index, int32_t(c.base_vertex), 0);
    }
    wgpuRenderPassEncoderSetScissorRect(pass, 0, 0, width, height);
}

void WebGpuRenderer::set_paused(bool paused) {
    {
        std::lock_guard lock(pause_mutex_);
        paused_ = paused;
    }
    pause_cv_.notify_all();
}

void WebGpuRenderer::request_close() {
    {
        std::lock_guard lock(pause_mutex_);
        closed_ = true;
    }
    pause_cv_.notify_all();
}

bool WebGpuRenderer::redraw() {
    if (!ok())
        return false;
    bool shown = false;
    run_sync([&] { shown = present_texture(last_view_, last_width_, last_height_, last_rect_); });
    return shown;
}

void WebGpuRenderer::wait_idle() {
#if defined(LGP_WEBGPU_WGPU_NATIVE)
    run_sync([this] {
        if (device_ != nullptr)
            wgpuDevicePoll(device_, true, nullptr);
    });
#endif
}

bool WebGpuRenderer::replace_surface(const WGPUChainedStruct* native) {
#if defined(__EMSCRIPTEN__)
    (void)native;
    return false;
#else
    // Not through run(): the GPU thread may be busy for seconds (a pipeline
    // with a cold driver cache) while the caller (Android's UI thread in
    // surfaceDestroyed) must not wait. wgpu-native objects may be used from
    // any thread; surface_mutex_ keeps present_texture() off the surface.
    std::lock_guard surface_lock(surface_mutex_);
    if (surface_ != nullptr) {
        wgpuSurfaceUnconfigure(surface_);
        wgpuSurfaceRelease(surface_);
        surface_ = nullptr;
    }
    if (native == nullptr)
        return true;
    WGPUSurfaceDescriptor desc = WGPU_SURFACE_DESCRIPTOR_INIT;
    desc.nextInChain = const_cast<WGPUChainedStruct*>(native);
    surface_ = wgpuInstanceCreateSurface(instance_, &desc);
    std::lock_guard lock(size_mutex_);
    surface_dirty_ = true;
    return surface_ != nullptr;
#endif
}

// --- IRenderer -------------------------------------------------------------

SurfaceSize WebGpuRenderer::surface_size() const {
    std::lock_guard lock(size_mutex_);
    return size_;
}

void WebGpuRenderer::resize(SurfaceSize size) {
    std::lock_guard lock(size_mutex_);
    if (size.width == size_.width && size.height == size_.height)
        return;
    size_ = size;
    surface_dirty_ = true;
}

GpuHandles WebGpuRenderer::gpu() const {
    GpuHandles h;
    h.instance = instance_;
    h.adapter = adapter_;
    h.device = device_;
    h.queue = queue_;
    {
        std::lock_guard lock(surface_mutex_);
        h.surface = surface_;
    }
    h.surface_format = static_cast<uint32_t>(surface_format_);
    return h;
}

void WebGpuRenderer::present_rgba8(const FrameView& frame) {
    if (!ok() || frame.width == 0 || frame.height == 0)
        return;
    run_sync([&] {
        if (staging_ == nullptr || staging_width_ != frame.width || staging_height_ != frame.height) {
            if (staging_ != nullptr)
                wgpuTextureRelease(staging_);
            WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
            desc.label = string_view("present_rgba8");
            desc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
            desc.size = {frame.width, frame.height, 1};
            desc.format = WGPUTextureFormat_RGBA8Unorm;
            staging_ = wgpuDeviceCreateTexture(device_, &desc);
            staging_width_ = frame.width;
            staging_height_ = frame.height;
        }
        // writeTexture takes any bytesPerRow >= width * 4 (no 256 alignment).
        WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
        dst.texture = staging_;
        WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
        layout.bytesPerRow = frame.stride;
        layout.rowsPerImage = frame.height;
        const WGPUExtent3D extent = {frame.width, frame.height, 1};
        const size_t bytes = size_t{frame.stride} * (frame.height - 1) + size_t{frame.width} * 4;
        wgpuQueueWriteTexture(queue_, &dst, frame.pixels, bytes, &layout, &extent);
        WGPUTextureView view = wgpuTextureCreateView(staging_, nullptr);
        present_texture(view, frame.width, frame.height, {});
        wgpuTextureViewRelease(view);
    });
}

bool WebGpuRenderer::end_frame() {
    const uint64_t n = ++frames_;
    if (paused_.load()) {
        std::unique_lock lock(pause_mutex_);
        pause_cv_.wait(lock, [&] { return !paused_.load() || closed_.load(); });
    }
    if (closed_.load())
        return false;
    return frame_limit_ == 0 || n < frame_limit_;
}

}  // namespace lgp::platform::webgpu
