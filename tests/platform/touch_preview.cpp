// Pictures of the on-screen controller without a device (P7.3): the TouchPad
// overlay shaded with overlay_shade (the WebGPU shader's reference) over a
// stand-in for the game image, fitted at 16:9 with black bars.
//   lgp_touch_preview [--size WxH] [--insets L,R,T,B] [--press BUTTONS]
//                     [--hidden] [--scale S] [--opacity O] [--points H] [--out FILE.ppm]
// --insets in pixels (safe area), --points the window height in points
// (iPad 834, iPhone 402: TouchPad::set_window_points), --press a list of a,b,x,y,l,r,zl,zr,plus,minus.
// Without --out it checks that every button is drawn (ctest).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "lgp/platform/overlay.hpp"
#include "lgp/platform/touch_pad.hpp"

using namespace lgp::platform;

namespace {

uint64_t button_bit(const std::string& name) {
    if (name == "a") return kButtonA;
    if (name == "b") return kButtonB;
    if (name == "x") return kButtonX;
    if (name == "y") return kButtonY;
    if (name == "l") return kButtonL;
    if (name == "r") return kButtonR;
    if (name == "zl") return kButtonZL;
    if (name == "zr") return kButtonZR;
    if (name == "plus") return kButtonPlus;
    if (name == "minus") return kButtonMinus;
    return 0;
}

// Overlay order of TouchPad::layout: A, B, X, Y, L, ZL, R, ZR, Minus, Plus.
const uint64_t kOrder[] = {kButtonA, kButtonB, kButtonX, kButtonY, kButtonL,
                           kButtonZL, kButtonR, kButtonZR, kButtonMinus, kButtonPlus};

// A calm stand-in for the game image: sky to grass, no game data.
void background(float u, float v, float* r, float* g, float* b) {
    if (v < 0.55f) {
        const float t = v / 0.55f;
        *r = 0.45f + 0.25f * t, *g = 0.68f + 0.17f * t, *b = 0.92f - 0.02f * t;
    } else {
        const float t = (v - 0.55f) / 0.45f;
        const float stripe = 0.03f * std::sin(u * 60.0f + v * 20.0f);
        *r = 0.36f - 0.12f * t + stripe, *g = 0.62f - 0.2f * t + stripe, *b = 0.30f - 0.1f * t;
    }
}

}  // namespace

int main(int argc, char** argv) {
    int width = 2556, height = 1179;
    float il = 0, ir = 0, it = 0, ib = 0, scale = 1, opacity = 1, points = 0;
    bool hidden = false;
    std::string out, press;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const char* next = i + 1 < argc ? argv[i + 1] : nullptr;
        if (a == "--size" && next && std::sscanf(next, "%dx%d", &width, &height) == 2) ++i;
        else if (a == "--insets" && next && std::sscanf(next, "%f,%f,%f,%f", &il, &ir, &it, &ib) == 4) ++i;
        else if (a == "--press" && next) press = argv[++i];
        else if (a == "--scale" && next) scale = std::strtof(argv[++i], nullptr);
        else if (a == "--opacity" && next) opacity = std::strtof(argv[++i], nullptr);
        else if (a == "--points" && next) points = std::strtof(argv[++i], nullptr);
        else if (a == "--out" && next) out = argv[++i];
        else if (a == "--hidden") hidden = true;
        else {
            std::fprintf(stderr, "usage: lgp_touch_preview [--size WxH] [--insets L,R,T,B] [--press BUTTONS] "
                                 "[--hidden] [--scale S] [--opacity O] [--points H] [--out FILE.ppm]\n");
            return 2;
        }
    }
    if (width < 16 || height < 16) {
        std::fprintf(stderr, "lgp_touch_preview: bad size\n");
        return 2;
    }
    TouchPad pad(float(width) / float(height));
    pad.set_insets({il / width, ir / width, it / height, ib / height});
    pad.set_scale(scale);
    pad.set_opacity(opacity);
    pad.set_window_points(points);
    pad.set_hidden(hidden);
    // Press buttons by putting a finger on each.
    {
        const auto circles = pad.overlay();
        uint64_t id = 1;
        size_t start = 0;
        while (start <= press.size() && !press.empty()) {
            const size_t comma = press.find(',', start);
            const std::string name = press.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            const uint64_t bit = button_bit(name);
            for (size_t i = 0; i < 10 && i < circles.size(); ++i)
                if (kOrder[i] == bit)
                    pad.finger_down(id++, circles[i].x, circles[i].y);
            if (comma == std::string::npos)
                break;
            start = comma + 1;
        }
    }
    const std::vector<OverlayCircle> circles = pad.overlay();

    if (out.empty()) {
        // Every button shows ink at its centre region and its name.
        int failed = 0;
        for (size_t i = 0; i + 1 < circles.size(); ++i) {
            const OverlayCircle& c = circles[i];
            const OverlayPixel px = overlay_shade(circles, c.x * width + 0.5f, c.y * height + 0.5f, float(width), float(height));
            if (px.a <= 0 || c.label == OverlayCircle::kNone) {
                std::fprintf(stderr, "button %zu not drawn\n", i);
                ++failed;
            }
        }
        std::printf("%zu circles, %s\n", circles.size(), failed ? "FAILED" : "ok");
        return failed ? 1 : 0;
    }

    // The game image fitted at 16:9 into the window.
    const float img_w = std::min(float(width), height * 16.0f / 9.0f), img_h = img_w * 9.0f / 16.0f;
    const float img_x = (width - img_w) / 2, img_y = (height - img_h) / 2;
    std::vector<unsigned char> rgb(size_t(width) * height * 3);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const float px = x + 0.5f, py = y + 0.5f;
            float r = 0, g = 0, b = 0;
            if (px >= img_x && px < img_x + img_w && py >= img_y && py < img_y + img_h)
                background((px - img_x) / img_w, (py - img_y) / img_h, &r, &g, &b);
            const OverlayPixel o = overlay_shade(circles, px, py, float(width), float(height));
            r = o.r * o.a + r * (1 - o.a), g = o.g * o.a + g * (1 - o.a), b = o.b * o.a + b * (1 - o.a);
            unsigned char* p = &rgb[(size_t(y) * width + x) * 3];
            p[0] = (unsigned char)std::lround(std::clamp(r, 0.0f, 1.0f) * 255);
            p[1] = (unsigned char)std::lround(std::clamp(g, 0.0f, 1.0f) * 255);
            p[2] = (unsigned char)std::lround(std::clamp(b, 0.0f, 1.0f) * 255);
        }
    FILE* f = std::fopen(out.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "lgp_touch_preview: cannot write %s\n", out.c_str());
        return 1;
    }
    std::fprintf(f, "P6\n%d %d\n255\n", width, height);
    std::fwrite(rgb.data(), 1, rgb.size(), f);
    std::fclose(f);
    std::printf("wrote %s (%dx%d, %zu circles)\n", out.c_str(), width, height, circles.size());
    return 0;
}
