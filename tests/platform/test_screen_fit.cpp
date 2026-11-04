// P7 "Bildformat": where the 16:9 game image lands on screens of other
// shapes, and window points mapped back onto it (lgp/platform/screen_fit.hpp).
#include <cmath>
#include <cstdio>

#include "../runtime/check.hpp"
#include "lgp/platform/screen_fit.hpp"

using namespace lgp::platform;

namespace {

bool near(float a, float b) { return std::fabs(a - b) < 1e-3f; }

void test_names() {
    AspectMode m = AspectMode::Fit;
    CHECK(parse_aspect_mode("fill", &m) && m == AspectMode::Fill);
    CHECK(parse_aspect_mode("stretch", &m) && m == AspectMode::Stretch);
    CHECK(parse_aspect_mode("fit", &m) && m == AspectMode::Fit);
    CHECK(!parse_aspect_mode("zoom", &m) && m == AspectMode::Fit);
    CHECK(!parse_aspect_mode("", &m));
    for (AspectMode mode : {AspectMode::Fit, AspectMode::Fill, AspectMode::Stretch}) {
        AspectMode back = AspectMode::Fit;
        CHECK(parse_aspect_mode(aspect_mode_name(mode), &back) && back == mode);
    }
}

void test_place() {
    // iPhone 15 in landscape: 2556x1179 px (19.5:9).
    ImageRect r = place_image(AspectMode::Fit, 1280, 720, 2556, 1179);
    CHECK(near(r.height, 1179) && near(r.width, 1179 * 16.0f / 9.0f) && near(r.y, 0));
    CHECK(r.x > 0 && near(r.x * 2 + r.width, 2556));  // bars left and right
    r = place_image(AspectMode::Fill, 1280, 720, 2556, 1179);
    CHECK(near(r.width, 2556) && near(r.height, 2556 * 9.0f / 16.0f) && r.y < 0 && near(r.x, 0));
    r = place_image(AspectMode::Stretch, 1280, 720, 2556, 1179);
    CHECK(near(r.x, 0) && near(r.y, 0) && near(r.width, 2556) && near(r.height, 1179));
    // iPad (4:3): 2732x2048 px.
    r = place_image(AspectMode::Fit, 1280, 720, 2732, 2048);
    CHECK(near(r.width, 2732) && r.y > 0 && near(r.y * 2 + r.height, 2048));  // bars top and bottom
    r = place_image(AspectMode::Fill, 1280, 720, 2732, 2048);
    CHECK(near(r.height, 2048) && r.x < 0 && near(r.x * 2 + r.width, 2732));
    // 16:9 screen: every mode is the whole screen.
    for (AspectMode m : {AspectMode::Fit, AspectMode::Fill, AspectMode::Stretch}) {
        r = place_image(m, 1280, 720, 1920, 1080);
        CHECK(near(r.x, 0) && near(r.y, 0) && near(r.width, 1920) && near(r.height, 1080));
    }
    // Nothing to place: the whole destination.
    r = place_image(AspectMode::Fit, 0, 0, 800, 600);
    CHECK(near(r.width, 800) && near(r.height, 600));
}

void test_window_to_image() {
    float ix = 0, iy = 0;
    // Pillarbox on a 2:1 window of 2000x1000: image 1777.8 wide from x = 111.1.
    ImageRect r = place_image(AspectMode::Fit, 1280, 720, 2000, 1000);
    CHECK(window_to_image(r, 2000, 1000, 0.5f, 0.5f, &ix, &iy) && near(ix, 0.5f) && near(iy, 0.5f));
    CHECK(!window_to_image(r, 2000, 1000, 0.02f, 0.5f, &ix, &iy) && ix < 0);  // on the left bar
    CHECK(window_to_image(r, 2000, 1000, r.x / 2000, 0, &ix, &iy) && near(ix, 0) && near(iy, 0));
    CHECK(window_to_image(r, 2000, 1000, (r.x + r.width) / 2000, 1, &ix, &iy) && near(ix, 1) && near(iy, 1));
    // The same window by fractions of a smaller surface (tvOS caps the
    // swapchain height): same answer.
    ImageRect half = place_image(AspectMode::Fit, 1280, 720, 1000, 500);
    float jx = 0, jy = 0;
    CHECK(window_to_image(half, 1000, 500, 0.3f, 0.7f, &jx, &jy));
    CHECK(window_to_image(r, 2000, 1000, 0.3f, 0.7f, &ix, &iy) && near(ix, jx) && near(iy, jy));
    // Fill on 4:3: the window's left edge is inside the image, the cut edge
    // cannot be reached.
    r = place_image(AspectMode::Fill, 1280, 720, 1024, 768);
    CHECK(window_to_image(r, 1024, 768, 0, 0.5f, &ix, &iy) && ix > 0.1f && near(iy, 0.5f));
    CHECK(window_to_image(r, 1024, 768, 0.5f, 0.5f, &ix, &iy) && near(ix, 0.5f));
    // Stretch: window fractions are image fractions.
    r = place_image(AspectMode::Stretch, 1280, 720, 1024, 768);
    CHECK(window_to_image(r, 1024, 768, 0.25f, 0.75f, &ix, &iy) && near(ix, 0.25f) && near(iy, 0.75f));
    // Outside the window, or no image yet.
    CHECK(!window_to_image(r, 1024, 768, 1.2f, 0.5f, &ix, &iy));
    CHECK(!window_to_image(ImageRect{}, 0, 0, 0.5f, 0.5f, &ix, &iy));
}

void test_safe_insets() {
    // iPhone 15 landscape in points: 852x393, safe area x 59..793, bottom 21.
    const SafeInsets s = safe_insets_from_rect(59, 0, 734, 372, 852, 393);
    CHECK(near(s.left, 59.0f / 852) && near(s.right, 59.0f / 852) && near(s.top, 0) && near(s.bottom, 21.0f / 393));
    const SafeInsets none = safe_insets_from_rect(0, 0, 1280, 720, 1280, 720);
    CHECK(none.left == 0 && none.right == 0 && none.top == 0 && none.bottom == 0);
    const SafeInsets empty = safe_insets_from_rect(0, 0, 0, 0, 1280, 720);
    CHECK(empty.left == 0 && empty.bottom == 0);
}

}  // namespace

int main() {
    RUN(test_names);
    RUN(test_place);
    RUN(test_window_to_image);
    RUN(test_safe_insets);
    return finish();
}
