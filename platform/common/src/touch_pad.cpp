#include "lgp/platform/touch_pad.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string_view>

namespace lgp::platform {

namespace {
// Drawn sizes in window heights at scale 1 (Paul 2026-10-26: smaller, with
// labels); the touch areas stay about as large as before (kHit).
constexpr float kFace = 0.058f;     // face button radius
constexpr float kFaceGap = 0.11f;   // centre of the diamond to a face button
constexpr float kShoulder = 0.048f;
constexpr float kSmall = 0.036f;    // Minus, Plus
// Shoulder shape (L, R, ZL, ZR alike, Paul 2026-10-26) in units of
// kShoulder: half width, half height, corner.
constexpr float kShoulderShape[3] = {1.4f, 0.8f, 0.5f};
constexpr float kHit = 1.7f;        // touch radius / drawn radius
constexpr float kStickZone = 0.42f; // fingers left of this fraction of the width drive the stick
constexpr float kMinusGap = 0.10f;  // top centre to Minus/Plus at scale <= 1
constexpr float kInnerShoulder = 0.30f;  // edge to ZL/ZR
constexpr float kToggle = 0.035f;   // hide/show toggle, bottom centre

// Largest scale at which Minus/Plus keep a small gap to ZL/ZR. Both move
// with the scale, the centre does not: w/2 - (gap + small) s >= side + (inner + shoulder) s + margin,
// side = the wider of the unsafe left and right edges (window heights).
float max_size(float aspect, float side) {
    constexpr float kMargin = 0.02f;
    return (aspect / 2 - side - kMargin) / (kMinusGap + kSmall + kInnerShoulder + kShoulderShape[0] * kShoulder);
}
}  // namespace

void TouchPad::set_aspect(float aspect) {
    aspect_ = aspect > 0.5f ? aspect : 0.5f;
    layout();
}

void TouchPad::set_scale(float scale) {
    // Above ~1.43 the diamond's X reaches ZR/R (same column).
    scale_ = std::clamp(scale, 0.5f, 1.4f);
    layout();
}

void TouchPad::set_window_points(float height) {
    height_pt_ = height > 0 ? height : 0;
    layout();
}

void TouchPad::set_opacity(float opacity) { opacity_ = std::clamp(opacity, 0.0f, 1.0f); }

void TouchPad::set_mirrored(bool mirrored) {
    mirrored_ = mirrored;
    layout();
}

void TouchPad::set_insets(const SafeInsets& insets) {
    const auto edge = [](float v) { return std::clamp(v, 0.0f, 0.4f); };
    insets_ = {edge(insets.left), edge(insets.right), edge(insets.top), edge(insets.bottom)};
    layout();
}

void TouchPad::layout() {
    // Distances from the edges and sizes grow with the scale, so larger
    // controls do not run off the screen.
    // Unsafe edges in window heights: the controls start where they end.
    const float w = aspect_;
    const float left = insets_.left * w, right = insets_.right * w, top = insets_.top, bottom = insets_.bottom;
    // Tall windows (tablets) keep the size the controls have at
    // kComfortHeight, so they stay easy to reach with the thumbs.
    const float fit = height_pt_ > kComfortHeight ? kComfortHeight / height_pt_ : 1.0f;
    size_ = std::max(0.1f, std::min(scale_ * fit, max_size(aspect_, std::max(left, right))));
    const float s = size_;
    const float cx = mirrored_ ? left + 0.30f * s : w - right - 0.30f * s, cy = 1 - bottom - 0.34f * s;
    const float gap = kFaceGap * s, face = kFace * s;
    buttons_ = {
        {kButtonA, cx + gap, cy, face},
        {kButtonB, cx, cy + gap, face},
        {kButtonX, cx, cy - gap, face},
        {kButtonY, cx - gap, cy, face},
        {kButtonL, left + 0.12f * s, top + 0.10f * s, kShoulder * s},
        {kButtonZL, left + kInnerShoulder * s, top + 0.10f * s, kShoulder * s},
        {kButtonR, w - right - 0.12f * s, top + 0.10f * s, kShoulder * s},
        {kButtonZR, w - right - kInnerShoulder * s, top + 0.10f * s, kShoulder * s},
        // Minus/Plus keep their height and never come closer to the top
        // centre, where the app menu's pause button sits (below the top
        // inset, like it).
        {kButtonMinus, w / 2 - kMinusGap * std::max(1.0f, s), top + 0.08f, kSmall * s},
        {kButtonPlus, w / 2 + kMinusGap * std::max(1.0f, s), top + 0.08f, kSmall * s},
    };
    // Its size does not follow the scale: it must stay small next to the
    // game's own touch targets and reachable with the controls hidden. Only
    // tall windows shrink it like the controls.
    toggle_ = {0, w / 2, 1 - bottom - 0.045f * fit, kToggle * fit};
}

bool TouchPad::on_toggle(float x, float y) const {
    return std::hypot(x - toggle_.x, y - toggle_.y) <= toggle_.r * 1.2f;
}

bool TouchPad::in_stick_zone(float x) const { return mirrored_ ? x > 1 - kStickZone : x < kStickZone; }

const TouchPad::Button* TouchPad::button_at(float x, float y) const {
    const Button* best = nullptr;
    float best_d = 0;
    for (const Button& b : buttons_) {
        const float d = std::hypot(x - b.x, y - b.y);
        if (d <= b.r * kHit && (best == nullptr || d < best_d)) {
            best = &b;
            best_d = d;
        }
    }
    return best;
}

TouchPad::Finger* TouchPad::find(uint64_t id) {
    for (Finger& f : fingers_)
        if (f.id == id)
            return &f;
    return nullptr;
}

namespace {
constexpr uint32_t kNoTouch = ~0u;  // a free finger that landed off the game image
}  // namespace

void TouchPad::set_controls(bool on) {
    if (on == controls_)
        return;
    controls_ = on;
    // Fingers keep the kind they landed with; without controls none may
    // hold a button or the stick.
    fingers_.erase(std::remove_if(fingers_.begin(), fingers_.end(), [](const Finger& f) { return f.kind != Kind::Free; }),
                   fingers_.end());
    latched_ = tap_bits_ = 0;
    swipe_pending_ = press_a_ = false;
}

void TouchPad::set_hidden(bool hidden) {
    if (hidden == hidden_)
        return;
    hidden_ = hidden;
    fingers_.erase(std::remove_if(fingers_.begin(), fingers_.end(),
                                  [](const Finger& f) { return f.kind == Kind::Stick || f.kind == Kind::Button; }),
                   fingers_.end());
    latched_ = tap_bits_ = 0;
    swipe_pending_ = press_a_ = false;
}

bool TouchPad::to_game(float x, float y, TouchPoint* out) const {
    if (image_mapper_) {
        float u = 0, v = 0;
        const bool inside = image_mapper_(x / aspect_, y, &u, &v);
        // The widescreen mod's touch diagnosis LGP_WIDESCREEN_TOUCH=wide passes
        // x beyond the 1280 surface (hid clamps it unless told otherwise).
        static const float max_u = [] {
            const char* env = std::getenv("LGP_WIDESCREEN_TOUCH");
            return env != nullptr && std::string_view(env) == "wide" ? 2.0f : 1.0f;
        }();
        out->x = std::clamp(u, 0.0f, max_u) * kTouchScreenWidth;
        out->y = std::clamp(v, 0.0f, 1.0f) * kTouchScreenHeight;
        return inside;
    }
    // The renderer fits the 16:9 image into the window: bars left and right
    // on wider windows, above and below on narrower ones (4:3 iPad).
    float gx = 0, gy = 0, gw = aspect_, gh = 1;
    if (aspect_ > kGameAspect) {
        gw = kGameAspect;
        gx = (aspect_ - gw) / 2;
    } else {
        gh = aspect_ / kGameAspect;
        gy = (1 - gh) / 2;
    }
    const float u = (x - gx) / gw, v = (y - gy) / gh;
    out->x = std::clamp(u, 0.0f, 1.0f) * kTouchScreenWidth;
    out->y = std::clamp(v, 0.0f, 1.0f) * kTouchScreenHeight;
    return u >= 0 && u <= 1 && v >= 0 && v <= 1;
}

uint32_t TouchPad::free_touch_id() const {
    for (uint32_t id = 0; id < kMaxTouches; ++id) {
        const bool used = std::any_of(fingers_.begin(), fingers_.end(),
                                      [id](const Finger& f) { return f.kind == Kind::Free && f.touch_id == id; }) ||
                          std::any_of(taps_.begin(), taps_.end(), [id](const TouchPoint& p) { return p.id == id; });
        if (!used)
            return id;
    }
    return kNoTouch;
}

void TouchPad::finger_down(uint64_t id, float x, float y, uint64_t time_ns) {
    finger_up(id);
    const float hx = x * aspect_, hy = y;
    // A button wins over the stick zone (L and ZL sit inside it).
    Kind kind = Kind::Free;
    if (controls_ && on_toggle(hx, hy)) {
        // Toggles when it leaves without moving (finger_up); a finger that
        // moves on (a throw swipe starting low in the middle) turns free.
        fingers_.push_back({id, Kind::Toggle, hx, hy, hx, hy, time_ns});
        return;
    }
    if (active()) {
        if (button_at(hx, hy) != nullptr)
            kind = Kind::Button;
        else if (in_stick_zone(x) && hy > insets_.top + 0.2f * size_ &&
                 std::none_of(fingers_.begin(), fingers_.end(), [](const Finger& f) { return f.kind == Kind::Stick; }))
            kind = Kind::Stick;
    }
    Finger f{id, kind, hx, hy, hx, hy, time_ns};
    if (kind == Kind::Free) {
        TouchPoint p;
        f.touch_id = to_game(hx, hy, &p) ? free_touch_id() : kNoTouch;
    }
    fingers_.push_back(f);
    latched_ |= buttons();
    if (kind == Kind::Button && time_ns != 0) {
        tap_bits_ |= button_at(hx, hy)->bit;
        tap_until_ns_ = std::max(tap_until_ns_, time_ns + kTapPressNs);
    }
}

void TouchPad::finger_move(uint64_t id, float x, float y, uint64_t time_ns) {
    Finger* f = find(id);
    if (f == nullptr)
        return;
    f->x = x * aspect_;
    f->y = y;
    latched_ |= buttons();
    if (f->kind == Kind::Toggle && std::hypot(f->x - f->x0, f->y - f->y0) > toggle_.r) {
        f->kind = Kind::Free;  // from its landing point, for the swipe too
        f->touch_id = kNoTouch;
        TouchPoint p;
        f->touch_id = to_game(f->x0, f->y0, &p) ? free_touch_id() : kNoTouch;
    }
    // Hidden controls never swipe: "Play with Pikachu" takes only touches
    // there, placed exactly (Paul 2026-10-22).
    if (f->kind != Kind::Free || f->swiped || !active() || swipe_action_ == SwipeAction::None)
        return;
    constexpr float kDegrees = 180.0f / 3.14159265f;
    const float up = f->y0 - f->y, side = f->x - f->x0;
    const uint64_t elapsed = time_ns >= f->down_ns ? time_ns - f->down_ns : 0;
    const float angle = std::atan2(side, up) * kDegrees;  // 0 straight up, positive to the right
    if (up >= kSwipeDistance && std::fabs(angle) <= kSwipeMaxAngle && elapsed <= kSwipeTimeNs) {
        f->swiped = true;
        swipe_pending_ = true;
        swipe_finger_ = id;
        swipe_ns_ = time_ns;
        swipe_side_ = std::clamp(angle / kSwipeFullSideAngle, -1.0f, 1.0f);
        swipe_strength_ = 1.0f;
        swipe_reach_ = 0.0f;
        if (time_ns != 0 && f->down_ns != 0 && elapsed > 0) {
            const float speed = std::hypot(up, side) / (static_cast<float>(elapsed) * 1e-9f);
            const float t = std::clamp((speed - kSwipeBaseSpeed) / (kSwipeFullSpeed - kSwipeBaseSpeed), 0.0f, 1.0f);
            swipe_strength_ += t;
            swipe_reach_ = 2.0f * t - 1.0f;
        }
    }
}

void TouchPad::finger_up(uint64_t id, bool canceled) {
    Finger* f = find(id);
    const bool toggle = f != nullptr && f->kind == Kind::Toggle;
    // A canceled finger (the system's edge gesture) leaves no tap and takes
    // its swipe back if apply() has not seen it yet.
    if (canceled && swipe_pending_ && swipe_finger_ == id)
        swipe_pending_ = false;
    if (!canceled && f != nullptr && f->kind == Kind::Free && f->touch_id != kNoTouch && !f->reported) {
        TouchPoint p;
        to_game(f->x, f->y, &p);
        p.id = f->touch_id;
        taps_.push_back(p);
    }
    fingers_.erase(std::remove_if(fingers_.begin(), fingers_.end(), [id](const Finger& f) { return f.id == id; }),
                   fingers_.end());
    if (toggle && controls_ && !canceled)
        set_hidden(!hidden_);
}

void TouchPad::release_all() {
    fingers_.clear();
    taps_.clear();
    latched_ = tap_bits_ = 0;
    swipe_pending_ = press_a_ = false;
}

uint64_t TouchPad::buttons() const {
    uint64_t bits = 0;
    for (const Finger& f : fingers_)
        if (f.kind == Kind::Button)
            if (const Button* b = button_at(f.x, f.y))
                bits |= b->bit;
    return bits;
}

void TouchPad::apply(HostPad* pad, uint64_t now_ns) {
    const uint64_t latched = latched_;
    latched_ = 0;
    if (swipe_pending_) {
        swipe_pending_ = false;
        if (swipe_action_ == SwipeAction::Throw) {
            pad->connected = true;
            pad->throw_gesture = true;
            pad->throw_side = swipe_side_;
            pad->throw_strength = swipe_strength_;
            pad->throw_reach = swipe_reach_;
        } else if (swipe_action_ == SwipeAction::PressA) {
            press_a_ = true;
            press_until_ns_ = swipe_ns_ + kSwipePressNs;
        }
    }
    if (press_a_) {
        pad->connected = true;
        pad->buttons |= kButtonA;
        if (now_ns == 0 || now_ns >= press_until_ns_)
            press_a_ = false;
    }
    uint64_t tapped = 0;
    if (tap_bits_ != 0) {
        if (now_ns != 0 && now_ns < tap_until_ns_)
            tapped = tap_bits_;
        else
            tap_bits_ = 0;
    }
    if (!active() || (fingers_.empty() && latched == 0 && tapped == 0))
        return;
    pad->connected = true;
    pad->buttons |= buttons() | latched | tapped;
    for (const Finger& f : fingers_) {
        if (f.kind != Kind::Stick)
            continue;
        float dx = (f.x - f.x0) / stick_radius(), dy = (f.y - f.y0) / stick_radius();
        const float len = std::hypot(dx, dy);
        if (len > 1.0f)
            dx /= len, dy /= len;
        pad->left_x = dx;
        pad->left_y = -dy;  // HostPad: up positive
    }
}

void TouchPad::take_touches(TouchScreenState* out) {
    out->count = 0;
    for (Finger& f : fingers_) {
        if (f.kind != Kind::Free || f.touch_id == kNoTouch || out->count >= kMaxTouches)
            continue;
        TouchPoint& p = out->points[out->count++];
        to_game(f.x, f.y, &p);  // clamped to the image's edge once it left it
        p.id = f.touch_id;
        f.reported = true;
    }
    for (const TouchPoint& p : taps_)
        if (out->count < kMaxTouches)
            out->points[out->count++] = p;
    taps_.clear();
}

std::vector<OverlayCircle> TouchPad::overlay() const {
    std::vector<OverlayCircle> out;
    // The toggle comes last; with the controls hidden it is all there is.
    const OverlayCircle toggle{toggle_.x / aspect_, toggle_.y, toggle_.r, OverlayCircle::kButton, hidden_, opacity_};
    if (!controls_)
        return out;
    if (hidden_)
        return {toggle};
    const auto label = [](uint64_t bit) {
        switch (bit) {
        case kButtonA: return OverlayCircle::kA;
        case kButtonB: return OverlayCircle::kB;
        case kButtonX: return OverlayCircle::kX;
        case kButtonY: return OverlayCircle::kY;
        case kButtonL: return OverlayCircle::kL;
        case kButtonR: return OverlayCircle::kR;
        case kButtonZL: return OverlayCircle::kZL;
        case kButtonZR: return OverlayCircle::kZR;
        case kButtonPlus: return OverlayCircle::kPlus;
        case kButtonMinus: return OverlayCircle::kMinus;
        default: return OverlayCircle::kNone;
        }
    };
    const uint64_t pressed = buttons();
    for (const Button& b : buttons_) {
        OverlayCircle c{b.x / aspect_, b.y, b.r, OverlayCircle::kButton, (pressed & b.bit) != 0, opacity_, label(b.bit)};
        if (b.bit == kButtonL || b.bit == kButtonR || b.bit == kButtonZL || b.bit == kButtonZR)
            c.half_w = kShoulderShape[0], c.half_h = kShoulderShape[1], c.corner = kShoulderShape[2];
        out.push_back(c);
    }
    // The floating stick is not drawn (Paul 2026-10-26): a thumb anywhere in
    // the stick zone works as before.
    out.push_back(toggle);
    return out;
}

}  // namespace lgp::platform
