// Host controllers -> Switch controllers (P5.1). The device backends (SDL3 on
// the desktop, the browser Gamepad API and keyboard events on the web) only
// sample their devices into HostPad structs; everything Switch-specific lives
// here so it is the same on every platform and testable without devices:
//
//   KeyboardMap      keys (USB HID usage ids) -> buttons, sticks, throw
//   ControllerMapper one HostPad -> the ControllerState of one Switch
//                    controller style (handheld, single Joy-Con, ...), with
//                    dead zones, analog triggers and a synthetic throwing
//                    motion for a single Joy-Con without a gyro
//   PadInput         IInput over a backend's HostPads, one mapper per slot
//
// Why a synthetic motion: LGP accepts only handheld, a single Joy-Con and
// the Poké Ball Plus (P1.3). With a single Joy-Con, throwing a Poké Ball is a
// motion gesture; a keyboard or a gamepad without a gyro gets a button that
// plays a recorded-looking swing instead. Its shape is an assumption until it
// is checked against the real game (docs/knowledge/host-input-auf-switch-controller.md).
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

#include "lgp/platform/input.hpp"

namespace lgp::platform {

// One sample of a host device in Switch terms. Backends map their buttons
// by label (a pad's "A" is Switch A), not by position.
struct HostPad {
    bool connected = false;
    uint64_t device = 0;  // backend's id of the device (ControllerState::device), 0: none
    uint64_t buttons = 0;  // Button bits; ZL/ZR may also come from zl/zr
    float left_x = 0, left_y = 0;    // -1..1, right and up positive
    float right_x = 0, right_y = 0;
    float zl = 0, zr = 0;            // analog triggers 0..1
    MotionState motion;              // real sensor data (units of MotionState)
    bool throw_gesture = false;      // the control that stands for a throw
    // How the throw goes (touch swipes, TouchPad); read when throw_gesture
    // starts. side: -1 left .. 1 right; strength: 1..2 times the peaks;
    // reach: -1 short .. 0 standard .. 1 far (the length of the swing).
    float throw_side = 0;
    float throw_strength = 1;
    float throw_reach = 0;
};

// USB HID keyboard usage ids (SDL_Scancode uses the same numbers; the web
// backend translates KeyboardEvent.code). Positions follow the US layout,
// so on a German keyboard "Z" is the key labelled Y.
namespace hid_usage {
inline constexpr uint8_t kA = 0x04;  // letters: kA + (letter - 'A')
inline constexpr uint8_t k1 = 0x1E;  // digits 1..9, then 0 at 0x27
inline constexpr uint8_t kEnter = 0x28, kEscape = 0x29, kBackspace = 0x2A, kTab = 0x2B, kSpace = 0x2C;
inline constexpr uint8_t kMinus = 0x2D, kEqual = 0x2E;
inline constexpr uint8_t kRight = 0x4F, kLeft = 0x50, kDown = 0x51, kUp = 0x52;
inline constexpr uint8_t kKeypadMinus = 0x56, kKeypadPlus = 0x57;
inline constexpr uint8_t kLeftShift = 0xE1, kRightShift = 0xE5;
constexpr uint8_t letter(char c) { return static_cast<uint8_t>(kA + (c - 'A')); }
}  // namespace hid_usage

// KeyboardEvent.code ("KeyA", "ArrowUp", "Numpad1", ...) -> usage id, 0 if
// the key has none here (web backend).
uint8_t hid_usage_from_dom_code(const char* code);

enum class KeyTarget : uint8_t {
    Button,  // `button` bits
    LeftUp, LeftDown, LeftLeft, LeftRight,
    RightUp, RightDown, RightLeft, RightRight,
    Throw,
};

struct KeyBinding {
    uint8_t usage = 0;
    KeyTarget target = KeyTarget::Button;
    uint64_t button = 0;
};

class KeyboardMap {
public:
    // Defaults close to Ryujinx' keyboard defaults: WASD left stick, arrows
    // d-pad, Z/X/C/V = A/B/X/Y, Q/E = ZL/L, U/O = R/ZR, IJKL right stick,
    // F/H stick clicks, -/= (and keypad, Enter) Minus/Plus, Space throws.
    static KeyboardMap defaults();
    void bind(const KeyBinding& b) { bindings_.push_back(b); }
    void clear() { bindings_.clear(); }
    const std::vector<KeyBinding>& bindings() const { return bindings_; }
    // ORs the pressed keys (down[usage], 256 entries) into pad. Whether the
    // keyboard counts as a connected controller is the backend's call.
    void apply(const bool* down, HostPad* pad) const;

private:
    std::vector<KeyBinding> bindings_;
};

struct MapperConfig {
    ControllerStyle style = ControllerStyle::Handheld;
    float stick_deadzone = 0.15f;    // radial, then rescaled to the full range
    // Radial length from which the stick counts as fully deflected. Host
    // pads do not all reach 1.0 at their physical stop: SDL's Switch driver
    // scales each axis from the controller's calibration and keeps widening
    // that range, so a Pro Controller at its stop wobbles somewhat below
    // 1.0. Games that switch from walking to running near full deflection
    // then flicker between both (LGP with a Pro Controller, 2026-10-13).
    float stick_outer = 0.9f;
    // Hysteresis of the mapped sticks (fraction of full deflection, 0 = off):
    // a held stick keeps its last reported value while neither axis moved
    // more than this. Experiment against noisy host sticks (the Pro
    // Controller through SDL jitters by a few percent; the console's own
    // hid filters it, unverified).
    float stick_hold = 0.0f;
    float trigger_threshold = 0.5f;  // analog trigger -> ZL/ZR (or SL/SR)
    // Single Joy-Con without sensor data: report a resting controller and
    // play the throw gesture on HostPad::throw_gesture. Off: no motion.
    bool synthesize_motion = true;
    // Throw gesture: a forward swing, sine-shaped. Checked on the PC against
    // LGP (2026-10-13): 300 ms / 4 G / 3 rev/s is not recognised, these values
    // throw straight ahead.
    uint32_t throw_duration_ms = 400;
    float throw_peak_accel_g = 6.0f;
    float throw_peak_gyro_rps = 5.0f;
    // Axes (0 x, 1 y, 2 z) of the swing acceleration and the rotation, and
    // where gravity points at rest (axis, sign): lying face up = z, -1.
    int throw_accel_axis = 1;  // 3: all axes
    int throw_gyro_axis = 0;
    int rest_gravity_axis = 2;
    float rest_gravity_sign = -1.0f;
    // Throws to the side (HostPad::throw_side, from touch swipes): at full
    // side the swing adds throw_side_gain times its peaks as a sideways
    // acceleration along throw_side_accel_axis and a turn about
    // throw_side_gyro_axis (sign * axis, -1 = none). Measured in LGP
    // (2026-10-21, PC, sidegain 1, full side): all three parts steer, + is
    // right; a turn about z the most (about +270 px of 1280 at the ball),
    // about y the least, x acceleration bends the ball downwards. Second run
    // (side = gain * swipe side): about z, -0.5 curves left and up and hits,
    // but every right turn from +0.5 on dives right and down and misses;
    // about y, +1 curves right and up and hits, +-2 dive on both sides.
    // Third run: left about y with gain 1 curves left and up like z at -0.5
    // and hits; z at -0.25 shows nothing. So both sides turn about y with
    // gain 1 (throw_side_left_* can still split them); the accel part stays
    // off. Half a side (a swipe about 22° off vertical) barely shows.
    int throw_side_accel_axis = -1;
    float throw_side_accel_sign = 1.0f;
    int throw_side_gyro_axis = 1;
    float throw_side_gyro_sign = 1.0f;
    float throw_side_gain = 1.0f;
    int throw_side_left_gyro_axis = 1;  // side < 0
    float throw_side_left_gyro_sign = 1.0f;
    float throw_side_left_gain = 1.0f;
    // Gains of the sideways acceleration alone (keys sideaccel/leftaccel),
    // NaN = throw_side_gain, mirrored to the left. May be negative. Measured
    // (2026-10-22, turn about z, gain 1): +x at full lifts the right throw
    // over the target, 0 and -x let it dive; to the left, +x hits.
    float throw_side_accel_gain = std::numeric_limits<float>::quiet_NaN();
    float throw_side_left_accel_gain = std::numeric_limits<float>::quiet_NaN();
    // Strength of a swipe (HostPad::throw_strength, 1..2) is cut to this.
    // Measured in LGP: doubled peaks throw a steep lob that falls short, not a
    // farther throw; so 1 (the swipe's tempo is not used) until a strength
    // that helps is found.
    float throw_strength_max = 1.0f;
    // Reach of a throw (HostPad::throw_reach, from the swipe's tempo): the
    // swing lasts throw_duration_ms at 0, down to throw_reach_short_ms at -1
    // and up to throw_reach_long_ms at 1. Measured in LGP (2026-10-21, one
    // Zubat, one distance): 300 ms lands a little short of 400, 550 a little
    // farther and slightly higher; 250 is the same as 300, 700 bends the ball
    // to the left. Peaks between 0.8 and 1.45 times change nothing.
    uint32_t throw_reach_short_ms = 300;
    // With real sensors (a phone's gyro on slot 0) the swing adds to what
    // they report; replace: during the swing only the swing, with the
    // resting gravity of throw_* (diagnostics, --throw sensors=replace).
    bool throw_replace_sensors = false;
    uint32_t throw_reach_long_ms = 550;
    // Tests against the game without a touch screen: every throw (Space, R3)
    // takes this side / strength instead of the pad's. NaN: the pad's.
    float throw_force_side = std::numeric_limits<float>::quiet_NaN();
    float throw_force_strength = std::numeric_limits<float>::quiet_NaN();
    float throw_force_reach = std::numeric_limits<float>::quiet_NaN();
    // Real sensor data (HostPad::motion) re-expressed in another frame:
    // out[i] = motion_sign[i] * in[motion_axis[i]], for accel and gyro alike.
    // Identity by default; for trying whether the game expects a single
    // Joy-Con in another orientation than the host pad is held in.
    int motion_axis[3] = {0, 1, 2};
    float motion_sign[3] = {1, 1, 1};
    // Gain on the real angular velocity (the accelerometer stays: gravity
    // must remain 1 G). 1 = the host sensor as measured.
    float gyro_gain = 1.0f;
};

// Sets motion_axis/motion_sign from "x,y,z" with an optional sign per item,
// e.g. "-y,x,z" (each axis exactly once); false on a bad spec.
bool parse_motion_frame(const std::string& spec, MapperConfig& config);

// Sets stick_deadzone, stick_outer and optionally stick_hold from
// "DEADZONE,OUTER[,HOLD]" (e.g. "0.15,0.9" or "0.15,0.9,0.05";
// 0 <= deadzone < outer <= 1, 0 <= hold < 1); false on a bad spec.
bool parse_stick_range(const std::string& spec, MapperConfig& config);

// Overrides the throw gesture from "key=value,..." (ms, g, rps, accel=x|y|z|all,
// gyro=x|y|z|all, rest=[+-]x|y|z, side=[+-]x|y|z|none, yaw=[+-]x|y|z|none,
// sidegain=F, dir=-1..1, strength=1..2: every throw goes that way / that
// hard) for tuning against the game; false on a bad spec.
bool parse_throw_spec(const std::string& spec, MapperConfig& config);

class ControllerMapper {
public:
    explicit ControllerMapper(const MapperConfig& config = {}) : config_(config) {}
    const MapperConfig& config() const { return config_; }
    // now_ns: monotonic host time, drives the throw gesture.
    ControllerState map(const HostPad& pad, uint64_t now_ns);
    bool throwing() const { return throw_start_ns_ != 0; }

private:
    MotionState synthetic_motion(bool throw_started, const HostPad& pad, uint64_t now_ns);

