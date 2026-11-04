// IRenderer on WebGPU (ADR-0003, roadmap P4.4): instance, adapter, device and
// an optional presentation surface, for every target.
//
//   Desktop: wgpu-native. The window backend (platform/desktop, SDL3) passes
//            the native window as a chained WGPUSurfaceSource* struct.
//   Web:     emdawnwebgpu on navigator.gpu, surface from a canvas selector.
//   Headless (no surface): tests and CI; present only counts frames.
//
// Threading. WebGPU objects in the browser belong to the JS thread that
// created them, and the game calls NVN from several guest threads (pthreads
// in Web Workers, ADR-0002). So every WebGPU call goes through run(): on the
// web it is proxied to a GPU thread that owns all WebGPU objects and the
// canvas: the browser's main thread or, with RendererOptions::gpu_thread,
// a pthread of its own (the canvas moves there as an OffscreenCanvas). The
// main thread's other work (page, input, audio) delays hand-offs to it, but
// in the game the Mac measured no frame-rate gain from the own thread
// (docs/analysis/21-internet-recherche-werkzeuge.md, A1). On the desktop it runs on the calling thread, serialized by a mutex
// (wgpu-native objects may be used from any thread). run() returns once the
// task called finish(), which may happen later from a WebGPU callback
// (buffer mapping, device creation); meanwhile the desktop keeps processing
// WebGPU events and the browser returns to its event loop.
//
// The window's event loop is not part of this class: on the desktop the
// host's main thread pumps window events (SdlWindow) and calls resize() and
// request_close(); end_frame() never touches the window, so guest threads
// may present.
#pragma once

#include <webgpu/webgpu.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(__EMSCRIPTEN__)
#include <emscripten/proxying.h>
#include <pthread.h>
#endif

#include "lgp/platform/overlay.hpp"
#include "lgp/platform/renderer.hpp"
#include "lgp/platform/screen_fit.hpp"
#include "lgp/platform/ui_draw.hpp"

namespace lgp::platform::webgpu {

struct RendererOptions {
    // Initial drawable size; resize() changes it.
    SurfaceSize size{1280, 720};
    // Desktop: a WGPUSurfaceSource{WindowsHWND,MetalLayer,XlibWindow,
    // WaylandSurface} struct (sType set), read during construction only.
    const WGPUChainedStruct* native_surface = nullptr;
    // Web: CSS selector of the canvas to present to ("#canvas"); empty = none.
    std::string canvas_selector;
    // Ask for the software adapter (CI without GPU) or the low-power GPU.
    bool force_fallback_adapter = false;
    bool low_power = false;
    // Fifo waits for the display's vsync at every present (frame rates
    // snap to 60/n Hz); Mailbox and Immediate do not (Mailbox falls back to
    // Immediate, both to Fifo where the surface lacks them).
    WGPUPresentMode present_mode = WGPUPresentMode_Fifo;
    // end_frame() returns false after this many frames (0 = never); tests.
    uint64_t frame_limit = 0;
    // Web: WebGPU and the canvas live on a pthread of their own instead of
    // the browser's main thread (needs a pthread as the creating thread and
    // -sOFFSCREENCANVAS_SUPPORT, which lgp_platform_webgpu links). Off by
    // default: the Mac measured no frame-rate gain in the game (docs/
    // analysis/20-route1-30fps-zeitaufteilung.md, E). LGP_WEBGPU_GPU_THREAD=0/1
    // overrides. Ignored on the desktop.
    bool gpu_thread = false;
};

// Source rectangle of a present, in texels of the presented texture, row 0 =
// first row in memory. width/height 0 = whole texture.
struct PresentRect {
    int32_t x = 0, y = 0, width = 0, height = 0;
    bool flip_y = false;  // show the source rows bottom to top
    bool filterable = false;  // the view's format can be sampled with filtering (PresentFilter)
};

// How present_texture() scales the game image onto the surface (the app's
// "Bildglättung"): nearest texel (sharp, uneven pixels when the scale is no
// whole number), bilinear, or FXAA on the source texels and bilinear (edges
// smoothed; text and the HUD a little softer too). Views that cannot be
// filtered (PresentRect::filterable false) always take the nearest texel.
enum class PresentFilter { Nearest, Linear, Fxaa };
bool parse_present_filter(const std::string& name, PresentFilter* out);  // "off", "linear", "fxaa"
const char* present_filter_name(PresentFilter f);

class WebGpuRenderer : public IRenderer {
public:
    explicit WebGpuRenderer(const RendererOptions& options);
    ~WebGpuRenderer() override;
    WebGpuRenderer(const WebGpuRenderer&) = delete;
    WebGpuRenderer& operator=(const WebGpuRenderer&) = delete;

