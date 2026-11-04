// Shapes a renderer draws over the game image (P7.3: the on-screen controller
// of touch screens, lgp/platform/touch_pad.hpp).
#pragma once

#include <cstdint>
#include <vector>

namespace lgp::platform {

// One control in surface coordinates: x and y from 0 to 1 (left/top to
// right/bottom), r as a fraction of the surface height. A circle by default;
// half_w, half_h and corner (in units of r) make it a rounded rectangle
// (the shoulder buttons).
struct OverlayCircle {
    enum Kind : uint8_t { kButton = 0, kStickBase = 1, kStickKnob = 2 };
    // Drawn inside a button: its name on the controller.
    enum Label : uint8_t { kNone = 0, kA, kB, kX, kY, kL, kR, kZL, kZR, kPlus, kMinus };
    float x = 0, y = 0, r = 0;
    Kind kind = kButton;
    bool pressed = false;
    float alpha = 1;  // opacity factor on ring and fill (0..1)
    Label label = kNone;
    float half_w = 1, half_h = 1, corner = 1;
};

// A label's strokes, in units of its circle's radius around the centre (x
// right, y down): a line from a to b, or (arc) the right half of a circle
// around a with radius r, open to the left. Drawn with kOverlayStrokeWidth.
struct OverlayStroke {
    float ax = 0, ay = 0, bx = 0, by = 0;
    bool arc = false;
    float r = 0;
};
inline constexpr float kOverlayStrokeWidth = 0.11f;  // in radii, the whole width

std::vector<OverlayStroke> overlay_label_strokes(OverlayCircle::Label label);

// What a renderer draws at a pixel (x, y from the top left, pixel centres at
// .5) of a width x height surface: straight-alpha colour, rgb 0..1. The
// reference for the WebGPU overlay shader (webgpu_renderer.cpp, same steps)
// and for pictures without a GPU (lgp_touch_preview).
struct OverlayPixel {
    float r = 0, g = 0, b = 0, a = 0;
};
OverlayPixel overlay_shade(const std::vector<OverlayCircle>& circles, float x, float y, float width, float height);

}  // namespace lgp::platform
