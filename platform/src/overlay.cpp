#include "lgp/platform/overlay.hpp"

#include <algorithm>
#include <cmath>

namespace lgp::platform {

namespace {

OverlayStroke line(float ax, float ay, float bx, float by) { return {ax, ay, bx, by, false, 0}; }
OverlayStroke arc(float cx, float cy, float r) { return {cx, cy, 0, 0, true, r}; }

// Strokes of a letter in a box from -1 to 1 (y down).
std::vector<OverlayStroke> letter(char c) {
    switch (c) {
    case 'A': return {line(-0.75f, 1, 0, -1), line(0, -1, 0.75f, 1), line(-0.45f, 0.3f, 0.45f, 0.3f)};
    case 'B':
        return {line(-0.6f, -1, -0.6f, 1), line(-0.6f, -1, 0.05f, -1), arc(0.05f, -0.5f, 0.5f), line(0.05f, 0, -0.6f, 0),
                line(-0.6f, 0, 0.1f, 0), arc(0.1f, 0.5f, 0.5f), line(0.1f, 1, -0.6f, 1)};
    case 'X': return {line(-0.7f, -1, 0.7f, 1), line(0.7f, -1, -0.7f, 1)};
    case 'Y': return {line(-0.7f, -1, 0, 0), line(0.7f, -1, 0, 0), line(0, 0, 0, 1)};
    case 'L': return {line(-0.5f, -1, -0.5f, 1), line(-0.5f, 1, 0.55f, 1)};
    case 'R':
        return {line(-0.6f, -1, -0.6f, 1), line(-0.6f, -1, 0.05f, -1), arc(0.05f, -0.475f, 0.525f),
                line(0.05f, 0.05f, -0.6f, 0.05f), line(0.0f, 0.05f, 0.65f, 1)};
    case 'Z': return {line(-0.6f, -1, 0.6f, -1), line(0.6f, -1, -0.6f, 1), line(-0.6f, 1, 0.6f, 1)};
    case '+': return {line(-0.8f, 0, 0.8f, 0), line(0, -0.8f, 0, 0.8f)};
    case '-': return {line(-0.8f, 0, 0.8f, 0)};
    default: return {};
    }
}

// Appends a letter scaled by `scale` radii per box unit, its centre at dx.
void place(std::vector<OverlayStroke>* out, char c, float scale, float dx) {
    for (OverlayStroke s : letter(c)) {
        s.ax = s.ax * scale + dx, s.ay *= scale;
        s.bx = s.bx * scale + dx, s.by *= scale;
        s.r *= scale;
        out->push_back(s);
    }
}

float segment_distance(float px, float py, const OverlayStroke& s) {
    if (s.arc) {
        if (px >= s.ax)
            return std::fabs(std::hypot(px - s.ax, py - s.ay) - s.r);
        return std::min(std::hypot(px - s.ax, py - (s.ay - s.r)), std::hypot(px - s.ax, py - (s.ay + s.r)));
    }
    const float vx = s.bx - s.ax, vy = s.by - s.ay;
    const float len2 = vx * vx + vy * vy;
    const float t = len2 > 0 ? std::clamp(((px - s.ax) * vx + (py - s.ay) * vy) / len2, 0.0f, 1.0f) : 0.0f;
    return std::hypot(px - (s.ax + t * vx), py - (s.ay + t * vy));
}

// Signed distance (pixels) from (px, py) to the edge of the control at
// (cx, cy): negative inside.
float shape_distance(const OverlayCircle& c, float px, float py, float cx, float cy, float r) {
    const float hw = c.half_w * r, hh = c.half_h * r, k = std::min({c.corner * r, hw, hh});
    const float qx = std::fabs(px - cx) - (hw - k), qy = std::fabs(py - cy) - (hh - k);
    return std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f)) + std::min(std::max(qx, qy), 0.0f) - k;
}

float smoothstep(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}

