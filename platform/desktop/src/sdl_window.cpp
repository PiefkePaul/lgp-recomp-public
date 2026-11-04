#include "lgp/platform/desktop/sdl_window.hpp"
#include "lgp/platform/host_env.hpp"

#include <SDL3/SDL.h>
#if defined(__APPLE__)
#include <SDL3/SDL_metal.h>
#endif

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#if defined(__ANDROID__)
#include <jni.h>
#include <sched.h>
#include <fcntl.h>
#include <sys/prctl.h>
#include <unistd.h>

#include <cerrno>

#include <atomic>
#include <chrono>
#include <thread>
#endif

namespace lgp::platform::desktop {

#if defined(LGP_DESKTOP_APP_ICON)
// platform/icon/lgp-icon-128.png, embedded by platform/CMakeLists.txt.
extern const unsigned char kAppIconPng[];
extern const std::size_t kAppIconPngSize;

namespace {

// The app icon in the title bar and window lists; on macOS also the Dock icon
// of a program started outside an app bundle. Best effort: a failure only
// leaves the system's default icon.
void set_app_icon(SDL_Window* window) {
    SDL_IOStream* io = SDL_IOFromConstMem(kAppIconPng, kAppIconPngSize);
    SDL_Surface* icon = io != nullptr ? SDL_LoadPNG_IO(io, true) : nullptr;
    if (icon == nullptr)
        return;
    SDL_SetWindowIcon(window, icon);
    SDL_DestroySurface(icon);
}

}  // namespace
#endif

#if defined(__ANDROID__)
// LGP_CLOCK_KEEPER=CPU[:PERCENT[:PERIOD_US]]: a thread pinned to CPU that
// spins PERCENT (default 100) of every PERIOD_US (default 10000). A diagnosis
// switch, not a fix: on the OnePlus 7 Pro the game ran 29 % slower once the
// SDLThread stopped spinning on CPU 7 (LGP_SDL_PUMP_MS); a keeper on any
// core, also a little one, brought it back (docs/knowledge/android-ndk-build.md).
// The suspicion is wakeup latency from deep idle states. A short period
// keeps idle stretches short, so the idle governor may pick shallow states.
// LGP_CPU_DMA_LATENCY=US: asks the kernel through /dev/cpu_dma_latency (PM
// QoS) for idle states that wake within US microseconds while the window
// lives; usually closed to apps, the log says whether it took.
struct SdlWindow::ClockKeeper {
    std::atomic<bool> stop{false};
    std::thread thread;
    int dma_fd = -1;

