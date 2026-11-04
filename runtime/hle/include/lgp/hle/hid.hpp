// hid (P5.1): controllers for the game. IHidServer configures which npads
// and styles the game accepts; the input itself reaches the game only
// through the 0x40000-byte shared memory (IAppletResource), which the host
// fills by calling HidState::update() once per sampling tick.
//
// Command ids and layouts from switchbrew "HID services" and libnx hid.h
// (interface documentation only); which commands exist follows the Ryujinx
// reference run (docs/analysis/02-services.md), anything else stops the
// guest as an unknown command. Details and open points:
// docs/knowledge/horizon-hid-npad-shared-memory.md.
//
// Controllers come from lgp::platform::IInput. Slot i of the backend is npad
// No(i+1); a slot in handheld style is the Handheld npad instead. A style the
// game did not declare supported (SetSupportedNpadStyleSet/IdType) is not
// presented, as on the console: LGP rejects the Pro Controller and the
// Joy-Con pair that way.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "lgp/hle/event.hpp"
#include "lgp/hle/service.hpp"
#include "lgp/platform/input.hpp"

namespace lgp::hle {

namespace hidmem {
struct SharedMemory;
}
class GestureTracker;

// nn::hid::NpadStyleTag bits.
namespace npad_style {
inline constexpr uint32_t kFullKey = 1u << 0;
inline constexpr uint32_t kHandheld = 1u << 1;
inline constexpr uint32_t kJoyDual = 1u << 2;
inline constexpr uint32_t kJoyLeft = 1u << 3;
inline constexpr uint32_t kJoyRight = 1u << 4;
inline constexpr uint32_t kGc = 1u << 5;
inline constexpr uint32_t kPalma = 1u << 6;
inline constexpr uint32_t kStandard = kFullKey | kHandheld | kJoyDual | kJoyLeft | kJoyRight;
}  // namespace npad_style

// nn::hid::NpadIdType values.
namespace npad_id {
inline constexpr uint32_t kNo1 = 0;
inline constexpr uint32_t kNo2 = 1;
inline constexpr uint32_t kNo8 = 7;
inline constexpr uint32_t kOther = 0x10;
inline constexpr uint32_t kHandheld = 0x20;
}  // namespace npad_id

struct HidConfig {
    platform::IInput* input = nullptr;  // nullptr: no controllers at all
    // Controller colors the shared memory reports (RGBA, body then buttons).
    // Defaults: grey Joy-Cons and a black Pro Controller.
    uint32_t joy_left_body = 0x828282FF, joy_left_buttons = 0x0F0F0FFF;
    uint32_t joy_right_body = 0x828282FF, joy_right_buttons = 0x0F0F0FFF;
    uint32_t full_key_body = 0x2D2D2DFF, full_key_buttons = 0xE6E6E6FF;
    // Diagnostics (--input-log): every change of a vibration value at Info,
    // with the handle, both frequencies and whether it reached a motor, and
    // once per second a summary (vibration commands, IsSixAxisSensorAtRest
    // answers, stick drops as the game sees them, button presses per npad,
    // gaps of the sampling tick).
    bool log_input = false;
    // Diagnostics (--motion-pose frozen): the six-axis samples keep angle 0 and
    // an identity direction; acceleration and angular velocity still pass.
    // Shows whether a title reads the integrated pose or only the rates.
    bool freeze_orientation = false;
};

class HidState {
public:
    explicit HidState(const HidConfig& config);
    ~HidState();
    HidState(const HidState&) = delete;
    HidState& operator=(const HidState&) = delete;

    // One sampling tick: polls the IInput backend and appends a sample to
    // every npad LIFO (and to the started motion sensors, the touch screen,
    // the gestures and the debug pad once the game activated them). now_ns is a monotonic
    // host time; it only feeds the motion sensors' delta time. Call it from
    // one host thread, e.g. every 5 ms or once per frame.
    void update(rt::Kernel& kernel, uint64_t now_ns);

