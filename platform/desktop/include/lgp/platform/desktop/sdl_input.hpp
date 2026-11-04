// Desktop controllers for hid (P5.1): SDL3 gamepads and the keyboard as
// IInput. SDL delivers device events on the thread that pumps them (the
// main thread, SdlWindow::pump_events), so this backend keeps its own state
// from events: the main thread calls handle_event() for every SDL event and
// service() once per loop (rumble), hid's sampling thread calls poll().
//
// Gamepad k is slot k (in connection order), except that the first pad on
// which a button is pressed becomes player 1 (slot 0, PlayerPick); the
// keyboard adds to slot 0,
// which counts as connected while the keyboard is in use (`keyboard`). On
// Android a TV remote counts as keyboard: D-pad, centre = A, Back = B,
// Menu = Plus (the window traps Back so it does not close the app).
// Buttons follow their labels: an Xbox pad's A is Switch A, a PlayStation
// cross as well (the Xbox-style position). Gyro and accelerometer of pads
// that have them (DualShock/DualSense, Switch controllers via HIDAPI) are
// passed on; the axis conversion is unchecked against the real game.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "lgp/platform/overlay.hpp"
#include "lgp/platform/pad_input.hpp"
#include "lgp/platform/touch_pad.hpp"

union SDL_Event;
struct SDL_Gamepad;
struct SDL_Sensor;

namespace lgp::platform::desktop {

struct SdlInputOptions {
    std::vector<MapperConfig> slots = default_slots();
    KeyboardMap keys = KeyboardMap::defaults();
    bool keyboard = true;  // keyboard drives slot 0
    // Gyro and accelerometer of pads that have them; off leaves the sensors
    // disabled in SDL (no sensor events, the pad reports no motion).
    bool motion = true;
    // On-screen controller from touch events (TouchPad), on slot 0. Default:
    // on for Android and iOS builds, except on Android TV (SDL_IsTV). Off,
    // fingers still reach the game's touch screen, nothing else.
    bool touch = default_touch();
    static bool default_touch();
    // A quick swipe upwards on free space throws (TouchPad::set_swipe_action),
    // while no gamepad with a button pressed since it connected holds slot 0,
    // unless touch_throw_with_gamepad.
    bool touch_throw = true;
    bool touch_throw_with_gamepad = false;
    // The phone's or tablet's own gyro and accelerometer (SDL sensors, not a
    // gamepad's) as the motion of slot 0 while no gamepad holds that slot:
    // aiming in handheld mode by tilting the device. Needs `motion` as well.
    // Default: where touch is (iOS, Android except TV). The axis conversion
    // (display orientation, signs) is unchecked against the real game.
    bool device_motion = default_touch();
    // Gamepads with neither a right stick nor shoulder buttons count as TV
    // remotes (Pad::remote): Android TV remotes (D-pad, centre, Back, Menu)
    // and the Siri Remote (its touch surface is SDL's left stick). Default:
    // on Android TV (SDL_IsTV) and tvOS only; a D-pad controller (SNES pad,
    // 8BitDo) has shoulder buttons and keeps its own slot anyway.
    bool tv_remotes = default_tv_remotes();
    static bool default_tv_remotes();
    // Player 1: the first pad with a button press (Press), or the first in
    // connection order (Order). The app's setting, --player1 press|order.
    PlayerPick::Mode player1 = PlayerPick::Mode::Press;
    // Diagnostics (--input-log): once per second per pad the SDL events by
    // kind, the longest gap between two events of that pad, short drops of
    // the held stick (StickDropCounter on SDL's values at hid's sampling tick), how
    // far the left stick reaches and how low it wobbles while held, the rumble sent,
    // and the longest gap between two service() calls of the main loop.
    bool log = false;
};

class SdlInput final : public PadInput {
public:
    // Main thread. nullptr and *error if SDL's gamepad subsystem fails.
    static std::unique_ptr<SdlInput> create(const SdlInputOptions& options, std::string* error);
    ~SdlInput() override;

    // Main thread: every SDL event, then service() once per loop iteration.
    void handle_event(const SDL_Event& event);
    void service();

    size_t gamepad_count() const;

    // Touch: the window's aspect ratio (width / height, also taken from
    // resize events), and the controller to draw (WebGpuRenderer::set_overlay):
    // empty without touch, or while a gamepad is connected and the screen was
    // not touched for a while.
    void set_touch_aspect(float aspect);
    // Touch: the window height in points (SdlWindow::height_points(), also
    // taken from resize events); tablets keep phone-sized controls.
    void set_touch_window_points(float height);
    // Touch: the window's safe area (SdlWindow::safe_insets(), also taken from
    // SDL_EVENT_WINDOW_SAFE_AREA_CHANGED); the controls stay inside it.
    void set_touch_insets(const SafeInsets& insets);
    // Touch: where the game image is in the window (TouchPad::set_image_mapper);
    // called on the input thread, so it must be thread-safe.
    void set_touch_image_mapper(TouchPad::ImageMapper mapper);
    // Size, opacity and side of the on-screen controller (app settings,
    // TouchPad::set_scale/set_opacity/set_mirrored); any thread.
    void set_touch_layout(float scale, float opacity, bool mirrored);
    // The on-screen controller on or off while running (the app's settings);
    // off lets go of every finger on it.
    void set_touch_enabled(bool enabled);
    // Swipe throws on or off, and whether also while a gamepad plays player 1
    // (the app's settings); any thread.
    void set_touch_throw(bool on, bool with_gamepad);
    // A scene that takes touches only ("Play with Pikachu", any thread): on
    // hides the on-screen controls (no buttons, stick or swipe), off brings
    // back what was shown before.
    void set_touch_only_scene(bool on);
    std::vector<OverlayCircle> touch_overlay() const;