    void start(int cpu, int percent, int period_us) {
        thread = std::thread([this, cpu, percent, period_us] {
            prctl(PR_SET_NAME, "lgp-clock", 0, 0, 0);
            cpu_set_t set;
            CPU_ZERO(&set);
            CPU_SET(cpu, &set);
            if (sched_setaffinity(0, sizeof(set), &set) != 0)
                SDL_Log("lgp: clock keeper cannot be pinned to CPU %d", cpu);
            using clock = std::chrono::steady_clock;
            const auto period = std::chrono::microseconds(period_us);
            const auto busy = period * percent / 100;
            while (!stop.load(std::memory_order_relaxed)) {
                const auto start = clock::now();
                while (clock::now() - start < busy && !stop.load(std::memory_order_relaxed)) {
                }
                if (busy < period)
                    std::this_thread::sleep_until(start + period);
            }
        });
    }
    ~ClockKeeper() {
        stop = true;
        if (thread.joinable())
            thread.join();
        if (dma_fd >= 0)
            close(dma_fd);
    }
};
#else
struct SdlWindow::ClockKeeper {};
#endif

std::string preferred_locale() {
    int count = 0;
    SDL_Locale** locales = SDL_GetPreferredLocales(&count);
    std::string out;
    if (locales != nullptr && count > 0 && locales[0] != nullptr && locales[0]->language != nullptr) {
        out = locales[0]->language;
        if (locales[0]->country != nullptr && *locales[0]->country != '\0')
            out += std::string("-") + locales[0]->country;
    }
    SDL_free(locales);
    return out;
}

DeviceKind device_kind() {
#if defined(SDL_PLATFORM_ANDROID)
    return SDL_IsTV() ? DeviceKind::AndroidTv : DeviceKind::Android;
#elif defined(SDL_PLATFORM_TVOS)
    return DeviceKind::AppleTv;  // before SDL_PLATFORM_IOS, which tvOS sets too
#elif defined(SDL_PLATFORM_IOS)
    return DeviceKind::Ios;
#else
    return DeviceKind::Desktop;
#endif
}

const char* device_kind_name(DeviceKind kind) {
    switch (kind) {
    case DeviceKind::Ios: return "ios";
    case DeviceKind::AppleTv: return "apple-tv";
    case DeviceKind::Android: return "android";
    case DeviceKind::AndroidTv: return "android-tv";
    case DeviceKind::Desktop: break;
    }
    return "desktop";
}

#if defined(__ANDROID__) || defined(SDL_PLATFORM_IOS)
namespace {
// The window whose surface goes away with the app's (one at a time).
std::mutex g_mobile_mutex;
SdlWindow* g_mobile_window = nullptr;

// iOS sends WILL_ENTER_BACKGROUND from the app delegate: a watch runs right
// there, before the app is suspended (pump_events would see it too late).
// Android sends it from SDL's event pump just before that blocks for the
// pause (SDL_HINT_ANDROID_BLOCK_ON_PAUSE); LgpSurface calls surface_lost()
// even earlier when the surface goes first.
// iOS and tvOS send WILL_ENTER_BACKGROUND already when the scene resigns active (a
// system alert such as the local network permission, Control Center, a call
// banner) and the app may come straight back. The GPU stays usable until
// the app is really in the background, so the surface is kept until
// DID_ENTER_BACKGROUND; the game pauses at once either way.
bool SDLCALL mobile_event_watch(void* window, SDL_Event* e) {
    auto* w = static_cast<SdlWindow*>(window);
    if (e->type == SDL_EVENT_WILL_ENTER_BACKGROUND) {
        w->lifecycle_event(true);  // the game stops and the GPU idles first
#if !defined(SDL_PLATFORM_IOS)
        w->surface_lost();
#endif
    } else if (e->type == SDL_EVENT_DID_ENTER_BACKGROUND) {
        w->surface_lost();
    } else if (e->type == SDL_EVENT_DID_ENTER_FOREGROUND) {
        w->lifecycle_event(false);
    } else if (e->type == SDL_EVENT_LOW_MEMORY) {
        // SDL 3 never queues it, only event watches see it (SDL_SendAppEvent;
        // Shield, 2026-10-29: onTrimMemory came, no warning was counted).
        note_low_memory();  // the WebGPU backend drops its caches at the next list
    }
    return true;
}
}  // namespace
#endif

namespace {
// Size of the WebGPU surface for a window of pw x ph pixels. The game draws
// 1280x720 (handheld) and the blit scales it by nearest neighbour, so a 4K
// swapchain costs three images of ≈ 32 MiB against the ≈ 2 GiB jetsam limit
// for no gain. tvOS caps the height at 1440: exactly twice 720p, so the
// nearest blit stays even (1080 would double every other line), and
// ≈ 14 MiB per image (P7.4). Elsewhere no cap. LGP_SURFACE_MAX_HEIGHT=N
// sets the cap, 0 turns it off; on Android TV (a Shield at a 4K set) 1440 is
// the candidate default once measured there (P7.3). The layer scales the
// drawable to the window.
SurfaceSize surface_size(int pw, int ph) {
#if defined(SDL_PLATFORM_TVOS)
    int max_h = 1440;
#else
    int max_h = 0;
#endif
    if (const char* v = SDL_getenv("LGP_SURFACE_MAX_HEIGHT"); v != nullptr && *v != '\0')
        max_h = SDL_atoi(v);
    if (max_h > 0 && ph > max_h && pw > 0) {
        pw = int((int64_t(pw) * max_h + ph / 2) / ph);
        ph = max_h;
    }
    return {uint32_t(pw), uint32_t(ph)};
}
}  // namespace

std::unique_ptr<SdlWindow> SdlWindow::create(const WindowOptions& options, std::string* error) {
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        *error = std::string("SDL video: ") + SDL_GetError();
        return nullptr;
    }
    std::unique_ptr<SdlWindow> w(new SdlWindow());
    SDL_WindowFlags flags = SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (options.resizable)
        flags |= SDL_WINDOW_RESIZABLE;
    if (options.fullscreen)
        flags |= SDL_WINDOW_FULLSCREEN;  // no mode set: borderless on the desktop
#if defined(__APPLE__)
    flags |= SDL_WINDOW_METAL;
#endif
#if defined(__ANDROID__)
    flags |= SDL_WINDOW_FULLSCREEN;  // immersive: no status and navigation bars
#elif defined(SDL_PLATFORM_IOS) && !defined(SDL_PLATFORM_TVOS)
    // SDL's view controller hides the status bar (prefersStatusBarHidden)
    // only for fullscreen or borderless windows; without the flag iPadOS
    // keeps it over the game in landscape (Info.plist's UIStatusBarHidden
    // only covers the launch, the view controller decides afterwards). The
    // view covers the whole screen either way, notch and corners included.
    flags |= SDL_WINDOW_FULLSCREEN;
    // Home indicator: "2" dims it and defers the system's edge gestures, so
    // the first swipe up only shows it and the second goes home; a stick or
    // button drag near the bottom edge no longer leaves the game. "1" would
    // hide it until the next touch, which in a touch game is all the time,
    // and keep the one-swipe home gesture. An SDL_IOS_HIDE_HOME_INDICATOR
    // in the environment wins (SDL_SetHint at normal priority).
    SDL_SetHint(SDL_HINT_IOS_HIDE_HOME_INDICATOR, "2");
#endif
#if defined(__ANDROID__) || defined(SDL_PLATFORM_IOS)
    // Landscape only, whatever the device's rotation lock says (SDL would
    // otherwise allow portrait for a resizable window).
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
#endif
#if defined(SDL_PLATFORM_TVOS)
    // The Siri Remote's touch surface is no screen: no mouse clicks from its
    // touches (the menus take it as a gamepad, AppMenu::set_touch_is_remote).
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
#endif
#if defined(__ANDROID__)
    // Back (remote, gesture, key) goes to the game as B instead of finishing
    // the activity mid-game (SdlInput); Home still leaves.
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
#endif
#if defined(__ANDROID__)
    if (const char* v = SDL_getenv("LGP_SDL_PUMP_MS"); v != nullptr && *v != '\0') {
        w->pump_period_ms_ = SDL_atoi(v);
        SDL_Log("lgp: events polled every %d ms (LGP_SDL_PUMP_MS)", w->pump_period_ms_);
    }
    if (const char* v = SDL_getenv("LGP_CLOCK_KEEPER"); v != nullptr && *v != '\0') {
        const int cpu = SDL_atoi(v);
        const char* colon = SDL_strchr(v, ':');
        const int percent = colon != nullptr ? std::clamp(SDL_atoi(colon + 1), 1, 100) : 100;
        const char* colon2 = colon != nullptr ? SDL_strchr(colon + 1, ':') : nullptr;
        const int period_us = colon2 != nullptr ? std::clamp(SDL_atoi(colon2 + 1), 100, 1000000) : 10000;
        if (cpu >= 0 && cpu < CPU_SETSIZE) {
            w->clock_keeper_ = std::make_unique<ClockKeeper>();
            w->clock_keeper_->start(cpu, percent, period_us);
            SDL_Log("lgp: clock keeper on CPU %d, %d %% of every %d us busy (LGP_CLOCK_KEEPER)", cpu, percent,
                    period_us);
        }
    }
    if (const char* v = SDL_getenv("LGP_CPU_DMA_LATENCY"); v != nullptr && *v != '\0') {
        const int32_t us = std::max(SDL_atoi(v), 0);
        const int fd = open("/dev/cpu_dma_latency", O_WRONLY | O_CLOEXEC);
        if (fd < 0) {
            SDL_Log("lgp: /dev/cpu_dma_latency: %s (LGP_CPU_DMA_LATENCY)", strerror(errno));
        } else if (write(fd, &us, sizeof(us)) != ssize_t(sizeof(us))) {
            SDL_Log("lgp: /dev/cpu_dma_latency write: %s (LGP_CPU_DMA_LATENCY)", strerror(errno));
            close(fd);
        } else {
            // The request holds while the file stays open.
            if (!w->clock_keeper_)
                w->clock_keeper_ = std::make_unique<ClockKeeper>();
            w->clock_keeper_->dma_fd = fd;
            SDL_Log("lgp: idle wakeup latency held at %d us (LGP_CPU_DMA_LATENCY)", int(us));
        }
    }
#endif
    w->window_ = SDL_CreateWindow(options.title.c_str(), int(options.width), int(options.height), flags);
    if (w->window_ == nullptr) {
        *error = std::string("SDL window: ") + SDL_GetError();
        return nullptr;
    }
#if defined(LGP_DESKTOP_APP_ICON)
    set_app_icon(w->window_);
#endif
    const char* driver = SDL_GetCurrentVideoDriver();
    w->driver_ = driver != nullptr ? driver : "";
#if defined(SDL_PLATFORM_IOS) && !defined(SDL_PLATFORM_TVOS)
    // iPadOS 26 may put the app in a resizable window although Info.plist asks
    // for full screen (UIRequiresFullScreen is deprecated there); size
    // changes reach the renderer, touch layout and menus as window events.
    if (const char* v = SDL_getenv("LGP_IOS_FULL_SIZE"); v == nullptr || SDL_strcmp(v, "0") != 0)
        request_full_size_scene(w->window_);
#endif

