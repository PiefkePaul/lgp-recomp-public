// Desktop window for the WebGPU renderer (P4.4): an SDL3 window whose native
// handle becomes the WebGPU surface (HWND on Windows, a CAMetalLayer on
// macOS, an X11 window or Wayland surface on Linux).
//
// SDL wants its window and event loop on the thread that initialized it
// (the main thread on macOS), so create() and pump_events() belong to the
// host's main thread while guest threads render and present through
// renderer() (webgpu_renderer.hpp).
#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "lgp/platform/desktop/lifecycle.hpp"
#include "lgp/platform/webgpu/webgpu_renderer.hpp"

struct SDL_Window;
union SDL_Event;

namespace lgp::platform::desktop {

// The kind of device, for defaults that differ per platform (P7.4: frames
// in flight). AndroidTv covers the NVIDIA Shield; it needs SDL's Android
// glue running (call after SdlWindow::create()).
enum class DeviceKind { Desktop, Ios, AppleTv, Android, AndroidTv };
DeviceKind device_kind();
const char* device_kind_name(DeviceKind kind);
// The user's first preferred locale as "lang[-COUNTRY]" (SDL_GetPreferredLocales),
// empty where the system names none. On Android after SDL's Java side started.
std::string preferred_locale();

// iOS/iPadOS builds only (ios_scene.mm): asks the window's scene for a
// window of the whole screen (iPadOS 26 runs apps in resizable windows).
// No-op on iPhone.
void request_full_size_scene(SDL_Window* window);
// iOS/tvOS builds only (uikit_state.mm): true while UIKit has the app in the
// background (UIApplication.applicationState). Main thread.
bool uikit_app_in_background();

struct WindowOptions {
    std::string title = "lgp";
    uint32_t width = 1280;  // logical size; the drawable may be larger (HiDPI)
    uint32_t height = 720;
    bool resizable = true;
    // Borderless fullscreen on the window's display; F11 toggles it either way
    // (Enter and the other keys belong to the game's controller, P5.1).
    // Always on for Android and iOS/iPadOS (no status bar, home indicator
    // dimmed; see create()).
    bool fullscreen = false;
    webgpu::RendererOptions renderer;  // size and surface are filled in by create()
};

class SdlWindow {
public:
    // nullptr and *error on failure (no display, no video driver, no WebGPU).
    static std::unique_ptr<SdlWindow> create(const WindowOptions& options, std::string* error);
    ~SdlWindow();
    SdlWindow(const SdlWindow&) = delete;
    SdlWindow& operator=(const SdlWindow&) = delete;

    webgpu::WebGpuRenderer& renderer() { return *renderer_; }
    // Handles window events, waiting up to timeout_ms for the first one.
    // Resizes reach the renderer; closing the window makes the renderer's
    // end_frame() return false. Returns false once the window was closed.
    bool pump_events(int timeout_ms);
    // Every event pump_events() sees also goes here (input backends, P5.1).
    void set_event_handler(std::function<void(const SDL_Event&)> handler) { handler_ = std::move(handler); }
    // SDL's video driver ("x11", "wayland", "windows", "cocoa").
    const std::string& video_driver() const { return driver_; }
    // The SDL window (text input, title); main thread only.
    SDL_Window* sdl_window() const { return window_; }
    // Main thread: the window's edges under notch, Dynamic Island, rounded
    // corners and home indicator (SDL_GetWindowSafeArea), for the app's own
    // controls; zero on the desktop. SDL sends
    // SDL_EVENT_WINDOW_SAFE_AREA_CHANGED when they change.
    SafeInsets safe_insets() const;
    // Main thread: the window height in points (iOS) or dp (Android): pixels
    // over SDL_GetWindowDisplayScale; 0 if unknown.
    float height_points() const;

    // Phones and tablets: the app's surface goes away in the background.
    // surface_lost() stops presenting (any thread, before the OS window is
    // gone); the image comes back on the next surface (reattach_surface(),
    // called by pump_events()). The game keeps running meanwhile.
    void surface_lost();
    void reattach_surface();
    // Phones and tablets: handler(true) when the app goes to the background,
    // handler(false) when it is back in the foreground. Runs from SDL's event
    // watch, i.e. while the OS waits for the app (iOS: inside the app
    // delegate; Android: before SDL blocks its event loop), possibly not on
    // the thread that calls pump_events(), and before the surface goes.
    // Called once per change; set while already in the background, it is
    // called with true at once (LifecycleRelay). Set nullptr before what the
    // handler uses goes away. Desktops never call it.
    void set_lifecycle_handler(std::function<void(bool background)> handler) {
        lifecycle_.set_handler(std::move(handler));
    }
    void lifecycle_event(bool background) { lifecycle_.event(background); }
    // Android: the activity's surface goes (LgpSurface.surfaceDestroyed,
    // before SDL releases it) and comes back (surfaceChanged, after SDL took
    // the new one). In between reattach_surface() waits: SDL still reports the
    // old ANativeWindow while it releases it (P7.3). Pausing without losing
    // the surface (notification shade) only calls surface_lost().
    void surface_destroying();
    void surface_ready();

private:
    SdlWindow() = default;
    void lose_surface_locked();  // surface_mutex_ held
    void handle_event(const SDL_Event& event);
    // Android: > 0 replaces SDL_WaitEventTimeout by polling every this many
    // ms (LGP_SDL_PUMP_MS). SDL 3.4's wait spins there: each round pushes a
    // poll sentinel, whose wakeup ends the lifecycle wait at once (P7.3).
    int pump_period_ms_ = 0;
    // Android: a thread kept busy on one CPU (LGP_CLOCK_KEEPER), to test
    // whether the spinning SDLThread made the game faster by keeping clocks up.
    struct ClockKeeper;
    std::unique_ptr<ClockKeeper> clock_keeper_;
    SDL_Window* window_ = nullptr;
    void* metal_view_ = nullptr;
    std::string driver_;
    std::function<void(const SDL_Event&)> handler_;
    std::mutex surface_mutex_;
    bool surface_lost_ = false;
    LifecycleRelay lifecycle_;
    bool surface_ready_ = true;  // Android: SDL's ANativeWindow may be used
    std::unique_ptr<webgpu::WebGpuRenderer> renderer_;
};

}  // namespace lgp::platform::desktop
