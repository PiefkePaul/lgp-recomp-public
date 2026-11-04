#include "lgp/platform/screen_fit.hpp"

#include <algorithm>

namespace lgp::platform {

bool parse_aspect_mode(std::string_view text, AspectMode* out) {
    if (text == "fit")
        *out = AspectMode::Fit;
    else if (text == "fill")
        *out = AspectMode::Fill;
    else if (text == "stretch")
        *out = AspectMode::Stretch;
    else
        return false;
    return true;
}

const char* aspect_mode_name(AspectMode mode) {
    switch (mode) {
    case AspectMode::Fill: return "fill";
    case AspectMode::Stretch: return "stretch";
    case AspectMode::Fit: break;
    }
    return "fit";
}

ImageRect place_image(AspectMode mode, float src_w, float src_h, float dst_w, float dst_h) {
    if (!(src_w > 0 && src_h > 0 && dst_w > 0 && dst_h > 0) || mode == AspectMode::Stretch)
        return {0, 0, std::max(dst_w, 0.0f), std::max(dst_h, 0.0f)};
    const float sx = dst_w / src_w, sy = dst_h / src_h;
    const float scale = mode == AspectMode::Fill ? std::max(sx, sy) : std::min(sx, sy);
    const float w = src_w * scale, h = src_h * scale;
    return {(dst_w - w) * 0.5f, (dst_h - h) * 0.5f, w, h};
}

bool window_to_image(const ImageRect& image, float dst_w, float dst_h, float wx, float wy, float* ix, float* iy) {
    if (!(image.width > 0 && image.height > 0)) {
        *ix = wx, *iy = wy;
        return false;
    }
    *ix = (wx * dst_w - image.x) / image.width;
    *iy = (wy * dst_h - image.y) / image.height;
    // Only the part of the image that is on the window can be touched; the
    // image's own edge counts despite rounding.
    constexpr float kEdge = 1e-4f;
    const auto inside = [](float v) { return v >= -kEdge && v <= 1 + kEdge; };
    return inside(wx) && inside(wy) && inside(*ix) && inside(*iy);
}

SafeInsets safe_insets_from_rect(float sx, float sy, float sw, float sh, float w, float h) {
    if (!(w > 0 && h > 0 && sw > 0 && sh > 0))
        return {};
    const auto edge = [](float v) { return std::clamp(v, 0.0f, 0.4f); };
    return {edge(sx / w), edge((w - sx - sw) / w), edge(sy / h), edge((h - sy - sh) / h)};
}

}  // namespace lgp::platform