    // Native window -> chained WGPUSurfaceSource struct.
    const SDL_PropertiesID props = SDL_GetWindowProperties(w->window_);
    [[maybe_unused]] WGPUSurfaceSourceWindowsHWND hwnd = WGPU_SURFACE_SOURCE_WINDOWS_HWND_INIT;
    [[maybe_unused]] WGPUSurfaceSourceMetalLayer metal = WGPU_SURFACE_SOURCE_METAL_LAYER_INIT;
    [[maybe_unused]] WGPUSurfaceSourceXlibWindow xlib = WGPU_SURFACE_SOURCE_XLIB_WINDOW_INIT;
    [[maybe_unused]] WGPUSurfaceSourceWaylandSurface wayland = WGPU_SURFACE_SOURCE_WAYLAND_SURFACE_INIT;
    [[maybe_unused]] WGPUSurfaceSourceAndroidNativeWindow android = WGPU_SURFACE_SOURCE_ANDROID_NATIVE_WINDOW_INIT;
    const WGPUChainedStruct* source = nullptr;
#if defined(_WIN32)
    hwnd.hwnd = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    hwnd.hinstance = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_INSTANCE_POINTER, nullptr);
    if (hwnd.hwnd != nullptr)
        source = &hwnd.chain;
#elif defined(__APPLE__)
    (void)props;
    w->metal_view_ = SDL_Metal_CreateView(w->window_);
    metal.layer = w->metal_view_ != nullptr ? SDL_Metal_GetLayer(w->metal_view_) : nullptr;
    if (metal.layer != nullptr)
        source = &metal.chain;