// Straight-alpha "over".
void over(OverlayPixel* dst, float r, float g, float b, float a) {
    const float out_a = a + dst->a * (1 - a);
    if (out_a <= 0) {
        *dst = {};
        return;
    }
    dst->r = (r * a + dst->r * dst->a * (1 - a)) / out_a;
    dst->g = (g * a + dst->g * dst->a * (1 - a)) / out_a;
    dst->b = (b * a + dst->b * dst->a * (1 - a)) / out_a;
    dst->a = out_a;
}

}  // namespace

std::vector<OverlayStroke> overlay_label_strokes(OverlayCircle::Label label) {
    // Single letters 0.9 radii high, pairs smaller side by side.
    constexpr float kOne = 0.45f, kTwo = 0.3f;
    std::vector<OverlayStroke> out;
    switch (label) {
    case OverlayCircle::kA: place(&out, 'A', kOne, 0); break;
    case OverlayCircle::kB: place(&out, 'B', kOne, 0); break;
    case OverlayCircle::kX: place(&out, 'X', kOne, 0); break;
    case OverlayCircle::kY: place(&out, 'Y', kOne, 0); break;
    case OverlayCircle::kL: place(&out, 'L', kOne, 0); break;
    case OverlayCircle::kR: place(&out, 'R', kOne, 0); break;
    case OverlayCircle::kZL: place(&out, 'Z', kTwo, -0.27f), place(&out, 'L', kTwo, 0.3f); break;
    case OverlayCircle::kZR: place(&out, 'Z', kTwo, -0.29f), place(&out, 'R', kTwo, 0.29f); break;
    case OverlayCircle::kPlus: place(&out, '+', kOne, 0); break;
    case OverlayCircle::kMinus: place(&out, '-', kOne, 0); break;
    case OverlayCircle::kNone: break;
    }
    return out;
}

OverlayPixel overlay_shade(const std::vector<OverlayCircle>& circles, float x, float y, float width, float height) {
    // Keep in step with kOverlayWgsl (webgpu_renderer.cpp).
    for (const OverlayCircle& c : circles) {
        const float r = c.r * height;
        const float sd = shape_distance(c, x, y, c.x * width, c.y * height, r);
        if (sd > 1)
            continue;
        const float edge = 1 - smoothstep(-1, 1, sd);
        const float rw = std::max(2.0f, 0.07f * r);
        const float ring = edge * smoothstep(-rw - 1, -rw, sd);
        OverlayPixel px;
        if (c.kind == OverlayCircle::kButton) {
            // Dark disc, faint ring and label (Paul 2026-10-26: unobtrusive but
            // readable); pressed: light disc, dark label.
            const float fill = c.pressed ? 0.6f : 0.12f, fv = c.pressed ? 1.0f : 0.0f;
            over(&px, fv, fv, fv, edge * fill * c.alpha);
            over(&px, 1, 1, 1, ring * 0.5f * c.alpha);
            float dist = 1e9f;
            for (const OverlayStroke& s : overlay_label_strokes(c.label))
                dist = std::min(dist, segment_distance((x - c.x * width) / r, (y - c.y * height) / r, s));
            const float half = 0.5f * kOverlayStrokeWidth, aa = 1.0f / std::max(r, 1.0f);
            const float ink = 1 - smoothstep(half - aa, half + aa, dist);
            const float lv = c.pressed ? 0.0f : 1.0f;
            over(&px, lv, lv, lv, ink * (c.pressed ? 0.8f : 0.6f) * c.alpha);
        } else {
            // Stick shapes (the controller draws none since 2026-10-26).
            const float fill = c.kind == OverlayCircle::kStickBase ? (c.pressed ? 0.12f : 0.05f) : (c.pressed ? 0.5f : 0.3f);
            const float line_a = c.kind == OverlayCircle::kStickBase ? 0.35f : 0.0f;
            over(&px, 1, 1, 1, std::max(ring * line_a, edge * fill) * c.alpha);
        }
        return px;
    }
    return {};
}

}  // namespace lgp::platform
