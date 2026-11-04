// P7.3: on-screen controller (platform/common/touch_pad). No devices.
#include <cmath>
#include <cstdio>

#include "../runtime/check.hpp"
#include "lgp/platform/touch_pad.hpp"

using namespace lgp::platform;

namespace {

bool near_equal(float a, float b) { return std::fabs(a - b) < 1e-4f; }

// Whether two drawn controls overlap (x in window heights): circles by their
// radii, shoulder shapes by their bounding boxes.
bool overlap(const OverlayCircle& a, const OverlayCircle& b, float aspect) {
    const float dx = std::fabs(a.x - b.x) * aspect, dy = std::fabs(a.y - b.y);
    const bool round_a = a.half_w == 1 && a.half_h == 1, round_b = b.half_w == 1 && b.half_h == 1;
    if (round_a && round_b)
        return std::hypot(dx, dy) <= a.r + b.r;
    return dx <= a.half_w * a.r + b.half_w * b.r && dy <= a.half_h * a.r + b.half_h * b.r;
}

// A window position of the button with `bit`, from the overlay.
bool centre_of(const TouchPad& t, uint64_t bit, float* x, float* y) {
    // Overlay order follows the layout: A, B, X, Y, L, ZL, R, ZR, Minus, Plus.
    const uint64_t order[] = {kButtonA, kButtonB, kButtonX, kButtonY, kButtonL,
                              kButtonZL, kButtonR, kButtonZR, kButtonMinus, kButtonPlus};
    const auto circles = t.overlay();
    for (size_t i = 0; i < 10 && i < circles.size(); ++i)
        if (order[i] == bit) {
            *x = circles[i].x;
            *y = circles[i].y;
            return true;
        }
    return false;
}

void test_buttons() {
    TouchPad t(2.0f);
    HostPad p;
    t.apply(&p);
    CHECK(!p.connected);  // no finger: the slot keeps a real controller's values
    float x = 0, y = 0;
    CHECK(centre_of(t, kButtonA, &x, &y));
    CHECK(x > 0.8f && x < 1.0f);  // right side
    t.finger_down(1, x, y);
    t.apply(&p);
    CHECK(p.connected);
    CHECK_EQ(p.buttons, kButtonA);
    CHECK(t.overlay()[0].pressed);
    // Sliding onto B: B instead of A.
    float bx = 0, by = 0;
    CHECK(centre_of(t, kButtonB, &bx, &by));
    t.finger_move(1, bx, by);
    CHECK_EQ(t.buttons(), kButtonB);
    // A second finger on Plus; then both up.
    CHECK(centre_of(t, kButtonPlus, &x, &y));
    t.finger_down(2, x, y);
    CHECK_EQ(t.buttons(), kButtonB | kButtonPlus);
    t.finger_up(1);
    t.finger_up(2);
    CHECK(!t.touched());
    CHECK_EQ(t.buttons(), 0u);
    // A quick tap (down and up between two samples) still reaches one sample.
    p = HostPad{};
    t.apply(&p);  // clears what the fingers above left latched
    CHECK(centre_of(t, kButtonX, &x, &y));
    t.finger_down(4, x, y);
    t.finger_up(4);
    p = HostPad{};
    t.apply(&p);
    CHECK_EQ(p.buttons, kButtonX);
    p = HostPad{};
    t.apply(&p);
    CHECK_EQ(p.buttons, 0u);
    CHECK(!p.connected);
    // Empty space on the right: nothing.
    t.finger_down(3, 0.6f, 0.95f);
    CHECK_EQ(t.buttons(), 0u);
}

void test_tap_hold() {
    // With event times a quick tap holds its button for kTapPressNs.
    constexpr uint64_t ms = 1'000'000;
    TouchPad t(2.0f);
    HostPad p;
    float x = 0, y = 0;
    CHECK(centre_of(t, kButtonA, &x, &y));
    t.finger_down(1, x, y, 1000 * ms);
    t.finger_up(1);
    t.apply(&p, 1005 * ms);
    CHECK_EQ(p.buttons, kButtonA);
    p = HostPad{};
    t.apply(&p, 1060 * ms);  // the next frame still sees it
    CHECK(p.connected);
    CHECK_EQ(p.buttons, kButtonA);
    p = HostPad{};
    t.apply(&p, 1000 * ms + TouchPad::kTapPressNs);
    CHECK_EQ(p.buttons, 0u);
    CHECK(!p.connected);
    // A held button is not cut short and ends with its finger after the hold.
    t.finger_down(2, x, y, 2000 * ms);
    p = HostPad{};
    t.apply(&p, 2500 * ms);
    CHECK_EQ(p.buttons, kButtonA);
    t.finger_up(2);
    p = HostPad{};
    t.apply(&p, 2510 * ms);
    CHECK_EQ(p.buttons, 0u);
    // release_all drops a held tap.
    t.finger_down(3, x, y, 3000 * ms);
    t.finger_up(3);
    t.release_all();
    p = HostPad{};
    t.apply(&p, 3010 * ms);
    CHECK_EQ(p.buttons, 0u);
}

void test_stick() {
    TouchPad t(2.0f);  // width = 2 window heights
    // Down at (0.2, 0.6) of the window, then a quarter radius right and a
    // full radius up (in window heights; x positions scale by the aspect).
    t.finger_down(7, 0.2f, 0.6f);
    HostPad p;
    t.apply(&p);
    CHECK(std::fabs(p.left_x) < 1e-6f && std::fabs(p.left_y) < 1e-6f);
    const float r = TouchPad::kStickRadius;
    t.finger_move(7, 0.2f + 0.25f * r / 2.0f, 0.6f - r);
    p = HostPad{};
    t.apply(&p);
    const float len = std::hypot(0.25f, 1.0f);
    CHECK(std::fabs(p.left_x - 0.25f / len) < 1e-4f);
    CHECK(std::fabs(p.left_y - 1.0f / len) < 1e-4f);  // up is positive
    CHECK_EQ(p.buttons, 0u);
    // The stick is not drawn, held or not: buttons and the toggle only.
    const auto o = t.overlay();
    CHECK(o.size() == 11u);
    for (const OverlayCircle& c : o)
        CHECK(c.kind == OverlayCircle::kButton);
    // A second finger in the stick zone does not take the stick over.
    t.finger_down(8, 0.1f, 0.9f);
    p = HostPad{};
    t.apply(&p);
    CHECK(p.left_y > 0.9f);
    t.release_all();
    CHECK(!t.touched());
}

void test_settings() {
    TouchPad t(2.0f);
    float ax = 0, ay = 0;
    CHECK(centre_of(t, kButtonA, &ax, &ay));
    const float r1 = t.overlay()[0].r;
    CHECK(t.overlay()[0].alpha == 1.0f);
    // Larger: the A button grows and moves away from the right edge.
    t.set_scale(1.4f);
    float x = 0, y = 0;
    CHECK(centre_of(t, kButtonA, &x, &y));
    CHECK(std::fabs(t.overlay()[0].r - 1.4f * r1) < 1e-5f);
    // Largest size: X stays below ZR, Minus/Plus clear of the top centre.
    {
        const auto o = t.overlay();
        CHECK(o[2].y - o[2].r > o[7].y + o[7].r);
        CHECK(o[8].y == 0.08f && (0.5f - o[8].x) * 2.0f - o[8].r > 0.05f);
    }
    CHECK(x < ax && y < ay);
    t.finger_down(1, x, y);
    CHECK_EQ(t.buttons(), kButtonA);
    t.release_all();
    // The stick reaches full deflection at the scaled radius.
    t.finger_down(2, 0.2f, 0.8f);
    t.finger_move(2, 0.2f, 0.8f - t.stick_radius());
    HostPad p;
    t.apply(&p);
    CHECK(std::fabs(p.left_y - 1.0f) < 1e-4f);
    t.release_all();
    t.set_scale(9.0f);
    CHECK(t.scale() == 1.4f);  // clamped
    t.set_scale(1.0f);
    // Opacity reaches every circle.
    t.set_opacity(0.4f);
    for (const OverlayCircle& c : t.overlay())
        CHECK(std::fabs(c.alpha - 0.4f) < 1e-6f);
    // Mirrored: face buttons left, stick zone right; L stays left, R right.
    t.set_mirrored(true);
    CHECK(centre_of(t, kButtonA, &x, &y));
    CHECK(x < 0.5f);
    CHECK(centre_of(t, kButtonL, &x, &y));
    CHECK(x < 0.5f);
    CHECK(centre_of(t, kButtonR, &x, &y));
    CHECK(x > 0.5f);
    t.finger_down(3, 0.9f, 0.7f);
    t.finger_move(3, 0.9f + t.stick_radius() / 2.0f, 0.7f);
    p = HostPad{};
    t.apply(&p);
    CHECK(std::fabs(p.left_x - 1.0f) < 1e-4f);
    CHECK_EQ(p.buttons, 0u);
    t.release_all();
    t.finger_down(4, 0.2f, 0.8f);  // the old stick zone is no stick now
    p = HostPad{};
    t.apply(&p);
    CHECK(std::fabs(p.left_x) < 1e-6f && std::fabs(p.left_y) < 1e-6f);
}

// No two drawn buttons overlap at any size, on 4:3 (iPad) to 21:9 phones.
void test_no_overlap() {
    for (float aspect : {4.0f / 3.0f, 3.0f / 2.0f, 16.0f / 10.0f, 16.0f / 9.0f, 2.0f, 19.5f / 9.0f, 21.0f / 9.0f})
        for (float scale : {0.5f, 0.8f, 1.0f, 1.2f, 1.3f, 1.4f})
            for (bool mirrored : {false, true}) {
                TouchPad t(aspect);
                t.set_scale(scale);
                t.set_mirrored(mirrored);
                CHECK(t.scale() == scale && t.size() <= scale);
                const auto o = t.overlay();
                for (size_t i = 0; i < 10; ++i) {
                    // Minus/Plus keep their height at the top centre (pause button).
                    if (i >= 8)
                        CHECK(o[i].y == 0.08f && std::fabs(o[i].x - 0.5f) * aspect - o[i].r > 0.05f);
                    // The hide/show toggle (last circle) stays clear of every button.
                    const OverlayCircle& tg = o.back();
                    CHECK(std::hypot((o[i].x - tg.x) * aspect, o[i].y - tg.y) > o[i].r + tg.r);
                    for (size_t j = i + 1; j < 10; ++j) {
                        if (overlap(o[i], o[j], aspect)) {
                            std::fprintf(stderr, "aspect %g scale %g mirrored %d: %zu/%zu overlap\n", aspect, scale,
                                         mirrored, i, j);
                            CHECK(false);
                        }
                    }
                }
            }
    // Only narrow windows shrink the controls.
    TouchPad wide(16.0f / 9.0f), narrow(4.0f / 3.0f);
    wide.set_scale(1.4f);
    narrow.set_scale(1.4f);
    CHECK(wide.size() == 1.4f && narrow.size() < 1.35f);
}


// Free fingers reach the game's touch screen in console pixels; fingers on
// a button or the stick do not.
void test_touches() {
    TouchPad t(16.0f / 9.0f);
    TouchScreenState ts;
    t.finger_down(1, 0.5f, 0.5f);  // centre of the window = centre of the image
    float ax = 0, ay = 0;
    CHECK(centre_of(t, kButtonA, &ax, &ay));
    t.finger_down(2, ax, ay);      // A
    t.finger_down(3, 0.2f, 0.6f);  // stick
    t.take_touches(&ts);
    CHECK_EQ(ts.count, 1u);
    CHECK_EQ(ts.points[0].id, 0u);
    CHECK(std::fabs(ts.points[0].x - 640.0f) < 0.01f && std::fabs(ts.points[0].y - 360.0f) < 0.01f);
    HostPad p;
    t.apply(&p);
    CHECK_EQ(p.buttons, kButtonA);  // the free finger presses nothing
    // A second free finger gets the next id; the first keeps its own.
    t.finger_down(4, 0.75f, 0.25f);
    t.take_touches(&ts);
    CHECK_EQ(ts.count, 2u);
    CHECK_EQ(ts.points[0].id, 0u);
    CHECK_EQ(ts.points[1].id, 1u);
    CHECK(std::fabs(ts.points[1].x - 960.0f) < 0.01f && std::fabs(ts.points[1].y - 180.0f) < 0.01f);
    // A free finger sliding over a button does not press it.
    t.finger_move(4, ax, ay);
    CHECK_EQ(t.buttons(), kButtonA);
    t.finger_up(2);
    CHECK_EQ(t.buttons(), 0u);
    // Lifting frees the id; a quick tap between two takes is reported once.
    t.finger_up(1);
    t.finger_down(5, 0.5f, 0.9f);
    t.finger_up(5);
    t.take_touches(&ts);
    CHECK_EQ(ts.count, 2u);  // finger 4 (id 1) and the tap (id 0)
    CHECK_EQ(ts.points[1].id, 0u);
    CHECK(std::fabs(ts.points[1].y - 0.9f * 720.0f) < 0.01f);
    t.take_touches(&ts);
    CHECK_EQ(ts.count, 1u);
    t.release_all();
    t.take_touches(&ts);
    CHECK_EQ(ts.count, 0u);

    // 4:3 (iPad): the image has bars above and below. A finger on a bar is
    // not on the touch screen; one on the image maps into it.
    TouchPad ipad(4.0f / 3.0f);
    const float bar = (1.0f - (4.0f / 3.0f) / (16.0f / 9.0f)) / 2.0f;  // 0.125 of the height
    ipad.finger_down(1, 0.5f, bar / 2);
    ipad.finger_down(2, 0.6f, bar + 0.25f * (1 - 2 * bar));
    ipad.take_touches(&ts);
    CHECK_EQ(ts.count, 1u);
    CHECK(std::fabs(ts.points[0].x - 768.0f) < 0.01f && std::fabs(ts.points[0].y - 180.0f) < 0.1f);
    // Moved off the image, the finger stays at its edge.
    ipad.finger_move(2, 0.6f, 0.99f);
    ipad.take_touches(&ts);
    CHECK(std::fabs(ts.points[0].y - 720.0f) < 0.01f);

    // Without controls every finger is free, and apply() adds nothing.
    TouchPad off(16.0f / 9.0f);
    off.set_controls(false);
    off.finger_down(1, ax / (16.0f / 9.0f), ay);
    off.finger_down(2, 0.2f, 0.6f);
    off.take_touches(&ts);
    CHECK_EQ(ts.count, 2u);
    p = HostPad{};
    off.apply(&p);
    CHECK(!p.connected);
    CHECK_EQ(p.buttons, 0u);

    // With the renderer's placement (here: stretch over a 4:3 window) the
    // whole window is the image, no bars.
    TouchPad stretched(4.0f / 3.0f);
    stretched.set_image_mapper([](float wx, float wy, float* ix, float* iy) {
        *ix = wx, *iy = wy;
        return true;
    });
    stretched.finger_down(1, 0.5f, 0.05f);
    stretched.take_touches(&ts);
    CHECK_EQ(ts.count, 1u);
    CHECK(std::fabs(ts.points[0].x - 640.0f) < 0.01f && std::fabs(ts.points[0].y - 36.0f) < 0.01f);
    // Off the image (mapper says so): no touch, a moved finger stays at the edge.
    TouchPad mapped(16.0f / 9.0f);
    mapped.set_image_mapper([](float wx, float wy, float* ix, float* iy) {
        *ix = (wx - 0.25f) * 2, *iy = wy;
        return *ix >= 0 && *ix <= 1;
    });
    mapped.set_controls(false);
    mapped.finger_down(1, 0.1f, 0.5f);
    mapped.finger_down(2, 0.5f, 0.5f);
    mapped.take_touches(&ts);
    CHECK_EQ(ts.count, 1u);
    CHECK(std::fabs(ts.points[0].x - 640.0f) < 0.01f);
    mapped.finger_move(2, 0.9f, 0.5f);
    mapped.take_touches(&ts);
    CHECK(std::fabs(ts.points[0].x - 1280.0f) < 0.01f);
}

// A quick upwards swipe on free space throws; a tap or a slow drag does not.
void test_swipe() {
    constexpr uint64_t ms = 1'000'000;
    const float d = TouchPad::kSwipeDistance;
    TouchPad t(16.0f / 9.0f);  // handheld: A
    // Tap on free space: nothing on the pad.
    t.finger_down(1, 0.55f, 0.6f, 1000 * ms);
    t.finger_up(1);
    HostPad p;
    t.apply(&p, 1010 * ms);
    CHECK_EQ(p.buttons, 0u);
    CHECK(!p.throw_gesture);
    // Fast swipe up: A for kSwipePressNs.
    t.finger_down(2, 0.55f, 0.7f, 2000 * ms);
    t.finger_move(2, 0.55f, 0.7f - d / 2, 2050 * ms);
    CHECK(t.buttons() == 0u);
    t.finger_move(2, 0.56f, 0.7f - d - 0.01f, 2100 * ms);
    t.finger_move(2, 0.56f, 0.7f - 2 * d, 2150 * ms);  // counts once
    p = HostPad{};
    t.apply(&p, 2105 * ms);
    CHECK(p.connected);
    CHECK_EQ(p.buttons, kButtonA);
    p = HostPad{};
    t.apply(&p, 2150 * ms);
    CHECK_EQ(p.buttons, kButtonA);  // still held
    p = HostPad{};
    t.apply(&p, 2100 * ms + TouchPad::kSwipePressNs);
    CHECK_EQ(p.buttons, kButtonA);  // last sample of the press
    p = HostPad{};
    t.apply(&p, 2300 * ms);
    CHECK_EQ(p.buttons, 0u);
    t.finger_up(2);
    // Too slow, sideways or downwards: nothing.
    t.finger_down(3, 0.55f, 0.7f, 3000 * ms);
    t.finger_move(3, 0.55f, 0.7f - 2 * d, 3000 * ms + TouchPad::kSwipeTimeNs + ms);
    t.finger_up(3);
    t.finger_down(4, 0.5f, 0.7f, 4000 * ms);
    t.finger_move(4, 0.5f + 2 * d, 0.7f - d, 4050 * ms);
    t.finger_up(4);
    t.finger_down(5, 0.55f, 0.3f, 5000 * ms);
    t.finger_move(5, 0.55f, 0.3f + 2 * d, 5050 * ms);
    t.finger_up(5);
    p = HostPad{};
    t.apply(&p, 5100 * ms);
    CHECK_EQ(p.buttons, 0u);
    // A swipe that starts on the stick or a button is no swipe.
    t.finger_down(6, 0.2f, 0.8f, 6000 * ms);
    t.finger_move(6, 0.2f, 0.8f - 2 * d, 6050 * ms);
    p = HostPad{};
    t.apply(&p, 6060 * ms);
    CHECK_EQ(p.buttons, 0u);
    CHECK(p.left_y > 0.9f);
    t.release_all();

    // Single Joy-Con: the throw gesture, for one sample.
    TouchPad j(16.0f / 9.0f);
    j.set_swipe_action(TouchPad::SwipeAction::Throw);
    j.finger_down(1, 0.55f, 0.7f, 1000 * ms);
    j.finger_move(1, 0.55f, 0.7f - 2 * d, 1080 * ms);
    p = HostPad{};
    j.apply(&p, 1090 * ms);
    CHECK(p.throw_gesture && p.connected);
    CHECK_EQ(p.buttons, 0u);
    p = HostPad{};
    j.apply(&p, 1100 * ms);
    CHECK(!p.throw_gesture);
    // The swiping finger is still on the touch screen.
    TouchScreenState ts;
    j.take_touches(&ts);
    CHECK_EQ(ts.count, 1u);
}

// Direction and speed of a swipe go to the throw (single Joy-Con).
void test_swipe_direction() {
    constexpr uint64_t ms = 1'000'000;
    const float aspect = 16.0f / 9.0f, d = 0.3f;
    TouchPad j(aspect);
    j.set_swipe_action(TouchPad::SwipeAction::Throw);
    // dx_h: sideways in window heights; the finger API takes width fractions.
    const auto swipe = [&](uint64_t id, float dx_h, float up, uint64_t t0, uint64_t took) {
        HostPad p;
        j.finger_down(id, 0.55f, 0.8f, t0);
        j.finger_move(id, 0.55f + dx_h / aspect, 0.8f - up, t0 + took);
        j.apply(&p, t0 + took + ms);
        j.finger_up(id);
        return p;
    };
    // Straight up, 0.3 heights in 60 ms (5 heights/s): full strength.
    HostPad p = swipe(1, 0, d, 1000 * ms, 60 * ms);
    CHECK(p.throw_gesture && std::fabs(p.throw_side) < 0.01f && p.throw_strength == 2.0f && p.throw_reach == 1.0f);
    // 0.3 heights in 250 ms (1.2 heights/s): strength 1.
    p = swipe(2, 0, d, 2000 * ms, 250 * ms);
    CHECK(p.throw_gesture && std::fabs(p.throw_strength - 1.0f) < 0.01f && std::fabs(p.throw_reach + 1.0f) < 0.02f);
    // In between: 0.3 in 125 ms = 2.4 heights/s, halfway.
    p = swipe(3, 0, d, 3000 * ms, 125 * ms);
    CHECK(p.throw_gesture && std::fabs(p.throw_strength - 1.5f) < 0.02f && std::fabs(p.throw_reach) < 0.04f);
    // 17.5 degrees to the right: side 0.5; the same to the left: -0.5.
    const float tan17 = 0.31530f;
    p = swipe(4, d * tan17, d, 4000 * ms, 60 * ms);
    CHECK(p.throw_gesture && std::fabs(p.throw_side - 0.5f) < 0.02f);
    p = swipe(5, -d * tan17, d, 5000 * ms, 60 * ms);
    CHECK(p.throw_gesture && std::fabs(p.throw_side + 0.5f) < 0.02f);
    // Full side from 35 degrees on (45 here), a throw up to 70 (tan 70 = 2.75).
    p = swipe(6, d, d, 6000 * ms, 60 * ms);
    CHECK(p.throw_gesture && p.throw_side > 0.99f);
    p = swipe(10, -d * 2.6f, d, 6500 * ms, 60 * ms);
    CHECK(p.throw_gesture && p.throw_side < -0.99f);
    p = swipe(7, d * 2.9f, d, 7000 * ms, 60 * ms);
    CHECK(!p.throw_gesture);
    // Without event times: strength 1.
    j.finger_down(8, 0.55f, 0.8f);
    j.finger_move(8, 0.55f, 0.8f - d);
    p = HostPad{};
    j.apply(&p);
    CHECK(p.throw_gesture && p.throw_strength == 1.0f && p.throw_reach == 0.0f);  // standard reach
    j.finger_up(8);
    // Switched off (SwipeAction::None): nothing.
    j.set_swipe_action(TouchPad::SwipeAction::None);
    p = swipe(9, 0, d, 9000 * ms, 60 * ms);
    CHECK(!p.throw_gesture && p.buttons == 0);
}

// Safe area (notch, corners, home indicator): the controls move inside it,
// and still never overlap.
// The toggle at the bottom centre hides the controls: every finger then
// reaches the game's touch screen (stick zone and upward swipes included,
// which neither throw nor press A), and only the toggle is drawn, which
// brings the controls back.
void test_toggle() {
    TouchPad t(16.0f / 9.0f);
    const auto all = t.overlay();
    CHECK_EQ(all.size(), 11u);  // 10 buttons, toggle
    const OverlayCircle tg = all.back();
    CHECK(near_equal(tg.x, 0.5f) && tg.y > 0.9f && !tg.pressed);
    t.finger_down(1, tg.x, tg.y);
    CHECK(!t.hidden());  // switches when the finger leaves
    TouchScreenState ts;
    t.take_touches(&ts);
    CHECK_EQ(ts.count, 0u);  // the toggle's finger is not a touch
    t.finger_move(1, tg.x + 0.005f, tg.y);  // a small wobble stays on it
    t.finger_up(1);
    CHECK(t.hidden());
    const auto hidden = t.overlay();
    CHECK_EQ(hidden.size(), 1u);
    CHECK(hidden[0].pressed);
    // Stick zone and a quick upward swipe: plain touches, no stick, no A.
    t.finger_down(2, 0.2f, 0.8f, 1'000'000);
    t.finger_move(2, 0.2f, 0.5f, 50'000'000);
    HostPad p;
    t.apply(&p, 60'000'000);
    CHECK_EQ(p.buttons, 0u);
    CHECK(!p.connected && std::fabs(p.left_y) < 1e-6f);
    t.take_touches(&ts);
    CHECK_EQ(ts.count, 1u);
    CHECK(std::fabs(ts.points[0].y - 0.5f * 720.0f) < 0.01f);
    t.finger_up(2);
    // Back on: the toggle again, then the stick works as before.
    t.finger_down(3, tg.x, tg.y);
    t.finger_up(3);
    CHECK(!t.hidden());
    CHECK_EQ(t.overlay().size(), 11u);
    t.finger_down(4, 0.2f, 0.7f);
    t.finger_move(4, 0.2f + t.stick_radius() / 1.7777f, 0.7f);
    p = HostPad{};
    t.apply(&p);
    CHECK(p.left_x > 0.5f);
    // Without controls there is no toggle to hit.
    t.release_all();
    t.set_controls(false);
    CHECK(t.overlay().empty());
    t.finger_down(5, tg.x, tg.y);
    CHECK(!t.hidden());
    t.take_touches(&ts);
    CHECK_EQ(ts.count, 1u);
}

// A throw swipe that starts on the toggle throws and leaves the controls as
// they are; its finger reaches the touch screen once it moved off.
void test_toggle_swipe() {
    TouchPad t(16.0f / 9.0f);
    t.set_swipe_action(TouchPad::SwipeAction::Throw);
    const OverlayCircle tg = t.overlay().back();
    const uint64_t ms = 1'000'000;
    t.finger_down(1, tg.x, tg.y, 1000 * ms);
    t.finger_move(1, tg.x, tg.y - 0.06f, 1020 * ms);
    t.finger_move(1, tg.x, tg.y - 0.2f, 1060 * ms);
    HostPad p;
    t.apply(&p, 1070 * ms);
    CHECK(p.throw_gesture && std::fabs(p.throw_side) < 0.05f);
    TouchScreenState ts;
    t.take_touches(&ts);
    CHECK_EQ(ts.count, 1u);
    t.finger_up(1);
    CHECK(!t.hidden());
    CHECK_EQ(t.overlay().size(), 11u);
    // A touch the system cancels (edge gesture) does not switch either.
    t.finger_down(2, tg.x, tg.y);
    t.finger_up(2, true);
    CHECK(!t.hidden());
    // An edge gesture from the toggle upwards, canceled before the next
    // apply(): no throw, no tap.
    t.take_touches(&ts);
    t.finger_down(3, tg.x, tg.y, 2000 * ms);
    t.finger_move(3, tg.x, tg.y - 0.2f, 2060 * ms);
    t.finger_up(3, true);
    p = HostPad{};
    t.apply(&p, 2070 * ms);
    CHECK(!p.throw_gesture);
    t.take_touches(&ts);
    CHECK_EQ(ts.count, 0u);
    CHECK(!t.hidden());
    // Canceling a finger whose swipe apply() already took leaves the pending
    // swipe of another finger alone.
    t.finger_down(4, 0.5f, 0.6f, 3000 * ms);
    t.finger_move(4, 0.5f, 0.4f, 3060 * ms);
    p = HostPad{};
    t.apply(&p, 3070 * ms);
    CHECK(p.throw_gesture);
    t.finger_down(5, 0.6f, 0.6f, 3100 * ms);
    t.finger_move(5, 0.6f, 0.4f, 3160 * ms);
    t.finger_up(4, true);
    p = HostPad{};
    t.apply(&p, 3170 * ms);
    CHECK(p.throw_gesture);
}

void test_insets() {
    const float aspect = 19.5f / 9.0f;
    TouchPad plain(aspect), safe(aspect);
    const SafeInsets insets{0.07f, 0.07f, 0.0f, 0.05f};
    safe.set_insets(insets);
    const auto a = plain.overlay(), b = safe.overlay();
    CHECK(b[0].x < a[0].x && b[0].y < a[0].y);  // A: in from the right, up from the bottom
    CHECK(b[4].x > a[4].x);                     // L: in from the left
    CHECK(b[6].x < a[6].x);                     // R: in from the right
    for (const auto& c : b) {
        CHECK(c.x * aspect - c.half_w * c.r >= insets.left * aspect - 1e-4f);
        CHECK(c.x * aspect + c.half_w * c.r <= (1 - insets.right) * aspect + 1e-4f);
        CHECK(c.y + c.half_h * c.r <= 1 - insets.bottom + 1e-4f);
    }
    // A finger on the moved A presses it.
    HostPad p;
    safe.finger_down(1, b[0].x, b[0].y);
    safe.apply(&p);
    CHECK_EQ(p.buttons, kButtonA);
    // Top inset: Minus/Plus come down with it.
    TouchPad top(aspect);
    top.set_insets({0, 0, 0.05f, 0});
    CHECK(near_equal(top.overlay()[8].y, 0.13f));
    // A wide inset on a narrow window shrinks the controls instead of overlapping.
    for (float scale : {1.0f, 1.4f}) {
        TouchPad t(4.0f / 3.0f);
        t.set_scale(scale);
        t.set_insets({0.05f, 0.05f, 0.02f, 0.03f});
        const auto o = t.overlay();
        for (size_t i = 0; i < 10; ++i)
            for (size_t j = i + 1; j < 10; ++j)
                CHECK(!overlap(o[i], o[j], 4.0f / 3.0f));
    }
}

// Tall windows (tablets) keep the controls at the size they have at
// kComfortHeight points; phones are unchanged.
void test_window_points() {
    TouchPad phone(19.5f / 9.0f), tablet(1194.0f / 834.0f), plain(1194.0f / 834.0f);
    phone.set_window_points(402);
    tablet.set_window_points(834);
    const float phone_r = phone.overlay()[0].r, plain_r = plain.overlay()[0].r;
    CHECK(near_equal(phone_r, TouchPad(19.5f / 9.0f).overlay()[0].r));
    // A in points: what it has on a kComfortHeight-high window.
    CHECK(std::fabs(tablet.overlay()[0].r * 834 - phone_r * TouchPad::kComfortHeight) < 0.5f);
    CHECK(tablet.overlay()[0].r < 0.6f * plain_r);
    // Still no overlaps, and A is still pressed where it is drawn.
    const auto o = tablet.overlay();
    for (size_t i = 0; i < 10; ++i)
        for (size_t j = i + 1; j < 10; ++j)
            CHECK(!overlap(o[i], o[j], 1194.0f / 834.0f));
    tablet.finger_down(1, o[0].x, o[0].y);
    CHECK_EQ(tablet.buttons(), kButtonA);
}

// Buttons carry their names; overlay_shade (the shader's reference) draws a
// dark disc with a light name, pressed a light disc with a dark name, and
// nothing outside the circles.
void test_labels() {
    TouchPad t(16.0f / 9.0f);
    const auto o = t.overlay();
    const OverlayCircle::Label want[] = {OverlayCircle::kA,  OverlayCircle::kB,  OverlayCircle::kX,     OverlayCircle::kY,
                                         OverlayCircle::kL,  OverlayCircle::kZL, OverlayCircle::kR,     OverlayCircle::kZR,
                                         OverlayCircle::kMinus, OverlayCircle::kPlus};
    for (size_t i = 0; i < 10; ++i) {
        CHECK(o[i].label == want[i]);
        CHECK(!overlay_label_strokes(o[i].label).empty());
    }
    CHECK(o.back().label == OverlayCircle::kNone);  // the toggle
    // L/R and ZL/ZR one rounded rectangle, wider than high; the rest round.
    for (size_t i : {4u, 5u, 6u, 7u})
        CHECK(o[i].half_w > o[i].half_h);
    for (size_t i : {0u, 1u, 2u, 3u, 8u, 9u, 10u})
        CHECK(o[i].half_w == 1 && o[i].half_h == 1);
    // A pill reaches past r to the side but not up.
    {
        OverlayCircle pill = o[4];
        pill.x = 0.5f, pill.y = 0.5f;
        const float r = pill.r * 900;
        CHECK(overlay_shade({pill}, 800 + 1.3f * r, 450.5f, 1600, 900).a > 0.08f);
        CHECK(overlay_shade({pill}, 800.5f, 450 + 0.9f * r, 1600, 900).a == 0.0f);
    }
    CHECK(overlay_label_strokes(OverlayCircle::kNone).empty());
    // Every stroke stays inside the ring.
    for (int l = OverlayCircle::kA; l <= OverlayCircle::kMinus; ++l)
        for (const OverlayStroke& st : overlay_label_strokes(OverlayCircle::Label(l))) {
            CHECK(std::hypot(st.ax, st.ay) + st.r < 0.8f);
            CHECK(std::hypot(st.bx, st.by) < 0.8f);
        }
    const float w = 1600, h = 900;
    OverlayCircle c{0.5f, 0.5f, 0.1f, OverlayCircle::kButton, false, 1, OverlayCircle::kA};
    // The crossbar of the A (y 0.3 * 0.45 radii below the centre): light ink.
    const OverlayPixel ink = overlay_shade({c}, 800.5f, 450 + 0.135f * 90, w, h);
    CHECK(ink.a > 0.5f && ink.r > 0.8f);
    // Beside the letter: the dark disc.
    const OverlayPixel disc = overlay_shade({c}, 800.5f + 0.6f * 90, 450.5f, w, h);
    CHECK(disc.a > 0.08f && disc.a < 0.16f && disc.r < 0.05f);
    // Outside: nothing.
    CHECK(overlay_shade({c}, 800.5f + 120, 450.5f, w, h).a == 0.0f);
    c.pressed = true;
    const OverlayPixel pressed_ink = overlay_shade({c}, 800.5f, 450 + 0.135f * 90, w, h);
    CHECK(pressed_ink.a > 0.9f && pressed_ink.r < 0.3f);
    const OverlayPixel pressed_disc = overlay_shade({c}, 800.5f + 0.6f * 90, 450.5f, w, h);
    CHECK(pressed_disc.r > 0.9f);
}

}  // namespace

int main() {
    RUN(test_buttons);
    RUN(test_tap_hold);
    RUN(test_stick);
    RUN(test_settings);
    RUN(test_no_overlap);
    RUN(test_labels);
    RUN(test_window_points);
    RUN(test_touches);
    RUN(test_swipe);
    RUN(test_swipe_direction);
    RUN(test_insets);
    RUN(test_toggle);
    RUN(test_toggle_swipe);
    return finish();
}