#elif defined(__ANDROID__)
    // The activity's surface (P7.3); pump_events() swaps it when the app
    // goes to the background and back.
    android.window = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr);
    if (android.window != nullptr)
        source = &android.chain;
#else
    if (w->driver_ == "x11") {
        xlib.display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
        xlib.window = uint64_t(SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
        if (xlib.display != nullptr && xlib.window != 0)
            source = &xlib.chain;
    } else if (w->driver_ == "wayland") {
        wayland.display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
        wayland.surface = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
        if (wayland.display != nullptr && wayland.surface != nullptr)
            source = &wayland.chain;
    }
#endif
    if (source == nullptr) {
        *error = "no native handle for SDL video driver '" + w->driver_ + "'";
        return nullptr;
    }

    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(w->window_, &pw, &ph);
    webgpu::RendererOptions ro = options.renderer;
    ro.size = surface_size(pw, ph);
    ro.native_surface = source;
    w->renderer_ = std::make_unique<webgpu::WebGpuRenderer>(ro);
    if (!w->renderer_->ok()) {
        *error = w->renderer_->error();
        return nullptr;
    }
#if defined(__ANDROID__) || defined(SDL_PLATFORM_IOS)
    {
        std::lock_guard lock(g_mobile_mutex);
        g_mobile_window = w.get();
    }
    SDL_AddEventWatch(mobile_event_watch, w.get());
#endif
    return w;
}