    // False if no adapter or device could be created (no WebGPU in this
    // browser, no Vulkan/Metal/DX12 driver); error() says why. The other
    // members must not be used then, except error().
    bool ok() const { return device_ != nullptr; }
    const std::string& error() const { return error_; }
    // "llvmpipe (Vulkan)" and the like.
    const std::string& adapter_name() const { return adapter_name_; }
    // The optional features the renderer looks at that the adapter offers
    // (requested or not, e.g. texture-compression-etc2); what the device got
    // is wgpuDeviceHasFeature(device(), ...).
    const std::vector<WGPUFeatureName>& adapter_features() const { return adapter_features_; }
    // Web: run() goes to a GPU pthread of its own (else to the browser's main
    // thread). Always false on the desktop.
    bool on_gpu_thread() const;
    bool has_surface() const {
        std::lock_guard lock(surface_mutex_);
        return surface_ != nullptr;
    }
    // The surface's present mode after fallbacks (meaningful with a surface).
    WGPUPresentMode present_mode() const { return present_mode_; }

    // IRenderer
    SurfaceSize surface_size() const override;
    GpuHandles gpu() const override;  // handles are only usable inside run()
    void present_rgba8(const FrameView& frame) override;
    bool end_frame() override;

    // --- GPU thread ------------------------------------------------------
    class Call {
    public:
        virtual void finish() = 0;
        // wgpu-native: once the task returned, run() blocks on this
        // submission (wgpuQueueSubmitForIndex) instead of the latest one
        // while it waits for finish(). For a task that waits on work it
        // submitted before later work (a compute read-back ahead of the
        // draws). No effect in the browser.
        void wait_for_submission(uint64_t index) {
            wait_submission_ = index;
            has_wait_submission_ = true;
        }
        bool has_wait_submission() const { return has_wait_submission_; }
        uint64_t wait_submission() const { return wait_submission_; }

    protected:
        ~Call() = default;

    private:
        uint64_t wait_submission_ = 0;
        bool has_wait_submission_ = false;
    };
    using Task = std::function<void(Call&)>;
    // Runs task on the GPU thread and waits until it called finish().
    void run(const Task& task);
    // Runs fn on the GPU thread (finishes when fn returns).
    void run_sync(const std::function<void()>& fn);

    // Inside run(): draws the texture behind `view` (width x height texels,
    // a float or unorm format, sampled raw) into the surface, placed by
    // aspect_mode() (default: scaled to fit with the aspect ratio kept), and
    // presents. No-op without a surface.
    // Returns false if the surface had no frame to give.
    bool present_texture(WGPUTextureView view, uint32_t width, uint32_t height, const PresentRect& rect);

