// hid gestures (nn::hid::GestureState) from the touch screen's fingers. On
// the console the hid sysmodule derives them; LGP imports GetGestureStates
// but no touch screen reader (docs/analysis/01b-imports.md), so whatever a
// game does with touch goes through here.
//
// Own model, not checked against the console: a new touch starts a context
// (Touch with IsNewTouch), a finger that stays put turns into Press after
// kPressNs, moving more than kPanPx makes it Pan. Lifting the last finger
// ends the context with Tap (short and still; IsDoubleTap when a tap ended
// close by just before), Swipe (fast at the end) or Complete. Two or more
// fingers report their centre; Pinch and Rotate are not produced.
#pragma once

#include <array>
#include <cstdint>

#include "hid_shared_memory.hpp"
#include "lgp/platform/input.hpp"

namespace lgp::hle {

namespace gesture {
// nn::hid::GestureType, GestureDirection, GestureAttribute (libnx hid.h).
inline constexpr uint32_t kIdle = 0, kComplete = 1, kCancel = 2, kTouch = 3, kPress = 4, kTap = 5, kPan = 6,
                          kSwipe = 7, kPinch = 8, kRotate = 9;
inline constexpr uint32_t kDirNone = 0, kDirLeft = 1, kDirUp = 2, kDirRight = 3, kDirDown = 4;
inline constexpr uint32_t kAttrNewTouch = 1u << 4;
inline constexpr uint32_t kAttrDoubleTap = 1u << 8;

// Thresholds in console pixels and ns (assumptions, see above).
inline constexpr float kPanPx = 16.0f;
inline constexpr uint64_t kPressNs = 500'000'000;
inline constexpr uint64_t kTapMaxNs = 400'000'000;
inline constexpr uint64_t kDoubleTapNs = 300'000'000;
inline constexpr float kDoubleTapPx = 48.0f;
inline constexpr float kSwipeSpeed = 1500.0f;          // px/s at the end of the touch
inline constexpr uint64_t kVelocityWindowNs = 50'000'000;  // velocity over the last 50 ms
}  // namespace gesture

class GestureTracker {
public:
    // One sampling tick with the fingers down now. Fills *out (all but the
    // sampling number) and returns true when the game gets a new sample:
    // every tick while fingers are down and once when the last one left.
    bool update(const platform::TouchScreenState& touch, uint64_t now_ns, hidmem::GestureState* out);

private:
    struct Sample {
        uint64_t ns;
        float x, y;
    };
    void velocity(uint64_t now_ns, float* vx, float* vy) const;

    bool active_ = false;
    bool moved_ = false;
    uint64_t context_ = 0;
    uint64_t start_ns_ = 0;
    float start_x_ = 0, start_y_ = 0;
    float last_x_ = 0, last_y_ = 0;
    int32_t last_count_ = 0;
    hidmem::GesturePoint last_points_[4] = {};
    std::array<Sample, 16> history_{};  // ring of recent centres
    size_t history_len_ = 0, history_pos_ = 0;
    uint64_t last_tap_ns_ = 0;
    float last_tap_x_ = 0, last_tap_y_ = 0;
};

}  // namespace lgp::hle