SafeInsets SdlWindow::safe_insets() const {
    SDL_Rect safe{};
    int w = 0, h = 0;
    if (window_ == nullptr || !SDL_GetWindowSafeArea(window_, &safe) || !SDL_GetWindowSize(window_, &w, &h))
        return {};
    return safe_insets_from_rect(float(safe.x), float(safe.y), float(safe.w), float(safe.h), float(w), float(h));
}

float SdlWindow::height_points() const {
    int pw = 0, ph = 0;
    if (window_ == nullptr || !SDL_GetWindowSizeInPixels(window_, &pw, &ph))
        return 0;
    const float scale = SDL_GetWindowDisplayScale(window_);
    return scale > 0 ? float(ph) / scale : 0;
}

void SdlWindow::surface_lost() {
    std::lock_guard lock(surface_mutex_);
    lose_surface_locked();
}

void SdlWindow::lose_surface_locked() {
    if (surface_lost_)
        return;
    renderer_->replace_surface(nullptr);
    surface_lost_ = true;
}

void SdlWindow::surface_destroying() {
    // One lock for both: a reattach_surface() in between would build on the
    // window SDL is about to release.
    std::lock_guard lock(surface_mutex_);
    surface_ready_ = false;
    lose_surface_locked();
}

void SdlWindow::surface_ready() {
    std::lock_guard lock(surface_mutex_);
    surface_ready_ = true;
}

void SdlWindow::reattach_surface() {
    std::lock_guard lock(surface_mutex_);
    if (!surface_lost_)
        return;
    bool ok = false;
#if defined(__ANDROID__)
    // Between surfaceDestroyed and SDL clearing the property the pointer is
    // the window being released (Vulkan crashed in CreateAndroidSurfaceKHR).
    if (!surface_ready_)
        return;
    WGPUSurfaceSourceAndroidNativeWindow android = WGPU_SURFACE_SOURCE_ANDROID_NATIVE_WINDOW_INIT;
    android.window =
        SDL_GetPointerProperty(SDL_GetWindowProperties(window_), SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr);
    if (android.window == nullptr)
        return;  // the activity has no surface yet: try again on the next pump
    ok = renderer_->replace_surface(&android.chain);
#elif defined(__APPLE__)
    WGPUSurfaceSourceMetalLayer metal = WGPU_SURFACE_SOURCE_METAL_LAYER_INIT;
    metal.layer = metal_view_ != nullptr ? SDL_Metal_GetLayer(metal_view_) : nullptr;
    if (metal.layer == nullptr)
        return;
    ok = renderer_->replace_surface(&metal.chain);
#endif
    surface_lost_ = false;
    if (!ok)
        SDL_Log("lgp: no WebGPU surface after returning to the foreground");
    else
        SDL_Log("lgp: WebGPU surface back in the foreground");
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(window_, &pw, &ph);
    renderer_->resize(surface_size(pw, ph));
}

