// IInput: controller state for the hid service (P3.6; filled in by P5.1).
// The backend maps keyboard, gamepads (SDL, the browser Gamepad API) and
// motion sources onto Switch controllers; hid writes the result into the
// shared memory the game reads.
//
// LGP accepts only handheld, a single Joy-Con and the Poké Ball Plus (P1.3
// measurement). With a single Joy-Con, throwing is a motion gesture, so a
// backend without a gyro has to synthesize motion for it (P5.1).
#pragma once

#include <cstddef>
#include <cstdint>

namespace lgp::platform {

// Bit numbers of nn::hid::NpadButton (the layout hid shared memory uses).
enum Button : uint64_t {
    kButtonA = 1ull << 0,
    kButtonB = 1ull << 1,
    kButtonX = 1ull << 2,
    kButtonY = 1ull << 3,
    kButtonStickL = 1ull << 4,
    kButtonStickR = 1ull << 5,
    kButtonL = 1ull << 6,
    kButtonR = 1ull << 7,
    kButtonZL = 1ull << 8,
    kButtonZR = 1ull << 9,
    kButtonPlus = 1ull << 10,
    kButtonMinus = 1ull << 11,
    kButtonLeft = 1ull << 12,
    kButtonUp = 1ull << 13,
    kButtonRight = 1ull << 14,
    kButtonDown = 1ull << 15,
    kButtonLeftSL = 1ull << 24,
    kButtonLeftSR = 1ull << 25,
    kButtonRightSL = 1ull << 26,
    kButtonRightSR = 1ull << 27,
};

enum class ControllerStyle : uint8_t {
    None,
    Handheld,
    JoyconLeft,   // held sideways
    JoyconRight,  // held sideways
    JoyconPair,
    ProController,
};

struct StickState {
    int32_t x = 0;  // -32767..32767, right positive
    int32_t y = 0;  // up positive
};

// Units as in hid SixAxisSensorState: acceleration in G, angular velocity in
// revolutions per second, both in the controller's frame.
struct MotionState {
    bool valid = false;
    float accel[3] = {0, 0, 0};
    float gyro[3] = {0, 0, 0};
};

// The console's touch screen in its own pixels: 1280x720, origin top left
// (nn::hid::TouchState). Backends report the fingers that are down at the
// sampling tick; hid derives the Start/End attributes and the gestures from
// how that set changes between two ticks.
inline constexpr float kTouchScreenWidth = 1280.0f;
inline constexpr float kTouchScreenHeight = 720.0f;
inline constexpr size_t kMaxTouches = 16;

struct TouchPoint {
    uint32_t id = 0;     // finger id, the same while the finger stays down
    float x = 0, y = 0;  // 0..kTouchScreenWidth, 0..kTouchScreenHeight
};

struct TouchScreenState {
    uint32_t count = 0;  // at most kMaxTouches
    TouchPoint points[kMaxTouches];
};

struct ControllerState {
    ControllerStyle style = ControllerStyle::None;  // None = not connected
    // The host device behind the slot (0: unknown or always the same). When
    // it changes, hid takes the slot's held buttons as new (another pad took
    // the slot, PlayerPick).
    uint64_t device = 0;
    uint64_t buttons = 0;
    StickState left;
    StickState right;
    MotionState motion;
};

// Diagnostics (--input-log): counts short drops of a stick that is held
// deflected, i.e. >= 50 % deflection falling below 20 % and coming back
// within 150 ms. A player who lets go of the stick does not come back that
// fast; a device or a pipeline that loses samples does.
struct StickDropCounter {
    uint32_t drops = 0;  // since the caller last reset it
    void sample(float length, uint64_t now_ns) {
        constexpr uint64_t kWindowNs = 150'000'000;
        if (dip_start_ns_ != 0 && now_ns - dip_start_ns_ > kWindowNs)
            dip_start_ns_ = 0;  // let go for real
        if (length >= 0.5f) {
            if (dip_start_ns_ != 0)
                ++drops;
            dip_start_ns_ = 0;
            held_ = true;
        } else if (length < 0.2f && held_) {
            dip_start_ns_ = now_ns != 0 ? now_ns : 1;
            held_ = false;
        }
    }

private:
    bool held_ = false;
    uint64_t dip_start_ns_ = 0;
};

class IInput {
public:
    virtual ~IInput() = default;
    // Called once per hid update (the service's sampling tick); afterwards
    // controller() returns that sample.
    virtual void poll() = 0;
    virtual size_t controller_count() const = 0;  // player slots offered
    virtual ControllerState controller(size_t index) const = 0;
    // Fingers on the console's touch screen at the last poll(); backends
    // without a touch screen report none.
    virtual TouchScreenState touch_screen() const { return {}; }
    // Rumble amplitudes 0..1; backends without rumble ignore it.
    virtual void set_vibration(size_t index, float low, float high) {
        (void)index;
        (void)low;
        (void)high;
    }
    // The game disconnected the controller of slot `index` (hid
    // DisconnectNpad) and waits for one to register again. On the console the
    // controller that registers first gets the npad; backends that choose
    // player 1 by a button press (PlayerPick) choose again for slot 0.
    virtual void npad_disconnected(size_t index) { (void)index; }
};

}  // namespace lgp::platform