    StickState hold(const StickState& in, StickState& last) const;

    MapperConfig config_;
    StickState last_left_, last_right_;  // stick_hold
    bool throw_was_pressed_ = false;
    uint64_t throw_start_ns_ = 0;  // 0 = no gesture playing
    float throw_side_ = 0, throw_strength_ = 1;  // of the gesture playing
    uint64_t throw_duration_ns_ = 0;
};

// IInput over HostPads. Slot i of the IInput is styles[i]; it is connected
// while the backend reports pad i connected. The backend overrides sample()
// (fill `pads`, one per slot, already sized) and optionally rumble(); both
// run under one lock, never concurrently.
class PadInput : public IInput {
public:
    explicit PadInput(std::vector<MapperConfig> slots);
    void poll() final;
    size_t controller_count() const final { return mappers_.size(); }
    ControllerState controller(size_t index) const final;
    void set_vibration(size_t index, float low, float high) final;
    TouchScreenState touch_screen() const final;
    // Replaces slot `index`'s mapping while the game runs (the app's
    // controller setting): hid presents the new style at its next update,
    // as if the controller had been swapped on the console.
    void set_slot(size_t index, const MapperConfig& config);
    MapperConfig slot(size_t index) const;

protected:
    // Monotonic host time in ns for the mappers.
    virtual uint64_t now_ns() const;
    virtual void sample(std::vector<HostPad>& pads) = 0;
    // The console touch screen at this tick (after sample(), same lock);
    // `touch` arrives empty.
    virtual void sample_touch(TouchScreenState& touch) { (void)touch; }
    virtual void rumble(size_t slot, float low, float high) {
        (void)slot;
        (void)low;
        (void)high;
    }

private:
    mutable std::mutex mutex_;
    std::vector<ControllerMapper> mappers_;
    std::vector<HostPad> pads_;
    std::vector<ControllerState> states_;
    TouchScreenState touch_;
};

// Who plays slot 0 (player 1) when several host pads are connected. On the
// console the controller that registers first on the game's controller
// screen gets npad No1 (a Joy-Con with SL+SR, the handheld with A); a host
// pad cannot register, so a button press decides instead. Mode Press: the
// first pad on which a button goes down after start, or after repick(),
// moves to slot 0, the others keep the backend's order behind it; then the
// choice holds until repick() or until that pad goes away. After repick()
// the slots stay as they are until the next press. A button already held
// when a pad shows up has not been pressed. Mode Order: the backend's order
// alone (connection order), as before.
class PlayerPick {
public:
    enum class Mode : uint8_t { Press, Order };
    explicit PlayerPick(Mode mode = Mode::Press) : mode_(mode) {}

    // ids: the pads in the backend's order, each id non-zero and stable while
    // the pad is connected; buttons: what each holds now (same index).
    // Returns the slot order as indices into ids.
    std::vector<size_t> update(const std::vector<uint64_t>& ids, const std::vector<uint64_t>& buttons);
    // The next button press chooses player 1 again (the game disconnected
    // npad No1, or the user asked for it in the app's settings).
    void repick();
    void set_mode(Mode mode);
    Mode mode() const { return mode_; }
    bool picking() const { return picking_; }  // the next press chooses
    uint64_t player1() const { return player1_; }  // 0: none chosen, backend order

private:
    Mode mode_;
    bool picking_ = mode_ == Mode::Press;
    uint64_t player1_ = 0;
    std::vector<std::pair<uint64_t, uint64_t>> held_;  // id, buttons at the last update
};

// Default slot layout for LGP: slot 0 handheld (the Handheld npad, player
// 1), slot 1 a single right Joy-Con (npad No2, player 2).
std::vector<MapperConfig> default_slots();

}  // namespace lgp::platform
