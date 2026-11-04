// On-screen controller for touch screens (P7.3, phones and tablets): fingers
// on the window -> one HostPad, plus the shapes to draw over the game image.
// No device code: the backends (SDL touch events on Android/iOS) pass finger
// positions in, the renderer draws overlay() (WebGpuRenderer::set_overlay).
//
// Layout in landscape, measured in units of the window height so the
// controls keep their size on any aspect ratio:
//   left part   floating stick: a finger landing there sets the stick's
//               centre, dragging moves it (full deflection at kStickRadius)
//   right       A, B, X, Y in the Switch diamond (A right, B bottom)
//   top corners L/R, ZL/ZR next to them; top centre Minus and Plus
// A finger on a button may slide to its neighbours (the button under the
// finger counts). Buttons carry their names (A, ZL, + ...); L/R and ZL/ZR are
// drawn as rounded rectangles, the rest round. The stick is not drawn,
// its zone works without a picture.
// On tablets set_window_points() keeps the controls at about phone size.
// Settings (app menu): set_scale() sizes all controls and their distance to
// the edges, set_opacity() fades the drawn shapes, set_mirrored() swaps the
// stick and the face buttons (stick right); L/ZL stay left, R/ZR right.
//
// Fingers that land on neither a button nor the stick are free: they reach
// the game's touch screen (take_touches(), in console pixels over the game
// image, which the renderer fits into the window at 16:9), and a quick swipe
// upwards with one of them throws (set_swipe_action): the throw gesture of a
// single Joy-Con, or a short A press in handheld mode, where A throws. A tap
// on free space does nothing else. With set_controls(false) there are no
// buttons, no stick and no swipe; every finger is free.
// set_insets() moves the controls off the window's unsafe edges (notch,
// rounded corners, home indicator); the stick zone stays where it is.
// A small toggle at the bottom centre hides the controls (hidden()): then
// every finger but one on the toggle is free, as with set_controls(false),
// and the overlay shows only the toggle, which brings them back. For screens
// the game plays by touch alone ("Play with Pikachu": petting, berries).
// The toggle switches when its finger leaves without having moved off it; a
// finger that lands on it and moves on is free from its landing point, so a
// throw swipe may start there.
#pragma once

#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include "lgp/platform/overlay.hpp"
#include "lgp/platform/pad_input.hpp"
#include "lgp/platform/screen_fit.hpp"

namespace lgp::platform {

class TouchPad {
public:
    static constexpr float kStickRadius = 0.16f;  // full deflection at scale 1, in window heights
    // Swipe to throw: a free finger that moves at least kSwipeDistance window
    // heights upwards within kSwipeTime of landing, at most kSwipeMaxAngle
    // degrees off straight up. Its angle becomes HostPad::throw_side (-1 at
    // kSwipeFullSideAngle and more to the left, 1 to the right; Paul
    // 2026-10-22 on the iPad: 45 degrees as the limit for both was too flat,
    // a strong swipe to the right did nothing), its speed so far
    // throw_strength (kSwipeBaseSpeed window heights/s and slower 1,
    // kSwipeFullSpeed and faster 2; 1 without event times) and throw_reach
    // (-1 .. 1 over the same speeds, 0 halfway and without event times).
    // Untuned on a device.
    static constexpr float kSwipeDistance = 0.12f;
    static constexpr uint64_t kSwipeTimeNs = 300'000'000;
    static constexpr float kSwipeMaxAngle = 70.0f;
    static constexpr float kSwipeFullSideAngle = 35.0f;
    static constexpr float kSwipeBaseSpeed = 1.2f;
    static constexpr float kSwipeFullSpeed = 3.6f;
    static constexpr uint64_t kSwipePressNs = 100'000'000;  // how long the A press of a swipe lasts
    // A button tap counts at least this long (event time to apply()'s now_ns):
    // the game compares one npad sample per frame with the last, so a press
    // that only one host sample saw went missing at low frame rates (emulator
    // 2026-10-25: short taps often lost, 250 ms reliable).
    static constexpr uint64_t kTapPressNs = 100'000'000;
    // Aspect ratio of the game image (1280x720 and 1920x1080).
    static constexpr float kGameAspect = 16.0f / 9.0f;

    enum class SwipeAction : uint8_t {
        None,
        Throw,   // HostPad::throw_gesture (single Joy-Con)
        PressA,  // kButtonA for kSwipePressNs (handheld)
    };