    // Player 1 (any thread): the next button press on a pad chooses it again
    // (the app's settings); the mode as in SdlInputOptions::player1; the name
    // of the pad on slot 0 ("" without one).
    void pick_player1();
    // Player 1's controller while the game runs (the app's setting): the
    // slot's mapping and the swipe action follow at once.
    void set_player1_controller(const MapperConfig& config);
    void set_player1_mode(PlayerPick::Mode mode);
    std::string player1_name() const;
    // hid: the game disconnected npad No1 (LGP's controller screen with a
    // single Joy-Con), the next press chooses player 1 again.
    void npad_disconnected(size_t index) override;

protected:
    void sample(std::vector<HostPad>& pads) override;
    void sample_touch(TouchScreenState& touch) override;
    void rumble(size_t slot, float low, float high) override;

private:
    struct Pad {
        uint32_t id = 0;  // SDL_JoystickID
        SDL_Gamepad* gamepad = nullptr;
        std::string name;
        HostPad state;
        bool has_motion = false;
        // A TV remote (tv_remotes; SDL makes remotes gamepads,
        // SDL_HINT_TV_REMOTE_AS_JOYSTICK). It takes no slot of its own; its
        // buttons and stick join slot 0, so a controller connected after it
        // still is player 1.
        bool remote = false;
        // A button pressed since it connected: someone plays with it (a
        // paired pad left lying does not switch the touch throw off).
        bool used = false;
        // Highest ZL/ZR value since the last sample(): a trigger pressed and
        // released between two samples still reaches the mapper once, which
        // applies its own threshold. On Android SDL knows no initial trigger
        // value, takes the first press as it and sends it only together with
        // the release (B9, first ZL after connect).
        float trigger_peak[2] = {0, 0};
        float rumble[2] = {0, 0};
        // Strongest value set since the last service(): a pulse that starts and
        // ends between two main-loop iterations still reaches the motor.
        float peak[2] = {0, 0};
        float sent[2] = {0, 0};
        uint64_t sent_ms = 0;
        bool rumble_failed = false;
        // --input-log, current one-second window.
        struct Stats {
            uint32_t axis = 0, sensor = 0, button = 0, rumble_writes = 0;
            uint64_t last_event_ns = 0, max_event_gap_ns = 0;
            float max_rumble[2] = {0, 0};
            StickDropCounter left, right;
            // Left stick length (SDL's value): the longest, and the shortest
            // while held past half (how far a stick at its stop wobbles).
            float left_max = 0, left_held_min = 2;
        } stats;
    };
    explicit SdlInput(const SdlInputOptions& options);
    Pad* find(uint32_t id);
    Pad* slot_pad(size_t slot);  // the slot-th pad that is not a remote
    void update_swipe_action();  // from touch_throw* and the pad on slot 0; state_mutex_ held
    Pad* player_pad(size_t slot);  // slot_pad after PlayerPick, as of the last sample()
    void add(uint32_t id);
    void set_button(Pad& pad, int sdl_button, bool down);
    void count_event(Pad& pad, uint64_t timestamp_ns, uint32_t Pad::Stats::*counter);

    void open_device_sensors();
    void update_orientation();

    SdlInputOptions options_;
    mutable std::mutex state_mutex_;
    // Device sensors (device_motion): gyro and accelerometer, both or none.
    SDL_Sensor* device_gyro_ = nullptr;
    SDL_Sensor* device_accel_ = nullptr;
    MotionState device_motion_;
    int orientation_ = 0;  // SDL_DisplayOrientation of the primary display
    std::array<bool, 256> keys_{};
    uint64_t remote_ = 0;  // Android: buttons held on a TV remote
    std::vector<Pad> pads_;
    PlayerPick pick_;
    std::vector<uint32_t> slot_ids_;  // pad id per slot from the last sample()
    // Rumble the game asked for, per slot: it moves with the slot when
    // PlayerPick puts another pad there (the old pad stops).
    std::vector<std::array<float, 2>> slot_rumble_;
    uint32_t logged_player1_ = 0;
    TouchPad touch_;
    bool touch_only_scene_ = false;    // set_touch_only_scene
    bool hidden_before_scene_ = false;  // the toggle's state when it began
    uint64_t last_touch_ms_ = 0;
    uint64_t log_start_ms_ = 0, last_service_ms_ = 0, max_service_gap_ms_ = 0;
};

}  // namespace lgp::platform::desktop