    // Introspection for tests and the host UI.
    uint32_t presented_style(uint32_t npad_id) const;  // 0 = not connected
    uint32_t supported_style_set() const;
    uint32_t joy_hold_type() const;
    bool six_axis_started(uint32_t handle) const;
    // Vibration values since the start: reached a motor, or dropped because
    // the handle's style is not the one the npad presents (or no npad).
    struct VibrationCounts {
        uint64_t sent = 0, dropped_style = 0, dropped_npad = 0;
    };
    VibrationCounts vibration_counts() const;
    // Button presses (bits newly set from one sample to the next) the game
    // got on the npad with this id since the start; 0 for an unknown id.
    uint64_t button_presses(uint32_t npad_id) const;

    // Npad id -> index into the shared memory npad array, or -1.
    static int npad_index(uint32_t npad_id);

private:
    friend class HidServer;
    friend class HidAppletResource;
    friend class HidActiveVibrationDeviceList;

    struct SixAxis {
        bool started = false;
        bool fusion = true;
        float fusion_params[2] = {0.03f, 0.4f};
        uint32_t zero_drift_mode = 1;  // Standard
        uint64_t sampling_number = 0;
        uint64_t last_ns = 0;
        float angle[3] = {0, 0, 0};
        float direction[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        uint32_t idle_ticks = 0;  // hid updates since it stopped
        // Back to the pose of a fresh start: angle 0, direction identity.
        // Without sensor fusion (no gravity correction) the integrated
        // direction only drifts away from it (VERDACHT: why aiming goes
        // wrong after the first throw on phones).
        void reset_orientation() {
            const SixAxis fresh;
            std::copy_n(fresh.angle, 3, angle);
            std::copy_n(&fresh.direction[0][0], 9, &direction[0][0]);
        }
    };
    std::mutex& mutex() { return mutex_; }
    // Shared memory object, created on the first call (IAppletResource).
    std::shared_ptr<rt::KSharedMemory> shared_memory(rt::Kernel& kernel);
    // Recomputes which controller every npad presents after a configuration
    // change, rewrites the npad headers and signals style set events. Caller
    // holds mutex().
    void refresh(rt::Kernel& kernel);
    Event& style_event(size_t npad_index) { return npads_[npad_index].style_event; }
    void set_supported_styles(uint32_t styles) { supported_styles_ = styles; }
    void set_supported_ids(const std::vector<uint32_t>& ids);
    void set_joy_mode(size_t npad_index, uint32_t mode) { npads_[npad_index].joy_mode = mode; }
    void disconnect(size_t npad_index);
    void set_hold_type(uint32_t t) { hold_type_ = t; }
    void set_handheld_activation(uint32_t m) { handheld_activation_ = m; }
    uint32_t handheld_activation() const { return handheld_activation_; }
    void activate_touch_screen() { touch_active_ = true; }
    void activate_debug_pad() { debug_pad_active_ = true; }
    void activate_gesture() { gesture_active_ = true; }
    void activate_npad(int32_t revision) { npad_revision_ = revision; }
    void set_palma_all_connectable(bool v) { palma_all_connectable_ = v; }
    void set_palma_boost_mode(bool v) { palma_boost_mode_ = v; }
    SixAxis& six_axis(uint32_t handle) { return six_axis_[handle]; }
    bool is_at_rest(uint32_t handle) const;
    // Rumble for the device behind a vibration handle (amplitudes 0..1).
    // cmd: the IPC command, for the log.
    void vibrate(uint32_t command, uint32_t handle, const float value[4]);
    void log_input_tick(uint64_t now_ns);