    // Any thread: how present_texture() places the game image on the surface
    // (lgp/platform/screen_fit.hpp, the app's "Bildformat"); from the next
    // present or redraw() on. Overlay and interface always cover the surface.
    void set_aspect_mode(AspectMode mode) { aspect_mode_.store(int(mode), std::memory_order_relaxed); }
    AspectMode aspect_mode() const { return AspectMode(aspect_mode_.load(std::memory_order_relaxed)); }
    // Any thread: the aspect ratio (width / height) the game image really
    // has, asked at each present; null or 0 = the crop rectangle's own.
    // The widescreen mod (lgp/mods/widescreen.hpp) renders a wider or taller
    // picture, which can reach the window texture squeezed into 16:9.
    void set_image_aspect(float (*aspect)()) { image_aspect_.store(aspect, std::memory_order_relaxed); }
    // Any thread: the scaling filter (PresentFilter) from the next present or redraw() on.
    void set_present_filter(PresentFilter f) { present_filter_.store(int(f), std::memory_order_relaxed); }
    PresentFilter present_filter() const { return PresentFilter(present_filter_.load(std::memory_order_relaxed)); }
    // Any thread: a window point in fractions of the window (SDL finger
    // coordinates) to the same point of the last presented image, in
    // fractions of that image (lgp::platform::window_to_image() with this
    // renderer's placement). False before the first present and outside the
    // image (bars, or the edge Fill cut off).
    bool window_to_image(float wx, float wy, float* ix, float* iy) const;

    // Any thread: circles drawn over every presented image from now on (the
    // touch controller, P7.3; at most kMaxOverlayCircles). Empty: none.
    static constexpr size_t kMaxOverlayCircles = 24;
    void set_overlay(std::vector<OverlayCircle> circles);
    // Any thread: the app's interface (menus, lgp/platform/ui_draw.hpp), drawn
    // over every presented image after the overlay. nullptr: none.
    void set_ui(std::shared_ptr<const UiDrawData> ui);

    // Pause (the app's menu, P7): while paused, end_frame() blocks the thread
    // that presents (the game then stands at its next present) until
    // set_paused(false) or request_close(). redraw() shows the last presented
    // image again with the current overlay and interface (black without one),
    // e.g. from the host's main thread while paused or before the game runs.
    void set_paused(bool paused);
    bool paused() const { return paused_.load(); }
    bool redraw();

    WGPUInstance instance() const { return instance_; }
    WGPUDevice device() const { return device_; }
    WGPUQueue queue() const { return queue_; }

    // Validation errors and device loss reported so far (tests expect 0).
    uint64_t error_count() const { return errors_.load(); }
    uint64_t frames() const { return frames_.load(); }
    // Tasks run so far and the GPU thread's time inside them (the synchronous
    // part: callbacks that finish a task later are not counted). Callers wait
    // longer than this: on the web also for the main thread to pick the task
    // up, on the desktop for the GPU when a task waits for it.
    uint64_t run_calls() const { return run_calls_.load(std::memory_order_relaxed); }
    uint64_t run_busy_ns() const { return run_busy_ns_.load(std::memory_order_relaxed); }
    // After a task returned unfinished (it waits for the GPU, e.g. a buffer
    // map): the time until it finished and the device polls in that wait.
    // Many polls per wait mean the loop spins instead of blocking (P7.3).
    uint64_t run_wait_ns() const { return run_wait_ns_.load(std::memory_order_relaxed); }
    uint64_t run_wait_polls() const { return run_wait_polls_.load(std::memory_order_relaxed); }
    // Parts of the time around tasks (measured always, printed with
    // --time-stats): run() calls that found another caller inside and how
    // long they waited for it; presents and the time in surface acquire, in
    // the blit's queue submit and in the surface present itself.
    uint64_t run_lock_waits() const { return run_lock_waits_.load(std::memory_order_relaxed); }
    uint64_t run_lock_wait_ns() const { return run_lock_wait_ns_.load(std::memory_order_relaxed); }
    uint64_t surface_presents() const { return surface_presents_.load(std::memory_order_relaxed); }
    uint64_t surface_acquire_ns() const { return surface_acquire_ns_.load(std::memory_order_relaxed); }
    uint64_t surface_submit_ns() const { return surface_submit_ns_.load(std::memory_order_relaxed); }
    uint64_t surface_present_ns() const { return surface_present_ns_.load(std::memory_order_relaxed); }