SdlWindow::~SdlWindow() {
#if defined(__ANDROID__) || defined(SDL_PLATFORM_IOS)
    {
        std::lock_guard lock(g_mobile_mutex);
        if (g_mobile_window == this)
            g_mobile_window = nullptr;
    }
    SDL_RemoveEventWatch(mobile_event_watch, this);
#endif
    renderer_.reset();  // the surface goes before the window
#if defined(__APPLE__)
    if (metal_view_ != nullptr)
        SDL_Metal_DestroyView(static_cast<SDL_MetalView>(metal_view_));
#endif
    if (window_ != nullptr)
        SDL_DestroyWindow(window_);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

bool SdlWindow::pump_events(int timeout_ms) {
    SDL_Event event;
#if defined(__ANDROID__)
    if (pump_period_ms_ > 0) {
        // Same contract as below: return after the first batch of events or
        // at the timeout. SDL_PollEvent still blocks while the app is paused
        // (SDL_HINT_ANDROID_BLOCK_ON_PAUSE).
        const Uint64 end = SDL_GetTicksNS() + SDL_MS_TO_NS(Uint64(timeout_ms));
        for (;;) {
            bool any = false;
            while (SDL_PollEvent(&event)) {
                any = true;
                handle_event(event);
            }
            const Uint64 now = SDL_GetTicksNS();
            if (any || now >= end)
                break;
            SDL_DelayNS(std::min(end - now, SDL_MS_TO_NS(Uint64(pump_period_ms_))));
        }
        reattach_surface();  // the new surface may arrive after the foreground event
        return !renderer_->close_requested();
    }
#endif
    bool waited = false;
    while (waited ? SDL_PollEvent(&event) : SDL_WaitEventTimeout(&event, timeout_ms)) {
        waited = true;
        handle_event(event);
    }
#if defined(__ANDROID__)
    // The new surface may arrive after the foreground event.
    reattach_surface();
#elif defined(SDL_PLATFORM_IOS)
    // UIKit reports the lifecycle twice (scene delegate and UIApplication
    // notifications, both forwarded by SDL). On iPhone and iPad a background
    // event dropped the surface after the last foreground event had already
    // been handled, and the image stayed frozen while the game ran on
    // (88c0513, Verdacht: a late application notification). So the state
    // UIKit reports now decides, not the order of the events.
    if (!uikit_app_in_background())
        reattach_surface();
#endif
    return !renderer_->close_requested();
}

void SdlWindow::handle_event(const SDL_Event& event) {
    if (handler_)
        handler_(event);
    // Window events of other windows (the software keyboard's dialog)
    // are not for the game window.
    const bool ours = event.window.windowID == SDL_GetWindowID(window_);
    switch (event.type) {
    case SDL_EVENT_QUIT:
        renderer_->request_close();
        break;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        if (ours)
            renderer_->request_close();
        break;
    case SDL_EVENT_KEY_DOWN:
        if (event.key.windowID == SDL_GetWindowID(window_) && event.key.key == SDLK_F11 && !event.key.repeat)
            SDL_SetWindowFullscreen(window_, (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) == 0);
        break;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        if (ours)
            renderer_->resize(surface_size(event.window.data1, event.window.data2));
        break;
#if defined(__ANDROID__) || defined(SDL_PLATFORM_IOS)
#if !defined(SDL_PLATFORM_IOS)
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
#endif
    case SDL_EVENT_DID_ENTER_BACKGROUND:
        surface_lost();  // usually done already (LgpSurface, event watch)
        break;
    case SDL_EVENT_DID_ENTER_FOREGROUND:
        reattach_surface();
        break;
#endif
    default:
        break;
    }
}

}  // namespace lgp::platform::desktop

#if defined(__ANDROID__)
// org.lgp.recomp.LgpSurface.surfaceDestroyed calls this before SDL releases
// the activity's window: WebGPU must stop presenting to it first, or wgpu
// aborts in the next configure (P7.3).
extern "C" JNIEXPORT void JNICALL Java_org_lgp_recomp_LgpSurface_nativeSurfaceDestroying(JNIEnv*, jclass) {
    using namespace lgp::platform::desktop;
    std::lock_guard lock(g_mobile_mutex);
    if (g_mobile_window != nullptr)
        g_mobile_window->surface_destroying();
}

// LgpSurface.surfaceChanged, after SDL stored the new ANativeWindow.
extern "C" JNIEXPORT void JNICALL Java_org_lgp_recomp_LgpSurface_nativeSurfaceReady(JNIEnv*, jclass) {
    using namespace lgp::platform::desktop;
    std::lock_guard lock(g_mobile_mutex);
    if (g_mobile_window != nullptr)
        g_mobile_window->surface_ready();
}
#endif