    // aspect = window width / height.
    explicit TouchPad(float aspect = 16.0f / 9.0f) { set_aspect(aspect); }
    void set_aspect(float aspect);
    // Clamped to 0.5..1.4; 1 = default size. Narrow windows (4:3) draw at
    // most size(), so Minus/Plus stay clear of ZL/ZR; scale() keeps the setting.
    void set_scale(float scale);
    void set_opacity(float opacity);  // clamped to 0..1; OverlayCircle::alpha
    // Window height in points (iOS) or dp (Android); 0 = unknown. Taller
    // windows than kComfortHeight (tablets) draw everything at the size it has
    // at that height instead of growing with the window (Paul 2026-10-26: on
    // the iPad the controls were far too big to reach comfortably).
    void set_window_points(float height);
    static constexpr float kComfortHeight = 480.0f;
    void set_mirrored(bool mirrored);
    void set_swipe_action(SwipeAction action) { swipe_action_ = action; }
    // Off: every finger is free (touch screen only), apply() adds nothing,
    // and there is no toggle.
    void set_controls(bool on);
    // Hidden by the toggle (or here): like set_controls(false), toggle kept.
    void set_hidden(bool hidden);
    bool hidden() const { return hidden_; }
    // Safe area of the window (SdlWindow::safe_insets()); zero: none.
    void set_insets(const SafeInsets& insets);
    // Where the game image is in the window (WebGpuRenderer::window_to_image:
    // window fractions in, image fractions out, false off the image). Without
    // one the image is taken as fitted at 16:9, which is wrong for the fill
    // and stretch aspect modes.
    using ImageMapper = std::function<bool(float wx, float wy, float* ix, float* iy)>;
    void set_image_mapper(ImageMapper mapper) { image_mapper_ = std::move(mapper); }
    float scale() const { return scale_; }
    float size() const { return size_; }  // scale in effect for this aspect
    float opacity() const { return opacity_; }
    bool mirrored() const { return mirrored_; }
    float stick_radius() const { return kStickRadius * size_; }

    // Finger positions from 0 to 1 over the window (x right, y down);
    // time_ns: the event's monotonic time (swipe detection, apply()).
    void finger_down(uint64_t id, float x, float y, uint64_t time_ns = 0);
    void finger_move(uint64_t id, float x, float y, uint64_t time_ns = 0);
    // canceled: the system took the touch (a gesture at the edge): the toggle
    // does not switch, a free finger leaves no tap, and a swipe apply() has
    // not seen yet is dropped.
    void finger_up(uint64_t id, bool canceled = false);
    // Where a window point (fractions, as above) lands on the game's touch
    // screen; false off the game image (diagnostics, --input-log).
    bool map_to_game(float x, float y, TouchPoint* out) const { return to_game(x * aspect_, y, out); }
    // The swipe the next apply() turns into a throw or A (diagnostics).
    bool pending_swipe(float* side, float* strength, float* reach) const {
        *side = swipe_side_, *strength = swipe_strength_, *reach = swipe_reach_;
        return swipe_pending_;
    }
    SwipeAction swipe_action() const { return swipe_action_; }
    void release_all();

    // Adds the touched buttons to pad->buttons and, while a finger holds the
    // stick, sets the left stick; marks the pad connected. No-op without
    // fingers (a real controller on the same slot keeps its values). A button
    // touched since the last apply() counts once even if its finger already
    // left (a quick tap between two samples), and with event times and now_ns
    // until kTapPressNs after it landed. A swipe adds its throw or its
    // A press (held until now_ns passes the swipe's time + kSwipePressNs;
    // with now_ns 0 for one apply()).
    void apply(HostPad* pad, uint64_t now_ns = 0);
    bool touched() const { return !fingers_.empty(); }
    uint64_t buttons() const;

    // The free fingers on the game image, in console pixels, ids 0..15 kept
    // while a finger stays down. A finger that landed and left since the last
    // call is reported once (a quick tap between two samples).
    void take_touches(TouchScreenState* out);

    std::vector<OverlayCircle> overlay() const;

private:
    struct Button {
        uint64_t bit;
        float x, y, r;  // in window heights from the top left
    };
    enum class Kind : uint8_t { Stick, Button, Free, Toggle };
    struct Finger {
        uint64_t id;
        Kind kind;
        float x0, y0;  // stick centre / where a free finger landed (window heights)
        float x, y;
        uint64_t down_ns = 0;
        uint32_t touch_id = 0;   // Free: id on the game's touch screen
        bool reported = false;   // Free: take_touches() saw it
        bool swiped = false;     // Free: its swipe already counted
    };
    void layout();
    // Window heights -> console pixels on the game image; false outside it.
    bool to_game(float x, float y, TouchPoint* out) const;
    uint32_t free_touch_id() const;
    bool in_stick_zone(float x) const;  // x: fraction of the width
    bool on_toggle(float x, float y) const;  // window heights
    bool active() const { return controls_ && !hidden_; }  // buttons, stick, swipe
    const Button* button_at(float x, float y) const;
    Finger* find(uint64_t id);

    float aspect_ = 16.0f / 9.0f;
    ImageMapper image_mapper_;
    SafeInsets insets_;
    float scale_ = 1, size_ = 1, opacity_ = 1;
    float height_pt_ = 0;
    bool mirrored_ = false;
    std::vector<Button> buttons_;
    std::vector<Finger> fingers_;
    uint64_t latched_ = 0;  // buttons touched since the last apply()
    uint64_t tap_bits_ = 0;      // buttons that landed recently, held until tap_until_ns_
    uint64_t tap_until_ns_ = 0;
    bool controls_ = true;
    bool hidden_ = false;
    Button toggle_{};  // bit unused
    SwipeAction swipe_action_ = SwipeAction::PressA;
    bool swipe_pending_ = false;  // a swipe since the last apply()
    uint64_t swipe_finger_ = 0;   // the finger of that swipe
    uint64_t swipe_ns_ = 0;
    float swipe_side_ = 0, swipe_strength_ = 1, swipe_reach_ = 0;  // of the last swipe
    bool press_a_ = false;        // the A press of a swipe is still held
    uint64_t press_until_ns_ = 0;
    std::vector<TouchPoint> taps_;  // free fingers that left before take_touches() saw them
};

}  // namespace lgp::platform