    // --- window side -----------------------------------------------------
    // New drawable size; the surface is reconfigured at the next present.
    void resize(SurfaceSize size);
    // Native only: drops the surface (native == nullptr; presents become
    // no-ops) or creates a new one for another native window of the same
    // kind, e.g. when an Android activity loses and regains its surface.
    // False if the new surface could not be created (then there is none).
    // Any thread; it waits for a present in progress, not for other GPU-thread
    // work (an Android surfaceDestroyed on the UI thread must not wait for a
    // pipeline being created there: ANR).
    bool replace_surface(const WGPUChainedStruct* native);
    // Native only: waits until the GPU has finished everything submitted so
    // far (the app goes to the background: iOS ends apps whose GPU work runs
    // there). Callers make sure nothing submits meanwhile.
    void wait_idle();
    // end_frame() returns false from now on (window closed).
    void request_close();
    bool close_requested() const { return closed_.load(); }

private:
    void create_device(const RendererOptions& options);
    void configure_surface_locked();
    WGPURenderPipeline blit_pipeline(WGPUTextureFormat format, PresentFilter filter);

    std::string error_;
    std::string adapter_name_;
    std::vector<WGPUFeatureName> adapter_features_;
    WGPUInstance instance_ = nullptr;
    WGPUAdapter adapter_ = nullptr;
    WGPUDevice device_ = nullptr;
    WGPUQueue queue_ = nullptr;
    // Native: replace_surface() and present_texture() hold surface_mutex_
    // while they use surface_ (before size_mutex_).
    mutable std::mutex surface_mutex_;
    WGPUSurface surface_ = nullptr;
    WGPUTextureFormat surface_format_ = WGPUTextureFormat_Undefined;
    WGPUPresentMode present_mode_ = WGPUPresentMode_Fifo;
    WGPUCompositeAlphaMode alpha_mode_ = WGPUCompositeAlphaMode_Auto;

    mutable std::mutex size_mutex_;
    SurfaceSize size_;
    bool surface_dirty_ = true;
    SurfaceSize logged_size_;            // last surface size written to the log
    bool surface_unconfigured_ = false;  // done by the destructor before releasing (web)

