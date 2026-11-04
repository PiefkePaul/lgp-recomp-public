// hid gestures from touch points, see hid_gesture.hpp.
#include "hid_gesture.hpp"

#include <algorithm>
#include <cmath>

namespace lgp::hle {

namespace {

uint32_t pixel(float v, float size) {
    return static_cast<uint32_t>(std::clamp(std::lround(v), 0L, static_cast<long>(size) - 1));
}

}  // namespace

void GestureTracker::velocity(uint64_t now_ns, float* vx, float* vy) const {
    *vx = *vy = 0;
    if (history_len_ < 2)
        return;
    // Newest sample, and the oldest one still inside the window.
    const Sample& newest = history_[(history_pos_ + history_.size() - 1) % history_.size()];
    const Sample* oldest = &newest;
    for (size_t i = 1; i < history_len_; ++i) {
        const Sample& s = history_[(history_pos_ + history_.size() - 1 - i) % history_.size()];
        if (now_ns - s.ns > gesture::kVelocityWindowNs)
            break;
        oldest = &s;
    }
    if (oldest == &newest || newest.ns <= oldest->ns)
        return;
    const float dt = static_cast<float>(newest.ns - oldest->ns) * 1e-9f;
    *vx = (newest.x - oldest->x) / dt;
    *vy = (newest.y - oldest->y) / dt;
}

bool GestureTracker::update(const platform::TouchScreenState& touch, uint64_t now_ns, hidmem::GestureState* out) {
    const uint32_t count = std::min<uint32_t>(touch.count, platform::kMaxTouches);
    if (count == 0 && !active_)
        return false;
    *out = hidmem::GestureState{};
    out->context_number = context_;
    out->scale = 1.0f;

    if (count == 0) {
        // The last finger left: end the context where it was.
        active_ = false;
        float vx = 0, vy = 0;
        velocity(now_ns, &vx, &vy);
        const uint64_t held = now_ns - start_ns_;
        out->x = pixel(last_x_, platform::kTouchScreenWidth);
        out->y = pixel(last_y_, platform::kTouchScreenHeight);
        out->point_count = last_count_;
        std::copy(std::begin(last_points_), std::end(last_points_), std::begin(out->points));
        if (!moved_ && held <= gesture::kTapMaxNs) {
            out->type = gesture::kTap;
            if (last_tap_ns_ != 0 && now_ns - last_tap_ns_ <= gesture::kDoubleTapNs &&
                std::hypot(last_x_ - last_tap_x_, last_y_ - last_tap_y_) <= gesture::kDoubleTapPx) {
                out->attributes |= gesture::kAttrDoubleTap;
                last_tap_ns_ = 0;  // a third tap starts over
            } else {
                last_tap_ns_ = now_ns;
                last_tap_x_ = last_x_;
                last_tap_y_ = last_y_;
            }
        } else if (moved_ && std::hypot(vx, vy) >= gesture::kSwipeSpeed) {
            out->type = gesture::kSwipe;
            out->direction = std::fabs(vx) > std::fabs(vy) ? (vx < 0 ? gesture::kDirLeft : gesture::kDirRight)
                                                           : (vy < 0 ? gesture::kDirUp : gesture::kDirDown);
            out->velocity_x = vx;
            out->velocity_y = vy;
        } else {
            out->type = gesture::kComplete;
        }
        history_len_ = 0;
        return true;
    }

    float cx = 0, cy = 0;
    for (uint32_t i = 0; i < count; ++i) {
        cx += touch.points[i].x;
        cy += touch.points[i].y;
    }
    cx /= static_cast<float>(count);
    cy /= static_cast<float>(count);
    if (!active_) {
        active_ = true;
        moved_ = false;
        out->context_number = ++context_;
        out->attributes |= gesture::kAttrNewTouch;
        start_ns_ = now_ns;
        start_x_ = last_x_ = cx;
        start_y_ = last_y_ = cy;
        history_len_ = 0;
    }
    history_[history_pos_] = {now_ns, cx, cy};
    history_pos_ = (history_pos_ + 1) % history_.size();
    history_len_ = std::min(history_len_ + 1, history_.size());
    if (std::hypot(cx - start_x_, cy - start_y_) > gesture::kPanPx)
        moved_ = true;
    float vx = 0, vy = 0;
    velocity(now_ns, &vx, &vy);
    out->type = moved_ ? gesture::kPan : now_ns - start_ns_ >= gesture::kPressNs ? gesture::kPress : gesture::kTouch;
    out->x = pixel(cx, platform::kTouchScreenWidth);
    out->y = pixel(cy, platform::kTouchScreenHeight);
    out->delta_x = static_cast<int32_t>(std::lround(cx - last_x_));
    out->delta_y = static_cast<int32_t>(std::lround(cy - last_y_));
    out->velocity_x = vx;
    out->velocity_y = vy;
    out->point_count = static_cast<int32_t>(std::min<uint32_t>(count, 4));
    for (int32_t i = 0; i < out->point_count; ++i)
        out->points[i] = {pixel(touch.points[i].x, platform::kTouchScreenWidth),
                          pixel(touch.points[i].y, platform::kTouchScreenHeight)};
    last_x_ = cx;
    last_y_ = cy;
    last_count_ = out->point_count;
    std::copy(std::begin(out->points), std::end(out->points), std::begin(last_points_));
    return true;
}

}  // namespace lgp::hle
