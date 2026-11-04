// Where the game image lands on the screen (P7, "Bildformat"): the 16:9
// image on a 19.5:9 iPhone or a 4:3 iPad either keeps its aspect ratio with
// black bars (fit, the default), fills the screen and loses its edges
// (fill), or is stretched to it (stretch). Pure arithmetic, no device code:
// the WebGPU renderer places its blit with place_image(), and touch input
// for the game's own touch screen maps window points back with
// window_to_image().
//
// The app's own layer (menus, the on-screen controller) is not placed this
// way: it covers the whole window and keeps clear of the safe area
// (SafeInsets) instead; the game image may reach under notch and corners.
#pragma once

#include <string_view>

namespace lgp::platform {

enum class AspectMode {
    Fit,      // whole image, aspect ratio kept, bars where the screen is wider or taller
    Fill,     // aspect ratio kept, covers the screen, the overhang is cut off
    Stretch,  // covers the screen, aspect ratio of the screen
};

// "fit", "fill", "stretch"; false for anything else (*out unchanged).
bool parse_aspect_mode(std::string_view text, AspectMode* out);
const char* aspect_mode_name(AspectMode mode);

// A rectangle on the destination, in its units (pixels or points). With Fill
// it reaches beyond the destination (negative x or y, larger width/height).
struct ImageRect {
    float x = 0, y = 0, width = 0, height = 0;
};

// The image (src_w x src_h) placed on a destination of dst_w x dst_h,
// centred. Empty source or destination: the whole destination.
ImageRect place_image(AspectMode mode, float src_w, float src_h, float dst_w, float dst_h);

// A point on the window in fractions of its size (0..1, x right, y down; SDL
// finger coordinates) to the same point on the image, in fractions of the
// image (0..1: multiply by 1280x720 for the game's touch screen). `image`
// is place_image() for a destination of dst_w x dst_h with the window's
// aspect ratio (the surface or the window, either). False if the point lies
// outside the image (on a bar, or on the part Fill cut off); *ix, *iy are
// still set, unclamped.
bool window_to_image(const ImageRect& image, float dst_w, float dst_h, float wx, float wy, float* ix, float* iy);

// Edges of the window the app's own controls keep clear of (notch, Dynamic
// Island, rounded corners, home indicator; SDL_GetWindowSafeArea), as
// fractions of the window: left/right of its width, top/bottom of its height.
struct SafeInsets {
    float left = 0, right = 0, top = 0, bottom = 0;
};

// From the safe rectangle (sx, sy, sw, sh) of a window of w x h, both in the
// same units; clamped to 0..0.4 per edge.
SafeInsets safe_insets_from_rect(float sx, float sy, float sw, float sh, float w, float h);

}  // namespace lgp::platform