    // GPU-thread state (blit resources, staging texture for present_rgba8).
    WGPUBindGroupLayout blit_layout_ = nullptr;
    WGPUPipelineLayout blit_pipeline_layout_ = nullptr;
    WGPUShaderModule blit_shader_ = nullptr;
    WGPUBuffer blit_uniforms_ = nullptr;
    // Filtered blits (PresentFilter::Linear, Fxaa): filterable texture and a
    // linear clamp-to-edge sampler.
    WGPUBindGroupLayout blit_filtered_layout_ = nullptr;
    WGPUPipelineLayout blit_filtered_pipeline_layout_ = nullptr;
    WGPUSampler blit_sampler_ = nullptr;
    std::unordered_map<uint64_t, WGPURenderPipeline> blit_pipelines_;  // key: format << 8 | filter
    // Overlay (set_overlay): created on first use.
    void draw_overlay(WGPURenderPassEncoder pass, uint32_t width, uint32_t height);
    std::mutex overlay_mutex_;
    std::vector<OverlayCircle> overlay_;
    WGPUBindGroupLayout overlay_layout_ = nullptr;
    WGPUPipelineLayout overlay_pipeline_layout_ = nullptr;
    WGPUShaderModule overlay_shader_ = nullptr;
    WGPUBuffer overlay_uniforms_ = nullptr;
    WGPUBindGroup overlay_bind_group_ = nullptr;
    std::unordered_map<uint32_t, WGPURenderPipeline> overlay_pipelines_;
    // Interface (set_ui): created on first use.
    void draw_ui(WGPURenderPassEncoder pass, uint32_t width, uint32_t height);
    std::mutex ui_mutex_;
    std::shared_ptr<const UiDrawData> ui_;
    WGPUBindGroupLayout ui_layout_ = nullptr;
    WGPUPipelineLayout ui_pipeline_layout_ = nullptr;
    WGPUShaderModule ui_shader_ = nullptr;
    WGPUBuffer ui_uniforms_ = nullptr;
    WGPUBuffer ui_vertices_ = nullptr, ui_indices_ = nullptr;
    uint64_t ui_vertex_bytes_ = 0, ui_index_bytes_ = 0;
    WGPUSampler ui_sampler_ = nullptr;
    // An uploaded UiTexture: the atlas, or one of UiDrawData::images.
    struct UiGpuTexture {
        const UiTexture* source = nullptr;
        uint64_t version = 0;
        WGPUTexture texture = nullptr;
        WGPUBindGroup bind_group = nullptr;
    };
    bool sync_ui_texture(UiGpuTexture& slot, const UiTexture& tex);
    static void release_ui_texture(UiGpuTexture& slot);
    UiGpuTexture ui_atlas_;
    std::vector<UiGpuTexture> ui_images_;
    std::unordered_map<uint32_t, WGPURenderPipeline> ui_pipelines_;
    // The last presented image (redraw()), a reference of its own.
    WGPUTextureView last_view_ = nullptr;
    uint32_t last_width_ = 0, last_height_ = 0;
    PresentRect last_rect_;
    std::atomic<int> aspect_mode_{int(AspectMode::Fit)};
    std::atomic<float (*)()> image_aspect_{nullptr};
    std::atomic<int> present_filter_{int(PresentFilter::Nearest)};
    // Where the last present put the image (window_to_image()), in surface pixels.
    mutable std::mutex placed_mutex_;
    ImageRect placed_;
    float placed_w_ = 0, placed_h_ = 0;
    std::atomic<bool> paused_{false};
    std::mutex pause_mutex_;
    std::condition_variable pause_cv_;
    WGPUTexture staging_ = nullptr;
    uint32_t staging_width_ = 0, staging_height_ = 0;

#if defined(__EMSCRIPTEN__)
    // The GPU pthread (RendererOptions::gpu_thread) and the queue run() uses
    // to reach it; gpu_thread_queue_ == nullptr: the browser's main thread.
    pthread_t gpu_thread_{};
    em_proxying_queue* gpu_thread_queue_ = nullptr;
    void start_gpu_thread(const RendererOptions& options);
    void stop_gpu_thread();
#endif
    std::mutex run_mutex_;  // serializes run()
    std::atomic<uint64_t> errors_{0};
    std::atomic<uint64_t> frames_{0};
    std::atomic<uint64_t> run_calls_{0}, run_busy_ns_{0};
    std::atomic<uint64_t> run_wait_ns_{0}, run_wait_polls_{0};
    std::atomic<uint64_t> run_lock_waits_{0}, run_lock_wait_ns_{0};
    std::atomic<uint64_t> surface_presents_{0}, surface_acquire_ns_{0}, surface_submit_ns_{0}, surface_present_ns_{0};
    // Presents in a row without a surface / a surface texture (under surface_mutex_).
    uint64_t no_surface_presents_ = 0, acquire_failures_ = 0;
    // LGP_GPU_SPIN_US: run() polls without blocking this long before it
    // blocks on the device (wgpu-native only).
    std::chrono::microseconds gpu_spin_{0};
    std::atomic<bool> closed_{false};
    uint64_t frame_limit_ = 0;
};

// Shared helpers for code that uses webgpu.h directly (gpu/webgpu).
inline WGPUStringView string_view(const char* s) { return WGPUStringView{s, WGPU_STRLEN}; }
std::string to_string(WGPUStringView s);
// Creates a WGSL shader module; logs and returns nullptr on failure.
WGPUShaderModule create_wgsl_module(WGPUDevice device, const char* label, const char* wgsl);
// The same format without the sRGB encoding (RGBA8UnormSrgb -> RGBA8Unorm),
// or the format itself.
WGPUTextureFormat linear_variant(WGPUTextureFormat format);

}  // namespace lgp::platform::webgpu