    struct Npad {
        uint32_t style = 0;        // presented style bit, 0 = none
        int slot = -1;             // IInput slot behind it
        uint32_t joy_mode = 0;     // 0 dual, 1 single
        bool disconnected = false;  // DisconnectNpad until the slot changes or a new press
        platform::ControllerStyle disconnected_style = platform::ControllerStyle::None;
        uint64_t held_since_disconnect = 0;  // buttons down since DisconnectNpad (not a new press)
        uint64_t disconnected_device = 0;    // ControllerState::device then; another device starts fresh
        uint64_t sampling[7] = {};
        float rumble[2][2] = {};  // [left/right][low/high]
        platform::StickDropCounter stick_drops;  // --input-log
        float stick_max = 0, stick_held_min = 2;  // --input-log, presented stick length
        // --input-log: changes of the longer (steering) stick while held, and
        // the largest direction change between two ticks (degrees).
        platform::StickState stick_last{};
        uint32_t stick_changes = 0;
        float stick_turn_max = 0;
        uint64_t pushed_buttons = 0;  // buttons of the last sample written
        uint64_t presses = 0;         // button_presses()
        // --input-log: presses of the current window and their buttons.
        uint32_t window_presses = 0;
        uint64_t window_buttons = 0;
        Event style_event;
    };

    void write_headers(hidmem::SharedMemory& mem) const;
    void push_samples(hidmem::SharedMemory& mem, uint64_t now_ns);
    void push_six_axis(hidmem::SharedMemory& mem, uint32_t handle, SixAxis& s, uint64_t now_ns);

    const HidConfig config_;
    mutable std::mutex mutex_;
    std::shared_ptr<rt::KSharedMemory> shm_;
    std::vector<platform::ControllerState> controllers_;
    std::array<Npad, 10> npads_;
    uint32_t supported_styles_ = npad_style::kStandard;
    std::array<bool, 10> supported_ids_;
    uint32_t hold_type_ = 0;  // vertical
    uint32_t handheld_activation_ = 0;
    int32_t npad_revision_ = -1;
    bool touch_active_ = false;
    bool debug_pad_active_ = false;
    bool gesture_active_ = false;
    bool palma_all_connectable_ = false;
    bool palma_boost_mode_ = false;
    uint64_t touch_sampling_ = 0;
    uint64_t gesture_sampling_ = 0;
    // Fingers of this tick and of the previous one (TouchState Start/End).
    platform::TouchScreenState touch_now_, touch_last_;
    std::unique_ptr<GestureTracker> gestures_;
    uint64_t debug_pad_sampling_ = 0;
    std::map<uint32_t, SixAxis> six_axis_;
    // Shoulder pair per controller (L+R, or SL+SR on a single Joy-Con):
    // the first button waits kChordWaitNs for the second; together they
    // reset the orientation and never reach the game.
    struct Chord {
        uint64_t first_ns = 0;     // a lone button held back since then
        uint64_t held = 0;         // that button
        uint64_t tap_until_ns = 0; // a held-back tap released early, shown late until then
        uint64_t tap = 0;
        bool active = false;       // both down: masked until both are up
    };
    std::vector<Chord> chords_;
    bool filter_recenter(platform::ControllerState& c, Chord& chord, uint64_t now_ns);
    VibrationCounts vibration_counts_;
    // --input-log: last value per handle (logs changes only) and the
    // counters of the current one-second window.
    std::map<uint32_t, std::array<float, 4>> vibration_last_;
    struct LogWindow {
        uint64_t start_ns = 0, last_tick_ns = 0, max_tick_gap_ns = 0;
        uint64_t ticks = 0, vib_sent = 0, vib_dropped = 0, at_rest_yes = 0, at_rest_no = 0;
        uint32_t touch_starts = 0;
        float touch_first_x = 0, touch_first_y = 0;
    } log_;
};

// Registers hid. config.input must outlive every session; the returned state
// is what the host drives with update().
std::shared_ptr<HidState> register_hid(ServiceManager& sm, const HidConfig& config);

// Touch positions arrive in 1280x720; the screen state carries them times
// (sx, sy), clamped to that range (widescreen: the game reads touches in its
// wider or taller render size). Default 1, 1.
void set_touch_scale(float sx, float sy);

}  // namespace lgp::hle
