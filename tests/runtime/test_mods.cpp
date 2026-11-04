// Game mods (P8, lgp/mods/): mod files, starting mods on hook doors and the
// hook log, without lifted code: the doors are declared by hand and entered
// with call_hooked(), as LGP_HOOKED does.
#include <bit>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "check.hpp"
#include "lgp/lifted/cpu_state.hpp"
#include "lgp/mods/mod_file.hpp"
#include "lgp/mods/mods.hpp"
#include "lgp/mods/widescreen.hpp"
#include "lgp/platform/ui/layout.hpp"
#include "lgp/platform/ui/sarc.hpp"
#include "lgp/runtime/dispatch.hpp"
#include "lgp/runtime/guest_mem.hpp"
#include "lgp/runtime/hooks.hpp"
#include "lgp/runtime/memory.hpp"
#include "lgp/runtime/patch.hpp"
#include "lgp/runtime/render_hints.hpp"
#include "ui_fixtures.hpp"

namespace fs = std::filesystem;
using namespace lgp;

namespace {

constexpr uint64_t kTouch = 0x0800'1000;
constexpr uint64_t kOther = 0x0800'2000;

int g_original_calls = 0;
void original(CpuState* ctx) {
    ++g_original_calls;
    ctx->x[0] += 1;
}

void parse() {
    mods::ModFile f;
    std::string error;
    CHECK(mods::parse_mod_file("\xEF\xBB\xBF# comment\n"
                               "hook encounter_touch 0x08001000  # trailing comment\r\n"
                               "\n"
                               "value species_offset 24\n"
                               "patch wide_fov 0x8003000 1f2003d5\n"
                               "patch ui_left 0x8f00000 data hw/-2\n",
                               "t.cfg", &f, &error));
    CHECK_EQ(f.lines.size(), 4u);
    CHECK(f.hook("encounter_touch") == kTouch);
    CHECK(!f.hook("species_offset"));
    CHECK(f.value("species_offset") == 24u);
    CHECK(f.lines[2].directive == "patch");
    CHECK(f.lines[2].rest == "1f2003d5");
    CHECK_EQ(f.hook_entries().size(), 1u);
    CHECK(f.patch("wide_fov") == 0x8003000u);
    CHECK(f.patch_entries() == std::vector<uint64_t>{0x8003000});

    const char* bad[] = {
        "hook encounter_touch\n",               // no number
        "hook Encounter 0x1000\n",              // name
        "hook a 0x1002\n",                      // alignment
        "hook a 0x1000 extra\n",                // trailing text
        "hook a 0x1000\nhook a 0x2000\n",       // twice
        "value a -1\n",                         // sign
        "value a 0x\n",                         // empty hex
        "patch a 0x1001\n",                     // alignment
        "patch a 0x1000\npatch a 0x2000\n",     // twice
    };
    for (const char* text : bad) {
        mods::ModFile g;
        error.clear();
        CHECK(!mods::parse_mod_file(text, "t.cfg", &g, &error));
        CHECK(error.rfind("t.cfg:", 0) == 0);
        std::printf("  rejected: %s\n", error.c_str());
    }
    CHECK(mods::parse_mod_list("wild-battles, b c,,b") ==
          (std::vector<std::string>{"wild-battles", "b", "c"}));
    CHECK(mods::join_mod_list({"a", "b"}) == "a b");
    CHECK(mods::find_mod("wild-battles") != nullptr);
    CHECK(mods::find_mod("nope") == nullptr);
    CHECK(mods::find_mod("widescreen") != nullptr);
}

void write(const fs::path& p, const std::string& text);

void widescreen_scales() {
    const mods::PictureSize from{1280, 720}, wide{1680, 720}, tall{1280, 960};
    auto eq = [](mods::PictureSize a, uint32_t w, uint32_t h) { return a.width == w && a.height == h; };
    CHECK(eq(mods::widescreen_scale({1280, 720}, from, wide), 1680, 720));
    CHECK(eq(mods::widescreen_scale({640, 360}, from, wide), 840, 360));
    CHECK(eq(mods::widescreen_scale({40, 23}, from, wide), 53, 23));     // chain step 5
    CHECK(eq(mods::widescreen_scale({20, 12}, from, wide), 27, 12));     // the chain, not 26
    CHECK(eq(mods::widescreen_scale({5, 3}, from, wide), 7, 3));
    CHECK(eq(mods::widescreen_scale({480, 270}, from, wide), 630, 270));  // 3/8, scaled
    CHECK(eq(mods::widescreen_scale({16, 10}, from, wide), 21, 10));
    CHECK(eq(mods::widescreen_scale({128, 64}, from, wide), 128, 64));  // 2:1 stays
    CHECK(eq(mods::widescreen_scale({256, 256}, from, wide), 256, 256));
    CHECK(eq(mods::widescreen_scale({1920, 1080}, from, wide), 1920, 1080));  // the window texture stays
    CHECK(eq(mods::widescreen_scale({640, 360}, from, tall), 640, 480));
    CHECK(eq(mods::widescreen_scale({1680, 720}, from, wide), 1680, 720));  // a scaled size stays
    CHECK(eq(mods::widescreen_scale({640, 360}, from, from), 640, 360));
    const mods::PictureSize mac{1280, 800};  // 16:10 is within 11 % of 16:9
    CHECK(eq(mods::widescreen_scale({1280, 720}, from, mac), 1280, 800));
    CHECK(eq(mods::widescreen_scale({1280, 800}, from, mac), 1280, 800));  // not again to 1280x889
    CHECK(eq(mods::widescreen_scale({640, 400}, from, mac), 640, 400));
    CHECK(eq(mods::widescreen_scale({480, 270}, from, mac), 480, 300));
    CHECK(eq(mods::widescreen_scale({480, 300}, from, mac), 480, 300));
}

void widescreen_projections() {
    const mods::PictureSize from{1280, 720};
    const float f = 1.f / std::tan(15.f * 3.14159265f / 180.f);  // fovy 30
    float m[16] = {f / (16.f / 9.f), 0, 0, 0, 0, f, 0, 0, 0, 0, -1.f, -0.2f, 0, 0, -1, 0};
    CHECK(mods::widescreen_projection(m, from, {1680, 720}));
    CHECK(std::abs(m[5] - f) < 1e-6f);                      // vertical field of view kept
    CHECK(std::abs(m[5] / m[0] - 1680.f / 720.f) < 1e-4f);  // more at the sides
    CHECK(!mods::widescreen_projection(m, from, {1680, 720}));  // already changed: stays
    float t[16] = {f / (16.f / 9.f), 0, 0, 0, 0, f, 0, 0, 0, 0, -1.f, -1, 0, 0, -0.2f, 0};
    const float m00 = t[0];
    CHECK(mods::widescreen_projection(t, from, {1280, 960}));
    CHECK(std::abs(t[0] - m00) < 1e-6f);                 // horizontal kept
    CHECK(std::abs(t[5] / t[0] - 4.f / 3.f) < 1e-4f);    // more above and below
    float c[16] = {f / (16.f / 9.f), 0, 0, 0, 0, f, 0, 0, 0, 0, -1.f, -1, 0, 0, -0.2f, 0};
    CHECK(mods::widescreen_projection(c, from, {1280, 960}, true));
    CHECK(std::abs(c[5] - f) < 1e-6f);                   // keep_height: vertical kept
    CHECK(std::abs(c[5] / c[0] - 4.f / 3.f) < 1e-4f);    // the sides go
    float other[16] = {1, 0, 0, 0, 0, 1.2f, 0, 0, 0, 0, -1, -0.2f, 0, 0, -1, 0};
    CHECK(!mods::widescreen_projection(other, from, {1680, 720}));  // another camera
    CHECK(!mods::widescreen_projection(m, from, from));
    // A rotated 2D matrix (orthographic times 45 degrees): only its own x row widens.
    {
        const float c = std::sqrt(0.5f), ax = 2.f / 1280, ay = -2.f / 720;
        // rows: x' = ax (c x - c y) - 1, y' = ay (c x + c y) + 1
        float rr[16] = {ax * c, -ax * c, 0, -1, ay * c, ay * c, 0, 1, 0, 0, -1, 0, 0, 0, 0, 1};
        CHECK(mods::widescreen_projection(rr, from, {1680, 720}));
        CHECK(std::abs(rr[4] - ay * c) < 1e-9f && std::abs(rr[5] - ay * c) < 1e-9f && rr[7] == 1);  // y row kept
        CHECK(std::abs(rr[1] / rr[0] + 1) < 1e-6f && std::abs(rr[3] + 1280.f / 1680) < 1e-6f);
        float rc[16] = {ax * c, ay * c, 0, 0, -ax * c, ay * c, 0, 0, 0, 0, -1, 0, -1, 1, 0, 1};  // columns
        CHECK(mods::widescreen_projection(rc, from, {1680, 720}));
        CHECK(std::abs(rc[1] - ay * c) < 1e-9f && std::abs(rc[5] - ay * c) < 1e-9f && rc[13] == 1);
        CHECK(std::abs(rc[4] / rc[0] + 1) < 1e-6f && std::abs(rc[12] + 1280.f / 1680) < 1e-6f);
        float centred[16] = {ax * c, -ax * c, 0, 0, ay * c, ay * c, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1};
        CHECK(!mods::widescreen_projection(centred, from, {1680, 720}));  // layout unknown: left alone
    }
    // 2D: orthographic 0..1280 x 0..720, origin top left (y flipped), rows and columns.
    float o[16] = {2.f / 1280, 0, 0, -1, 0, -2.f / 720, 0, 1, 0, 0, -1, 0, 0, 0, 0, 1};
    CHECK(mods::widescreen_projection(o, from, {1680, 720}));
    auto clip_x = [](const float* r, float x) { return r[0] * x + r[3]; };
    CHECK(std::abs(clip_x(o, 640) - 0.f) < 1e-6f);                  // the middle stays in the middle
    CHECK(std::abs(clip_x(o, 1280) - 1280.f / 1680) < 1e-5f);       // 1280 wide in 1680, unstretched
    CHECK(std::abs(o[5] + 2.f / 720) < 1e-9f);
    CHECK(!mods::widescreen_projection(o, from, {1680, 720}));
    float oc[16] = {2.f / 1280, 0, 0, 0, 0, -2.f / 720, 0, 0, 0, 0, -1, 0, -1, 1, 0, 1};
    CHECK(mods::widescreen_projection(oc, from, {1280, 960}));
    CHECK(std::abs(oc[0] - 2.f / 1280) < 1e-9f);
    CHECK(std::abs(oc[5] * 360 + oc[13]) < 1e-6f);         // y 360 stays the middle
    CHECK(std::abs(oc[5] + 2.f / 960) < 1e-7f);                     // 720 high in 960
    float tall_bg[4] = {1280, 720, -640, 360};
    CHECK(mods::widescreen_rect(tall_bg, from, {1280, 960}));
    CHECK(tall_bg[0] == 1280.f && tall_bg[1] == 960.f && tall_bg[2] == -640.f && tall_bg[3] == 480.f);
    float box[4] = {800, 200, -400, -100};
    CHECK(!mods::widescreen_rect(box, from, {1680, 720}));
    // Model-placed elements: the turned fade (square, 90 degrees, ty -279) is widened.
    const float square[4] = {1280, 1280, -640, 640};
    float turned[8] = {0, -1, 0, 0, 1, 0, 0, -279};
    CHECK(mods::widescreen_element(square, turned, from, {1680, 720}));
    CHECK(std::abs(turned[1] + 1680.f / 1280) < 1e-6f && turned[4] == 1.f && turned[7] == -279.f);
    CHECK(!mods::widescreen_element(square, turned, from, {1680, 720}));  // again: stays
    const float full[4] = {1280, 720, -640, 360};
    float zoomed[8] = {1.5f, 0, 0, 0, 0, 1.5f, 0, 0};  // already wider than 1680: stays
    CHECK(!mods::widescreen_element(full, zoomed, from, {1680, 720}));
    float plain[8] = {1, 0, 0, 0, 0, 1, 0, 0};
    CHECK(mods::widescreen_element(full, plain, from, {1280, 960}));
    CHECK(plain[0] == 1.f && std::abs(plain[5] - 960.f / 720) < 1e-6f);
    float icon[8] = {1, 0, 0, 434, 0, 1, 0, -116};
    const float save[4] = {128, 128, -64, 64};
    CHECK(!mods::widescreen_element(save, icon, from, {1680, 720}));
    // Texture: the widened grid (1280x360, model row 0 x1.3125) repeats more, centred.
    const float grid[4] = {1280, 360, -640, 180};
    const float plain_identity[8] = {1, 0, 0, 0, 0, 1, 0, 0};
    const float wide_rows[8] = {1680.f / 1280, 0, 0, 0, 0, 1, 0, 100};
    float tex[8] = {1, 0, 0, 0, 0, -1, 0, 1};
    CHECK(mods::widescreen_texture(grid, wide_rows, tex, from, {1680, 720}));
    CHECK(std::abs(tex[0] - 1.3125f) < 1e-6f && std::abs(tex[3] + 0.15625f) < 1e-6f);  // u -0.156 .. 1.156
    CHECK(tex[5] == -1.f && tex[7] == 1.f);                                             // v untouched
    float tex2[8] = {1, 0, 0, 0, 0, -1, 0, 1};
    CHECK(!mods::widescreen_texture(grid, plain_identity, tex2, from, {1680, 720}));  // not widened: stays
    float tex3[8] = {1, 0, 0, 0, 0, 1, 0, 0};
    CHECK(!mods::widescreen_texture(square, turned, tex3, from, {1680, 720}));  // turned: stays
    // Edge: a header bar at the old left edge (x -640..270) reaches the new one, its right end stays.
    const float header[4] = {910, 45, -640, 360};
    float hm[8] = {1, 0, 0, 0, 0, 1, 0, 0};
    float htex[8] = {1, 0, 0, 0, 0, -1, 0, 1};
    CHECK(mods::widescreen_edge(header, hm, htex, from, {1680, 720}));
    const float a = 1110.f / 910;
    CHECK(std::abs(hm[0] - a) < 1e-5f && std::abs(hm[0] * -640 + hm[3] + 840) < 1e-3f);  // left at -840
    CHECK(std::abs(hm[0] * 270 + hm[3] - 270) < 1e-3f);                                // right stays
    CHECK(std::abs(htex[0] - a) < 1e-5f && std::abs(htex[3] + 200.f / 910) < 1e-5f);    // u from -0.22: texels kept
    CHECK(htex[5] == -1.f && htex[7] == 1.f && hm[5] == 1.f);
    CHECK(!mods::widescreen_edge(header, hm, htex, from, {1680, 720}));  // again: stays
    // 4:3: a bottom bar (y -360..-320) reaches -480, v continues below.
    const float footer[4] = {1280, 40, -640, -320};
    float fm[8] = {1, 0, 0, 0, 0, 1, 0, 0};
    float ftex[8] = {1, 0, 0, 0, 0, -1, 0, 1};
    CHECK(mods::widescreen_edge(footer, fm, ftex, from, {1280, 960}));
    CHECK(std::abs(fm[5] - 4.f) < 1e-5f && std::abs(fm[5] * -360 + fm[7] + 480) < 1e-3f);
    CHECK(std::abs(fm[5] * -320 + fm[7] + 320) < 1e-3f && fm[0] == 1.f);
    CHECK(std::abs(ftex[5] + 4.f) < 1e-5f && std::abs(ftex[7] - 1.f) < 1e-5f);  // v 1 at the top, -3 at -480
    float sm[8] = {1, 0, 0, 0, 0, 1, 0, 0};
    CHECK(!mods::widescreen_edge(full, sm, nullptr, from, {1680, 720}));  // spanning: widescreen_element's
    CHECK(!mods::widescreen_edge(save, icon, nullptr, from, {1680, 720}));  // inside: stays
    float tm[8] = {0, -1, 0, 0, 1, 0, 0, 0};
    CHECK(!mods::widescreen_edge(header, tm, nullptr, from, {1680, 720}));  // turned: stays
    // The game's header bar overhangs the edge by 5 (x -645..291): it still counts, the overhang stays.
    const float over[4] = {936, 76, -645, 360};
    float om[8] = {1, 0, 0, 0, 0, 1, 0, 0};
    CHECK(mods::widescreen_edge(over, om, nullptr, from, {1680, 720}));
    CHECK(std::abs(om[0] * -645 + om[3] + 845) < 1e-3f && std::abs(om[0] * 291 + om[3] - 291) < 1e-3f);
    // widen: bands beside the widened frame scale with it, centred.
    float wm[8] = {1, 0, 0, 0, 0, 1, 0, -319};
    CHECK(mods::widescreen_widen(wm, from, {1680, 720}));
    CHECK(std::abs(wm[0] - 1.3125f) < 1e-6f && wm[3] == 0.f && wm[5] == 1.f && wm[7] == -319.f);
    CHECK(!mods::widescreen_widen(wm, from, from));
    // xwiden: only along x; a taller picture leaves it.
    float xm[8] = {1, 0, 0, 600, 0, 1, 0, -319};
    CHECK(mods::widescreen_widen(xm, from, {1568, 720}, true));
    CHECK(std::abs(xm[0] - 1.225f) < 1e-6f && std::abs(xm[3] - 735.f) < 1e-3f && xm[5] == 1.f && xm[7] == -319.f);
    float ym[8] = {1, 0, 0, 0, 0, 1, 0, -319};
    CHECK(!mods::widescreen_widen(ym, from, {1280, 960}, true));
    CHECK(ym[5] == 1.f && ym[7] == -319.f);
    // span: a band 852 wide (x -426..426) across 1680, ends at the edges; again: stays; 4:3: nothing.
    const float tint[4] = {852, 78, -426, 360};
    float spm[8] = {1, 0, 0, 0, 0, 1, 0, 0};
    CHECK(mods::widescreen_span(tint, spm, from, {1680, 720}));
    CHECK(std::abs(spm[0] * -426 + spm[3] + 840) < 1e-3f && std::abs(spm[0] * 426 + spm[3] - 840) < 1e-3f);
    CHECK(!mods::widescreen_span(tint, spm, from, {1680, 720}) && spm[5] == 1.f);
    float sp4[8] = {1, 0, 0, 0, 0, 1, 0, 2};  // 4:3: the y row scales like widen, x stays
    CHECK(mods::widescreen_span(tint, sp4, from, {1280, 960}));
    CHECK(sp4[0] == 1.f && sp4[3] == 0.f && std::abs(sp4[5] - 4.f / 3.f) < 1e-6f &&
          std::abs(sp4[7] - 8.f / 3.f) < 1e-6f);
    // stretch: unturned like edge without tex; a plate turned by 9 degrees at the top edge grows along its own y.
    {
        float um[8] = {1, 0, 0, 0, 0, 1, 0, 0}, em[8] = {1, 0, 0, 0, 0, 1, 0, 0};
        const float box[4] = {300, 200, -640, -160};
        CHECK(mods::widescreen_stretch(box, um, from, {1280, 960}));
        CHECK(mods::widescreen_edge(box, em, nullptr, from, {1280, 960}));
        for (int i = 0; i < 8; ++i) CHECK(std::abs(um[i] - em[i]) < 1e-4f);
        const float a = 9.f * 3.14159265f / 180.f, co = std::cos(a), si = std::sin(a);
        // local (0..350, -174..0) turned, its highest corner just above y 360
        float pm[8] = {co, -si, 0, 290, si, co, 0, 0};
        const float plate[4] = {350, 174, 0, 0};
        float top = -1e30f;
        for (float lx : {0.f, 350.f})
            for (float ly : {-174.f, 0.f}) top = std::max(top, pm[4] * lx + pm[5] * ly);
        pm[7] = 370 - top;
        CHECK(mods::widescreen_stretch(plate, pm, from, {1280, 960}));
        float top2 = -1e30f, bottom2 = 1e30f;
        for (float lx : {0.f, 350.f})
            for (float ly : {-174.f, 0.f}) {
                const float y = pm[4] * lx + pm[5] * ly + pm[7];
                top2 = std::max(top2, y);
                bottom2 = std::min(bottom2, y);
            }
        CHECK(std::abs(top2 - 490) < 1e-2f);  // 120 higher
        CHECK(std::abs(bottom2 - (370 - top + std::min(0.f, 350 * si) - 174 * co)) < 1e-2f);  // far side stays
        CHECK(std::abs(pm[0] - co) < 1e-6f && std::abs(pm[4] - si) < 1e-6f);  // local x untouched
        float inner[8] = {co, -si, 0, 0, si, co, 0, 0};  // in the middle: stays
        // xstretch (iPhone battle text box at the bottom left): along x only, a taller picture leaves it
        const float tbox[4] = {980, 136, -640, -224};
        float tw[8] = {1, 0, 0, 0, 0, 1, 0, 0}, tt[8] = {1, 0, 0, 0, 0, 1, 0, 0}, ts[8] = {1, 0, 0, 0, 0, 1, 0, 0};
        CHECK(mods::widescreen_stretch(tbox, tw, from, {1568, 720}, true));
        CHECK(std::abs(tw[0] * -640 + tw[3] + 784) < 1e-3f && std::abs(tw[0] * 340 + tw[3] - 340) < 1e-3f);
        CHECK(tw[5] == 1.f && tw[7] == 0.f);
        CHECK(!mods::widescreen_stretch(tbox, tt, from, {1280, 960}, true));
        CHECK(mods::widescreen_stretch(tbox, ts, from, {1280, 960}) && ts[5] != 1.f);  // plain stretch grows down
        CHECK(!mods::widescreen_stretch(plate, inner, from, {1280, 960}));
    }
    {  // zone with side (battle corners at 4:3): only groups wholly beside the middle; a turned plate cut by the edge counts
        const float cmd[4] = {280, 264, -640, -96}, ball[4] = {120, 120, -60, -230};
        float cm[8] = {1, 0, 0, 0, 0, 1, 0, 0}, bm[8] = {1, 0, 0, 0, 0, 1, 0, 0};
        CHECK(mods::widescreen_zone(cmd, cm, 90, -90, from, {1280, 960}, 280) && cm[7] == -120.f);
        CHECK(!mods::widescreen_zone(ball, bm, 90, -90, from, {1280, 960}, 280) && bm[7] == 0.f);
        CHECK(mods::widescreen_zone(ball, bm, 90, -90, from, {1280, 960}) && bm[7] == -120.f);  // without side it goes
        const float a = 9.f * 3.14159265f / 180.f;
        float pm[8] = {std::cos(a), -std::sin(a), 0, 290, std::sin(a), std::cos(a), 0, 340};  // reaches above 360
        const float plate[4] = {350, 174, 0, 0};
        CHECK(mods::widescreen_zone(plate, pm, 90, -90, from, {1280, 960}, 280) && pm[7] == 460.f);
        const float glow[4] = {216, 128, -640, 40};  // y -88..40 straddles -90: only as a listed member, to its middle's side
        float gm[8] = {1, 0, 0, 0, 0, 1, 0, 0};
        CHECK(!mods::widescreen_zone(glow, gm, 90, -90, from, {1280, 960}, 280));
        CHECK(mods::widescreen_zone(glow, gm, 90, -90, from, {1280, 960}, 280, true) && gm[7] == -120.f);
    }
    {  // spread (Pokédex rows at 4:3): middles move apart by 4/3, sizes stay; full-width and outer elements stay
        const float row[4] = {60, 40, 200, -180}, wide[4] = {1280, 100, -640, 50}, header[4] = {300, 40, -150, 330};
        float rm[8] = {1, 0, 0, 0, 0, 1, 0, 0}, wm[8] = {1, 0, 0, 0, 0, 1, 0, 0}, hm2[8] = {1, 0, 0, 0, 0, 1, 0, 0};
        CHECK(mods::widescreen_spread(row, rm, 278, -278, from, {1280, 960}));
        CHECK(std::abs(rm[7] - (-200.f / 3)) < 1e-3f && rm[5] == 1.f);  // middle -200 -> -266.7
        CHECK(!mods::widescreen_spread(wide, wm, 278, -278, from, {1280, 960}));
        CHECK(!mods::widescreen_spread(header, hm2, 278, -278, from, {1280, 960}));
        float r21[8] = {1, 0, 0, 0, 0, 1, 0, 0};
        CHECK(!mods::widescreen_spread(row, r21, 278, -278, from, {1680, 720}));  // not taller
    }
    {  // clip_zone: side and spread (text)
        float t1[16] = {1, 0, 0, -0.75f, 0, 1, 0, -200.f / 480, 0, 0, 1, 0, 0, 0, 0, 1};  // x -480, y -200
        CHECK(mods::widescreen_clip_zone(t1, 90, from, {1280, 960}, 280) && std::abs(t1[7] * 480 + 320) < 1e-3f);
        float t2[16] = {1, 0, 0, -0.2f, 0, 1, 0, -200.f / 480, 0, 0, 1, 0, 0, 0, 0, 1};  // x -128: not beside
        CHECK(!mods::widescreen_clip_zone(t2, 90, from, {1280, 960}, 280));
        float t3[16] = {1, 0, 0, 0.1f, 0, 1, 0, 150.f / 480, 0, 0, 1, 0, 0, 0, 0, 1};  // y 150 inside 278: spreads
        CHECK(!mods::widescreen_clip_zone(t3, 278, from, {1280, 960}));
        CHECK(mods::widescreen_clip_zone(t3, 278, from, {1280, 960}, 0, true) && std::abs(t3[7] * 480 - 200) < 1e-3f);
    }
    // zone (4:3): a header group above y 278 moves up by 120, a footer below -278 down, the list stays.
    const float band[4] = {852, 78, -426, 360}, text[4] = {200, 30, -300, -330}, list[4] = {852, 556, -426, 278};
    float zm[8] = {1, 0, 0, 0, 0, 1, 0, 0};
    CHECK(mods::widescreen_zone(band, zm, 278, -278, from, {1280, 960}) && zm[7] == 120.f);
    float zt[8] = {1, 0, 0, 0, 0, 1, 0, 0};
    CHECK(mods::widescreen_zone(text, zt, 278, -278, from, {1280, 960}) && zt[7] == -120.f);
    float zl[8] = {1, 0, 0, 0, 0, 1, 0, 0};
    CHECK(!mods::widescreen_zone(list, zl, 278, -278, from, {1280, 960}) && zl[7] == 0.f);
    const float off_top[4] = {125, 125, -662, 532};  // above the old picture (ty 470): stays
    float zo[8] = {1, 0, 0, 0, 0, 1, 0, 0};
    CHECK(!mods::widescreen_zone(off_top, zo, 278, -278, from, {1280, 960}));
    float zw[8] = {1, 0, 0, 0, 0, 1, 0, 0};
    CHECK(!mods::widescreen_zone(band, zw, 278, -278, from, {1680, 720}));  // wider: nothing to move
    // Clip-space zone (text through a full matrix): after Vert+ 720 -> 960, y translation 0.7 is layout 336.
    float cm[16] = {2.f / 1280, 0, 0, 0.1f, 0, 2.f / 960, 0, 0.7f, 0, 0, -1, 0, 0, 0, 0, 1};
    CHECK(mods::widescreen_clip_zone(cm, 278, from, {1280, 960}));
    CHECK(std::abs(cm[7] - (0.7f + 120.f / 480)) < 1e-6f && cm[3] == 0.1f);  // up by 120 layout units
    float cn[16] = {2.f / 1280, 0, 0, 0, 0, -2.f / 960, 0, 0, 0, 0, -1, 0, 0.2f, -0.75f, 0, 1};  // columns, y down
    CHECK(mods::widescreen_clip_zone(cn, 278, from, {1280, 960}) && std::abs(cn[13] + 1.f) < 1e-6f);
    float cl[16] = {2.f / 1280, 0, 0, 0, 0, 2.f / 960, 0, 0.2f, 0, 0, -1, 0, 0, 0, 0, 1};
    CHECK(!mods::widescreen_clip_zone(cl, 278, from, {1280, 960}) && cl[7] == 0.2f);  // the list stays
    CHECK(!mods::widescreen_clip_zone(cm, 278, from, {1680, 720}));                    // wider: nothing
    // xzone (21:9, 1680x720, iPhone battle corners): the same along x, by half the extra width (200).
    {
        const float hp[4] = {180, 80, 400, 330}, cmd[4] = {180, 80, -630, -250}, mid[4] = {200, 40, -100, 0};
        float xm[8] = {1, 0, 0, 0, 0, 1, 0, 0}, xl[8] = {1, 0, 0, 0, 0, 1, 0, 0}, xc[8] = {1, 0, 0, 0, 0, 1, 0, 0};
        CHECK(mods::widescreen_xzone(hp, xm, 250, -250, from, {1680, 720}) && xm[3] == 200.f && xm[7] == 0.f);
        CHECK(mods::widescreen_xzone(cmd, xl, 250, -250, from, {1680, 720}) && xl[3] == -200.f);
        CHECK(!mods::widescreen_xzone(mid, xc, 250, -250, from, {1680, 720}) && xc[3] == 0.f);
        float xs[8] = {1, 0, 0, 0, 0, 1, 0, 0}, xt[8] = {1, 0, 0, 0, 0, 1, 0, 0};
        CHECK(!mods::widescreen_xzone(hp, xs, 250, -250, from, {1680, 720}, 300));  // side: y 250..330 not wholly >= 300
        CHECK(mods::widescreen_xzone(hp, xs, 250, -250, from, {1680, 720}, 250) && xs[3] == 200.f);
        CHECK(!mods::widescreen_xzone(hp, xt, 250, -250, from, {1280, 960}));  // taller: nothing along x
        const float row[4] = {40, 60, 100, -100}, tall[4] = {40, 720, 100, 360};
        float xr[8] = {1, 0, 0, 0, 0, 1, 0, 0}, xh[8] = {1, 0, 0, 0, 0, 1, 0, 0};
        CHECK(mods::widescreen_xspread(row, xr, 250, -250, from, {1680, 720}) &&
              std::abs(xr[3] - 120.f * (1680.f / 1280 - 1)) < 1e-3f);
        CHECK(!mods::widescreen_xspread(tall, xh, 250, -250, from, {1680, 720}));  // full height: stays
        float tr[16] = {2.f / 1680, 0, 0, 500.f / 840, 0, 2.f / 720, 0, 0.1f, 0, 0, -1, 0, 0, 0, 0, 1};  // rows, x 500
        CHECK(mods::widescreen_clip_xzone(tr, 400, from, {1680, 720}) && std::abs(tr[3] * 840 - 700) < 1e-3f &&
              tr[7] == 0.1f);
        float tc[16] = {2.f / 1680, 0, 0, 0, 0, 2.f / 720, 0, 0, 0, 0, -1, 0, -500.f / 840, 0.2f, 0, 1};  // columns
        CHECK(mods::widescreen_clip_xzone(tc, 400, from, {1680, 720}) && std::abs(tc[12] * 840 + 700) < 1e-3f);
        float tm[16] = {2.f / 1680, 0, 0, 100.f / 840, 0, 2.f / 720, 0, 0.1f, 0, 0, -1, 0, 0, 0, 0, 1};  // middle
        CHECK(!mods::widescreen_clip_xzone(tm, 400, from, {1680, 720}));
        CHECK(mods::widescreen_clip_xzone(tm, 400, from, {1680, 720}, 0, true) &&
              std::abs(tm[3] * 840 - 100.f * 1680 / 1280) < 1e-3f);
        float ty[16] = {2.f / 1680, 0, 0, 500.f / 840, 0, 2.f / 720, 0, 0.1f, 0, 0, -1, 0, 0, 0, 0, 1};
        CHECK(!mods::widescreen_clip_xzone(ty, 400, from, {1680, 720}, 100));  // side: y 36 < 100
        CHECK(!mods::widescreen_clip_xzone(ty, 400, from, {1280, 960}));       // taller: nothing
    }
}

void widescreen_sizes() {
    auto is = [](double aspect, uint32_t w, uint32_t h) {
        const mods::PictureSize s = mods::widescreen_size(aspect);
        std::printf("  %.3f -> %ux%u\n", aspect, s.width, s.height);
        CHECK_EQ(s.width, w);
        CHECK_EQ(s.height, h);
    };
    is(16.0 / 9.0, 1280, 720);   // the game's own
    is(1920.0 / 1088, 1280, 720);  // within 1 %
    is(0, 1280, 720);            // unknown
    is(21.0 / 9.0, 1680, 720);   // ultrawide: Hor+
    is(2796.0 / 1290, 1568, 720);  // iPhone 17 Pro Max landscape
    is(32.0 / 9.0, 1920, 544);   // limited by the window texture: lower instead
    is(4.0 / 3.0, 1280, 960);    // iPad: Vert+
    is(2388.0 / 1668, 1280, 896);  // iPad Pro 11"
    is(1.0, 1088, 1080);         // square: limited by the height
}

// The widescreen mod replaces move-wide immediates; the instruction is read
// from guest memory, the patch run by hand as LGP_PATCH would.
void widescreen_start(const fs::path& dir) {
    rt::GuestMemory memory(rt::Layout::packed(0x0800'0000, 0x10'0000, 0x10'0000, 0x10'0000, 0x10'0000, 0x40'0000));
    const uint64_t code = memory.layout().code.base;
    CHECK(memory.map(code, rt::kPageSize, rt::MemState::Normal, rt::kPermRW).ok());
    const uint32_t movk_x3 = 0xF2C0'0000u | (720u << 5) | 3u;  // movk x3, #720, lsl #32
    const uint32_t movk_w4 = 0x7280'0000u | (1280u << 5) | 4u;  // movk w4, #1280
    CHECK(memory.copy_to_user(code, &movk_x3, 4));
    CHECK(memory.copy_to_user(code + 4, &movk_w4, 4));
    char cfg[1024];
    std::snprintf(cfg, sizeof cfg, "patch size 0x%llx hw hh\npatch width 0x%llx hw\n",
                  static_cast<unsigned long long>(code), static_cast<unsigned long long>(code + 4));
    write(dir / "widescreen.cfg", cfg);
    mods::StartOptions o;
    o.dir = dir;
    o.enabled = {"widescreen"};
    std::string error;
    {  // no screen aspect: not started
        o.memory = &memory;
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active.empty());
        CHECK_EQ(rt::patch_count(), 0u);
    }
    {  // 21:9
        o.display_aspect = 21.0 / 9.0;
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CHECK_EQ(r.active.size(), 1u);
        CHECK_EQ(rt::patch_count(), 2u);
        CpuState cpu{};
        CHECK(rt::run_patch(&cpu, code));
        CHECK(rt::run_patch(&cpu, code + 4));
        CHECK_EQ(cpu.x[3], 1680u | uint64_t{720} << 32);
        CHECK_EQ(cpu.x[4], 1680u);
        CHECK(mods::widescreen_active().width == 1680 && mods::widescreen_active().height == 720);
        CHECK(!mods::widescreen_docked_wide());
        CHECK(mods::widescreen_docked_picture().width == 0);
    }
    {  // `docked wide 1`: the mod also runs in the TV mode, with the handheld picture times 1.5
        o.display_aspect = 4.0 / 3.0;
        const uint64_t data_va = code + 0x200;
        const float f360 = 360.f;
        CHECK(memory.copy_to_user(data_va, &f360, 4));
        std::snprintf(cfg, sizeof cfg,
                      "docked wide 1\npatch dock 0x%llx dw dh\npatch dwidth 0x%llx dw\npatch r 0x%llx size w2 w3 skip 1920x1080\n"
                      "patch bound 0x%llx data hh/2\npatch px 0x%llx float hw docked dw\npatch half 0x%llx float hh/2\n",
                      static_cast<unsigned long long>(code), static_cast<unsigned long long>(code + 4),
                      static_cast<unsigned long long>(code + 12), static_cast<unsigned long long>(data_va),
                      static_cast<unsigned long long>(code + 16), static_cast<unsigned long long>(code + 20));
        const uint32_t ldr_s1 = 0x1C00'0000u | (0x80u << 5) | 1u;  // ldr s1, .+0x200 (the game's 360)
        CHECK(memory.copy_to_user(code + 16, &ldr_s1, 4));
        CHECK(memory.copy_to_user(code + 20, &ldr_s1, 4));
        write(dir / "widescreen.cfg", cfg);
        mods::set_widescreen_docked(true);  // docked at start
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CHECK_EQ(r.active.size(), 1u);
        CHECK(mods::widescreen_docked_wide());
        CHECK(mods::widescreen_docked_picture().width == 1920 && mods::widescreen_docked_picture().height == 1440);
        // Not resting: the handheld picture (the aspect), the docked render size in dw/dh.
        CHECK(mods::widescreen_active().width == 1280 && mods::widescreen_active().height == 960);
        CpuState cpu{};
        CHECK(rt::run_patch(&cpu, code));
        CHECK(rt::run_patch(&cpu, code + 4));
        CHECK_EQ(cpu.x[3], 1920u | uint64_t{1440} << 32);
        CHECK_EQ(cpu.x[4], 1920u);
        // Docked the game's targets are of the 1920x1080 family.
        cpu.x[2] = 1920;
        cpu.x[3] = 1080;
        rt::run_patch(&cpu, code + 12);
        CHECK(cpu.x[2] == 1920 && cpu.x[3] == 1440);
        cpu.x[2] = 960;
        cpu.x[3] = 540;
        rt::run_patch(&cpu, code + 12);
        CHECK(cpu.x[2] == 960 && cpu.x[3] == 720);
        cpu.x[2] = 2880;  // the skip size times 1.5 stays while docked
        cpu.x[3] = 1620;
        rt::run_patch(&cpu, code + 12);
        CHECK(cpu.x[2] == 2880 && cpu.x[3] == 1620);
        float held = 0;
        CHECK(memory.copy_from_user(&held, data_va, 4));
        CHECK(held == 480.f);  // ours while docked too
        auto loaded = [&] {
            float f = 0;
            const uint32_t bits = uint32_t(cpu.vreg[1].lo);
            std::memcpy(&f, &bits, 4);
            return f;
        };
        CHECK(rt::run_patch(&cpu, code + 16));
        CHECK(loaded() == 1920.f);  // `docked dw`: the docked picture's width
        CHECK(rt::run_patch(&cpu, code + 20));
        CHECK(loaded() == 480.f);  // no `docked`: the same value in both modes
        mods::set_widescreen_docked(false);  // handheld: the 1280x720 family as without the line
        CHECK(rt::run_patch(&cpu, code));  // and dw/dh lines yield the game's docked size
        CHECK(rt::run_patch(&cpu, code + 4));
        CHECK_EQ(cpu.x[3], 1920u | uint64_t{1080} << 32);
        CHECK_EQ(cpu.x[4], 1920u);
        cpu.x[2] = 1280;
        cpu.x[3] = 720;
        rt::run_patch(&cpu, code + 12);
        CHECK(cpu.x[2] == 1280 && cpu.x[3] == 960);
        cpu.x[2] = 1920;  // handheld the skip size itself stays
        cpu.x[3] = 1080;
        rt::run_patch(&cpu, code + 12);
        CHECK(cpu.x[2] == 1920 && cpu.x[3] == 1080);
        CHECK(rt::run_patch(&cpu, code + 16));
        CHECK(loaded() == 1280.f);
        mods::stop();
        CHECK(!mods::widescreen_docked_wide());
        CHECK(memory.copy_from_user(&held, data_va, 4));
        CHECK(held == 360.f);  // the game's value back at the stop
    }
    {  // `docked wide 2`: the docked picture no larger in area than 1920x1080
        std::snprintf(cfg, sizeof cfg,
                      "docked wide 2\npatch dock 0x%llx dw dh\npatch width 0x%llx hw\n"
                      "patch r 0x%llx size w2 w3 skip 480x270 docked 1920x1080\npatch n 0x%llx size w2 w3 skip 1920x1080 docked none\n"
                      "patch v 0x%llx size w2 w3 format w5 skip 1920x1080,960x540 docked 960x540@13,1920x1080@1\n",
                      static_cast<unsigned long long>(code), static_cast<unsigned long long>(code + 4),
                      static_cast<unsigned long long>(code + 8), static_cast<unsigned long long>(code + 12),
                      static_cast<unsigned long long>(code + 24));
        write(dir / "widescreen.cfg", cfg);
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CHECK_EQ(r.active.size(), 1u);
        CHECK(mods::widescreen_docked_picture().width == 1648 && mods::widescreen_docked_picture().height == 1232);
        CpuState cpu{};
        mods::set_widescreen_docked(true);
        CHECK(rt::run_patch(&cpu, code));
        CHECK_EQ(cpu.x[3], 1648u | uint64_t{1232} << 32);
        cpu.x[2] = 1920;  // `docked 1920x1080`: this size stays while docked
        cpu.x[3] = 1080;
        rt::run_patch(&cpu, code + 8);
        CHECK(cpu.x[2] == 1920 && cpu.x[3] == 1080);
        cpu.x[2] = 720;  // and 480x270 times 1.5 no longer
        cpu.x[3] = 405;
        rt::run_patch(&cpu, code + 8);
        CHECK(cpu.x[2] != 720 || cpu.x[3] != 405);
        cpu.x[2] = 1920;  // `docked none`: the scene target grows while docked
        cpu.x[3] = 1080;
        rt::run_patch(&cpu, code + 12);
        CHECK(cpu.x[2] == 1648 && cpu.x[3] == 1232);
        // `format w5 … docked 960x540@13,1920x1080@1`: docked a video's planes stay, the scene target grows.
        auto v = [&](uint64_t width, uint64_t height, uint64_t format) {
            cpu.x[2] = width;
            cpu.x[3] = height;
            cpu.x[5] = format;
            rt::run_patch(&cpu, code + 24);
        };
        v(1920, 1080, 1);  // R8 luma
        CHECK(cpu.x[2] == 1920 && cpu.x[3] == 1080);
        v(960, 540, 13);  // RG8 chroma
        CHECK(cpu.x[2] == 960 && cpu.x[3] == 540);
        v(1920, 1080, 56);  // RGBA8_SRGB scene target
        CHECK(cpu.x[2] == 1648 && cpu.x[3] == 1232);
        v(960, 540, 56);  // a half-size pass
        CHECK(cpu.x[2] != 960 || cpu.x[3] != 540);
        mods::set_widescreen_docked(false);
        v(1920, 1080, 56);  // handheld: `skip 1920x1080` for any format
        CHECK(cpu.x[2] == 1920 && cpu.x[3] == 1080);
        cpu.x[2] = 480;  // handheld the skip list applies: 480x270 stays
        cpu.x[3] = 270;
        rt::run_patch(&cpu, code + 8);
        CHECK(cpu.x[2] == 480 && cpu.x[3] == 270);
        mods::stop();
    }
    {  // `@N` in a skip list without `format`: not started
        std::snprintf(cfg, sizeof cfg, "patch size 0x%llx hw hh\npatch v 0x%llx size w2 w3 skip 960x540@13\n",
                      static_cast<unsigned long long>(code), static_cast<unsigned long long>(code + 24));
        write(dir / "widescreen.cfg", cfg);
        mods::StartResult r;
        mods::start(o, &r, &error);
        CHECK(r.active.empty());
    }
    {  // a wrong `docked` line: not started
        std::snprintf(cfg, sizeof cfg, "docked wide 3\npatch size 0x%llx hw hh\n", static_cast<unsigned long long>(code));
        write(dir / "widescreen.cfg", cfg);
        mods::StartResult r;
        mods::start(o, &r, &error);
        CHECK(r.active.empty());
    }
    {  // float: LDR St (unsigned offset, literal) yields a value of the picture size, in TV mode the game's
        o.display_aspect = 4.0 / 3.0;
        const uint32_t ldr_s5 = 0xBD40'0000u | (2u << 10) | (6u << 5) | 5u;  // ldr s5, [x6, #8]
        const uint32_t ldr_s7 = 0x1C00'0000u | (2u << 5) | 7u;               // ldr s7, .+8
        const float f360 = 360.f, f640 = 640.f;
        CHECK(memory.copy_to_user(code + 40, &ldr_s5, 4));
        CHECK(memory.copy_to_user(code + 44, &ldr_s7, 4));
        CHECK(memory.copy_to_user(code + 52, &f640, 4));
        CHECK(memory.copy_to_user(code + 0x100, &f360, 4));
        const uint32_t ldur_s3 = 0xBC40'0000u | (0x1FCu << 12) | (6u << 5) | 3u;  // ldur s3, [x6, #-4]
        const uint32_t movz_w9 = 0x5280'0000u | (360u << 5) | 9u;                 // movz w9, #360
        CHECK(memory.copy_to_user(code + 48, &ldur_s3, 4));
        CHECK(memory.copy_to_user(code + 56, &movz_w9, 4));
        std::snprintf(cfg, sizeof cfg,
                      "patch size 0x%llx hw hh\npatch half 0x%llx float hh/2\npatch lit 0x%llx float hw*0.5\n"
                      "patch inv 0x%llx float 2/hh\npatch whole 0x%llx hh/2\n",
                      static_cast<unsigned long long>(code), static_cast<unsigned long long>(code + 40),
                      static_cast<unsigned long long>(code + 44), static_cast<unsigned long long>(code + 48),
                      static_cast<unsigned long long>(code + 56));
        write(dir / "widescreen.cfg", cfg);
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CHECK_EQ(r.active.size(), 1u);
        CHECK_EQ(rt::patch_count(), 5u);
        CpuState cpu{};
        cpu.x[6] = code + 0xF8;
        cpu.vreg[5].hi = 7;
        auto as_float = [](uint64_t lo) {
            float f = 0;
            const uint32_t bits = uint32_t(lo);
            std::memcpy(&f, &bits, 4);
            return f;
        };
        CHECK(rt::run_patch(&cpu, code + 40));
        CHECK(rt::run_patch(&cpu, code + 44));
        CHECK(as_float(cpu.vreg[5].lo) == 480.f && cpu.vreg[5].hi == 0 && (cpu.vreg[5].lo >> 32) == 0);
        CHECK(as_float(cpu.vreg[7].lo) == 640.f);
        CHECK(rt::run_patch(&cpu, code + 48));
        CHECK(as_float(cpu.vreg[3].lo) == 2.f / 960);
        CHECK(rt::run_patch(&cpu, code + 56));
        CHECK_EQ(cpu.x[9], 480u);
        mods::set_widescreen_docked(true);  // the game's load
        CHECK(rt::run_patch(&cpu, code + 40));
        CHECK(rt::run_patch(&cpu, code + 44));
        CHECK(as_float(cpu.vreg[5].lo) == 360.f && as_float(cpu.vreg[7].lo) == 640.f);
        CHECK_EQ(cpu.halt, uint32_t{kHaltNone});
        mods::set_widescreen_docked(false);
        {  // vfov: STR St of a camera's vertical angle stores the Vert+ angle; stored again: stays
            const uint32_t str_s0 = 0xBD00'0000u | (0x34u << 10) | (8u << 5) | 0u;  // str s0, [x8, #0xd0]
            const uint32_t stur_s1 = 0xBC00'0000u | (0x1FCu << 12) | (8u << 5) | 1u;  // stur s1, [x8, #-4]
            CHECK(memory.copy_to_user(code + 60, &str_s0, 4));
            CHECK(memory.copy_to_user(code + 64, &stur_s1, 4));
            std::snprintf(cfg, sizeof cfg, "patch size 0x%llx hw hh\npatch fov 0x%llx vfov\npatch fov2 0x%llx vfov\n",
                          static_cast<unsigned long long>(code), static_cast<unsigned long long>(code + 60),
                          static_cast<unsigned long long>(code + 64));
            write(dir / "widescreen.cfg", cfg);
            mods::StartResult v;
            CHECK(mods::start(o, &v, &error));
            CHECK_EQ(rt::patch_count(), 3u);
            const float deg30 = 30.f * 3.14159265f / 180.f;
            const float want = 2.f * std::atan(std::tan(deg30 / 2) * (16.f / 9.f) / (4.f / 3.f));
            CHECK(memory.copy_to_user(code + 0x200 + 0xd0, &deg30, 4));  // the constructor's angle there already
            CpuState c{};
            c.x[8] = code + 0x200;
            uint32_t bits = 0;
            std::memcpy(&bits, &deg30, 4);
            c.vreg[0].lo = bits;
            c.vreg[1].lo = bits;
            float got = 0;
            CHECK(rt::run_patch(&c, code + 60));
            CHECK(memory.copy_from_user(&got, code + 0x200 + 0xd0, 4));
            CHECK(std::abs(got - want) < 1e-6f);
            CHECK(as_float(c.vreg[0].lo) == deg30);  // the register stays the game's
            std::memcpy(&bits, &got, 4);
            c.vreg[0].lo = bits;  // read back and stored again: not widened twice
            CHECK(rt::run_patch(&c, code + 60));
            CHECK(memory.copy_from_user(&got, code + 0x200 + 0xd0, 4));
            CHECK(std::abs(got - want) < 1e-6f);
            CHECK(rt::run_patch(&c, code + 64));  // stur
            CHECK(memory.copy_from_user(&got, code + 0x200 - 4, 4));
            CHECK(std::abs(got - want) < 1e-6f);
            mods::set_widescreen_docked(true);  // docked: the game's angle
            c.vreg[0].lo = uint64_t(std::bit_cast<uint32_t>(deg30));
            CHECK(rt::run_patch(&c, code + 60));
            CHECK(memory.copy_from_user(&got, code + 0x200 + 0xd0, 4));
            CHECK(got == deg30);
            mods::set_widescreen_docked(false);
            o.display_aspect = 21.0 / 9.0;  // wider: no patch (Hor+ keeps the angle)
            CHECK(mods::start(o, &v, &error));
            CHECK_EQ(rt::patch_count(), 1u);
            o.display_aspect = 4.0 / 3.0;
        }
        {  // cullfov: LDP of a frustum's (fovy, aspect) hands on the picture's view, memory stays
            const uint32_t ldp = 0x2D40'0000u | ((0xd0u / 4) << 15) | (0u << 10) | (9u << 5) | 1u;  // ldp s1, s0, [x9, #0xd0]
            CHECK(memory.copy_to_user(code + 68, &ldp, 4));
            std::snprintf(cfg, sizeof cfg, "patch size 0x%llx hw hh\npatch cull 0x%llx cullfov\n",
                          static_cast<unsigned long long>(code), static_cast<unsigned long long>(code + 68));
            write(dir / "widescreen.cfg", cfg);
            mods::StartResult v;
            CHECK(mods::start(o, &v, &error));
            CHECK_EQ(rt::patch_count(), 2u);
            const float deg30 = 30.f * 3.14159265f / 180.f;
            const float pair[2] = {deg30, 16.f / 9.f};
            CHECK(memory.copy_to_user(code + 0x300 + 0xd0, pair, 8));
            CpuState c{};
            c.x[9] = code + 0x300;
            c.vreg[0].hi = 5;
            CHECK(rt::run_patch(&c, code + 68));
            const float want = 2.f * std::atan(std::tan(deg30 / 2) * (16.f / 9.f) / (4.f / 3.f));
            CHECK(std::abs(as_float(c.vreg[1].lo) - want) < 1e-6f);
            CHECK(as_float(c.vreg[0].lo) == 4.f / 3.f && c.vreg[0].hi == 0);
            float mem[2] = {};
            CHECK(memory.copy_from_user(mem, code + 0x300 + 0xd0, 8));
            CHECK(mem[0] == deg30 && mem[1] == 16.f / 9.f);  // the camera keeps the game's values
            const float other[2] = {deg30, 1.f};  // not a 16:9 frustum: stays
            CHECK(memory.copy_to_user(code + 0x300 + 0xd0, other, 8));
            CHECK(rt::run_patch(&c, code + 68));
            CHECK(as_float(c.vreg[1].lo) == deg30 && as_float(c.vreg[0].lo) == 1.f);
            CHECK(memory.copy_to_user(code + 0x300 + 0xd0, pair, 8));
            mods::set_widescreen_docked(true);  // docked: the game's values
            CHECK(rt::run_patch(&c, code + 68));
            CHECK(as_float(c.vreg[1].lo) == deg30 && as_float(c.vreg[0].lo) == 16.f / 9.f);
            mods::set_widescreen_docked(false);
            o.display_aspect = 21.0 / 9.0;  // wider: the angle stays, the aspect is the picture's
            CHECK(mods::start(o, &v, &error));
            CHECK_EQ(rt::patch_count(), 2u);
            CHECK(rt::run_patch(&c, code + 68));
            const auto wide = mods::widescreen_size(21.0 / 9.0);
            CHECK(as_float(c.vreg[1].lo) == deg30);
            CHECK(std::abs(as_float(c.vreg[0].lo) - float(wide.width) / float(wide.height)) < 1e-6f);
            o.display_aspect = 16.0 / 9.0;  // 16:9: no patch
            CHECK(mods::start(o, &v, &error));
            CHECK_EQ(rt::patch_count(), 1u);
            std::snprintf(cfg, sizeof cfg, "patch size 0x%llx hw hh\npatch cull 0x%llx cullfov\n",
                          static_cast<unsigned long long>(code), static_cast<unsigned long long>(code + 64));
            write(dir / "widescreen.cfg", cfg);  // not an LDP: refused
            o.display_aspect = 4.0 / 3.0;
            mods::StartResult bad;
            std::string why;
            mods::start(o, &bad, &why);
            CHECK(bad.active.empty());
        }
        {  // const: a draw cull's "outside" bit (and w9, w8, #1) set to 0; docked the instruction's result
            const uint32_t and_w9 = 0x1200'0109u;                         // and w9, w8, #1
            const uint32_t add_x3 = 0x9100'0000u | (0x10u << 10) | (2u << 5) | 3u;  // add x3, x2, #0x10
            const uint32_t ands = 0x7200'0109u;                           // ands w9, w8, #1: refused
            CHECK(memory.copy_to_user(code + 72, &and_w9, 4));
            CHECK(memory.copy_to_user(code + 76, &add_x3, 4));
            CHECK(memory.copy_to_user(code + 80, &ands, 4));
            std::snprintf(cfg, sizeof cfg, "patch size 0x%llx hw hh\npatch cull 0x%llx const 0\npatch a 0x%llx const 0x123456789\n",
                          static_cast<unsigned long long>(code), static_cast<unsigned long long>(code + 72),
                          static_cast<unsigned long long>(code + 76));
            write(dir / "widescreen.cfg", cfg);
            mods::StartResult v;
            CHECK(mods::start(o, &v, &error));
            CHECK_EQ(rt::patch_count(), 3u);
            CpuState c{};
            c.x[8] = 0xFFFF'FFFE'0000'0002u;  // inside: mvn gives ...fffd, bit 0 set = outside
            c.x[9] = 0xFFFF'FFFF'FFFF'FFFFu;
            CHECK(rt::run_patch(&c, code + 72));
            CHECK_EQ(c.x[9], 0u);
            c.x[2] = 0x100;
            CHECK(rt::run_patch(&c, code + 76));
            CHECK_EQ(c.x[3], 0x123456789u);
            mods::set_widescreen_docked(true);  // docked: and w9, w8, #1 / add x3, x2, #0x10 as the game runs them
            c.x[8] = 0xFFFF'FFFF'FFFF'FFFDu;
            CHECK(rt::run_patch(&c, code + 72));
            CHECK_EQ(c.x[9], 1u);
            CHECK(rt::run_patch(&c, code + 76));
            CHECK_EQ(c.x[3], 0x110u);
            mods::set_widescreen_docked(false);
            o.display_aspect = 16.0 / 9.0;  // 16:9: no patch
            CHECK(mods::start(o, &v, &error));
            CHECK_EQ(rt::patch_count(), 1u);
            o.display_aspect = 4.0 / 3.0;
            std::snprintf(cfg, sizeof cfg, "patch size 0x%llx hw hh\npatch cull 0x%llx const 0 taller\npatch a 0x%llx const 1 wider\n",
                          static_cast<unsigned long long>(code), static_cast<unsigned long long>(code + 72),
                          static_cast<unsigned long long>(code + 76));
            write(dir / "widescreen.cfg", cfg);
            CHECK(mods::start(o, &v, &error));
            CHECK_EQ(rt::patch_count(), 2u);  // 4:3: only the taller one
            o.display_aspect = 21.0 / 9.0;
            CHECK(mods::start(o, &v, &error));
            CHECK_EQ(rt::patch_count(), 2u);  // 21:9: only the wider one
            CHECK(rt::run_patch(&c, code + 76));
            CHECK(!rt::run_patch(&c, code + 72));
            o.display_aspect = 4.0 / 3.0;
            // ANDS (flags), a value beyond a w register: refused.
            for (const auto& [at, value] : {std::pair{80, "0"}, std::pair{72, "0x100000000"}}) {
                std::snprintf(cfg, sizeof cfg, "patch size 0x%llx hw hh\npatch b 0x%llx const %s\n",
                              static_cast<unsigned long long>(code), static_cast<unsigned long long>(code + at), value);
                write(dir / "widescreen.cfg", cfg);
                mods::StartResult bad;
                std::string why;
                mods::start(o, &bad, &why);
                CHECK(bad.active.empty());
            }
        }
        {  // before: w9 = 0 ahead of the instruction at the site, which still runs; not docked, not 21:9
            std::snprintf(cfg, sizeof cfg, "patch size 0x%llx hw hh\npatch draw 0x%llx before w9 0 taller\n",
                          static_cast<unsigned long long>(code), static_cast<unsigned long long>(code + 84));
            write(dir / "widescreen.cfg", cfg);
            mods::StartResult v;
            CHECK(mods::start(o, &v, &error));
            CHECK_EQ(rt::patch_count(), 2u);
            CpuState c{};
            c.x[9] = 1;
            CHECK(!rt::run_patch(&c, code + 84));  // observing: the instruction still runs
            CHECK_EQ(c.x[9], 0u);
            mods::set_widescreen_docked(true);
            c.x[9] = 1;
            CHECK(!rt::run_patch(&c, code + 84));
            CHECK_EQ(c.x[9], 1u);
            mods::set_widescreen_docked(false);
            o.display_aspect = 21.0 / 9.0;
            CHECK(mods::start(o, &v, &error));
            CHECK_EQ(rt::patch_count(), 1u);
            o.display_aspect = 4.0 / 3.0;
            std::snprintf(cfg, sizeof cfg, "patch size 0x%llx hw hh\npatch draw 0x%llx before w9 0x100000000\n",
                          static_cast<unsigned long long>(code), static_cast<unsigned long long>(code + 84));
            write(dir / "widescreen.cfg", cfg);
            mods::StartResult bad;
            std::string why;
            mods::start(o, &bad, &why);
            CHECK(bad.active.empty());
        }
        // Not a float load: refused.
        std::snprintf(cfg, sizeof cfg, "patch size 0x%llx hw hh\npatch bad 0x%llx float hh/2\n",
                      static_cast<unsigned long long>(code), static_cast<unsigned long long>(code + 4));
        write(dir / "widescreen.cfg", cfg);
        mods::StartResult bad;
        std::string why;
        mods::start(o, &bad, &why);
        CHECK(bad.active.empty());
        o.display_aspect = 21.0 / 9.0;
    }
    {  // movie: a video stretched over the 4:3 picture fits with bars, over 21:9 fills (cropped)
        std::snprintf(cfg, sizeof cfg, "patch size 0x%llx hw hh\nmovie clip 16 9 fit\nmovie wide 32 18 fill\n",
                      static_cast<unsigned long long>(code));
        write(dir / "widescreen.cfg", cfg);
        o.display_aspect = 4.0 / 3.0;
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CHECK_EQ(r.active.size(), 1u);
        auto frame = [](uint32_t w, uint32_t h, int base = 100) {
            std::vector<uint8_t> f(size_t(w) * h, 200);
            for (uint32_t y = 0; y < h; ++y)
                for (uint32_t x = 0; x < w; ++x) f[y * w + x] = uint8_t(base + 8 * y);
            f.resize(f.size() + size_t((w + 1) / 2) * ((h + 1) / 2) * 2, 90);
            return f;
        };
        std::vector<uint8_t> f = frame(16, 9, 20);
        CHECK(mods::widescreen_movie_frame(f.data(), f.size(), 16, 9));
        CHECK(f[0] == 16 && f[8 * 16 + 15] == 16);           // bars: black luma at the top and bottom
        CHECK(f[4 * 16 + 3] == 52);                          // the middle row stays
        CHECK(f[1 * 16] == 20 && f[7 * 16] == 84);           // the whole content squeezed between them
        CHECK(f[144] == 128 && f[144 + 4 * 16 + 15] == 128);  // grey chroma in the bars (8x5 UV)
        CHECK(f[144 + 2 * 16] == 90);
        f = frame(16, 9, 150);  // light at the top and bottom only: bars stay black
        CHECK(mods::widescreen_movie_frame(f.data(), f.size(), 16, 9));
        CHECK(f[0] == 16);
        for (int i = 0; i < 3; ++i) {  // faded to white (3 frames white all over): white bars
            f.assign(f.size(), 230);
            CHECK(mods::widescreen_movie_frame(f.data(), f.size(), 16, 9));
            CHECK(f[0] == (i < 2 ? 16 : 230) && f[144] == 128);  // neutral chroma in the bars
        }
        f = frame(16, 9, 20);
        CHECK(mods::widescreen_movie_frame(f.data(), f.size(), 16, 9));  // dark again: black
        CHECK(f[0] == 16);
        std::vector<uint8_t> other = frame(32, 18), same = other;
        CHECK(!mods::widescreen_movie_frame(other.data(), other.size(), 20, 9));  // no line for this size
        CHECK(!mods::widescreen_movie_frame(other.data(), 10, 32, 18));           // too small
        mods::set_widescreen_docked(true);
        CHECK(!mods::widescreen_movie_frame(other.data(), other.size(), 32, 18));  // TV mode: as the game
        mods::set_widescreen_docked(false);
        CHECK(other == same);
        std::vector<uint8_t> g = frame(32, 18);
        CHECK(mods::widescreen_movie_frame(g.data(), g.size(), 32, 18));  // fill at 4:3: cropped at the sides
        CHECK(g[0] == 100 && g[17 * 32 + 31] == 236);                  // no bars, every row kept
        CHECK(g[3 * 32] > g[3 * 32 + 31] - 1 && g[576] == 90);
        o.display_aspect = 21.0 / 9.0;  // wider: the game draws its video unstretched in the middle
        CHECK(mods::start(o, &r, &error));
        f = frame(16, 9, 20);
        const std::vector<uint8_t> before = f;
        CHECK(!mods::widescreen_movie_frame(f.data(), f.size(), 16, 9));
        CHECK(f == before);
    }
    {  // size rules: a description in guest memory, registers with a condition
        const uint64_t desc = code + rt::kPageSize;
        CHECK(memory.map(desc, rt::kPageSize, rt::MemState::Normal, rt::kPermRW).ok());
        std::snprintf(cfg, sizeof cfg, "patch size 0x%llx hw hh\npatch b 0x%llx size [x1+0x10] [x1+0x14] skip 480x270\n"
                      "patch r 0x%llx size w2 w3 x4&8 skip 160x90\npatch cam 0x%llx proj [x5+0x40] rect [x5+0x80] zone 278 log 4\n"
                      "patch vp 0x%llx size [x6+8]f [x6+12]f\npatch ui 0x%llx rect [x7+0x10]\npatch el 0x%llx rect [x8+0x10] model [x8+0x40] movie 1280x1280\n"
                      "patch tx 0x%llx tex [x9+0x70] rect [x9+0x210] model [x9+0x40]\nrepeat grid 1280 360 u\n"
                      "patch et 0x%llx rect [x10+0x210] model [x10+0x40] tex [x10+0x70] only 1280x360,1280x606 edge 910x45 zone 278 -278 log 8\n",
                      static_cast<unsigned long long>(code), static_cast<unsigned long long>(code + 8),
                      static_cast<unsigned long long>(code + 12), static_cast<unsigned long long>(code + 16),
                      static_cast<unsigned long long>(code + 20), static_cast<unsigned long long>(code + 24), static_cast<unsigned long long>(code + 28),
                      static_cast<unsigned long long>(code + 32), static_cast<unsigned long long>(code + 36));
        write(dir / "widescreen.cfg", cfg);
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CHECK_EQ(r.active.size(), 1u);
        CHECK_EQ(rt::patch_count(), 9u);
        bool ru = false, rv = true;
        CHECK(rt::repeat_hint(1280, 360, &ru, &rv) && ru && !rv);
        CHECK(!rt::repeat_hint(1280, 720, &ru, &rv));
        auto put = [&](uint32_t w, uint32_t h) {
            const uint32_t wh[2] = {w, h};
            CHECK(memory.copy_to_user(desc + 0x10, wh, 8));
        };
        auto got = [&] {
            uint32_t wh[2] = {};
            CHECK(memory.copy_from_user(wh, desc + 0x10, 8));
            return uint64_t{wh[0]} | uint64_t{wh[1]} << 32;
        };
        CpuState cpu{};
        cpu.x[1] = desc;
        put(1280, 720);
        CHECK(!rt::run_patch(&cpu, code + 8));  // observing: the instruction still runs
        CHECK_EQ(got(), 1680u | uint64_t{720} << 32);
        CHECK(!rt::run_patch(&cpu, code + 8));  // handed in again: not scaled twice
        CHECK_EQ(got(), 1680u | uint64_t{720} << 32);
        put(640, 360);
        rt::run_patch(&cpu, code + 8);
        CHECK_EQ(got(), 840u | uint64_t{360} << 32);
        put(480, 270);  // listed after `skip`: stays
        rt::run_patch(&cpu, code + 8);
        CHECK_EQ(got(), 480u | uint64_t{270} << 32);
        // TV mode: the mod rests, the game's 16:9 sizes stay and nothing is reported active.
        mods::set_widescreen_docked(true);
        CHECK(mods::widescreen_active().width == 0 && mods::widescreen_active().height == 0);
        put(320, 180);
        rt::run_patch(&cpu, code + 8);
        CHECK_EQ(got(), 320u | uint64_t{180} << 32);
        mods::set_widescreen_docked(false);
        CHECK(mods::widescreen_active().width == 1680);
        rt::run_patch(&cpu, code + 8);
        CHECK_EQ(got(), 420u | uint64_t{180} << 32);
        put(256, 256);
        rt::run_patch(&cpu, code + 8);
        CHECK_EQ(got(), 256u | uint64_t{256} << 32);
        cpu.x[2] = 0xFFFF'FFFF'0000'0140u;  // w2 = 320: the write zero-extends
        cpu.x[3] = 180;
        cpu.x[4] = 4;  // condition not met
        rt::run_patch(&cpu, code + 12);
        CHECK_EQ(cpu.x[2], 0xFFFF'FFFF'0000'0140u);
        cpu.x[4] = 0xC;
        rt::run_patch(&cpu, code + 12);
        CHECK_EQ(cpu.x[2], 420u);
        CHECK_EQ(cpu.x[3], 180u);
        cpu.x[2] = 160;  // condition met, but skipped
        cpu.x[3] = 90;
        rt::run_patch(&cpu, code + 12);
        CHECK_EQ(cpu.x[2], 160u);
        CHECK_EQ(cpu.x[3], 90u);
        const float proj[16] = {1.f, 0, 0, 0, 0, 16.f / 9.f, 0, 0, 0, 0, -1, -0.2f, 0, 0, -1, 0};
        CHECK(memory.copy_to_user(desc + 0x40, proj, 64));
        cpu.x[5] = desc;
        CHECK(!rt::run_patch(&cpu, code + 16));
        float got_proj[16] = {};
        CHECK(memory.copy_from_user(got_proj, desc + 0x40, 64));
        CHECK(std::abs(got_proj[0] - (16.f / 9.f) / (1680.f / 720.f)) < 1e-5f && got_proj[5] == 16.f / 9.f);
        // 2D from the corner: the translation in the row goes back to guest memory too.
        const float ortho[16] = {2.f / 1280, 0, 0, -1, 0, -2.f / 720, 0, 1, 0, 0, -1, 0, 0, 0, 0, 1};
        CHECK(memory.copy_to_user(desc + 0x40, ortho, 64));
        CHECK(!rt::run_patch(&cpu, code + 16));
        CHECK(memory.copy_from_user(got_proj, desc + 0x40, 64));
        CHECK(std::abs(got_proj[3] + 1280.f / 1680) < 1e-6f && std::abs(got_proj[0] - 2.f / 1680) < 1e-8f);
        // The element rectangle next to it: full width reaches the edges, a smaller one stays.
        const float full[4] = {1280, 720, -640, 360}, small[4] = {300, 36, -150, -300};
        float got_rect4[4] = {};
        CHECK(memory.copy_to_user(desc + 0x80, full, 16));
        CHECK(!rt::run_patch(&cpu, code + 16));
        CHECK(memory.copy_from_user(got_rect4, desc + 0x80, 16));
        CHECK(got_rect4[0] == 1680.f && got_rect4[1] == 720.f && got_rect4[2] == -840.f && got_rect4[3] == 360.f);
        CHECK(!rt::run_patch(&cpu, code + 16));  // again: stays
        CHECK(memory.copy_from_user(got_rect4, desc + 0x80, 16));
        CHECK(got_rect4[0] == 1680.f && got_rect4[2] == -840.f);
        CHECK(memory.copy_to_user(desc + 0x110, small, 16));
        cpu.x[7] = desc + 0x100;
        CHECK(!rt::run_patch(&cpu, code + 24));
        CHECK(memory.copy_from_user(got_rect4, desc + 0x110, 16));
        CHECK(got_rect4[0] == 300.f && got_rect4[2] == -150.f);
        const float bar[4] = {1280, 36, -640, -324};  // a bar across the screen
        CHECK(memory.copy_to_user(desc + 0x110, bar, 16));
        CHECK(!rt::run_patch(&cpu, code + 24));
        CHECK(memory.copy_from_user(got_rect4, desc + 0x110, 16));
        CHECK(got_rect4[0] == 1680.f && got_rect4[1] == 36.f && got_rect4[2] == -840.f && got_rect4[3] == -324.f);
        // rect + model: the model row is widened, the rectangle stays.
        const uint64_t el = desc + 0x200;
        const float sq[4] = {1280, 1280, -640, 640}, rows[8] = {0, -1, 0, 0, 1, 0, 0, -279};
        CHECK(memory.copy_to_user(el + 0x10, sq, 16));
        CHECK(memory.copy_to_user(el + 0x40, rows, 32));
        cpu.x[8] = el;
        float got_rows[8] = {}, got_sq[4] = {};
        // `movie 1280x1280`: while a video plays the element stays as it is.
        std::vector<uint8_t> dark(16 * 9 + 8 * 5 * 2, 16);
        CHECK(!mods::widescreen_movie_frame(dark.data(), dark.size(), 16, 9));  // a dark frame (no movie line: unchanged)
        CHECK(!rt::run_patch(&cpu, code + 28));
        CHECK(memory.copy_from_user(got_rows, el + 0x40, 32));
        CHECK(got_rows[1] == -1.f);
        std::vector<uint8_t> light(16 * 9 + 8 * 5 * 2, 235);  // faded to white: widened as usual
        for (int i = 0; i < 3; ++i) CHECK(!mods::widescreen_movie_frame(light.data(), light.size(), 16, 9));
        CHECK(!rt::run_patch(&cpu, code + 28));
        CHECK(memory.copy_from_user(got_rows, el + 0x40, 32));
        CHECK(std::abs(got_rows[1] + 1680.f / 1280) < 1e-6f);
        CHECK(memory.copy_to_user(el + 0x40, rows, 32));
        CHECK(!mods::widescreen_movie_frame(dark.data(), dark.size(), 16, 9));
        CHECK(!rt::run_patch(&cpu, code + 28));
        CHECK(memory.copy_from_user(got_rows, el + 0x40, 32));
        CHECK(got_rows[1] == -1.f);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));  // the video is over
        CHECK(!rt::run_patch(&cpu, code + 28));
        CHECK(memory.copy_from_user(got_rows, el + 0x40, 32));
        CHECK(memory.copy_from_user(got_sq, el + 0x10, 16));
        CHECK(std::abs(got_rows[1] + 1680.f / 1280) < 1e-6f && got_rows[7] == -279.f && got_sq[0] == 1280.f);
        // tex: buffer layout like the game's (model +0x40, texture rows +0x70, rectangle +0x210).
        const uint64_t buf = desc + 0x400;
        const float g[4] = {1280, 360, -640, 180}, wrows[8] = {1680.f / 1280, 0, 0, 0, 0, 1, 0, 100},
                    trows[8] = {1, 0, 0, 0, 0, -1, 0, 1};
        CHECK(memory.copy_to_user(buf + 0x210, g, 16));
        CHECK(memory.copy_to_user(buf + 0x40, wrows, 32));
        CHECK(memory.copy_to_user(buf + 0x70, trows, 32));
        cpu.x[9] = buf;
        CHECK(!rt::run_patch(&cpu, code + 32));
        CHECK(!rt::run_patch(&cpu, code + 32));  // the same buffer again: not widened twice
        float got_t[8] = {};
        CHECK(memory.copy_from_user(got_t, buf + 0x70, 32));
        CHECK(std::abs(got_t[0] - 1.3125f) < 1e-6f && std::abs(got_t[3] + 0.15625f) < 1e-6f);
        CHECK(memory.copy_to_user(buf + 0x70, trows, 32));  // the game writes the rows again
        CHECK(!rt::run_patch(&cpu, code + 32));
        CHECK(memory.copy_from_user(got_t, buf + 0x70, 32));
        CHECK(std::abs(got_t[0] - 1.3125f) < 1e-6f);
        // rect + model + tex in one line: model widened first, then the texture rows.
        const uint64_t buf2 = desc + 0x700;
        const float ident[8] = {1, 0, 0, 0, 0, 1, 0, 100};
        CHECK(memory.copy_to_user(buf2 + 0x210, g, 16));
        CHECK(memory.copy_to_user(buf2 + 0x40, ident, 32));
        CHECK(memory.copy_to_user(buf2 + 0x70, trows, 32));
        cpu.x[10] = buf2;
        CHECK(!rt::run_patch(&cpu, code + 36));
        float got_m[8] = {};
        CHECK(memory.copy_from_user(got_m, buf2 + 0x40, 32));
        CHECK(memory.copy_from_user(got_t, buf2 + 0x70, 32));
        CHECK(std::abs(got_m[0] - 1.3125f) < 1e-6f && std::abs(got_t[0] - 1.3125f) < 1e-6f);
        CHECK(memory.copy_to_user(buf2 + 0x70, trows, 32));  // next copy: model stays widened, rows again
        CHECK(!rt::run_patch(&cpu, code + 36));
        CHECK(memory.copy_from_user(got_m, buf2 + 0x40, 32));
        CHECK(memory.copy_from_user(got_t, buf2 + 0x70, 32));
        CHECK(std::abs(got_m[0] - 1.3125f) < 1e-6f && std::abs(got_t[0] - 1.3125f) < 1e-6f);
        // A framed 1280x720 element (not listed): widened, but its texture stays (no doubled frame).
        const uint64_t buf3 = desc + 0xa00;
        const float frame[4] = {1280, 720, -640, 360};
        CHECK(memory.copy_to_user(buf3 + 0x210, frame, 16));
        CHECK(memory.copy_to_user(buf3 + 0x40, ident, 32));
        CHECK(memory.copy_to_user(buf3 + 0x70, trows, 32));
        cpu.x[10] = buf3;
        CHECK(!rt::run_patch(&cpu, code + 36));
        CHECK(memory.copy_from_user(got_m, buf3 + 0x40, 32));
        CHECK(memory.copy_from_user(got_t, buf3 + 0x70, 32));
        CHECK(std::abs(got_m[0] - 1.3125f) < 1e-6f && got_t[0] == 1.f && got_t[3] == 0.f);
        // A listed header bar at the left edge (edge 910x45): model and rows move, also on the next copy.
        const uint64_t buf4 = desc + 0xd00;
        const float header_bar[4] = {910, 45, -640, 360}, plain_rows[8] = {1, 0, 0, 0, 0, 1, 0, 0};
        CHECK(memory.copy_to_user(buf4 + 0x210, header_bar, 16));
        CHECK(memory.copy_to_user(buf4 + 0x40, plain_rows, 32));
        CHECK(memory.copy_to_user(buf4 + 0x70, trows, 32));
        cpu.x[10] = buf4;
        for (int pass = 0; pass < 2; ++pass) {
            CHECK(memory.copy_to_user(buf4 + 0x70, trows, 32));  // the game writes the rows each copy
            CHECK(!rt::run_patch(&cpu, code + 36));
            CHECK(memory.copy_from_user(got_m, buf4 + 0x40, 32));
            CHECK(memory.copy_from_user(got_t, buf4 + 0x70, 32));
            CHECK(std::abs(got_m[0] * -640 + got_m[3] + 840) < 1e-3f);
            CHECK(std::abs(got_t[3] + 200.f / 910) < 1e-5f);
        }
        CHECK(!rt::run_patch(&cpu, code + 36));  // rows unchanged since: not moved twice
        CHECK(memory.copy_from_user(got_t, buf4 + 0x70, 32));
        CHECK(std::abs(got_t[3] + 200.f / 910) < 1e-5f);
        const float rect[4] = {0, 0, 1280, 720};  // a viewport rectangle (floats)
        CHECK(memory.copy_to_user(desc + 0x100, rect, 16));
        cpu.x[6] = desc + 0x100;
        rt::run_patch(&cpu, code + 20);
        float got_rect[4] = {};
        CHECK(memory.copy_from_user(got_rect, desc + 0x100, 16));
        CHECK(got_rect[2] == 1680.f && got_rect[3] == 720.f);
    }
    {  // zone at 4:3, gated by a footer: only while it is drawn
        const uint64_t desc = code + rt::kPageSize;
        std::snprintf(cfg, sizeof cfg, "patch size 0x%llx hw hh\npatch z 0x%llx rect [x10+0x210] model [x10+0x40] zone 278 -278 when 1280x36\n",
                      static_cast<unsigned long long>(code), static_cast<unsigned long long>(code + 36));
        write(dir / "widescreen.cfg", cfg);
        o.display_aspect = 4.0 / 3.0;
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CHECK_EQ(rt::patch_count(), 2u);
        const float label[4] = {200, 30, -300, -330}, footer[4] = {1280, 36, -640, -324},
                    ident[8] = {1, 0, 0, 0, 0, 1, 0, 0};
        const uint64_t a = desc + 0x400, b = desc + 0x800;
        CHECK(memory.copy_to_user(a + 0x210, label, 16));
        CHECK(memory.copy_to_user(a + 0x40, ident, 32));
        CHECK(memory.copy_to_user(b + 0x210, footer, 16));
        CHECK(memory.copy_to_user(b + 0x40, ident, 32));
        CpuState cpu{};
        float got[8] = {};
        cpu.x[10] = a;
        CHECK(!rt::run_patch(&cpu, code + 36));  // no footer yet (battle): stays
        CHECK(memory.copy_from_user(got, a + 0x40, 32));
        CHECK(got[7] == 0.f);
        cpu.x[10] = b;
        CHECK(!rt::run_patch(&cpu, code + 36));  // the footer itself moves down
        CHECK(memory.copy_from_user(got, b + 0x40, 32));
        CHECK(got[7] == -120.f);
        cpu.x[10] = a;
        CHECK(!rt::run_patch(&cpu, code + 36));  // now its group follows
        CHECK(memory.copy_from_user(got, a + 0x40, 32));
        CHECK(got[7] == -120.f);
        o.display_aspect = 21.0 / 9.0;
    }
    {  // two zones, each with its own gate: battle corners (side) and Pokédex (spread); touches follow the open one
        const uint64_t desc = code + rt::kPageSize;
        std::snprintf(cfg, sizeof cfg,
                      "patch size 0x%llx hw hh\npatch z 0x%llx rect [x10+0x210] model [x10+0x40] zone 278 -278 spread when 1280x36 "
                      "zone 90 -90 side 280 with 216x128 when 280x264,350x174 log 4\n"
                      "patch t 0x%llx proj [x5+0x40] zone 278 spread when 1280x36 zone 90 side 280 when 280x264 log 4\n",
                      static_cast<unsigned long long>(code), static_cast<unsigned long long>(code + 36),
                      static_cast<unsigned long long>(code + 16));
        write(dir / "widescreen.cfg", cfg);
        o.display_aspect = 4.0 / 3.0;
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CHECK_EQ(rt::patch_count(), 3u);
        const float box[4] = {280, 264, -640, -96}, row[4] = {60, 40, 200, -180}, footer[4] = {1280, 36, -640, -324},
                    ident[8] = {1, 0, 0, 0, 0, 1, 0, 0};
        const uint64_t a = desc + 0x400, b = desc + 0x800, c = desc + 0xc00;
        CHECK(memory.copy_to_user(a + 0x210, box, 16));
        CHECK(memory.copy_to_user(a + 0x40, ident, 32));
        CHECK(memory.copy_to_user(b + 0x210, row, 16));
        CHECK(memory.copy_to_user(b + 0x40, ident, 32));
        CHECK(memory.copy_to_user(c + 0x210, footer, 16));
        CHECK(memory.copy_to_user(c + 0x40, ident, 32));
        CpuState cpu{};
        float got[8] = {};
        cpu.x[10] = a;
        CHECK(!rt::run_patch(&cpu, code + 36));  // the battle box opens its own gate and moves to the corner
        CHECK(memory.copy_from_user(got, a + 0x40, 32));
        CHECK(got[7] == -120.f);
        cpu.x[10] = b;
        CHECK(!rt::run_patch(&cpu, code + 36));  // a middle element in battle: the Pokédex zone is shut
        CHECK(memory.copy_from_user(got, b + 0x40, 32));
        CHECK(got[7] == 0.f);
        float u = 140.f / 1280, v = (480.f + 300) / 960;  // touch on the box at the new bottom left
        mods::widescreen_touch(&u, &v, mods::widescreen_active());
        CHECK(std::abs(v - (360.f + 180) / 720) < 1e-5f);
        cpu.x[10] = c;
        CHECK(!rt::run_patch(&cpu, code + 36));  // footer: Pokédex gate open
        cpu.x[10] = b;
        CHECK(!rt::run_patch(&cpu, code + 36));
        CHECK(memory.copy_from_user(got, b + 0x40, 32));
        CHECK(std::abs(got[7] - (-200.f / 3)) < 1e-3f);
        std::snprintf(cfg, sizeof cfg, "patch size 0x%llx hw hh\npatch z 0x%llx rect [x10+0x210] model [x10+0x40] zone 90 -90 side -5\n",
                      static_cast<unsigned long long>(code), static_cast<unsigned long long>(code + 36));
        write(dir / "widescreen.cfg", cfg);
        mods::StartResult bad;
        CHECK(mods::start(o, &bad, &error));
        CHECK(bad.active.empty());
        o.display_aspect = 21.0 / 9.0;
    }
    {  // text matrix changed while its gate was shut: when the gate opens it moves to the edge (once)
        const uint64_t desc = code + rt::kPageSize;
        std::snprintf(cfg, sizeof cfg,
                      "patch size 0x%llx hw hh\npatch t 0x%llx proj [x5+0x40] zone 278 when 1280x36\n"
                      "patch z 0x%llx rect [x10+0x210] model [x10+0x40] zone 278 -278 when 1280x36\n",
                      static_cast<unsigned long long>(code), static_cast<unsigned long long>(code + 16),
                      static_cast<unsigned long long>(code + 36));
        write(dir / "widescreen.cfg", cfg);
        o.display_aspect = 4.0 / 3.0;
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CHECK_EQ(rt::patch_count(), 3u);
        const float text[16] = {2.f / 1280, 0, 0, 0, 0, 2.f / 720, 0, -300.f / 360, 0, 0, 1, 0, 0, 0, 0, 1};
        const float footer[4] = {1280, 36, -640, -324}, ident[8] = {1, 0, 0, 0, 0, 1, 0, 0};
        const uint64_t t = desc + 0x400, f = desc + 0x800, u = desc + 0xc00;
        CHECK(memory.copy_to_user(t + 0x40, text, 64));
        CHECK(memory.copy_to_user(u + 0x40, text, 64));
        CHECK(memory.copy_to_user(f + 0x210, footer, 16));
        CHECK(memory.copy_to_user(f + 0x40, ident, 32));
        CpuState cpu{};
        float got[16] = {};
        cpu.x[5] = t;
        CHECK(!rt::run_patch(&cpu, code + 16));  // built before the footer: Vert+ only, stays in place
        CHECK(memory.copy_from_user(got, t + 0x40, 64));
        CHECK(std::abs(got[7] + 0.625f) < 1e-6f);
        CHECK(!rt::run_patch(&cpu, code + 16));  // gate still shut: nothing
        CHECK(memory.copy_from_user(got, t + 0x40, 64));
        CHECK(std::abs(got[7] + 0.625f) < 1e-6f);
        cpu.x[5] = u;
        CHECK(!rt::run_patch(&cpu, code + 16));
        float other[16] = {};  // the game rebuilds this one with its own values: not ours any more
        CHECK(memory.copy_from_user(other, u + 0x40, 64));
        other[7] = -0.5f;
        CHECK(memory.copy_to_user(u + 0x40, other, 64));
        cpu.x[10] = f;
        CHECK(!rt::run_patch(&cpu, code + 36));  // footer: gate opens, the waiting matrix moves at once
        CHECK(memory.copy_from_user(got, t + 0x40, 64));
        CHECK(std::abs(got[7] + 0.875f) < 1e-6f);
        CHECK(memory.copy_from_user(got, u + 0x40, 64));
        CHECK(got[7] == -0.5f);  // rewritten by the game: left alone
        cpu.x[5] = t;
        CHECK(!rt::run_patch(&cpu, code + 16));  // a later pass: stays at the new bottom edge
        CHECK(memory.copy_from_user(got, t + 0x40, 64));
        CHECK(std::abs(got[7] + 0.875f) < 1e-6f);
        CHECK(std::abs(got[5] - 0.75f * 2.f / 720) < 1e-8f);
        CHECK(!rt::run_patch(&cpu, code + 16));  // not twice
        CHECK(memory.copy_from_user(got, t + 0x40, 64));
        CHECK(std::abs(got[7] + 0.875f) < 1e-6f);
        cpu.x[5] = u;
        CHECK(!rt::run_patch(&cpu, code + 16));
        CHECK(memory.copy_from_user(got, u + 0x40, 64));
        CHECK(got[7] == -0.5f);
        o.display_aspect = 21.0 / 9.0;
    }
    {  // proj line `cull above Y when WxH`: text beyond the list frame is not drawn while the frame is
        const uint64_t desc = code + rt::kPageSize;
        std::snprintf(cfg, sizeof cfg,
                      "patch size 0x%llx hw hh\npatch t 0x%llx proj [x5+0x40] cull above 300 when 192x18\n"
                      "patch z 0x%llx rect [x10+0x210] model [x10+0x40]\n",
                      static_cast<unsigned long long>(code), static_cast<unsigned long long>(code + 16),
                      static_cast<unsigned long long>(code + 36));
        write(dir / "widescreen.cfg", cfg);
        o.display_aspect = 4.0 / 3.0;
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        const float strip[16] = {2.f / 1280, 0, 0, 0, 0, 2.f / 720, 0, 330.f / 360, 0, 0, 1, 0, 0, 0, 0, 1};
        const float row[16] = {2.f / 1280, 0, 0, 0, 0, 2.f / 720, 0, 100.f / 360, 0, 0, 1, 0, 0, 0, 0, 1};
        const float bar[4] = {192, 18, -96, 9}, ident[8] = {1, 0, 0, 0, 0, 1, 0, 0};
        const uint64_t t = desc + 0x400, u = desc + 0x800, f = desc + 0xc00;
        CHECK(memory.copy_to_user(t + 0x40, strip, 64));
        CHECK(memory.copy_to_user(u + 0x40, row, 64));
        CHECK(memory.copy_to_user(f + 0x210, bar, 16));
        CHECK(memory.copy_to_user(f + 0x40, ident, 32));
        CpuState cpu{};
        float got[16] = {};
        cpu.x[5] = t;
        CHECK(!rt::run_patch(&cpu, code + 16));  // no frame drawn: Vert+ only, stays
        CHECK(memory.copy_from_user(got, t + 0x40, 64));
        CHECK(std::abs(got[7] - 0.6875f) < 1e-6f);  // y 330 in the new picture
        cpu.x[10] = f;
        CHECK(!rt::run_patch(&cpu, code + 36));  // the frame passes: gate open
        CHECK(!rt::run_patch(&cpu, code + 16));  // the kept matrix goes
        CHECK(memory.copy_from_user(got, t + 0x40, 64));
        CHECK(std::all_of(got, got + 16, [](float v) { return v == 0; }));
        CHECK(!rt::run_patch(&cpu, code + 16));  // and stays gone
        CHECK(memory.copy_from_user(got, t + 0x40, 64));
        CHECK(got[0] == 0 && got[15] == 0);
        cpu.x[5] = u;
        CHECK(!rt::run_patch(&cpu, code + 16));  // inside the limit: drawn
        CHECK(memory.copy_from_user(got, u + 0x40, 64));
        CHECK(std::abs(got[7] - 100.f / 360 * 0.75f) < 1e-6f);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));  // frame gone: gate shut
        cpu.x[5] = t;
        CHECK(!rt::run_patch(&cpu, code + 16));  // the game's matrix comes back
        CHECK(memory.copy_from_user(got, t + 0x40, 64));
        CHECK(std::abs(got[7] - 0.6875f) < 1e-6f);
        CHECK(std::abs(got[5] - 0.75f * 2.f / 720) < 1e-8f);
        std::snprintf(cfg, sizeof cfg, "patch t 0x%llx proj [x5+0x40] cull when 192x18\n",
                      static_cast<unsigned long long>(code + 16));
        write(dir / "widescreen.cfg", cfg);
        mods::StartResult bad;
        CHECK(mods::start(o, &bad, &error));
        CHECK(bad.active.empty());  // no limit: refused
        // a band in x and y, taller pictures only: the money text beside the counts stays
        std::snprintf(cfg, sizeof cfg,
                      "patch size 0x%llx hw hh\npatch t 0x%llx proj [x5+0x40] cull y 345 395 x -420 1000 taller when 192x18\n"
                      "patch z 0x%llx rect [x10+0x210] model [x10+0x40]\n",
                      static_cast<unsigned long long>(code), static_cast<unsigned long long>(code + 16),
                      static_cast<unsigned long long>(code + 36));
        write(dir / "widescreen.cfg", cfg);
        for (const double aspect : {4.0 / 3.0, 1568.0 / 720.0}) {
            o.display_aspect = aspect;
            CHECK(mods::start(o, &r, &error));
            const bool taller = aspect < 16.0 / 9.0;
            const float yk = taller ? 0.75f : 1.0f, xk = taller ? 1.0f : 1280.f / 1568;
            float count[16] = {2.f / 1280, 0, 0, 100.f / 640, 0, 2.f / 720, 0, 357.f / 360, 0, 0, 1, 0, 0, 0, 0, 1};
            float money[16] = {2.f / 1280, 0, 0, -500.f / 640, 0, 2.f / 720, 0, 357.f / 360, 0, 0, 1, 0, 0, 0, 0, 1};
            CHECK(memory.copy_to_user(t + 0x40, count, 64));
            CHECK(memory.copy_to_user(u + 0x40, money, 64));
            CHECK(memory.copy_to_user(f + 0x210, bar, 16));
            CHECK(memory.copy_to_user(f + 0x40, ident, 32));
            cpu.x[10] = f;
            CHECK(!rt::run_patch(&cpu, code + 36));  // gate open
            cpu.x[5] = t;
            CHECK(!rt::run_patch(&cpu, code + 16));
            CHECK(memory.copy_from_user(got, t + 0x40, 64));
            if (taller)
                CHECK(std::all_of(got, got + 16, [](float v) { return v == 0; }));
            else
                CHECK(std::abs(got[7] - 357.f / 360 * yk) < 1e-6f && std::abs(got[3] - 100.f / 640 * xk) < 1e-6f);
            cpu.x[5] = u;
            CHECK(!rt::run_patch(&cpu, code + 16));  // x -500: outside the band
            CHECK(memory.copy_from_user(got, u + 0x40, 64));
            CHECK(std::abs(got[7] - 357.f / 360 * yk) < 1e-6f);
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
        o.display_aspect = 21.0 / 9.0;
    }
    {  // `crop WxH`: while the title logo is drawn a taller picture keeps the camera's height (Hor-)
        const uint64_t desc = code + rt::kPageSize;
        std::snprintf(cfg, sizeof cfg,
                      "touch render 1\npatch size 0x%llx hw hh\npatch cam 0x%llx proj [x5+0x40] crop 800x214,900x150 hold 600 ease 0\n"
                      "patch z 0x%llx rect [x10+0x210] model [x10+0x40]\n",
                      static_cast<unsigned long long>(code), static_cast<unsigned long long>(code + 16),
                      static_cast<unsigned long long>(code + 36));
        write(dir / "widescreen.cfg", cfg);
        o.display_aspect = 4.0 / 3.0;
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CHECK_EQ(rt::patch_count(), 3u);
        {  // `touch render 1`: touches scale to the render size of the running picture only
            const auto [sx, sy] = mods::widescreen_touch_scale(mods::widescreen_active());
            CHECK(std::abs(sx - 1.f) < 1e-6f && std::abs(sy - 4.f / 3.f) < 1e-6f);
            const auto [ox, oy] = mods::widescreen_touch_scale({1568, 720});
            CHECK(ox == 1.f && oy == 1.f);
            float u = 0.94f, v = 0.8f;  // no zone moves it back: hid scales the centred position
            mods::widescreen_touch(&u, &v, mods::widescreen_active());
            CHECK(std::abs(u - 0.94f) < 1e-6f && std::abs(v - (0.8f * 960 - 120) / 720) < 1e-5f);
        }
        const float f = 1.f / std::tan(15.f * 3.14159265f / 180.f);
        const float camera[16] = {f / (16.f / 9.f), 0, 0, 0, 0, f, 0, 0, 0, 0, -1.f, -1, 0, 0, -0.2f, 0};
        const float logo[4] = {900, 150, -450, 75}, ident[8] = {1, 0, 0, 0, 0, 1, 0, 0};
        const uint64_t c = desc + 0x400, l = desc + 0x800;
        CHECK(memory.copy_to_user(c + 0x40, camera, 64));
        CHECK(memory.copy_to_user(l + 0x210, logo, 16));
        CHECK(memory.copy_to_user(l + 0x40, ident, 32));
        CpuState cpu{};
        float got[16] = {};
        cpu.x[5] = c;
        CHECK(!rt::run_patch(&cpu, code + 16));  // no logo yet: Vert+
        CHECK(memory.copy_from_user(got, c + 0x40, 64));
        CHECK(std::abs(got[0] - camera[0]) < 1e-6f);
        CHECK(std::abs(got[5] / got[0] - 4.f / 3.f) < 1e-4f);
        CHECK(!rt::run_patch(&cpu, code + 16));  // the game keeps it: stays
        CHECK(memory.copy_from_user(got, c + 0x40, 64));
        CHECK(std::abs(got[0] - camera[0]) < 1e-6f);
        cpu.x[10] = l;
        CHECK(!rt::run_patch(&cpu, code + 36));  // the logo passes
        cpu.x[5] = c;
        CHECK(!rt::run_patch(&cpu, code + 16));  // the kept matrix switches to Hor-
        CHECK(memory.copy_from_user(got, c + 0x40, 64));
        CHECK(std::abs(got[5] - f) < 1e-6f);  // the game's vertical view
        CHECK(std::abs(got[5] / got[0] - 4.f / 3.f) < 1e-4f);
        CHECK(memory.copy_to_user(c + 0x40, camera, 64));  // the game builds the camera again
        CHECK(!rt::run_patch(&cpu, code + 16));
        CHECK(memory.copy_from_user(got, c + 0x40, 64));
        CHECK(std::abs(got[5] - f) < 1e-6f);
        CHECK(std::abs(got[5] / got[0] - 4.f / 3.f) < 1e-4f);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));  // `hold 600`: still open
        CHECK(!rt::run_patch(&cpu, code + 16));
        CHECK(memory.copy_from_user(got, c + 0x40, 64));
        CHECK(std::abs(got[5] - f) < 1e-6f);
        std::this_thread::sleep_for(std::chrono::milliseconds(400));  // logo gone: Vert+ again
        CHECK(!rt::run_patch(&cpu, code + 16));
        CHECK(memory.copy_from_user(got, c + 0x40, 64));
        CHECK(std::abs(got[0] - camera[0]) < 1e-6f);
        CHECK(std::abs(got[5] / got[0] - 4.f / 3.f) < 1e-4f);
        o.display_aspect = 21.0 / 9.0;
    }
    {  // `crop … except WxH`: the next screen's element shuts the gate at once, without hold or easing
        const uint64_t desc = code + rt::kPageSize;
        std::snprintf(cfg, sizeof cfg,
                      "patch size 0x%llx hw hh\npatch cam 0x%llx proj [x5+0x40] crop 900x150 hold 2000 except 560x64\n"
                      "patch z 0x%llx rect [x10+0x210] model [x10+0x40]\n",
                      static_cast<unsigned long long>(code), static_cast<unsigned long long>(code + 16),
                      static_cast<unsigned long long>(code + 36));
        write(dir / "widescreen.cfg", cfg);
        o.display_aspect = 4.0 / 3.0;
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        const float f = 1.f / std::tan(15.f * 3.14159265f / 180.f);
        const float camera[16] = {f / (16.f / 9.f), 0, 0, 0, 0, f, 0, 0, 0, 0, -1.f, -1, 0, 0, -0.2f, 0};
        const float logo[4] = {900, 150, -450, 75}, button[4] = {560, 64, -280, -200}, ident[8] = {1, 0, 0, 0, 0, 1, 0, 0};
        const uint64_t c = desc + 0x400, l = desc + 0x800, b = desc + 0xc00;
        CHECK(memory.copy_to_user(c + 0x40, camera, 64));
        CHECK(memory.copy_to_user(l + 0x210, logo, 16));
        CHECK(memory.copy_to_user(l + 0x40, ident, 32));
        CHECK(memory.copy_to_user(b + 0x210, button, 16));
        CHECK(memory.copy_to_user(b + 0x40, ident, 32));
        CpuState cpu{};
        float got[16] = {};
        cpu.x[10] = l;
        CHECK(!rt::run_patch(&cpu, code + 36));  // the logo passes
        cpu.x[5] = c;
        CHECK(!rt::run_patch(&cpu, code + 16));  // Hor-
        CHECK(memory.copy_from_user(got, c + 0x40, 64));
        CHECK(std::abs(got[5] - f) < 1e-6f);
        cpu.x[10] = b;
        CHECK(!rt::run_patch(&cpu, code + 36));  // the next screen's button: within the hold, still shut at once
        cpu.x[5] = c;
        CHECK(!rt::run_patch(&cpu, code + 16));
        CHECK(memory.copy_from_user(got, c + 0x40, 64));
        CHECK(std::abs(got[0] - camera[0]) < 1e-6f);  // Vert+ at once, no easing
        CHECK(std::abs(got[5] / got[0] - 4.f / 3.f) < 1e-4f);
        o.display_aspect = 21.0 / 9.0;
    }
    {  // `data`: a float the game copies from (the 2D bounds), the game's value back when docked
        const uint64_t desc = code + rt::kPageSize, d = desc + 0x400;
        const float bounds[2] = {-640, 640};
        CHECK(memory.copy_to_user(d, bounds, 8));
        std::snprintf(cfg, sizeof cfg, "patch size 0x%llx hw hh\npatch ui_l 0x%llx data hw/-2\npatch ui_r 0x%llx data hw/2\n",
                      static_cast<unsigned long long>(code), static_cast<unsigned long long>(d),
                      static_cast<unsigned long long>(d + 4));
        write(dir / "widescreen.cfg", cfg);
        o.display_aspect = 1568.0 / 720.0;
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        float got[2] = {};
        CHECK(memory.copy_from_user(got, d, 8));
        CHECK(got[0] == -784 && got[1] == 784);
        mods::set_widescreen_docked(true);
        CHECK(memory.copy_from_user(got, d, 8));
        CHECK(got[0] == -640 && got[1] == 640);
        mods::set_widescreen_docked(false);
        CHECK(memory.copy_from_user(got, d, 8));
        CHECK(got[1] == 784);
        o.display_aspect = 4.0 / 3.0;  // a new start: the earlier value back first, then the new one
        CHECK(mods::start(o, &r, &error));
        CHECK(memory.copy_from_user(got, d, 8));
        CHECK(got[0] == -640 && got[1] == 640);
        std::snprintf(cfg, sizeof cfg, "patch size 0x%llx hw hh\n", static_cast<unsigned long long>(code));
        write(dir / "widescreen.cfg", cfg);
        CHECK(mods::start(o, &r, &error));
        CHECK(memory.copy_from_user(got, d, 8));
        CHECK(got[0] == -640 && got[1] == 640);  // rule gone: the game's value
        std::snprintf(cfg, sizeof cfg, "patch size 0x%llx hw hh\npatch ui_l 0x%llx data hw/-2\n",
                      static_cast<unsigned long long>(code), static_cast<unsigned long long>(d));
        write(dir / "widescreen.cfg", cfg);
        o.display_aspect = 1568.0 / 720.0;
        CHECK(mods::start(o, &r, &error));
        CHECK(memory.copy_from_user(got, d, 4));
        CHECK(got[0] == -784);
        mods::stop();  // the game's value back while the memory lives, then no memory kept
        CHECK(memory.copy_from_user(got, d, 4));
        CHECK(got[0] == -640);
        const float other = 7;
        CHECK(memory.copy_to_user(d, &other, 4));
        mods::set_widescreen_docked(true);  // after the stop: touches nothing
        mods::set_widescreen_docked(false);
        CHECK(memory.copy_from_user(got, d, 4));
        CHECK(got[0] == 7);
        CHECK(memory.copy_to_user(d, bounds, 8));
        o.display_aspect = 21.0 / 9.0;
    }
    {  // `proj … keep`: a matrix built wide already (data bounds) stays, its zone still moves it, once
        const uint64_t desc = code + rt::kPageSize, t = desc + 0x400;
        std::snprintf(cfg, sizeof cfg, "patch size 0x%llx hw hh\npatch t 0x%llx proj [x5+0x40] keep zone 278\n",
                      static_cast<unsigned long long>(code), static_cast<unsigned long long>(code + 16));
        write(dir / "widescreen.cfg", cfg);
        o.display_aspect = 4.0 / 3.0;
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        const float text[16] = {2.f / 1280, 0, 0, 0, 0, 2.f / 960, 0, -300.f / 480, 0, 0, 1, 0, 0, 0, 0, 1};
        CHECK(memory.copy_to_user(t + 0x40, text, 64));
        CpuState cpu{};
        cpu.x[5] = t;
        float got[16] = {};
        CHECK(!rt::run_patch(&cpu, code + 16));
        CHECK(memory.copy_from_user(got, t + 0x40, 64));
        CHECK(got[5] == 2.f / 960 && std::abs(got[7] + 0.875f) < 1e-6f);  // projection kept, moved to the edge
        CHECK(!rt::run_patch(&cpu, code + 16));  // ours: not twice
        CHECK(memory.copy_from_user(got, t + 0x40, 64));
        CHECK(std::abs(got[7] + 0.875f) < 1e-6f);
        CHECK(memory.copy_to_user(t + 0x40, text, 64));  // the game writes it again
        CHECK(!rt::run_patch(&cpu, code + 16));
        CHECK(memory.copy_from_user(got, t + 0x40, 64));
        CHECK(std::abs(got[7] + 0.875f) < 1e-6f);
        const float inner[16] = {2.f / 1280, 0, 0, 0, 0, 2.f / 960, 0, 100.f / 480, 0, 0, 1, 0, 0, 0, 0, 1};
        CHECK(memory.copy_to_user(t + 0x40, inner, 64));
        CHECK(!rt::run_patch(&cpu, code + 16));
        CHECK(memory.copy_from_user(got, t + 0x40, 64));
        CHECK(std::memcmp(got, inner, 64) == 0);  // inside the zone: as the game made it
        mods::set_widescreen_docked(true);  // TV mode: a camera built handheld goes back to 16:9
        CHECK(memory.copy_to_user(t + 0x40, text, 64));
        CHECK(!rt::run_patch(&cpu, code + 16));
        CHECK(memory.copy_from_user(got, t + 0x40, 64));
        CHECK(std::abs(got[5] - 2.f / 720) < 1e-8f && std::abs(got[7] + 300.f / 360) < 1e-6f && got[0] == 2.f / 1280);
        CHECK(!rt::run_patch(&cpu, code + 16));  // 16:9 now: stays
        CHECK(memory.copy_from_user(got, t + 0x40, 64));
        CHECK(std::abs(got[5] - 2.f / 720) < 1e-8f);
        mods::set_widescreen_docked(false);  // handheld again: a camera built docked (16:9) widens as before
        const float docked[16] = {2.f / 1280, 0, 0, 0, 0, 2.f / 720, 0, -300.f / 360, 0, 0, 1, 0, 0, 0, 0, 1};
        CHECK(memory.copy_to_user(t + 0x40, docked, 64));
        CHECK(!rt::run_patch(&cpu, code + 16));
        CHECK(memory.copy_from_user(got, t + 0x40, 64));
        CHECK(std::abs(got[5] - 2.f / 960) < 1e-8f && std::abs(got[7] + 0.875f) < 1e-6f);
        o.display_aspect = 21.0 / 9.0;
    }
    {  // HUD bounds 50 %: a zone moves text half the way to the new edge
        const uint64_t desc = code + rt::kPageSize, t = desc + 0x400;
        std::snprintf(cfg, sizeof cfg, "patch size 0x%llx hw hh\npatch t 0x%llx proj [x5+0x40] zone 278\n",
                      static_cast<unsigned long long>(code), static_cast<unsigned long long>(code + 16));
        write(dir / "widescreen.cfg", cfg);
        o.display_aspect = 4.0 / 3.0;
        o.hud_bounds = 50;
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        const float text[16] = {2.f / 1280, 0, 0, 0, 0, 2.f / 720, 0, -300.f / 360, 0, 0, 1, 0, 0, 0, 0, 1};
        CHECK(memory.copy_to_user(t + 0x40, text, 64));
        CpuState cpu{};
        cpu.x[5] = t;
        float got[16] = {};
        CHECK(!rt::run_patch(&cpu, code + 16));
        CHECK(memory.copy_from_user(got, t + 0x40, 64));
        CHECK(std::abs(got[7] + 0.75f) < 1e-6f);  // -300 - 60 of the 120 extra units, in 480
        float u = 0.5f, v = (480.f + 360.f) / 960;  // a touch there finds the text's 16:9 place
        std::vector<mods::TouchZone> zones = {{278, -278, 0, false, false}};
        mods::widescreen_touch(&u, &v, mods::widescreen_active(), {1280, 720}, zones.data(), zones.size());
        CHECK(std::abs(v - (360.f + 300.f) / 720) < 1e-5f);
        o.hud_bounds = 100;
        o.display_aspect = 21.0 / 9.0;
    }
    {  // `span … except`: not while the box's frame is drawn (its bands go with the header instead)
        const uint64_t desc = code + rt::kPageSize;
        std::snprintf(cfg, sizeof cfg,
                      "patch size 0x%llx hw hh\npatch s 0x%llx rect [x10+0x210] model [x10+0x40] span 852x78 except 128x720\n",
                      static_cast<unsigned long long>(code), static_cast<unsigned long long>(code + 36));
        write(dir / "widescreen.cfg", cfg);
        o.display_aspect = 4.0 / 3.0;
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        const float band[4] = {852, 78, -426, 360}, frame[4] = {128, 720, -64, 360}, ident[8] = {1, 0, 0, 0, 0, 1, 0, 0};
        const uint64_t a = desc + 0x400, b = desc + 0x800;
        CHECK(memory.copy_to_user(a + 0x210, band, 16));
        CHECK(memory.copy_to_user(a + 0x40, ident, 32));
        CHECK(memory.copy_to_user(b + 0x210, frame, 16));
        CHECK(memory.copy_to_user(b + 0x40, ident, 32));
        CpuState cpu{};
        float got[8] = {};
        cpu.x[10] = a;
        CHECK(!rt::run_patch(&cpu, code + 36));  // Pokédex: no frame, spans
        CHECK(memory.copy_from_user(got, a + 0x40, 32));
        CHECK(std::abs(got[5] - 960.f / 720) < 1e-6f);
        cpu.x[10] = b;
        CHECK(!rt::run_patch(&cpu, code + 36));  // the box's frame
        CHECK(memory.copy_to_user(a + 0x40, ident, 32));
        cpu.x[10] = a;
        CHECK(!rt::run_patch(&cpu, code + 36));  // box: stays as the game made it
        CHECK(memory.copy_from_user(got, a + 0x40, 32));
        CHECK(std::memcmp(got, ident, 32) == 0);
        o.display_aspect = 1568.0 / 720.0;  // wider: the box's bands still span the whole width
        CHECK(mods::start(o, &r, &error));
        cpu.x[10] = b;
        CHECK(!rt::run_patch(&cpu, code + 36));
        CHECK(memory.copy_to_user(a + 0x40, ident, 32));
        cpu.x[10] = a;
        CHECK(!rt::run_patch(&cpu, code + 36));
        CHECK(memory.copy_from_user(got, a + 0x40, 32));
        CHECK(std::abs(got[0] - 1568.f / 852) < 1e-4f);
        o.display_aspect = 21.0 / 9.0;
    }
    {  // `xwiden … when`: the Pokédex's row masks follow its frame x1.225 while the frame is drawn, else they span
        const uint64_t desc = code + rt::kPageSize;
        std::snprintf(cfg, sizeof cfg,
                      "patch size 0x%llx hw hh\npatch s 0x%llx rect [x10+0x210] model [x10+0x40] span 852x78 "
                      "xwiden 852x78 when 936x76\n",
                      static_cast<unsigned long long>(code), static_cast<unsigned long long>(code + 36));
        write(dir / "widescreen.cfg", cfg);
        o.display_aspect = 1568.0 / 720.0;
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        const float mask[4] = {852, 78, -426, 360}, frame[4] = {936, 76, -468, 360}, ident[8] = {1, 0, 0, 0, 0, 1, 0, 0};
        const uint64_t a = desc + 0x400, b = desc + 0x800;
        CHECK(memory.copy_to_user(a + 0x210, mask, 16));
        CHECK(memory.copy_to_user(a + 0x40, ident, 32));
        CHECK(memory.copy_to_user(b + 0x210, frame, 16));
        CHECK(memory.copy_to_user(b + 0x40, ident, 32));
        CpuState cpu{};
        float got[8] = {};
        cpu.x[10] = a;
        CHECK(!rt::run_patch(&cpu, code + 36));  // no frame yet: spans the whole width
        CHECK(memory.copy_from_user(got, a + 0x40, 32));
        CHECK(std::abs(got[0] - 1568.f / 852) < 1e-4f);
        cpu.x[10] = b;
        CHECK(!rt::run_patch(&cpu, code + 36));  // the Pokédex's frame
        CHECK(memory.copy_to_user(a + 0x40, ident, 32));
        cpu.x[10] = a;
        CHECK(!rt::run_patch(&cpu, code + 36));  // now x1.225 like the frame
        CHECK(memory.copy_from_user(got, a + 0x40, 32));
        CHECK(std::abs(got[0] - 1568.f / 1280) < 1e-5f && got[5] == 1.f);
        o.display_aspect = 4.0 / 3.0;  // taller: xwiden stays out, the mask spans (y scaled) as before
        CHECK(mods::start(o, &r, &error));
        cpu.x[10] = b;
        CHECK(!rt::run_patch(&cpu, code + 36));
        CHECK(memory.copy_to_user(a + 0x40, ident, 32));
        cpu.x[10] = a;
        CHECK(!rt::run_patch(&cpu, code + 36));
        CHECK(memory.copy_from_user(got, a + 0x40, 32));
        CHECK(got[0] == 1.f && std::abs(got[5] - 960.f / 720) < 1e-6f);
        o.display_aspect = 21.0 / 9.0;
    }
    {  // HUD bounds 50 %: `fill`: the footer's bar still reaches the edge, its label stops halfway
        const uint64_t desc = code + rt::kPageSize;
        std::snprintf(cfg, sizeof cfg,
                      "patch size 0x%llx hw hh\npatch z 0x%llx rect [x10+0x210] model [x10+0x40] zone 278 -278 fill 1280x36\n"
                      "patch f 0x%llx rect [x10+0x210] model [x10+0x40] fill 1280x132\n",
                      static_cast<unsigned long long>(code), static_cast<unsigned long long>(code + 36),
                      static_cast<unsigned long long>(code + 40));
        write(dir / "widescreen.cfg", cfg);
        o.display_aspect = 4.0 / 3.0;
        o.hud_bounds = 50;
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        const float label[4] = {200, 30, -300, -330}, footer[4] = {1280, 36, -640, -324},
                    ident[8] = {1, 0, 0, 0, 0, 1, 0, 0};
        const uint64_t a = desc + 0x400, b = desc + 0x800;
        CHECK(memory.copy_to_user(a + 0x210, label, 16));
        CHECK(memory.copy_to_user(a + 0x40, ident, 32));
        CHECK(memory.copy_to_user(b + 0x210, footer, 16));
        CHECK(memory.copy_to_user(b + 0x40, ident, 32));
        CpuState cpu{};
        float got[8] = {};
        cpu.x[10] = b;
        CHECK(!rt::run_patch(&cpu, code + 36));  // bar from -384 (top, 60 down) to -480 (the edge)
        CHECK(memory.copy_from_user(got, b + 0x40, 32));
        CHECK(std::abs(got[5] - 96.f / 36) < 1e-5f && std::abs(got[7] + 384 - 324 * got[5]) < 1e-3f);
        CHECK(!rt::run_patch(&cpu, code + 36));  // ours: not twice
        float again[8] = {};
        CHECK(memory.copy_from_user(again, b + 0x40, 32));
        CHECK(std::memcmp(got, again, 32) == 0);
        cpu.x[10] = a;
        CHECK(!rt::run_patch(&cpu, code + 36));  // not listed and not at the edge: 60 down only
        CHECK(memory.copy_from_user(got, a + 0x40, 32));
        CHECK(got[5] == 1.f && got[7] == -60.f);
        // A band the layout shift already put on the line (-420 at the bottom), a line without zones: out to the edge.
        const float band[4] = {1280, 132, -640, -288}, at_line[8] = {1, 0, 0, 0, 0, 1, 0, 0};
        const float inside[4] = {1280, 132, -640, 100};
        CHECK(memory.copy_to_user(a + 0x210, band, 16));
        CHECK(memory.copy_to_user(a + 0x40, at_line, 32));
        CHECK(!rt::run_patch(&cpu, code + 40));
        CHECK(memory.copy_from_user(got, a + 0x40, 32));
        CHECK(std::abs(got[5] - 192.f / 132) < 1e-5f && std::abs(-288 * got[5] + got[7] + 288) < 1e-3f);
        CHECK(memory.copy_to_user(a + 0x210, inside, 16));  // the same size away from the line: stays
        CHECK(memory.copy_to_user(a + 0x40, at_line, 32));
        CHECK(!rt::run_patch(&cpu, code + 40));
        CHECK(memory.copy_from_user(got, a + 0x40, 32));
        CHECK(std::memcmp(got, at_line, 32) == 0);
        o.hud_bounds = 100;
        o.display_aspect = 21.0 / 9.0;
    }
    {  // `touch render 2`: the place on the picture goes to the game unchanged, scaled to the render size
        std::snprintf(cfg, sizeof cfg, "touch render 2\npatch size 0x%llx hw hh\n", static_cast<unsigned long long>(code));
        write(dir / "widescreen.cfg", cfg);
        o.display_aspect = 4.0 / 3.0;
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        const auto [sx, sy] = mods::widescreen_touch_scale(mods::widescreen_active());
        CHECK(std::abs(sx - 1.f) < 1e-6f && std::abs(sy - 4.f / 3.f) < 1e-6f);
        float u = 0.3f, v = 0.95f;
        mods::widescreen_touch(&u, &v, mods::widescreen_active());
        CHECK(u == 0.3f && v == 0.95f);  // hid: 0.95 * 720 * 4/3 = picture row 912
        std::snprintf(cfg, sizeof cfg, "touch render 3\npatch size 0x%llx hw hh\n", static_cast<unsigned long long>(code));
        write(dir / "widescreen.cfg", cfg);
        mods::StartResult bad;
        CHECK(mods::start(o, &bad, &error));
        CHECK(bad.active.empty());
        o.display_aspect = 21.0 / 9.0;
    }
    {  // `cull WxH`: a list row below the old view is not drawn, one inside stays
        const uint64_t desc = code + rt::kPageSize;
        std::snprintf(cfg, sizeof cfg, "patch size 0x%llx hw hh\npatch z 0x%llx rect [x10+0x210] model [x10+0x40] cull 64x64\n",
                      static_cast<unsigned long long>(code), static_cast<unsigned long long>(code + 36));
        write(dir / "widescreen.cfg", cfg);
        o.display_aspect = 4.0 / 3.0;
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        const float icon[4] = {64, 64, -32, 32}, below[8] = {1, 0, 0, 100, 0, 1, 0, -400},
                    inside[8] = {1, 0, 0, 100, 0, 1, 0, -300}, other[4] = {70, 28, -35, 14};
        const uint64_t e = desc + 0x400;
        CpuState cpu{};
        cpu.x[10] = e;
        float got[8] = {};
        CHECK(memory.copy_to_user(e + 0x210, icon, 16));
        CHECK(memory.copy_to_user(e + 0x40, below, 32));
        CHECK(!rt::run_patch(&cpu, code + 36));
        CHECK(memory.copy_from_user(got, e + 0x40, 32));
        CHECK(got[0] == 0 && got[5] == 0 && got[3] == 0 && got[7] == 0);  // entirely below -360: gone
        CHECK(memory.copy_to_user(e + 0x40, inside, 32));
        CHECK(!rt::run_patch(&cpu, code + 36));
        CHECK(memory.copy_from_user(got, e + 0x40, 32));
        CHECK(got[0] == 1 && got[7] == -300);  // reaches into the old view: stays
        CHECK(memory.copy_to_user(e + 0x210, other, 16));
        CHECK(memory.copy_to_user(e + 0x40, below, 32));
        CHECK(!rt::run_patch(&cpu, code + 36));
        CHECK(memory.copy_from_user(got, e + 0x40, 32));
        CHECK(got[0] == 1 && got[7] == -400);  // not listed: stays
        // `below -270`: a row whose top edge is below -270 goes (a footer covered it in 16:9)
        std::snprintf(cfg, sizeof cfg, "patch size 0x%llx hw hh\npatch z 0x%llx rect [x10+0x210] model [x10+0x40] cull 64x64 below -270\n",
                      static_cast<unsigned long long>(code), static_cast<unsigned long long>(code + 36));
        write(dir / "widescreen.cfg", cfg);
        CHECK(mods::start(o, &r, &error));
        const float row6[8] = {1, 0, 0, 100, 0, 1, 0, -308}, row5[8] = {1, 0, 0, 100, 0, 1, 0, -200};
        CHECK(memory.copy_to_user(e + 0x210, icon, 16));
        CHECK(memory.copy_to_user(e + 0x40, row6, 32));  // y -340..-276: top edge below -270
        CHECK(!rt::run_patch(&cpu, code + 36));
        CHECK(memory.copy_from_user(got, e + 0x40, 32));
        CHECK(got[0] == 0 && got[7] == 0);
        CHECK(memory.copy_to_user(e + 0x40, row5, 32));
        CHECK(!rt::run_patch(&cpu, code + 36));
        CHECK(memory.copy_from_user(got, e + 0x40, 32));
        CHECK(got[0] == 1 && got[7] == -200);
        // two groups, the first only while its list frame 500x400 is drawn
        std::snprintf(cfg, sizeof cfg,
                      "patch size 0x%llx hw hh\npatch z 0x%llx rect [x10+0x210] model [x10+0x40] "
                      "cull 64x64 below -270 when 500x400 cull 32x32 above 320\n",
                      static_cast<unsigned long long>(code), static_cast<unsigned long long>(code + 36));
        write(dir / "widescreen.cfg", cfg);
        CHECK(mods::start(o, &r, &error));
        CHECK(memory.copy_to_user(e + 0x40, row6, 32));
        CHECK(!rt::run_patch(&cpu, code + 36));
        CHECK(memory.copy_from_user(got, e + 0x40, 32));
        CHECK(got[0] == 1 && got[7] == -308);  // no frame drawn: stays
        const float frame[4] = {500, 400, -250, 200}, ident[8] = {1, 0, 0, 0, 0, 1, 0, 0};
        CHECK(memory.copy_to_user(e + 0x210, frame, 16));
        CHECK(memory.copy_to_user(e + 0x40, ident, 32));
        CHECK(!rt::run_patch(&cpu, code + 36));  // the frame passes: gate open
        CHECK(memory.copy_to_user(e + 0x210, icon, 16));
        CHECK(memory.copy_to_user(e + 0x40, row6, 32));
        CHECK(!rt::run_patch(&cpu, code + 36));
        CHECK(memory.copy_from_user(got, e + 0x40, 32));
        CHECK(got[0] == 0 && got[7] == 0);
        const float badge[4] = {32, 32, -16, 16}, strip[8] = {1, 0, 0, 0, 0, 1, 0, 350},
                    side[8] = {1, 0, 0, 900, 0, 1, 0, 0};
        CHECK(memory.copy_to_user(e + 0x210, badge, 16));
        CHECK(memory.copy_to_user(e + 0x40, strip, 32));  // y 334..366: above 320
        CHECK(!rt::run_patch(&cpu, code + 36));
        CHECK(memory.copy_from_user(got, e + 0x40, 32));
        CHECK(got[0] == 0 && got[7] == 0);
        CHECK(memory.copy_to_user(e + 0x40, side, 32));  // beside the old view, not above or below
        CHECK(!rt::run_patch(&cpu, code + 36));
        CHECK(memory.copy_from_user(got, e + 0x40, 32));
        CHECK(got[0] == 1 && got[3] == 900);
        o.display_aspect = 21.0 / 9.0;
    }
    {  // `crop … ease`: back from Hor- gradually, the aspect kept all the way
        const uint64_t desc = code + rt::kPageSize;
        std::snprintf(cfg, sizeof cfg,
                      "patch size 0x%llx hw hh\npatch cam 0x%llx proj [x5+0x40] crop 900x150 ease 1000\n"
                      "patch z 0x%llx rect [x10+0x210] model [x10+0x40]\n",
                      static_cast<unsigned long long>(code), static_cast<unsigned long long>(code + 16),
                      static_cast<unsigned long long>(code + 36));
        write(dir / "widescreen.cfg", cfg);
        o.display_aspect = 4.0 / 3.0;
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        const float f = 1.f / std::tan(15.f * 3.14159265f / 180.f);
        const float camera[16] = {f / (16.f / 9.f), 0, 0, 0, 0, f, 0, 0, 0, 0, -1.f, -1, 0, 0, -0.2f, 0};
        const float logo[4] = {900, 150, -450, 75}, ident[8] = {1, 0, 0, 0, 0, 1, 0, 0};
        const uint64_t c = desc + 0x400, l = desc + 0x800;
        CHECK(memory.copy_to_user(c + 0x40, camera, 64));
        CHECK(memory.copy_to_user(l + 0x210, logo, 16));
        CHECK(memory.copy_to_user(l + 0x40, ident, 32));
        CpuState cpu{};
        float got[16] = {};
        cpu.x[10] = l;
        CHECK(!rt::run_patch(&cpu, code + 36));
        cpu.x[5] = c;
        CHECK(!rt::run_patch(&cpu, code + 16));
        CHECK(memory.copy_from_user(got, c + 0x40, 64));
        CHECK(std::abs(got[5] - f) < 1e-6f);  // Hor- at once
        std::this_thread::sleep_for(std::chrono::milliseconds(300));  // gate shut: easing back
        CHECK(!rt::run_patch(&cpu, code + 16));
        CHECK(memory.copy_from_user(got, c + 0x40, 64));
        const float plain11 = camera[0] * 4.f / 3.f;
        CHECK(got[5] < f && got[5] > plain11);
        CHECK(std::abs(got[5] / got[0] - 4.f / 3.f) < 1e-4f);
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        CHECK(!rt::run_patch(&cpu, code + 16));
        CHECK(memory.copy_from_user(got, c + 0x40, 64));
        CHECK(std::abs(got[0] - camera[0]) < 1e-6f);  // the plain rule again
        CHECK(std::abs(got[5] - plain11) < 1e-5f);
        o.display_aspect = 21.0 / 9.0;
    }
    {  // `inside`: text follows the corner box a zone moved, other text beside it stays (battle dialog)
        const uint64_t desc = code + rt::kPageSize;
        std::snprintf(cfg, sizeof cfg,
                      "patch size 0x%llx hw hh\npatch t 0x%llx proj [x5+0x40] zone 50 side 280 inside when 280x264\n"
                      "patch z 0x%llx rect [x10+0x210] model [x10+0x40] zone 90 -90 side 280 when 280x264\n",
                      static_cast<unsigned long long>(code), static_cast<unsigned long long>(code + 16),
                      static_cast<unsigned long long>(code + 36));
        write(dir / "widescreen.cfg", cfg);
        o.display_aspect = 4.0 / 3.0;
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CHECK_EQ(rt::patch_count(), 3u);
        const auto text = [&](uint64_t at, float x, float y) {
            const float m[16] = {2.f / 1280, 0, 0, x / 640, 0, 2.f / 720, 0, y / 360, 0, 0, 1, 0, 0, 0, 0, 1};
            CHECK(memory.copy_to_user(at + 0x40, m, 64));
        };
        const float box[4] = {280, 264, -640, -96}, ident[8] = {1, 0, 0, 0, 0, 1, 0, 0};
        const uint64_t a = desc + 0x400, b = desc + 0x800, c = desc + 0xc00, k = desc + 0x200;
        text(a, -490, -200);  // on the box
        text(b, -300, -245);  // beside it: corner by side, but on no moved box
        CHECK(memory.copy_to_user(k + 0x210, box, 16));
        CHECK(memory.copy_to_user(k + 0x40, ident, 32));
        CpuState cpu{};
        float got[16] = {};
        cpu.x[5] = a;
        CHECK(!rt::run_patch(&cpu, code + 16));  // before the box: waits
        cpu.x[5] = b;
        CHECK(!rt::run_patch(&cpu, code + 16));
        CHECK(memory.copy_from_user(got, a + 0x40, 64));
        CHECK(std::abs(got[7] * 480 + 200) < 1e-3f);
        cpu.x[10] = k;
        CHECK(!rt::run_patch(&cpu, code + 36));  // the box moves down 120; the text on it follows
        CHECK(memory.copy_from_user(got, k + 0x40, 32));
        CHECK(got[7] == -120.f);
        CHECK(memory.copy_from_user(got, a + 0x40, 64));
        CHECK(std::abs(got[7] * 480 + 320) < 1e-3f);
        CHECK(memory.copy_from_user(got, b + 0x40, 64));
        CHECK(std::abs(got[7] * 480 + 245) < 1e-3f);
        text(c, -500, -150);  // built while the box is drawn: at once
        cpu.x[5] = c;
        CHECK(!rt::run_patch(&cpu, code + 16));
        CHECK(memory.copy_from_user(got, c + 0x40, 64));
        CHECK(std::abs(got[7] * 480 + 270) < 1e-3f);
        cpu.x[5] = b;
        CHECK(!rt::run_patch(&cpu, code + 16));  // still beside it
        CHECK(memory.copy_from_user(got, b + 0x40, 64));
        CHECK(std::abs(got[7] * 480 + 245) < 1e-3f);
        o.display_aspect = 21.0 / 9.0;
    }
    {  // `follow`: text on a spread box moves as far as the box (Pokédex entry), not by its own spread
        const uint64_t desc = code + rt::kPageSize;
        std::snprintf(cfg, sizeof cfg,
                      "patch size 0x%llx hw hh\npatch t 0x%llx proj [x5+0x40] zone 278 follow when 300x200 "
                      "zone 278 spread when 300x200\n"
                      "patch z 0x%llx rect [x10+0x210] model [x10+0x40] zone 278 -278 spread when 300x200\n",
                      static_cast<unsigned long long>(code), static_cast<unsigned long long>(code + 16),
                      static_cast<unsigned long long>(code + 36));
        write(dir / "widescreen.cfg", cfg);
        o.display_aspect = 4.0 / 3.0;
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CHECK_EQ(rt::patch_count(), 3u);
        const auto text = [&](uint64_t at, float x, float y) {
            const float m[16] = {2.f / 1280, 0, 0, x / 640, 0, 2.f / 720, 0, y / 360, 0, 0, 1, 0, 0, 0, 0, 1};
            CHECK(memory.copy_to_user(at + 0x40, m, 64));
        };
        const float box[4] = {300, 200, -150, 250}, ident[8] = {1, 0, 0, 0, 0, 1, 0, 0};  // y 50..250
        const uint64_t k = desc + 0x200, a = desc + 0x400, b = desc + 0x800, c = desc + 0xc00;
        CHECK(memory.copy_to_user(k + 0x210, box, 16));
        CHECK(memory.copy_to_user(k + 0x40, ident, 32));
        CpuState cpu{};
        float got[16] = {};
        cpu.x[10] = k;
        CHECK(!rt::run_patch(&cpu, code + 36));  // the box spreads: middle 150 -> 200, size stays
        CHECK(memory.copy_from_user(got, k + 0x40, 32));
        CHECK(std::abs(got[7] - 50.f) < 1e-3f);
        text(a, 0, 60);     // near the box's lower edge: 110, inside it (its own spread would give 80)
        text(b, 400, -100); // on no box: the next zone spreads it
        text(c, 0, 290);    // only where the box is now: built from it, went along already
        for (uint64_t at : {a, b, c}) {
            cpu.x[5] = at;
            CHECK(!rt::run_patch(&cpu, code + 16));
        }
        CHECK(memory.copy_from_user(got, a + 0x40, 64));
        CHECK(std::abs(got[7] * 480 - 110) < 1e-3f);
        CHECK(memory.copy_from_user(got, b + 0x40, 64));
        CHECK(std::abs(got[7] * 480 + 400.f / 3) < 1e-3f);
        CHECK(memory.copy_from_user(got, c + 0x40, 64));
        CHECK(std::abs(got[7] * 480 - 290) < 1e-3f);
        o.display_aspect = 21.0 / 9.0;
    }
    {  // `zone TOP BOTTOM` on a proj line: only upwards (Pokédex entry boxes hang at the top, the text below spreads)
        const uint64_t desc = code + rt::kPageSize;
        std::snprintf(cfg, sizeof cfg,
                      "patch size 0x%llx hw hh\npatch t 0x%llx proj [x5+0x40] zone 1 -1000 side 280 when 300x200 "
                      "zone 278 spread when 300x200\n"
                      "patch z 0x%llx rect [x10+0x210] model [x10+0x40] zone 278 -278 when 300x200\n",
                      static_cast<unsigned long long>(code), static_cast<unsigned long long>(code + 16),
                      static_cast<unsigned long long>(code + 36));
        write(dir / "widescreen.cfg", cfg);
        o.display_aspect = 4.0 / 3.0;
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CHECK_EQ(rt::patch_count(), 3u);
        const auto text = [&](uint64_t at, float x, float y) {
            const float m[16] = {2.f / 1280, 0, 0, x / 640, 0, 2.f / 720, 0, y / 360, 0, 0, 1, 0, 0, 0, 0, 1};
            CHECK(memory.copy_to_user(at + 0x40, m, 64));
        };
        const float gate[4] = {300, 200, -150, 100}, ident[8] = {1, 0, 0, 0, 0, 1, 0, 0};
        const uint64_t k = desc + 0x200, a = desc + 0x400, b = desc + 0x800, c = desc + 0xc00;
        CHECK(memory.copy_to_user(k + 0x210, gate, 16));
        CHECK(memory.copy_to_user(k + 0x40, ident, 32));
        CpuState cpu{};
        float got[16] = {};
        cpu.x[10] = k;
        CHECK(!rt::run_patch(&cpu, code + 36));  // opens the gate, stays (y -100..100)
        text(a, -500, 40);    // in a box at the side, above the middle: to the top edge with it
        text(b, -359, -232);  // below the middle at the side: not this zone, spreads
        text(c, 0, 40);       // in the middle: spreads
        for (uint64_t at : {a, b, c}) {
            cpu.x[5] = at;
            CHECK(!rt::run_patch(&cpu, code + 16));
        }
        CHECK(memory.copy_from_user(got, a + 0x40, 64));
        CHECK(std::abs(got[7] * 480 - 160) < 1e-3f);
        CHECK(memory.copy_from_user(got, b + 0x40, 64));
        CHECK(std::abs(got[7] * 480 + 232.f * 4 / 3) < 1e-2f);
        CHECK(memory.copy_from_user(got, c + 0x40, 64));
        CHECK(std::abs(got[7] * 480 - 160.f / 3) < 1e-3f);
        o.display_aspect = 21.0 / 9.0;
    }
    {  // `xzone` (21:9, iPhone): corner boxes move to the new side edges, text on them follows, touches find them
        const uint64_t desc = code + rt::kPageSize;
        std::snprintf(cfg, sizeof cfg,
                      "patch size 0x%llx hw hh\npatch t 0x%llx proj [x5+0x40] xzone 250 inside when 340x300\n"
                      "patch z 0x%llx rect [x10+0x210] model [x10+0x40] xzone 250 -250 when 340x300 log 4\n",
                      static_cast<unsigned long long>(code), static_cast<unsigned long long>(code + 16),
                      static_cast<unsigned long long>(code + 36));
        write(dir / "widescreen.cfg", cfg);
        o.display_aspect = 21.0 / 9.0;
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CHECK_EQ(rt::patch_count(), 3u);
        CHECK(mods::widescreen_active().width == 1680u);
        const auto text = [&](uint64_t at, float x, float y) {
            const float m[16] = {2.f / 1280, 0, 0, x / 640, 0, 2.f / 720, 0, y / 360, 0, 0, 1, 0, 0, 0, 0, 1};
            CHECK(memory.copy_to_user(at + 0x40, m, 64));
        };
        const float box[4] = {340, 300, 290, -50}, left[4] = {180, 80, -630, 330}, mid[4] = {200, 40, -100, 0},
                    ident[8] = {1, 0, 0, 0, 0, 1, 0, 0};
        const uint64_t k = desc + 0x200, l = desc + 0x300, n = desc + 0x380, a = desc + 0x400, b = desc + 0x800,
                       c = desc + 0xc00;
        CHECK(memory.copy_to_user(k + 0x210, box, 16));
        CHECK(memory.copy_to_user(k + 0x40, ident, 32));
        CHECK(memory.copy_to_user(l + 0x210, left, 16));
        CHECK(memory.copy_to_user(l + 0x40, ident, 32));
        CHECK(memory.copy_to_user(n + 0x210, mid, 16));
        CHECK(memory.copy_to_user(n + 0x40, ident, 32));
        text(a, 400, -150);  // on the box
        text(b, 400, 200);   // right, but on no moved box
        text(c, 0, -150);    // middle
        CpuState cpu{};
        float got[16] = {};
        cpu.x[10] = k;
        CHECK(!rt::run_patch(&cpu, code + 36));  // the box opens its gate and moves right by 200
        CHECK(memory.copy_from_user(got, k + 0x40, 32));
        CHECK(got[3] == 200.f && got[7] == 0.f);
        cpu.x[10] = l;
        CHECK(!rt::run_patch(&cpu, code + 36));  // left corner: left by 200
        CHECK(memory.copy_from_user(got, l + 0x40, 32));
        CHECK(got[3] == -200.f);
        cpu.x[10] = n;
        CHECK(!rt::run_patch(&cpu, code + 36));  // middle: stays
        CHECK(memory.copy_from_user(got, n + 0x40, 32));
        CHECK(got[3] == 0.f);
        for (uint64_t t : {a, b, c}) {
            cpu.x[5] = t;
            CHECK(!rt::run_patch(&cpu, code + 16));
        }
        CHECK(memory.copy_from_user(got, a + 0x40, 64));
        CHECK(std::abs(got[3] * 840 - 600) < 1e-3f);  // follows its box
        CHECK(memory.copy_from_user(got, b + 0x40, 64));
        CHECK(std::abs(got[3] * 840 - 400) < 1e-3f);  // not on it: stays
        CHECK(memory.copy_from_user(got, c + 0x40, 64));
        CHECK(std::abs(got[3] * 840) < 1e-3f);
        float u = (650.f + 840) / 1680, v = 0.5f;  // touch on the moved box
        mods::widescreen_touch(&u, &v, mods::widescreen_active());
        CHECK(std::abs(u - (450.f + 640) / 1280) < 1e-5f);
    }
    {  // stretch at 4:3: a box at the bottom edge reaches the new edge, its texture rows stay (soft end stretched)
        const uint64_t desc = code + rt::kPageSize;
        std::snprintf(cfg, sizeof cfg,
                      "patch size 0x%llx hw hh\npatch s 0x%llx rect [x10+0x210] model [x10+0x40] tex [x10+0x70] stretch 300x200\n",
                      static_cast<unsigned long long>(code), static_cast<unsigned long long>(code + 36));
        write(dir / "widescreen.cfg", cfg);
        o.display_aspect = 4.0 / 3.0;
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CHECK_EQ(rt::patch_count(), 2u);
        const float box[4] = {300, 200, -640, -160}, ident[8] = {1, 0, 0, 0, 0, 1, 0, 0},
                    rows[8] = {1, 0, 0, 0, 0, 1, 0, 0};
        const uint64_t a = desc + 0x400;
        CHECK(memory.copy_to_user(a + 0x210, box, 16));
        CHECK(memory.copy_to_user(a + 0x40, ident, 32));
        CHECK(memory.copy_to_user(a + 0x70, rows, 32));
        CpuState cpu{};
        cpu.x[10] = a;
        CHECK(!rt::run_patch(&cpu, code + 36));
        float got[8] = {}, got_t[8] = {};
        CHECK(memory.copy_from_user(got, a + 0x40, 32));
        CHECK(memory.copy_from_user(got_t, a + 0x70, 32));
        CHECK(std::abs(got[5] * -160 + got[7] + 160) < 1e-3f && std::abs(got[5] * -360 + got[7] + 480) < 1e-3f);
        CHECK(got[0] == 1.f && got[3] == 0.f);  // width unchanged at 4:3
        CHECK(std::memcmp(got_t, rows, 32) == 0);
        CHECK(!rt::run_patch(&cpu, code + 36));  // again: not moved twice
        CHECK(memory.copy_from_user(got, a + 0x40, 32));
        CHECK(std::abs(got[5] * -360 + got[7] + 480) < 1e-3f);
        o.display_aspect = 21.0 / 9.0;
    }
    {  // a size rule with bad operands: not started
        std::snprintf(cfg, sizeof cfg, "patch size 0x%llx hw hh\npatch b 0x%llx size [w1+0x10] w3\n",
                      static_cast<unsigned long long>(code), static_cast<unsigned long long>(code + 8));
        write(dir / "widescreen.cfg", cfg);
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active.empty());
        CHECK_EQ(rt::patch_count(), 0u);
    }
    {  // not a move-wide instruction: not started, no patch left
        write(dir / "widescreen.cfg", "patch size 0x8000008 hw hh\n");
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active.empty());
        CHECK_EQ(rt::patch_count(), 0u);
    }
    {  // without hw/hh
        std::snprintf(cfg, sizeof cfg, "patch size 0x%llx dw dh\n", static_cast<unsigned long long>(code));
        write(dir / "widescreen.cfg", cfg);
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active.empty());
        CHECK_EQ(rt::patch_count(), 0u);
    }
    mods::stop();
    bool ru = false, rv = false;
    CHECK(!rt::repeat_hint(1280, 360, &ru, &rv));  // stop() drops the hints
    CHECK(mods::widescreen_active().width == 0 && mods::widescreen_active().height == 0);
}

// `file … scale`: a float in a named RomFS file times the extra size along its
// axis as fsp-srv reads it (the title's white fade); not docked, not 16:9.
void widescreen_file_scale(const fs::path& dir) {
    rt::GuestMemory memory(rt::Layout::packed(0x0800'0000, 0x10'0000, 0x10'0000, 0x10'0000, 0x10'0000, 0x40'0000));
    const uint64_t code = memory.layout().code.base;
    CHECK(memory.map(code, rt::kPageSize, rt::MemState::Normal, rt::kPermRW).ok());
    const uint32_t movk_x3 = 0xF2C0'0000u | (720u << 5) | 3u;
    CHECK(memory.copy_to_user(code, &movk_x3, 4));
    char cfg[512];
    std::snprintf(cfg, sizeof cfg,
                  "patch size 0x%llx hw hh\nfile fade_x 0x8 romfs:/bin/fx/fade.ptcl scale x was 150\n"
                  "file fade_y 0xc /bin/fx/fade.ptcl scale y was 100\nfile other_y 0x10 /bin/fx/fade.ptcl scale y was 7\n",
                  static_cast<unsigned long long>(code));
    write(dir / "widescreen.cfg", cfg);
    mods::StartOptions o;
    o.dir = dir;
    o.enabled = {"widescreen"};
    o.memory = &memory;
    o.display_aspect = 4.0 / 3.0;
    std::string error;
    mods::StartResult r;
    CHECK(mods::start(o, &r, &error));
    std::vector<uint8_t> file(32);
    const float values[3] = {150, 100, 9};  // 9: not the `was` value, stays
    std::memcpy(file.data() + 8, values, 12);
    const std::vector<uint8_t> original = file;
    std::vector<std::string> at_mount;
    CHECK(mods::widescreen_layouts_begin(&at_mount));  // no layouts line: the file alone
    CHECK(at_mount.empty());
    const uint8_t head[16] = {'V', 'F', 'X', 'B'};
    CHECK(mods::widescreen_layouts_wants("/bin/fx/fade.ptcl", head));
    CHECK(!mods::widescreen_layouts_wants("/bin/fx/other.ptcl", head));
    CHECK(mods::widescreen_layouts_change("/bin/fx/fade.ptcl", file));
    float got[3] = {};
    std::memcpy(got, file.data() + 8, 12);
    CHECK(got[0] == 150.f);  // taller: x stays
    CHECK(std::abs(got[1] - 100.f * 960 / 720) < 1e-4f);
    CHECK(got[2] == 9.f);

    o.display_aspect = 1568.0 / 720.0;  // wider: x only
    CHECK(mods::start(o, &r, &error));
    CHECK(mods::widescreen_layouts_begin(&at_mount));
    file = original;
    CHECK(mods::widescreen_layouts_change("/bin/fx/fade.ptcl", file));
    std::memcpy(got, file.data() + 8, 12);
    CHECK(std::abs(got[0] - 150.f * 1568 / 1280) < 1e-3f && got[1] == 100.f);

    mods::set_widescreen_docked(true);  // docked at start: stays
    CHECK(!mods::widescreen_layouts_begin(&at_mount));
    mods::set_widescreen_docked(false);

    o.display_aspect = 16.0 / 9.0;  // 16:9: no file to change
    CHECK(mods::start(o, &r, &error));
    CHECK(!mods::widescreen_layouts_begin(&at_mount));
    CHECK(!mods::widescreen_layouts_wants("/bin/fx/fade.ptcl", head));
    mods::stop();
}

// The `layouts` line: header and footer units of the game's layouts move to
// the edges of a taller picture as fsp-srv reads them; y zones step aside.
void widescreen_layouts(const fs::path& dir) {
    rt::GuestMemory memory(rt::Layout::packed(0x0800'0000, 0x10'0000, 0x10'0000, 0x10'0000, 0x10'0000, 0x40'0000));
    const uint64_t code = memory.layout().code.base;
    CHECK(memory.map(code, rt::kPageSize, rt::MemState::Normal, rt::kPermRW).ok());
    const uint32_t movk_x3 = 0xF2C0'0000u | (720u << 5) | 3u;
    CHECK(memory.copy_to_user(code, &movk_x3, 4));
    char cfg[512];
    std::snprintf(cfg, sizeof cfg,
                  "patch size 0x%llx hw hh\nlayouts ui 360 in /bin/app skip other pack /bin/pack.gfpak\n"
                  "patch z 0x%llx rect [x10+0x210] model [x10+0x40] zone 278 -278 xzone 600 -600\n",
                  static_cast<unsigned long long>(code), static_cast<unsigned long long>(code + 36));
    write(dir / "widescreen.cfg", cfg);
    mods::StartOptions o;
    o.dir = dir;
    o.enabled = {"widescreen"};
    o.memory = &memory;
    o.display_aspect = 4.0 / 3.0;
    std::string error;
    mods::StartResult r;
    CHECK(mods::start(o, &r, &error));
    CHECK_EQ(r.active.size(), 1u);
    CHECK(mods::widescreen_layout_shift() == 120.f);
    std::vector<std::string> at_mount;
    CHECK(mods::widescreen_layouts_begin(&at_mount));
    CHECK(at_mount == std::vector<std::string>{"/bin/pack.gfpak"});
    const uint8_t sarc[16] = {'S', 'A', 'R', 'C'}, yaz[16] = {'Y', 'a', 'z', '0'};
    CHECK(mods::widescreen_layouts_wants("/bin/app/a.arc", sarc));
    CHECK(!mods::widescreen_layouts_wants("/bin/application/a.arc", sarc));  // a folder, not a prefix
    CHECK(!mods::widescreen_layouts_wants("/bin/app/a.arc", yaz));

    // A footer group, a header and a middle label; layout "other" is skipped.
    lgp::test::LayoutBuilder b(1280, 720, false);
    b.materials({{"M", -1}});
    b.null("RootPane", 0, 0, 1280, 720);
    b.begin_children();
    b.null("N_foot", 0, 40, 1280, 80, uint8_t(2 << 6));
    b.begin_children();
    b.picture("P_bar", 0, 0, 0, 1280, 80);
    b.end_children();
    b.picture("P_head", 0, 0, 320, 1280, 80);
    b.picture("P_side", 0, -100, 0, 160, 100);
    b.end_children();
    const std::vector<uint8_t> file = b.finish();
    std::vector<uint8_t> arc = lgp::test::make_sarc({{"blyt/a.bflyt", std::string(file.begin(), file.end())},
                                                    {"blyt/other.bflyt", std::string(file.begin(), file.end())}});
    const size_t size = arc.size();
    CHECK(mods::widescreen_layouts_change("/bin/app/a.arc", arc));
    CHECK_EQ(arc.size(), size);
    namespace ui = lgp::platform::ui;
    const auto moved = ui::Sarc::parse(arc, &error);
    const auto a = moved ? ui::parse_bflyt(moved->file("blyt/a.bflyt"), &error) : std::nullopt;
    const auto other = moved ? ui::parse_bflyt(moved->file("blyt/other.bflyt"), &error) : std::nullopt;
    CHECK(a && other);
    if (a && other) {
        CHECK(std::abs(a->panes[size_t(a->find("N_foot"))].translate[1] - (40 - 120)) < 1e-3f);
        CHECK(std::abs(a->panes[size_t(a->find("P_head"))].translate[1] - (320 + 120)) < 1e-3f);
        CHECK(a->panes[size_t(a->find("P_side"))].translate[1] == 0.f);
        CHECK(other->panes[size_t(other->find("N_foot"))].translate[1] == 40.f);
    }
    std::vector<uint8_t> text = {'n', 'o', 'n', 'e'};
    CHECK(!mods::widescreen_layouts_change("/bin/app/b.arc", text));

    // The y zone is left out: a footer element stays; touches find moved units.
    const uint64_t desc = code + rt::kPageSize;
    CHECK(memory.map(desc, rt::kPageSize, rt::MemState::Normal, rt::kPermRW).ok());
    const float footer[4] = {1280, 36, -640, -324}, ident[8] = {1, 0, 0, 0, 0, 1, 0, 0};
    CHECK(memory.copy_to_user(desc + 0x210, footer, 16));
    CHECK(memory.copy_to_user(desc + 0x40, ident, 32));
    CpuState cpu{};
    cpu.x[10] = desc;
    rt::run_patch(&cpu, code + 36);
    float got[8] = {};
    CHECK(memory.copy_from_user(got, desc + 0x40, 32));
    CHECK(got[7] == 0.f);
    float u = 0.5f, v = (480.f + 450) / 960;  // a footer unit at the new bottom
    mods::widescreen_touch(&u, &v, mods::widescreen_active());
    CHECK(std::abs(v - (360.f + 330) / 720) < 1e-5f);
    u = 0.5f, v = (480.f + 100) / 960;  // the middle stays
    mods::widescreen_touch(&u, &v, mods::widescreen_active());
    CHECK(std::abs(v - (360.f + 100) / 720) < 1e-5f);

    // Docked at start: layouts stay.
    mods::set_widescreen_docked(true);
    CHECK(!mods::widescreen_layouts_begin(&at_mount));
    CHECK(mods::widescreen_layout_shift() == 0.f);
    mods::set_widescreen_docked(false);

    // Stopped (a later start without the mod): the earlier layout settings change nothing.
    mods::stop();
    CHECK(!mods::widescreen_layouts_begin(&at_mount));
    CHECK(mods::widescreen_layout_shift() == 0.f);
    {
        mods::StartResult again;
        CHECK(mods::start(o, &again, &error));
        CHECK(mods::widescreen_layouts_begin(&at_mount));
    }

    // A wider picture (iPhone): nothing to move, y zones stay.
    o.display_aspect = 21.0 / 9.0;
    mods::StartResult wide;
    CHECK(mods::start(o, &wide, &error));
    CHECK_EQ(wide.active.size(), 1u);
    CHECK(!mods::widescreen_layouts_begin(&at_mount));
    CHECK(!mods::widescreen_layouts_wants("/bin/app/a.arc", sarc));

    // A second line with `axis x` moves right and left units on a wider picture
    // (iPhone, half the extra width); there the xzone steps aside, y zones stay.
    // The same cfg on 4:3 takes the y line.
    std::snprintf(cfg, sizeof cfg,
                  "patch size 0x%llx hw hh\npatch hint 0x%llx data 640+lx\npatch head 0x%llx data -360-ly\n"
                  "layouts ui 360 in /bin/app\nlayouts side 300 axis x in /bin/app\n"
                  "patch z 0x%llx rect [x10+0x210] model [x10+0x40] zone 278 -278 xzone 600 -600\n",
                  static_cast<unsigned long long>(code), static_cast<unsigned long long>(desc + 0x400),
                  static_cast<unsigned long long>(desc + 0x404), static_cast<unsigned long long>(code + 36));
    write(dir / "widescreen.cfg", cfg);
    o.display_aspect = 1568.0 / 720.0;
    {
        mods::StartResult x;
        CHECK(mods::start(o, &x, &error));
        CHECK_EQ(x.active.size(), 1u);
        CHECK(std::abs(mods::widescreen_layout_shift() - 144.f) < 1e-3f);
        float held[2] = {};  // lx, ly from the layouts lines below the data lines
        CHECK(memory.copy_from_user(held, desc + 0x400, 8));
        CHECK(std::abs(held[0] - 784) < 1e-3f && std::abs(held[1] + 360) < 1e-3f);
        CHECK(mods::widescreen_layouts_begin(&at_mount));
        lgp::test::LayoutBuilder c(1280, 720, false);
        c.materials({{"M", -1}});
        c.null("RootPane", 0, 0, 1280, 720);
        c.begin_children();
        c.picture("P_corner", 0, 540, 200, 200, 100);
        c.picture("P_mid", 0, 0, 0, 160, 100);
        c.end_children();
        const std::vector<uint8_t> cf = c.finish();
        std::vector<uint8_t> carc = lgp::test::make_sarc({{"blyt/c.bflyt", std::string(cf.begin(), cf.end())}});
        CHECK(mods::widescreen_layouts_change("/bin/app/c.arc", carc));
        const auto cs = ui::Sarc::parse(carc, &error);
        const auto cl = cs ? ui::parse_bflyt(cs->file("blyt/c.bflyt"), &error) : std::nullopt;
        CHECK(cl.has_value());
        if (cl) {
            CHECK(std::abs(cl->panes[size_t(cl->find("P_corner"))].translate[0] - (540 + 144)) < 1e-3f);
            CHECK(cl->panes[size_t(cl->find("P_corner"))].translate[1] == 200.f);
            CHECK(cl->panes[size_t(cl->find("P_mid"))].translate[0] == 0.f);
        }
        // The xzone is left out: an element at the right stays where the game put it.
        const float right[4] = {200, 100, 620, 0};
        CHECK(memory.copy_to_user(desc + 0x210, right, 16));
        CHECK(memory.copy_to_user(desc + 0x40, ident, 32));
        rt::run_patch(&cpu, code + 36);
        CHECK(memory.copy_from_user(got, desc + 0x40, 32));
        CHECK(got[3] == 0.f);
        // A touch on the moved corner finds its old place; the middle stays.
        float tu = (784.f + 684) / 1568, tv = 0.5f;
        mods::widescreen_touch(&tu, &tv, mods::widescreen_active());
        CHECK(std::abs(tu - (640.f + 540) / 1280) < 1e-5f);
        tu = (784.f + 100) / 1568;
        mods::widescreen_touch(&tu, &tv, mods::widescreen_active());
        CHECK(std::abs(tu - (640.f + 100) / 1280) < 1e-5f);
    }
    o.display_aspect = 4.0 / 3.0;
    {
        mods::StartResult y;
        CHECK(mods::start(o, &y, &error));
        CHECK_EQ(y.active.size(), 1u);
        CHECK(mods::widescreen_layout_shift() == 120.f);
        float held[2] = {};
        CHECK(memory.copy_from_user(held, desc + 0x400, 8));
        CHECK(std::abs(held[0] - 640) < 1e-3f && std::abs(held[1] + 480) < 1e-3f);
    }
    // Below 100 % HUD bounds a unit the x line moved less than the overhang band
    // (20 %: 28.8) and that `stretch` then pulls to the edge ends at the new
    // edge, not 28.8 beyond it.
    std::snprintf(cfg, sizeof cfg,
                  "patch size 0x%llx hw hh\nlayouts side 300 axis x in /bin/app\n"
                  "patch z 0x%llx rect [x10+0x210] model [x10+0x40] stretch 200x720\n",
                  static_cast<unsigned long long>(code), static_cast<unsigned long long>(code + 36));
    write(dir / "widescreen.cfg", cfg);
    o.display_aspect = 1568.0 / 720.0;
    o.hud_bounds = 20;
    {
        mods::StartResult x;
        CHECK(mods::start(o, &x, &error));
        CHECK_EQ(x.active.size(), 1u);
        CHECK(std::abs(mods::widescreen_layout_shift() - 28.8f) < 1e-3f);
        const float panel[4] = {200, 720, 440 + 28.8f, 360};
        CHECK(memory.copy_to_user(desc + 0x210, panel, 16));
        CHECK(memory.copy_to_user(desc + 0x40, ident, 32));
        rt::run_patch(&cpu, code + 36);
        CHECK(memory.copy_from_user(got, desc + 0x40, 32));
        const float lo = got[0] * panel[2] + got[3], hi = got[0] * (panel[2] + panel[0]) + got[3];
        CHECK(std::abs(lo - panel[2]) < 1e-2f);
        CHECK(std::abs(hi - 784) < 1e-2f);
    }
    // At 50 % (72, beyond the overhang band) the moved unit's edge element still reaches the edge.
    o.hud_bounds = 50;
    {
        mods::StartResult x;
        CHECK(mods::start(o, &x, &error));
        CHECK(std::abs(mods::widescreen_layout_shift() - 72.f) < 1e-3f);
        const float panel[4] = {200, 720, 440 + 72.f, 360};
        CHECK(memory.copy_to_user(desc + 0x210, panel, 16));
        CHECK(memory.copy_to_user(desc + 0x40, ident, 32));
        rt::run_patch(&cpu, code + 36);
        CHECK(memory.copy_from_user(got, desc + 0x40, 32));
        const float lo = got[0] * panel[2] + got[3], hi = got[0] * (panel[2] + panel[0]) + got[3];
        CHECK(std::abs(lo - panel[2]) < 1e-2f);
        CHECK(std::abs(hi - 784) < 1e-2f);
    }
    o.hud_bounds = 100;
    o.display_aspect = 4.0 / 3.0;
    // The same along y with `edge` (4:3, 50 %: 60): a header band of a moved unit
    // (936x76 at 344..420) reaches the new top edge 480, its texels unstretched.
    std::snprintf(cfg, sizeof cfg,
                  "patch size 0x%llx hw hh\nlayouts ui 360 in /bin/app\n"
                  "patch z 0x%llx rect [x10+0x210] model [x10+0x40] edge 936x76\n",
                  static_cast<unsigned long long>(code), static_cast<unsigned long long>(code + 36));
    write(dir / "widescreen.cfg", cfg);
    o.hud_bounds = 50;
    {
        mods::StartResult y;
        CHECK(mods::start(o, &y, &error));
        CHECK(std::abs(mods::widescreen_layout_shift() - 60.f) < 1e-3f);
        const float band[4] = {936, 76, -468, 420};
        CHECK(memory.copy_to_user(desc + 0x210, band, 16));
        CHECK(memory.copy_to_user(desc + 0x40, ident, 32));
        rt::run_patch(&cpu, code + 36);
        CHECK(memory.copy_from_user(got, desc + 0x40, 32));
        const float top = got[5] * band[3] + got[7], bottom = got[5] * (band[3] - band[1]) + got[7];
        CHECK(std::abs(top - 480) < 1e-2f);
        CHECK(std::abs(bottom - 344) < 1e-2f);
    }
    o.hud_bounds = 100;

    // `hud L,…`: only the battle layout follows the HUD bounds per side (right
    // 50 %: 72, left 0), the other layouts go to the edge (144) whatever they are.
    std::snprintf(cfg, sizeof cfg, "patch size 0x%llx hw hh\nlayouts side 300 axis x in /bin/app hud battle\n",
                  static_cast<unsigned long long>(code));
    write(dir / "widescreen.cfg", cfg);
    o.display_aspect = 1568.0 / 720.0;
    o.hud_bounds = 0;  // the sides win
    o.hud_sides = {-1, -1, 0, 50};
    {
        mods::StartResult x;
        CHECK(mods::start(o, &x, &error));
        CHECK_EQ(x.active.size(), 1u);
        CHECK(std::abs(mods::widescreen_layout_shift() - 144.f) < 1e-3f);
        CHECK(mods::widescreen_layouts_begin(&at_mount));
        lgp::test::LayoutBuilder c(1280, 720, false);
        c.materials({{"M", -1}});
        c.null("RootPane", 0, 0, 1280, 720);
        c.begin_children();
        c.picture("P_right", 0, 540, 200, 200, 100);
        c.picture("P_left", 0, -540, 200, 200, 100);
        c.end_children();
        const std::vector<uint8_t> cf = c.finish();
        std::vector<uint8_t> carc = lgp::test::make_sarc({{"blyt/battle.bflyt", std::string(cf.begin(), cf.end())},
                                                         {"blyt/menu.bflyt", std::string(cf.begin(), cf.end())}});
        CHECK(mods::widescreen_layouts_change("/bin/app/c.arc", carc));
        const auto cs = ui::Sarc::parse(carc, &error);
        const auto b = cs ? ui::parse_bflyt(cs->file("blyt/battle.bflyt"), &error) : std::nullopt;
        const auto m = cs ? ui::parse_bflyt(cs->file("blyt/menu.bflyt"), &error) : std::nullopt;
        CHECK(b && m);
        if (b && m) {
            CHECK(std::abs(b->panes[size_t(b->find("P_right"))].translate[0] - (540 + 72)) < 1e-3f);
            CHECK(b->panes[size_t(b->find("P_left"))].translate[0] == -540.f);
            CHECK(std::abs(m->panes[size_t(m->find("P_right"))].translate[0] - (540 + 144)) < 1e-3f);
            CHECK(std::abs(m->panes[size_t(m->find("P_left"))].translate[0] - (-540 - 144)) < 1e-3f);
        }
        // Bounds changed while the game runs (the app's screen): a new
        // generation for the battle's files only, and their next change uses
        // the new bounds (right 100 %: 144, left 25 %: 36).
        const uint64_t was = mods::widescreen_layouts_epoch("/bin/app/battle_ger.arc");
        CHECK(was != 0);
        CHECK_EQ(mods::widescreen_layouts_epoch("/bin/app/menu_ger.arc"), 0u);
        CHECK(mods::set_widescreen_hud_sides({-1, -1, 25, 100}));
        CHECK(mods::widescreen_layouts_epoch("/bin/app/battle_ger.arc") != was);
        CHECK(mods::set_widescreen_hud_sides({-1, -1, 25, 100}));  // the same again: no new generation
        CHECK_EQ(mods::widescreen_layouts_epoch("/bin/app/battle_ger.arc"), was + 1);
        std::vector<uint8_t> again = lgp::test::make_sarc({{"blyt/battle.bflyt", std::string(cf.begin(), cf.end())}});
        CHECK(mods::widescreen_layouts_change("/bin/app/battle_ger.arc", again));
        const auto as = ui::Sarc::parse(again, &error);
        const auto ab = as ? ui::parse_bflyt(as->file("blyt/battle.bflyt"), &error) : std::nullopt;
        CHECK(ab.has_value());
        if (ab) {
            CHECK(std::abs(ab->panes[size_t(ab->find("P_right"))].translate[0] - (540 + 144)) < 1e-3f);
            CHECK(std::abs(ab->panes[size_t(ab->find("P_left"))].translate[0] - (-540 - 36)) < 1e-3f);
        }
    }
    o.hud_bounds = 100;
    o.hud_sides = {-1, -1, -1, -1};
    o.display_aspect = 4.0 / 3.0;

    // Two lines for the same axis stop the mod.
    std::snprintf(cfg, sizeof cfg, "patch size 0x%llx hw hh\nlayouts ui 360\nlayouts two 300 axis y\n",
                  static_cast<unsigned long long>(code));
    write(dir / "widescreen.cfg", cfg);
    {
        mods::StartResult twice;
        CHECK(mods::start(o, &twice, &error));
        CHECK(twice.active.empty());
    }

    // A wrong line stops the mod.
    std::snprintf(cfg, sizeof cfg, "patch size 0x%llx hw hh\nlayouts ui 360 skip\n",
                  static_cast<unsigned long long>(code));
    write(dir / "widescreen.cfg", cfg);
    o.display_aspect = 4.0 / 3.0;
    mods::StartResult bad;
    CHECK(mods::start(o, &bad, &error));
    CHECK(bad.active.empty());
    mods::stop();
}

// Touches on the widened picture: the 1280x720 layout sits in its middle.
void widescreen_touches() {
    auto at = [](float u, float v, mods::PictureSize picture, float eu, float ev) {
        mods::widescreen_touch(&u, &v, picture);
        CHECK(std::abs(u - eu) < 1e-5f && std::abs(v - ev) < 1e-5f);
    };
    at(0.5f, 0.5f, {1680, 720}, 0.5f, 0.5f);                   // middle stays
    at(200.0f / 1680, 0.25f, {1680, 720}, 0.0f, 0.25f);        // old left edge at x 200
    at(1480.0f / 1680, 0.25f, {1680, 720}, 1.0f, 0.25f);       // old right edge
    at(0.05f, 0.25f, {1680, 720}, 0.0f, 0.25f);                // side strip: clamped
    at(0.5f, 120.0f / 960, {1280, 960}, 0.5f, 0.0f);           // 4:3: old top edge at y 120
    at(0.5f, 600.0f / 960, {1280, 960}, 0.5f, 480.0f / 720);   // y 600 -> 480
    at(0.3f, 0.7f, {1280, 720}, 0.3f, 0.7f);                   // no widening
    at(0.3f, 0.7f, {0, 0}, 0.3f, 0.7f);                        // mod off
    // With zones (4:3, 1280x960, layout y up): moved groups and spread rows find their old place.
    const mods::TouchZone zones[] = {{278, -278, 0, true}, {90, -90, 280, false}};
    auto zat = [&](float x, float y, size_t first, float ex, float ey) {  // zone `first` alone in force
        float u = (x + 640) / 1280, v = (480 - y) / 960;
        mods::widescreen_touch(&u, &v, {1280, 960}, {1280, 720}, zones + first, 1);
        CHECK(std::abs(u - (ex + 640) / 1280) < 1e-5f && std::abs(v - (360 - ey) / 720) < 1e-5f);
    };
    zat(500, -450, 0, 500, -330);    // Pokédex: footer button at the new bottom -> its old place
    zat(0, 420, 0, 0, 300);          // header
    zat(100, -200, 0, 100, -150);    // a spread row
    zat(-500, -300, 1, -500, -180);  // battle: box in the corner
    zat(0, -300, 1, 0, -300);        // battle: middle, not beside, stays
    // xzone (21:9, 1680x720): corner elements moved out by 200 find their old place; y zones do not act here.
    const mods::TouchZone xzones[] = {{250, -250, 0, false, true}, {278, -278, 0, true}};
    auto xat = [&](float x, float y, float ex, float ey) {
        float u = (x + 840) / 1680, v = (360 - y) / 720;
        mods::widescreen_touch(&u, &v, {1680, 720}, {1280, 720}, xzones, 2);
        CHECK(std::abs(u - (ex + 640) / 1280) < 1e-5f && std::abs(v - (360 - ey) / 720) < 1e-5f);
    };
    xat(650, 300, 450, 300);    // HP box at the new right edge
    xat(-700, -300, -500, -300);  // command at the new left edge
    xat(300, 0, 300, 0);        // middle: as before
}

void write(const fs::path& p, const std::string& text) {
    std::ofstream(p, std::ios::binary) << text;
}

void start(const fs::path& dir) {
    const uint64_t doors[] = {kTouch, kOther};
    rt::declare_hookable(doors, 2);
    mods::StartOptions o;
    o.dir = dir;
    o.enabled = {"wild-battles"};
    std::string error;

    {  // no file: not started, no error
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active.empty());
        CHECK_EQ(r.messages.size(), 1u);
        CHECK(!mods::enabled("wild-battles"));
    }
    {  // the role has no door: not started, nothing left installed
        write(dir / "wild-battles.cfg", "hook encounter_touch 0x8009000\n");
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active.empty());
        CHECK(!r.messages.empty() && r.messages[0].find("no door") != std::string::npos);
        CHECK_EQ(rt::hook_count(), 0u);
    }
    {  // the role is missing
        write(dir / "wild-battles.cfg", "value x 1\n");
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active.empty());
    }
    {  // started: the hook runs and calls the original
        write(dir / "wild-battles.cfg", "hook encounter_touch 0x8001000\nvalue x 7\n");
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CHECK_EQ(r.active.size(), 1u);
        CHECK(mods::enabled("wild-battles"));
        CHECK(mods::mod_file("wild-battles") != nullptr && mods::mod_file("wild-battles")->value("x") == 7u);
        CHECK(rt::find_hook(kTouch) != nullptr);
        CHECK(rt::find_hook(kOther) == nullptr);
        CpuState cpu{};
        g_original_calls = 0;
        rt::call_hooked(&cpu, kTouch, &original);
        rt::call_hooked(&cpu, kOther, &original);
        CHECK_EQ(g_original_calls, 2);
        CHECK_EQ(cpu.x[0], 2u);
    }
    {  // hook log on the free door only
        o.hook_log = 2;
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CHECK_EQ(rt::hook_count(), 2u);
        CpuState cpu{};
        g_original_calls = 0;
        for (int i = 0; i < 3; ++i)
            rt::call_hooked(&cpu, kOther, &original);
        CHECK_EQ(g_original_calls, 3);
    }
    {  // unknown id and a broken file are errors
        mods::StartOptions bad = o;
        bad.enabled = {"nope"};
        mods::StartResult r;
        error.clear();
        CHECK(!mods::start(bad, &r, &error));
        CHECK(!error.empty());
        write(dir / "wild-battles.cfg", "hook encounter_touch\n");
        error.clear();
        CHECK(!mods::start(o, &r, &error));
        CHECK(!error.empty());
    }
    mods::stop();
    CHECK_EQ(rt::hook_count(), 0u);
    CHECK(!mods::enabled("wild-battles"));

    std::vector<mods::ModFile> files;
    write(dir / "wild-battles.cfg", "hook encounter_touch 0x8001000\n");
    write(dir / "other.cfg", "hook x 0x8002000\nhook y 0x8001000\n");
    write(dir / "notes.txt", "not a mod\n");
    CHECK(mods::load_mod_files(dir, &files, &error));
    CHECK_EQ(files.size(), 2u);
    CHECK(!files.empty() && files[0].id == "other");
    files.clear();
    CHECK(mods::load_mod_files(dir / "missing", &files, &error));
    CHECK(files.empty());
}

void dumps() {
    std::vector<mods::HookDump> d;
    std::string error;
    CHECK(mods::parse_hook_dumps("0x8001000:x1>0+0x10:32,0x8002000:sp:8,", &d, &error));
    CHECK_EQ(d.size(), 2u);
    CHECK_EQ(d[0].va, kTouch);
    CHECK_EQ(d[0].reg, 1u);
    CHECK_EQ(d[0].steps.size(), 2u);
    CHECK(d[0].steps[0].load && d[0].steps[0].offset == 0);
    CHECK(!d[0].steps[1].load && d[0].steps[1].offset == 0x10);
    CHECK_EQ(d[0].length, 32u);
    CHECK(d[0].expr == "x1>0+0x10");
    CHECK_EQ(d[1].reg, 31u);
    CHECK(!d[0].after && !d[1].after);
    {
        std::vector<mods::HookDump> a;
        CHECK(mods::parse_hook_dumps("0x8001000:x1+8:16:after,0x8001000:ret>0:4:after", &a, &error));
        CHECK(a.size() == 2 && a[0].after && a[1].after && a[1].reg == mods::HookDump::kRet);
    }
    for (const char* bad : {"0x8001000:x31:8", "0x8001000:x1:0", "0x8001000:x1:4097", "0x8001000:x1", "0x8001000:y1:8",
                            "0x8001000:x1>:8", "0x8001000:x1*8:8", "0x8001000:ret:8", "0x8001000:x1:8:before",
                            "0x8001000:x1:8:"}) {
        std::vector<mods::HookDump> e;
        error.clear();
        CHECK(!mods::parse_hook_dumps(bad, &e, &error));
        CHECK(!error.empty());
    }

    // x1 points at a pointer to an object; dump 20 bytes from 0x10 into it.
    rt::GuestMemory memory(rt::Layout::packed(0x0800'0000, 0x10'0000, 0x10'0000, 0x10'0000, 0x10'0000, 0x40'0000));
    const uint64_t heap = memory.layout().heap.base;
    CHECK(memory.map(heap, rt::kPageSize, rt::MemState::Normal, rt::kPermRW).ok());
    rt::store<uint64_t>(heap, heap + 0x100);
    for (unsigned i = 0; i < 0x40; ++i)
        rt::store<uint8_t>(heap + 0x100 + i, static_cast<uint8_t>(i));
    CpuState cpu{};
    cpu.x[1] = heap;
    mods::HookDump one = d[0];
    one.length = 20;
    std::vector<std::string> lines = mods::hook_dump_lines(one, cpu, 3);
    for (const std::string& l : lines)
        std::printf("  %s\n", l.c_str());
    CHECK_EQ(lines.size(), 3u);
    CHECK(lines.size() == 3 && lines[1] == "hook dump: 0x8001000 call 3 +000 10 11 12 13 14 15 16 17 18 19 1a 1b 1c 1d 1e 1f");
    CHECK(lines.size() == 3 && lines[2] == "hook dump: 0x8001000 call 3 +010 20 21 22 23");
    // After the call: the entry registers, ret for x0 after the call.
    std::vector<mods::HookDump> a;
    CHECK(mods::parse_hook_dumps("0x8001000:ret+0x20:2:after", &a, &error) && a.size() == 1);
    lines = mods::hook_dump_lines(a[0], cpu, 1, heap + 0x100);
    CHECK(lines.size() == 2 && lines[0].find(" call 1 after ret+0x20 = ") != std::string::npos);
    CHECK(lines.size() == 2 && lines[1] == "hook dump: 0x8001000 call 1 after +000 20 21");
    cpu.x[1] = heap + rt::kPageSize;  // the pointer is outside the mapping
    lines = mods::hook_dump_lines(one, cpu, 3);
    CHECK(lines.size() == 1 && lines[0].find("pointer at") != std::string::npos);
    cpu.x[1] = heap;
    one.length = rt::kPageSize;  // runs past the mapping
    lines = mods::hook_dump_lines(one, cpu, 3);
    CHECK(lines.size() == 1 && lines[0].find("unreadable") != std::string::npos);
}

// LGP_HOOK_ACT (Fang-4b): a call into a host function in the dispatch
// table, stores and a return value on a hook-log door.
constexpr uint64_t kActFn = 0x0801'6000;
void fake_act_fn(CpuState* ctx) {  // stores x1 at x2 + 0x18, clobbers x5, returns 2 * x0
    rt::store<uint64_t>(ctx->x[2] + 0x18, ctx->x[1]);
    ctx->x[5] = 99;
    ctx->x[0] *= 2;
}

void acts(const fs::path& dir) {
    std::string error;
    {
        std::vector<mods::HookAct> a;
        CHECK(mods::parse_hook_acts("0x8002000#2:call:0x8016000(x0=5;x1=x3>0+4),0x8002000:set:x2+8=-1/8,"
                                    "0x8002000#3-:ret:0x77,0x8002000#1-1:set:x2+0x10=ret:after",
                                    &a, &error));
        CHECK_EQ(a.size(), 4u);
        CHECK(a.size() == 4 && a[0].kind == mods::HookAct::kCall && a[0].fn == kActFn && a[0].first == 2 &&
              a[0].last == 2 && a[0].args.size() == 2);
        CHECK(a.size() == 4 && a[0].args.size() == 2 && a[0].args[0].first == 0 && a[0].args[0].second.constant &&
              a[0].args[0].second.number == 5 && a[0].args[1].first == 1 && !a[0].args[1].second.constant);
        CHECK(a.size() == 4 && a[1].kind == mods::HookAct::kSet && a[1].size == 8 && a[1].value.number == ~0ull &&
              a[1].first == 1 && a[1].last == UINT32_MAX);
        CHECK(a.size() == 4 && a[2].kind == mods::HookAct::kRet && a[2].first == 3 && a[2].last == UINT32_MAX);
        CHECK(a.size() == 4 && a[3].after && a[3].size == 4 && a[3].value.expr.reg == mods::HookDump::kRet);
        CHECK(a.size() == 4 && a[2].text == "ret:0x77");
    }
    {  // retf: a float in s0, the original skipped
        std::vector<mods::HookAct> a;
        CHECK(mods::parse_hook_acts("0x8002000#2:retf:1.5", &a, &error));
        CHECK(a.size() == 1 && a[0].kind == mods::HookAct::kRetFloat && a[0].real == 1.5f && a[0].first == 2);
        CpuState c{};
        uint64_t ret = 0;
        CHECK(!mods::run_hook_acts(a, false, &c, c, 2, &ret));
        CHECK_EQ(c.vreg[0].lo, 0x3fc00000u);
        CHECK(mods::run_hook_acts(a, false, &c, c, 1, &ret));  // not this call
    }
    for (const char* bad : {"0x8002000:ret:1:after", "0x8002000:jump:1", "0x8002000#0:ret:1", "0x8002000#3-2:ret:1",
                            "0x8002000:set:x2=1/3", "0x8002000:set:x2", "0x8002000:call:0x8016000", "0x8002000:ret:",
                            "0x8002000:call:0x8016000(x31=1)", "0x8002000:call:0x8016000(x1>0=1)", "zz:ret:1",
                            "0x8002000:retf:", "0x8002000:retf:1x", "0x8002000:retf:1:after"}) {
        std::vector<mods::HookAct> e;
        error.clear();
        CHECK(!mods::parse_hook_acts(bad, &e, &error));
        CHECK(!error.empty());
    }

    rt::GuestMemory memory(rt::Layout::packed(0x0800'0000, 0x10'0000, 0x10'0000, 0x10'0000, 0x10'0000, 0x40'0000));
    const uint64_t heap = memory.layout().heap.base;
    CHECK(memory.map(heap, rt::kPageSize, rt::MemState::Normal, rt::kPermRW).ok());
    rt::store<uint64_t>(heap + 0x100, heap + 0x200);
    const uint64_t doors[] = {kOther};
    rt::declare_hookable(doors, 1);
    const LiftedEntry fns[] = {{kActFn, &fake_act_fn}};
    rt::register_functions(fns, 1);

    mods::StartOptions o;
    o.dir = dir;
    o.hook_log = 1;
    CHECK(mods::parse_hook_acts("0x8002000#2:call:0x8016000(x0=5;x1=x3>0+4),0x8002000:set:x2+8=-1/8,"
                                "0x8002000#3-:ret:0x77,0x8002000#1:set:x2+0x10=ret:after,0x8003000:ret:1",
                                &o.acts, &error));
    mods::StartResult r;
    CHECK(mods::start(o, &r, &error));
    bool ignored = false;  // 0x8003000 is not a door
    for (const std::string& m : r.messages)
        ignored |= m.find("hook act: 0x8003000 ret:1 ignored") != std::string::npos;
    CHECK(ignored);
    CpuState cpu{};
    cpu.x[0] = 10;
    cpu.x[2] = heap;
    cpu.x[3] = heap + 0x100;
    cpu.x[5] = 3;
    g_original_calls = 0;
    rt::call_hooked(&cpu, kOther, &original);  // call 1: the store, the original, the store after it
    CHECK_EQ(g_original_calls, 1);
    CHECK_EQ(cpu.x[0], 11u);
    CHECK_EQ(rt::load<uint64_t>(heap + 8), ~0ull);
    CHECK_EQ(rt::load<uint32_t>(heap + 0x10), 11u);
    rt::store<uint64_t>(heap + 8, 0);
    rt::call_hooked(&cpu, kOther, &original);  // call 2: the call; its registers are put back
    CHECK_EQ(g_original_calls, 2);
    CHECK_EQ(rt::load<uint64_t>(heap + 0x18), heap + 0x204);
    CHECK_EQ(cpu.x[5], 3u);
    CHECK_EQ(cpu.x[0], 12u);
    CHECK_EQ(rt::load<uint64_t>(heap + 8), ~0ull);
    CHECK_EQ(rt::load<uint32_t>(heap + 0x10), 11u);  // the store after is on call 1 only
    for (int i = 0; i < 2; ++i)  // calls 3 and 4: x0 = 0x77 without the original
        rt::call_hooked(&cpu, kOther, &original);
    CHECK_EQ(g_original_calls, 2);
    CHECK_EQ(cpu.x[0], 0x77u);
    {  // a call after the original: the door's registers, not the returned ones
        std::vector<mods::HookAct> a;
        CHECK(mods::parse_hook_acts("0x8002000:call:0x8016000(x1=7):after", &a, &error));
        CpuState entry{};
        entry.x[0] = 4;
        entry.x[2] = heap;
        CpuState c = entry;
        c.x[0] = 0x1234;  // the original's return value
        c.x[2] = 0;       // clobbered
        uint64_t ret = 0;
        CHECK(mods::run_hook_acts(a, true, &c, entry, 1, &ret));
        CHECK_EQ(rt::load<uint64_t>(heap + 0x18), 7u);
        CHECK_EQ(ret, 8u);
        CHECK_EQ(c.x[0], 0x1234u);  // put back
    }
    mods::stop();
    CHECK_EQ(rt::hook_count(), 0u);
}

// battle-throw (Fang-4b): recall at throw start, throw wait held, send-out
// after a miss; the success flag is a byte at x1 + 4 of the handler.
constexpr uint64_t kThrowHandler = 0x0801'7000;
constexpr uint64_t kThrowStart = 0x0801'8000;
constexpr uint64_t kThrowWait = 0x0801'9000;
constexpr uint64_t kRecall = 0x0801'a000;
constexpr uint64_t kSendOut = 0x0801'b000;
int g_recalls = 0;
uint64_t g_recall_args[3] = {};
int g_sendouts = 0;
uint64_t g_recall_text_va = 0;  // the recall's path text (Fang-22), read at each recall
std::string g_recall_text;
uint64_t g_recall_ball_va = 0;  // the ball slot's kind, which the recall sets to the Pokémon's own ball (Fang-32b)
void fake_recall(CpuState* ctx) {
    ++g_recalls;
    if (g_recall_ball_va != 0)
        rt::store<uint32_t>(g_recall_ball_va, 4);
    g_recall_text.clear();
    for (uint64_t va = g_recall_text_va; va != 0; ++va) {
        const char c = static_cast<char>(rt::load<uint8_t>(va));
        if (c == 0)
            break;
        g_recall_text += c;
    }
    for (int i = 0; i < 3; ++i)
        g_recall_args[i] = ctx->x[i];
    ctx->x[7] = 1234;  // clobbered, the mod puts it back
}
void fake_sendout(CpuState* ctx) { ctx->x[0] = ++g_sendouts >= 3 ? 1 : 0; }
constexpr uint64_t kRecallWait = 0x0801'c000;
int g_recall_waits = 0;
int g_recall_wait_done = 1;  // calls until it reports 1
uint64_t g_recall_wait_view = 0;
constexpr uint64_t kSendOutStart = 0x0801'd000;
std::vector<uint64_t> g_sendout_start_args;  // x0..x3 of each call
void fake_sendout_start(CpuState* ctx) {
    CHECK_EQ(g_sendouts, 0);  // before the update
    g_sendout_start_args.insert(g_sendout_start_args.end(), ctx->x, ctx->x + 4);
}
void fake_recall_wait(CpuState* ctx) {
    g_recall_wait_view = ctx->x[0];
    ctx->x[0] = ++g_recall_waits >= g_recall_wait_done ? 1 : 0;
}
void throw_done(CpuState* ctx) { ctx->x[0] = 1; }  // throw wait as in LGP: done at once
constexpr uint64_t kKeepPlay = 0x0801'e000;  // the send-out sequence without reloading (Fang-29)
uint64_t g_keep_busy_va = 0;                 // the sequence controller's state, counted down by the test
int g_keep_plays = 0;
uint64_t g_keep_play_args[2] = {};
void fake_keep_play(CpuState* ctx) {
    ++g_keep_plays;
    g_keep_play_args[0] = ctx->x[0];
    g_keep_play_args[1] = ctx->x[1];
    rt::store<uint8_t>(g_keep_busy_va, 2);  // the sequence runs
    ctx->x[3] = 99;                         // clobbered, the mod puts it back
}

constexpr uint64_t kPartner = 0x0801'e800;  // the view's partner slot (Fang-40)
uint64_t g_partner_result = 0;
uint64_t g_partner_args[2] = {};
void fake_partner(CpuState* ctx) {
    g_partner_args[0] = ctx->x[0];
    g_partner_args[1] = ctx->x[1];
    ctx->x[0] = g_partner_result;
    ctx->x[7] = 4321;  // clobbered, the mod puts it back
}

// The held ball (Fang-42): the ball's doors and the game's removal.
constexpr uint64_t kHoldInit = 0x0801'f000;
constexpr uint64_t kHoldDeinitDoor = 0x0801'f100;
constexpr uint64_t kHoldRelease = 0x0801'f200;
constexpr uint64_t kHoldDeinit = 0x0801'f300;
constexpr uint64_t kHoldListRemove = 0x0801'f400;
std::vector<uint64_t> g_hold_calls;  // 1, ball for deinit; 2, list, *x1 for list_remove
void fake_hold_deinit(CpuState* ctx) {
    g_hold_calls.insert(g_hold_calls.end(), {1, ctx->x[0]});
    ctx->x[5] = 0;  // clobbered, the mod puts it back
}
void fake_hold_list_remove(CpuState* ctx) {
    CHECK(ctx->sp < ctx->x[1]);  // the pointer lies above the call's frame
    g_hold_calls.insert(g_hold_calls.end(), {2, ctx->x[0], rt::load<uint64_t>(ctx->x[1])});
}

// The thrown ball stored with the catch (Fang-48).
constexpr uint64_t kCatchStore = 0x0801'f500;
constexpr uint64_t kBallSetter = 0x0801'f600;
std::vector<uint64_t> g_setter_calls;  // x0, x1 of each call
void fake_ball_setter(CpuState* ctx) {
    g_setter_calls.insert(g_setter_calls.end(), {ctx->x[0], ctx->x[1]});
    ctx->x[1] = 0;  // clobbered, the mod puts it back
}

void battle_throw(const fs::path& dir) {
    rt::GuestMemory memory(rt::Layout::packed(0x0800'0000, 0x10'0000, 0x10'0000, 0x10'0000, 0x10'0000, 0x40'0000));
    const uint64_t heap = memory.layout().heap.base;
    CHECK(memory.map(heap, rt::kPageSize, rt::MemState::Normal, rt::kPermRW).ok());
    const uint64_t doors[] = {kThrowHandler, kThrowStart, kThrowWait};
    rt::declare_hookable(doors, 3);
    const LiftedEntry fns[] = {{kRecall, &fake_recall},
                               {kSendOut, &fake_sendout},
                               {kRecallWait, &fake_recall_wait},
                               {kSendOutStart, &fake_sendout_start},
                               {kKeepPlay, &fake_keep_play},
                               {kPartner, &fake_partner}};
    rt::register_functions(fns, 6);
    mods::StartOptions o;
    o.dir = dir;
    o.enabled = {"battle-throw"};
    std::string error;
    {  // no success flag: not started
        write(dir / "battle-throw.cfg", "hook throw_start 0x8018000\nhook throw_wait 0x8019000\n"
                                        "value recall_start 0x801a000\nvalue sendout_update 0x801b000\n");
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active.empty());
        CHECK(!r.messages.empty() && r.messages[0].find("throw_success") != std::string::npos);
        mods::stop();
    }
    {  // no send-out start: not started (a miss would crash, Fang-15)
        write(dir / "battle-throw.cfg", "hook throw_start 0x8018000\nhook throw_wait 0x8019000\n"
                                        "value recall_start 0x801a000\nvalue sendout_update 0x801b000\n"
                                        "expr throw_success 1 x1+4\nvalue recall_wait 0x801c000\n");
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active.empty());
        CHECK(!r.messages.empty() && r.messages[0].find("sendout_start") != std::string::npos);
        mods::stop();
    }
    {  // no recall wait: not started (the battle's end would hang after a miss, Fang-14)
        write(dir / "battle-throw.cfg", "hook throw_start 0x8018000\nhook throw_wait 0x8019000\n"
                                        "value recall_start 0x801a000\nvalue sendout_update 0x801b000\n"
                                        "expr throw_success 1 x1+4\n");
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active.empty());
        CHECK(!r.messages.empty() && r.messages[0].find("recall_wait") != std::string::npos);
        mods::stop();
    }
    write(dir / "battle-throw.cfg", "hook throw_handler 0x8017000\nhook throw_start 0x8018000\n"
                                    "hook throw_wait 0x8019000\nvalue recall_start 0x801a000\n"
                                    "value sendout_update 0x801b000\nexpr throw_success 1 x1+4\nvalue wait_frames 3\n"
                                    "value recall_wait 0x801c000\nvalue sendout_start 0x801d000\nvalue sendout_side 1\n");
    mods::StartResult r;
    CHECK(mods::start(o, &r, &error));
    CHECK_EQ(r.active.size(), 1u);
    for (int caught = 1; caught >= 0; --caught) {
        rt::store<uint8_t>(heap + 4, static_cast<uint8_t>(caught));
        g_recalls = 0;
        g_sendouts = 0;
        g_sendout_start_args.clear();
        CpuState cpu{};
        cpu.x[1] = heap;
        rt::call_hooked(&cpu, kThrowHandler, &original);
        cpu.x[0] = 0x5000;  // the battle view
        cpu.x[1] = 2;       // the position
        cpu.x[7] = 7;
        g_original_calls = 0;
        rt::call_hooked(&cpu, kThrowStart, &original);
        CHECK_EQ(g_recalls, 1);
        CHECK(g_recall_args[0] == 0x5000 && g_recall_args[1] == 2 && g_recall_args[2] == 0);
        CHECK_EQ(cpu.x[7], 7u);
        CHECK_EQ(g_original_calls, 1);
        int calls = 0;
        uint64_t done = 0;
        while (done == 0 && calls < 20) {
            cpu.x[0] = 0x5000;
            rt::call_hooked(&cpu, kThrowWait, &throw_done);
            done = cpu.x[0];
            ++calls;
        }
        CHECK_EQ(calls, caught ? 4 : 6);  // 3 held, then done; a miss adds the send-out until it reports 1
        CHECK_EQ(g_sendouts, caught ? 0 : 3);
        // A miss starts the send-out once: view, position, the other side, team 0.
        CHECK(g_sendout_start_args ==
              (caught ? std::vector<uint64_t>{} : std::vector<uint64_t>{0x5000, 2, 1, 0}));
    }
    mods::stop();
    {  // recall_wait (Fang-14): after the hold until it reports 1, before the send-out
        write(dir / "battle-throw.cfg", "hook throw_handler 0x8017000\nhook throw_start 0x8018000\n"
                                        "hook throw_wait 0x8019000\nvalue recall_start 0x801a000\n"
                                        "value sendout_update 0x801b000\nexpr throw_success 1 x1+4\n"
                                        "value wait_frames 3\nvalue recall_wait 0x801c000\nvalue sendout_start 0x801d000\nvalue sendout_side 1\n");
        mods::StartResult rw;
        CHECK(mods::start(o, &rw, &error));
        CHECK_EQ(rw.active.size(), 1u);
        for (int run = 0; run < 3; ++run) {
            const bool caught = run == 0;
            g_recall_wait_done = run == 2 ? 7 : 2;  // run 2: a longer wait, still waited for
            rt::store<uint8_t>(heap + 4, static_cast<uint8_t>(caught));
            g_recalls = g_sendouts = g_recall_waits = 0;
            g_recall_wait_view = 0;
            CpuState cpu{};
            cpu.x[1] = heap;
            rt::call_hooked(&cpu, kThrowHandler, &original);
            cpu.x[0] = 0x5000;
            cpu.x[1] = 2;
            rt::call_hooked(&cpu, kThrowStart, &original);
            int calls = 0;
            uint64_t done = 0;
            while (done == 0 && calls < 30) {
                cpu.x[0] = 0x5000;
                rt::call_hooked(&cpu, kThrowWait, &throw_done);
                done = cpu.x[0];
                ++calls;
                if (g_sendouts > 0)
                    CHECK(g_recall_waits >= (run == 2 ? 7 : 2));  // the send-out only after the recall wait
            }
            // 3 held, the recall wait (2 calls, run 2: 7), then done or the send-out (3 calls, the first
            // in the recall wait's last call).
            CHECK_EQ(calls, run == 0 ? 5 : run == 1 ? 7 : 12);
            CHECK_EQ(g_recall_waits, run == 2 ? 7 : 2);
            CHECK_EQ(g_recall_wait_view, 0x5000u);
            CHECK_EQ(g_sendouts, caught ? 0 : 3);
        }
        mods::stop();
        write(dir / "battle-throw.cfg", "hook throw_handler 0x8017000\nhook throw_start 0x8018000\n"
                                        "hook throw_wait 0x8019000\nvalue recall_start 0x801a000\n"
                                        "value sendout_update 0x801b000\nexpr throw_success 1 x1+4\n"
                                        "value wait_frames 3\nvalue recall_wait 0x801c000\nvalue sendout_start 0x801d000\nvalue sendout_side 1\n");
        g_recall_wait_done = 1;
        CHECK(mods::start(o, &rw, &error));
    }
    {  // the throw's own sequences (Fang-22): the path text only for the time of our recall
        mods::stop();
        const uint64_t text = heap + 0x100;
        const char original_text[] = "bin/seq/ee610.bseq";
        for (size_t i = 0; i < sizeof original_text; ++i)
            rt::store<uint8_t>(text + i, static_cast<uint8_t>(original_text[i]));
        const std::string base = "hook throw_handler 0x8017000\nhook throw_start 0x8018000\n"
                                 "hook throw_wait 0x8019000\nvalue recall_start 0x801a000\n"
                                 "value sendout_update 0x801b000\nexpr throw_success 1 x1+4\nexpr throw_shakes 1 x1+5\n"
                                 "value wait_frames 3\nvalue recall_wait 0x801c000\nvalue sendout_start 0x801d000\n"
                                 "value sendout_side 1\n";
        const std::string seqs = "path throw_seq_success 0 bin/seq/ee301.bseq\npath throw_seq_fail1 0 bin/seq/ee302.bseq\n"
                                 "path throw_seq_fail2 0 bin/seq/ee3020.bseq\n";
        {  // sequences without the text: not started
            write(dir / "battle-throw.cfg", base + seqs);
            mods::StartResult rs;
            CHECK(mods::start(o, &rs, &error));
            CHECK(rs.active.empty());
            CHECK(!rs.messages.empty() && rs.messages[0].find("recall_seq_text") != std::string::npos);
            mods::stop();
        }
        char text_line[64];
        std::snprintf(text_line, sizeof text_line, "value recall_seq_text 0x%llx\n",
                      static_cast<unsigned long long>(text));
        {  // with ball_create: not started, the sequence builds the ball
            write(dir / "battle-throw.cfg", base + seqs + text_line + "value ball_create 0x8020000\n");
            mods::StartResult rs;
            CHECK(mods::start(o, &rs, &error));
            CHECK(rs.active.empty());
            CHECK(!rs.messages.empty() && rs.messages[0].find("exclude") != std::string::npos);
            mods::stop();
        }
        write(dir / "battle-throw.cfg", base + seqs + text_line);
        mods::StartResult rs;
        CHECK(mods::start(o, &rs, &error));
        CHECK_EQ(rs.active.size(), 1u);
        g_recall_text_va = text;
        const struct {
            bool caught;
            uint8_t shakes;
            const char* played;
        } runs[] = {{true, 3, "bin/seq/ee301.bseq"},
                    {false, 0, "bin/seq/ee302.bseq"},  // fail0 missing: the nearest with more shakes
                    {false, 3, "bin/seq/ee610.bseq"},  // fail2 is the nearest fewer, but too long: plain recall
                    {false, 1, "bin/seq/ee302.bseq"}};
        for (const auto& run : runs) {
            rt::store<uint8_t>(heap + 4, run.caught ? 1 : 0);
            rt::store<uint8_t>(heap + 5, run.shakes);
            g_recalls = g_sendouts = g_recall_waits = 0;
            g_sendout_start_args.clear();
            CpuState cpu{};
            cpu.x[1] = heap;
            rt::call_hooked(&cpu, kThrowHandler, &original);
            cpu.x[0] = 0x5000;
            cpu.x[1] = 2;
            rt::call_hooked(&cpu, kThrowStart, &original);
            CHECK(g_recall_text == run.played);
            CHECK_EQ(rt::load<uint8_t>(text + 10), static_cast<uint8_t>('6'));  // the original back right after
            int calls = 0;
            uint64_t done = 0;
            while (done == 0 && calls < 20) {
                cpu.x[0] = 0x5000;
                rt::call_hooked(&cpu, kThrowWait, &throw_done);
                done = cpu.x[0];
                ++calls;
            }
            CHECK_EQ(g_sendouts, run.caught ? 0 : 3);
            CHECK_EQ(rt::load<uint8_t>(text + 10), static_cast<uint8_t>('6'));  // the original back
            CHECK_EQ(rt::load<uint8_t>(text + sizeof original_text - 1), 0u);
        }
        {  // the partner's own sequences (Fang-40): chosen through the view's vtable slot
            mods::stop();
            const uint64_t view = heap + 0x400, vtable = heap + 0x600;
            rt::store<uint64_t>(view, vtable);
            rt::store<uint64_t>(vtable + 0x38, kPartner);
            const std::string partner = "value throw_partner_slot 0x38\n"
                                        "path throw_seq_partner_success 0 bin/seq/ex301.bseq\n"
                                        "path throw_seq_partner_fail1 0 bin/seq/ex302.bseq\n";
            {  // partner sequences without the slot: not started
                write(dir / "battle-throw.cfg", base + seqs + text_line +
                                                    "path throw_seq_partner_success 0 bin/seq/ex301.bseq\n");
                mods::StartResult r2;
                CHECK(mods::start(o, &r2, &error));
                CHECK(r2.active.empty());
                CHECK(!r2.messages.empty() && r2.messages[0].find("throw_partner_slot") != std::string::npos);
                mods::stop();
            }
            {  // the slot without partner sequences: not started
                write(dir / "battle-throw.cfg", base + seqs + text_line + "value throw_partner_slot 0x38\n");
                mods::StartResult r2;
                CHECK(mods::start(o, &r2, &error));
                CHECK(r2.active.empty());
                CHECK(!r2.messages.empty() && r2.messages[0].find("throw_seq_partner") != std::string::npos);
                mods::stop();
            }
            write(dir / "battle-throw.cfg", base + seqs + text_line + partner);
            mods::StartResult r2;
            CHECK(mods::start(o, &r2, &error));
            CHECK_EQ(r2.active.size(), 1u);
            const struct {
                uint64_t result;
                bool caught;
                uint8_t shakes;
                const char* played;
            } partner_runs[] = {{1, true, 3, "bin/seq/ex301.bseq"},
                                {3, false, 0, "bin/seq/ex302.bseq"},  // bit 0; fail0 missing: the nearest more
                                {0, true, 3, "bin/seq/ee301.bseq"},   // not the partner
                                {2, false, 1, "bin/seq/ee302.bseq"}};
            for (const auto& run : partner_runs) {
                g_partner_result = run.result;
                g_partner_args[0] = g_partner_args[1] = 99;
                rt::store<uint8_t>(heap + 4, run.caught ? 1 : 0);
                rt::store<uint8_t>(heap + 5, run.shakes);
                g_recalls = g_sendouts = g_recall_waits = 0;
                CpuState cpu{};
                cpu.x[1] = heap;
                rt::call_hooked(&cpu, kThrowHandler, &original);
                cpu.x[0] = view;
                cpu.x[1] = 2;
                cpu.x[7] = 7;
                rt::call_hooked(&cpu, kThrowStart, &original);
                CHECK(g_partner_args[0] == view && g_partner_args[1] == 0);
                CHECK(g_recall_args[0] == view && g_recall_args[1] == 2);
                CHECK(g_recall_text == run.played);
                CHECK_EQ(cpu.x[7], 7u);
                CHECK_EQ(rt::load<uint8_t>(text + 10), static_cast<uint8_t>('6'));
                int calls = 0;
                uint64_t done = 0;
                while (done == 0 && calls < 20) {
                    cpu.x[0] = view;
                    rt::call_hooked(&cpu, kThrowWait, &throw_done);
                    done = cpu.x[0];
                    ++calls;
                }
                CHECK(done != 0);
            }
            {  // an unreadable view: not the partner, the normal sequence
                g_partner_result = 1;
                rt::store<uint8_t>(heap + 4, 1);
                CpuState cpu{};
                cpu.x[1] = heap;
                rt::call_hooked(&cpu, kThrowHandler, &original);
                cpu.x[0] = 0x5000;
                cpu.x[1] = 2;
                rt::call_hooked(&cpu, kThrowStart, &original);
                CHECK(g_recall_text == "bin/seq/ee301.bseq");
                uint64_t done = 0;
                for (int calls = 0; done == 0 && calls < 20; ++calls) {
                    cpu.x[0] = 0x5000;
                    rt::call_hooked(&cpu, kThrowWait, &throw_done);
                    done = cpu.x[0];
                }
            }
        }
        {  // caught_release without the success sequence: not started
            mods::stop();
            write(dir / "battle-throw.cfg", base + "path throw_seq_fail1 0 bin/seq/ee302.bseq\n" + text_line +
                                                "value caught_release 1\n");
            mods::StartResult r2;
            CHECK(mods::start(o, &r2, &error));
            CHECK(r2.active.empty());
            CHECK(!r2.messages.empty() && r2.messages[0].find("caught_release") != std::string::npos);
        }
        {  // caught_release (Fang-27 A7): a caught ball frees the wait early, without the recall wait
            mods::stop();
            write(dir / "battle-throw.cfg", base + seqs + text_line + "value caught_release 1\n");
            mods::StartResult r2;
            CHECK(mods::start(o, &r2, &error));
            CHECK_EQ(r2.active.size(), 1u);
            for (const bool caught : {true, false}) {
                rt::store<uint8_t>(heap + 4, caught ? 1 : 0);
                rt::store<uint8_t>(heap + 5, 1);
                g_recalls = g_sendouts = g_recall_waits = 0;
                CpuState cpu{};
                cpu.x[1] = heap;
                rt::call_hooked(&cpu, kThrowHandler, &original);
                cpu.x[0] = 0x5000;
                cpu.x[1] = 2;
                rt::call_hooked(&cpu, kThrowStart, &original);
                int calls = 0;
                uint64_t done = 0;
                while (done == 0 && calls < 20) {
                    cpu.x[0] = 0x5000;
                    rt::call_hooked(&cpu, kThrowWait, &throw_done);
                    done = cpu.x[0];
                    ++calls;
                }
                CHECK(done != 0);
                if (caught) {
                    CHECK_EQ(calls, 2);  // one held call, then the game's own wait
                    CHECK_EQ(g_recall_waits, 0);
                    CHECK_EQ(g_sendouts, 0);
                } else {
                    CHECK(g_recall_waits > 0);  // a miss waits for the recall as before
                    CHECK_EQ(g_sendouts, 3);
                }
            }
        }
        {  // the held ball (Fang-42): noted at its init from our throw on, removed at the battle's end
            mods::stop();
            const uint64_t hold_doors[] = {kHoldInit, kHoldDeinitDoor, kHoldRelease};
            rt::declare_hookable(hold_doors, 3);
            const LiftedEntry hold_fns[] = {{kHoldDeinit, &fake_hold_deinit}, {kHoldListRemove, &fake_hold_list_remove}};
            rt::register_functions(hold_fns, 2);
            const std::string hold = "hook ball_hold_init 0x801f000\nhook ball_hold_deinit 0x801f100\n"
                                     "hook ball_hold_release 0x801f200\nexpr ball_hold_list 0 x0+0x488\n"
                                     "value ball_hold_lr 0x8030040\nvalue ball_deinit 0x801f300\n"
                                     "value ball_list_remove 0x801f400\n";
            {  // without the removal: not started
                write(dir / "battle-throw.cfg", base + seqs + text_line +
                                                    "hook ball_hold_init 0x801f000\nhook ball_hold_deinit 0x801f100\n"
                                                    "hook ball_hold_release 0x801f200\nexpr ball_hold_list 0 x0+0x488\n");
                mods::StartResult r2;
                CHECK(mods::start(o, &r2, &error));
                CHECK(r2.active.empty());
                CHECK(!r2.messages.empty() && r2.messages[0].find("ball_deinit") != std::string::npos);
                mods::stop();
            }
            {  // without the throw's sequences: not started
                write(dir / "battle-throw.cfg", base + hold);
                mods::StartResult r2;
                CHECK(mods::start(o, &r2, &error));
                CHECK(r2.active.empty());
                CHECK(!r2.messages.empty() && r2.messages[0].find("throw_seq") != std::string::npos);
                mods::stop();
            }
            write(dir / "battle-throw.cfg", base + seqs + text_line + hold);
            mods::StartResult r2;
            CHECK(mods::start(o, &r2, &error));
            CHECK_EQ(r2.active.size(), 1u);
            g_hold_calls.clear();
            const uint64_t stack = heap + 0xc00;
            CpuState cpu{};
            cpu.sp = stack;
            cpu.x[0] = 0xb000;
            cpu.x[30] = 0x8030040;
            rt::call_hooked(&cpu, kHoldInit, &original);  // before our throw: not noted
            rt::store<uint8_t>(heap + 4, 1);
            rt::store<uint8_t>(heap + 5, 3);
            cpu.x[1] = heap;
            rt::call_hooked(&cpu, kThrowHandler, &original);
            cpu.x[0] = 0x5000;
            cpu.x[1] = 2;
            rt::call_hooked(&cpu, kThrowStart, &original);
            for (const uint64_t ball : {0xb100ull, 0xb200ull, 0xb300ull}) {  // the sequence builds three balls
                cpu.x[0] = ball;
                cpu.x[30] = ball == 0xb300 ? 0x8030080 : 0x8030040;  // the last from elsewhere: not noted
                g_original_calls = 0;
                rt::call_hooked(&cpu, kHoldInit, &original);
                CHECK_EQ(g_original_calls, 1);
            }
            cpu.x[0] = 0xb200;  // the game removes one itself (a 0x4e with its task)
            rt::call_hooked(&cpu, kHoldDeinitDoor, &original);
            CHECK(g_hold_calls.empty());
            cpu.x[0] = 0x7777;
            cpu.x[5] = 55;
            g_original_calls = 0;
            rt::call_hooked(&cpu, kHoldRelease, &original);
            CHECK_EQ(g_original_calls, 1);
            // deinit(ball), then list_remove(view + 0x488, &ball): only 0xb100 is left.
            CHECK(g_hold_calls == (std::vector<uint64_t>{1, 0xb100, 2, 0x5488, 0xb100}));
            CHECK_EQ(cpu.sp, stack);
            CHECK_EQ(cpu.x[5], 55u);
            g_hold_calls.clear();
            rt::call_hooked(&cpu, kHoldRelease, &original);  // nothing noted any more
            CHECK(g_hold_calls.empty());
            cpu.x[0] = 0xb400;
            cpu.x[30] = 0x8030040;
            rt::call_hooked(&cpu, kHoldInit, &original);  // after the release: not armed until the next throw
            rt::call_hooked(&cpu, kHoldRelease, &original);
            CHECK(g_hold_calls.empty());
        }
        {  // the thrown ball stored with the catch (Fang-48)
            mods::stop();
            const uint64_t store_door[] = {kCatchStore};
            rt::declare_hookable(store_door, 1);
            const LiftedEntry setter_fns[] = {{kBallSetter, &fake_ball_setter}};
            rt::register_functions(setter_fns, 1);
            {  // without the setter: not started
                write(dir / "battle-throw.cfg", base + "hook catch_store 0x801f500\nvalue catch_store_lr 0x8030100\n");
                mods::StartResult r2;
                CHECK(mods::start(o, &r2, &error));
                CHECK(r2.active.empty());
                CHECK(!r2.messages.empty() && r2.messages[0].find("ball_setter") != std::string::npos);
                mods::stop();
            }
            write(dir / "battle-throw.cfg",
                  base + "hook catch_store 0x801f500\nvalue catch_store_lr 0x8030100\nvalue ball_setter 0x801f600\n");
            mods::StartResult r2;
            CHECK(mods::start(o, &r2, &error));
            CHECK_EQ(r2.active.size(), 1u);
            const uint64_t data = heap + 0x300;  // x1 at the store points to the Pokémon's data
            rt::store<uint64_t>(data, 0xc000);
            auto throw_ball = [&](bool caught, uint64_t ball) {
                g_recalls = g_sendouts = g_recall_waits = 0;
                g_sendout_start_args.clear();
                rt::store<uint8_t>(heap + 4, caught ? 1 : 0);
                rt::store<uint8_t>(heap + 5, 3);
                CpuState cpu{};
                cpu.x[1] = heap;
                rt::call_hooked(&cpu, kThrowHandler, &original);
                cpu.x[0] = 0x5000;
                cpu.x[1] = 2;
                cpu.x[2] = ball;
                rt::call_hooked(&cpu, kThrowStart, &original);
                uint64_t done = 0;
                for (int calls = 0; done == 0 && calls < 30; ++calls) {  // until the throw is over
                    cpu.x[0] = 0x5000;
                    rt::call_hooked(&cpu, kThrowWait, &throw_done);
                    done = cpu.x[0];
                }
                CHECK(done != 0);
            };
            auto store = [&](uint64_t lr) {
                CpuState cpu{};
                cpu.x[1] = data;
                cpu.x[30] = lr;
                g_original_calls = 0;
                rt::call_hooked(&cpu, kCatchStore, &original);
                CHECK_EQ(g_original_calls, 1);
                CHECK_EQ(cpu.x[1], data);
            };
            g_setter_calls.clear();
            store(0x8030100);  // a Go catch without our throw: the game's own ball
            CHECK(g_setter_calls.empty());
            throw_ball(true, 3);  // a Great Ball caught
            store(0x8030200);     // another caller: not ours, still noted
            CHECK(g_setter_calls.empty());
            store(0x8030100);
            CHECK(g_setter_calls == (std::vector<uint64_t>{0xc000, 3}));
            g_setter_calls.clear();
            store(0x8030100);  // the next catch (a Go catch in a new battle): the game's own ball
            CHECK(g_setter_calls.empty());
            throw_ball(true, 1);   // caught, but ...
            throw_ball(false, 2);  // ... a new throw that breaks free clears it
            store(0x8030100);
            CHECK(g_setter_calls.empty());
            throw_ball(true, 12);  // a Premier Ball
            store(0x8030100);
            CHECK(g_setter_calls == (std::vector<uint64_t>{0xc000, 12}));
        }
        {  // keep_busy without keep_sendout: not started
            mods::stop();
            write(dir / "battle-throw.cfg", base + "expr keep_busy 1 x1+0x200\n");
            mods::StartResult r2;
            CHECK(mods::start(o, &r2, &error));
            CHECK(r2.active.empty());
            CHECK(!r2.messages.empty() && r2.messages[0].find("keep_sendout") != std::string::npos);
        }
        {  // a miss that keeps the model (Fang-29): no recall wait, no send-out calls, the acts once
            mods::stop();
            write(dir / "battle-throw.cfg", base + seqs + text_line +
                                                "expr keep_busy 1 x1+0x200\n"
                                                "acts keep_sendout 0 set:x1+0x210=5/1,call:0x801e000(x1=0x630)\n");
            mods::StartResult r2;
            CHECK(mods::start(o, &r2, &error));
            CHECK_EQ(r2.active.size(), 1u);
            g_keep_busy_va = heap + 0x200;
            for (const bool caught : {false, true}) {
                rt::store<uint8_t>(heap + 4, caught ? 1 : 0);
                rt::store<uint8_t>(heap + 5, 1);
                rt::store<uint8_t>(heap + 0x200, 3);  // the recall sequence runs
                rt::store<uint8_t>(heap + 0x210, 0);
                g_recalls = g_sendouts = g_recall_waits = g_keep_plays = 0;
                g_sendout_start_args.clear();
                CpuState cpu{};
                cpu.x[1] = heap;
                rt::call_hooked(&cpu, kThrowHandler, &original);
                cpu.x[0] = 0x5000;
                cpu.x[1] = heap;
                rt::call_hooked(&cpu, kThrowStart, &original);
                int calls = 0;
                uint64_t done = 0;
                while (done == 0 && calls < 30) {
                    if (const uint8_t b = rt::load<uint8_t>(heap + 0x200); b > 0)
                        rt::store<uint8_t>(heap + 0x200, static_cast<uint8_t>(b - 1));  // the sequence player
                    cpu.x[0] = 0x5000;
                    cpu.x[1] = heap;
                    cpu.x[3] = 7;
                    rt::call_hooked(&cpu, kThrowWait, &throw_done);
                    if (g_keep_plays > 0 && done == 0)
                        CHECK_EQ(cpu.x[3], 7u);  // put back after the call act
                    done = cpu.x[0];
                    ++calls;
                }
                CHECK(done != 0);
                CHECK_EQ(g_sendouts, 0);
                CHECK(g_sendout_start_args.empty());
                if (caught) {
                    CHECK_EQ(g_keep_plays, 0);  // a caught ball waits for the recall as before
                    CHECK(g_recall_waits > 0);
                } else {
                    CHECK_EQ(g_recall_waits, 0);  // the model is not unloaded
                    CHECK_EQ(g_keep_plays, 1);
                    CHECK_EQ(g_keep_play_args[0], 0x5000u);
                    CHECK_EQ(g_keep_play_args[1], 0x630u);
                    CHECK_EQ(rt::load<uint8_t>(heap + 0x210), 5u);
                    CHECK_EQ(rt::load<uint8_t>(heap + 0x200), 0u);  // released only once the send-out is done
                }
            }
            g_keep_busy_va = 0;
        }
        {  // trainer_battle (Fang-32): not wild, the game's own throw; wild with throw_prepare before the recall
            mods::stop();
            write(dir / "battle-throw.cfg", base + "expr trainer_battle 4 x1+0x230\n"
                                                "acts throw_prepare 0 set:x1+0x220=3/1\n");
            mods::StartResult r2;
            CHECK(mods::start(o, &r2, &error));
            CHECK_EQ(r2.active.size(), 1u);
            for (const uint32_t kind : {1u, 0u}) {
                rt::store<uint32_t>(heap + 0x230, kind);
                rt::store<uint8_t>(heap + 0x220, 0);
                rt::store<uint8_t>(heap + 4, 1);
                g_recalls = 0;
                CpuState cpu{};
                cpu.x[1] = heap;
                rt::call_hooked(&cpu, kThrowHandler, &original);
                cpu.x[0] = 0x5000;
                cpu.x[1] = heap;
                rt::call_hooked(&cpu, kThrowStart, &original);
                CHECK_EQ(cpu.x[1], heap);
                if (kind != 0) {
                    CHECK_EQ(g_recalls, 0);
                    CHECK_EQ(rt::load<uint8_t>(heap + 0x220), 0u);
                    cpu.x[0] = 0x5000;
                    rt::call_hooked(&cpu, kThrowWait, &throw_done);
                    CHECK_EQ(cpu.x[0], 1u);  // the game's wait, not held
                } else {
                    CHECK_EQ(g_recalls, 1);
                    CHECK_EQ(rt::load<uint8_t>(heap + 0x220), 3u);
                    int calls = 0;
                    uint64_t done = 0;
                    while (done == 0 && calls < 200) {
                        cpu.x[0] = 0x5000;
                        cpu.x[1] = heap;
                        rt::call_hooked(&cpu, kThrowWait, &throw_done);
                        done = cpu.x[0];
                        ++calls;
                    }
                    CHECK(done != 0);
                }
            }
        }
        {  // throw_started (Fang-32b): the thrown ball's kind after the recall wrote the Pokémon's, and while held
            mods::stop();
            write(dir / "battle-throw.cfg", base + "acts throw_started 0 set:x1+0x240=x2/4,call:0x801e000()\n");
            mods::StartResult r2;
            CHECK(mods::start(o, &r2, &error));
            CHECK_EQ(r2.active.size(), 1u);
            g_recall_ball_va = heap + 0x240;
            g_keep_busy_va = heap + 0x200;  // fake_keep_play writes there
            g_keep_plays = 0;
            rt::store<uint8_t>(heap + 4, 1);
            CpuState cpu{};
            cpu.x[1] = heap;
            rt::call_hooked(&cpu, kThrowHandler, &original);
            cpu.x[0] = 0x5000;
            cpu.x[1] = heap;
            cpu.x[2] = 3;  // a Great Ball
            rt::call_hooked(&cpu, kThrowStart, &original);
            CHECK_EQ(rt::load<uint32_t>(heap + 0x240), 3u);
            CHECK_EQ(cpu.x[2], 3u);
            rt::store<uint32_t>(heap + 0x240, 9);
            cpu.x[0] = 0x5000;
            cpu.x[1] = 0;  // other registers at the wait: the acts use throw_start's
            cpu.x[2] = 0;
            rt::call_hooked(&cpu, kThrowWait, &throw_done);
            CHECK_EQ(cpu.x[0], 0u);  // held
            CHECK_EQ(rt::load<uint32_t>(heap + 0x240), 3u);
            CHECK_EQ(cpu.x[1], 0u);
            CHECK_EQ(g_keep_plays, 2);  // after the recall and at the held wait
            CHECK_EQ(g_keep_play_args[0], 0x5000u);  // a call without arguments gets throw_start's registers
            CHECK_EQ(g_keep_play_args[1], heap);
            g_recall_ball_va = 0;
            int calls = 0;
            uint64_t done = 0;
            while (done == 0 && calls < 200) {
                cpu.x[0] = 0x5000;
                rt::call_hooked(&cpu, kThrowWait, &throw_done);
                done = cpu.x[0];
                ++calls;
            }
            CHECK(done != 0);
            g_keep_busy_va = 0;
        }
        {  // keep_busy unreadable at the end of the hold: the plain recall wait and send-out, no hang
            mods::stop();
            write(dir / "battle-throw.cfg", base + text_line + "expr keep_busy 1 x1>0x200\n"
                                                "path keep_sendout_seq 0 bin/seq/ee630.bseq\n");
            mods::StartResult r2;
            CHECK(mods::start(o, &r2, &error));
            CHECK_EQ(r2.active.size(), 1u);
            rt::store<uint8_t>(heap + 4, 0);
            rt::store<uint8_t>(heap + 5, 1);
            rt::store<uint64_t>(heap + 0x200, 0x10);  // a pointer to an unmapped page
            g_recalls = g_sendouts = g_recall_waits = 0;
            CpuState cpu{};
            cpu.x[1] = heap;
            rt::call_hooked(&cpu, kThrowHandler, &original);
            cpu.x[0] = 0x5000;
            cpu.x[1] = heap;
            rt::call_hooked(&cpu, kThrowStart, &original);
            int calls = 0;
            uint64_t done = 0;
            while (done == 0 && calls < 30) {
                cpu.x[0] = 0x5000;
                cpu.x[1] = heap;
                rt::call_hooked(&cpu, kThrowWait, &throw_done);
                done = cpu.x[0];
                ++calls;
            }
            CHECK(done != 0);
            CHECK_EQ(g_recalls, 1);  // no send-out sequence through recall_start
            CHECK(g_recall_waits > 0);
            CHECK_EQ(g_sendouts, 3);
        }
        {  // keep_sendout_seq without recall_seq_text: not started
            mods::stop();
            write(dir / "battle-throw.cfg", base + "expr keep_busy 1 x1+0x200\n"
                                                "path keep_sendout_seq 0 bin/seq/ee630.bseq\n");
            mods::StartResult r2;
            CHECK(mods::start(o, &r2, &error));
            CHECK(r2.active.empty());
            CHECK(!r2.messages.empty() && r2.messages[0].find("recall_seq_text") != std::string::npos);
        }
        {  // keep_sendout_seq alone (Fang-29 b): the send-out sequence through recall_start, no unload
            mods::stop();
            write(dir / "battle-throw.cfg", base + text_line + "expr keep_busy 1 x1+0x200\n"
                                                "path keep_sendout_seq 0 bin/seq/ee630.bseq\n");
            mods::StartResult r2;
            CHECK(mods::start(o, &r2, &error));
            CHECK_EQ(r2.active.size(), 1u);
            rt::store<uint8_t>(heap + 4, 0);
            rt::store<uint8_t>(heap + 5, 1);
            rt::store<uint8_t>(heap + 0x200, 3);
            g_recalls = g_sendouts = g_recall_waits = 0;
            g_sendout_start_args.clear();
            CpuState cpu{};
            cpu.x[1] = heap;
            rt::call_hooked(&cpu, kThrowHandler, &original);
            cpu.x[0] = 0x5000;
            cpu.x[1] = heap;
            rt::call_hooked(&cpu, kThrowStart, &original);
            CHECK_EQ(g_recalls, 1);
            int calls = 0;
            uint64_t done = 0;
            while (done == 0 && calls < 30) {
                if (const uint8_t b = rt::load<uint8_t>(heap + 0x200); b > 0)
                    rt::store<uint8_t>(heap + 0x200, static_cast<uint8_t>(b - 1));
                cpu.x[0] = 0x5000;
                cpu.x[1] = heap;
                rt::call_hooked(&cpu, kThrowWait, &throw_done);
                done = cpu.x[0];
                ++calls;
            }
            CHECK(done != 0);
            CHECK_EQ(g_recalls, 2);
            CHECK(g_recall_text == "bin/seq/ee630.bseq");
            CHECK_EQ(g_recall_args[0], 0x5000u);  // throw_start's x0/x1
            CHECK_EQ(g_recall_args[1], heap);
            CHECK_EQ(g_recall_waits, 0);
            CHECK_EQ(g_sendouts, 0);
            CHECK(g_sendout_start_args.empty());
            CHECK_EQ(rt::load<uint8_t>(text + 11), static_cast<uint8_t>('1'));  // the original back
        }
        g_recall_text_va = 0;
        mods::stop();
        write(dir / "battle-throw.cfg", base);
        CHECK(mods::start(o, &rs, &error));
    }
    {  // switched off: the doors only run the originals
        CHECK(mods::set_active("battle-throw", false));
        g_recalls = 0;
        CpuState cpu{};
        rt::call_hooked(&cpu, kThrowStart, &original);
        rt::call_hooked(&cpu, kThrowWait, &throw_done);
        CHECK_EQ(g_recalls, 0);
        CHECK_EQ(cpu.x[0], 1u);
    }
    mods::stop();
    fs::remove(dir / "battle-throw.cfg");  // a live mod with a file would start in standby below
}

// battle-throw with the ball on the ground (Fang-4e): the ball functions
// are host functions, the buffer and the model come from a bump allocator.
constexpr uint64_t kBallFns = 0x0802'0000;  // 15 functions, 0x1000 apart
uint64_t g_bump = 0;
int g_polls = 0;
std::vector<std::string> g_ball_calls;
std::vector<std::string> g_played;
std::string guest_text(uint64_t va) {
    std::string t;
    for (char c; (c = static_cast<char>(rt::load<uint8_t>(va))) != 0; ++va)
        t += c;
    return t;
}
void fake_alloc(CpuState* ctx) {
    g_bump = (g_bump + ctx->x[2] - 1) & ~(ctx->x[2] - 1);
    ctx->x[0] = g_bump;
    g_bump += ctx->x[1];
}
uint64_t g_heap_stack = 0;  // the heap stack: 16 entries, the count at +0x80
bool heap_on_top(uint64_t heap) {
    const uint32_t n = rt::load<uint32_t>(g_heap_stack + 0x80);
    return n > 0 && rt::load<uint64_t>(g_heap_stack + 8 * (n - 1)) == heap;
}
void fake_path_text(CpuState* ctx) { rt::store<uint64_t>(ctx->x[8], ctx->x[0]); }  // the path object: its text
void fake_preload(CpuState* ctx) {
    const uint64_t file = rt::load<uint64_t>(ctx->x[2]);  // the model path holds 0x5151, see fake_path_model
    g_ball_calls.push_back("preload " + guest_text(rt::load<uint64_t>(ctx->x[1])) + " " +
                           (file == 0x5151 ? std::string("model") : guest_text(file)) + " " + std::to_string(ctx->x[4]) +
                           (heap_on_top(ctx->x[3]) ? " pushed" : ""));
}
void fake_res_poll(CpuState* ctx) {  // only after the preload; everything is there after the 2nd poll
    CHECK(!g_ball_calls.empty());
    ctx->x[0] = ++g_polls >= 2 ? 1 : 0;
}
void fake_res_anim(CpuState* ctx) { ctx->x[0] = g_polls >= 1 ? 0x7770 : 0; }  // before res_poll reports 1 (Fang-18)
void fake_path_model(CpuState* ctx) { rt::store<uint64_t>(ctx->x[8], 0x5150 + ctx->x[1]); }  // the slot in w1
void fake_res_model(CpuState* ctx) {  // there once preloaded and polled; res_poll reports 1 only at the 2nd
    const bool preloaded = !g_ball_calls.empty() && g_ball_calls[0] == "preload bin/test/ball.gfpak model 0 pushed";
    ctx->x[0] = preloaded && g_polls >= 1 && rt::load<uint64_t>(ctx->x[1]) == 0x5151 ? 0x8880 : 0;
}
uint64_t g_ref_vtable = 0;  // the ball's reference counter at +0x48: AddRef at +0x10, Release at +0x18
void fake_addref(CpuState* ctx) { g_ball_calls.push_back("addref " + std::to_string(ctx->x[0] & 0xff)); }
void fake_ball_create(CpuState* ctx) {
    rt::store<uint64_t>(ctx->x[0] + 0x48, g_ref_vtable);
    const uint32_t n = rt::load<uint32_t>(g_heap_stack + 0x80);
    g_ball_calls.push_back("create " + std::to_string(n) + " " +
                           std::to_string(n > 0 && rt::load<uint64_t>(g_heap_stack + 8 * (n - 1)) == ctx->x[2]));
}
void fake_ball_init(CpuState* ctx) {
    g_ball_calls.push_back("init " + std::to_string(ctx->x[2]) + " " + std::to_string(ctx->x[3]));
}
void fake_attach(CpuState* ctx) { g_ball_calls.push_back("attach " + std::to_string(ctx->x[2])); }
void fake_play(CpuState* ctx) {
    const std::string name = guest_text(rt::load<uint64_t>(ctx->x[1] + 8));
    uint64_t h = 0xcbf29ce484222325ull;
    for (char c : name)
        h = (h ^ static_cast<uint8_t>(c)) * 0x100000001b3ull;
    CHECK_EQ(rt::load<uint64_t>(ctx->x[1]), h);
    CHECK_EQ(rt::load<uint64_t>(ctx->x[1] + 0x10), 0x3fu);
    CHECK_EQ(ctx->x[8], ctx->x[1] - 0x210 + 0x380);  // the search result goes to the throw's buffer (Fang-19)
    g_played.push_back(name);
}
void fake_list_add(CpuState* ctx) {  // moves the reference out of its slot, as the game's list does
    g_ball_calls.push_back("list_add " + std::to_string(ctx->x[3]));
    rt::store<uint64_t>(ctx->x[2], 0);
}
void fake_list_remove(CpuState* ctx) {
    g_ball_calls.push_back(std::string("list_remove") + (rt::load<uint64_t>(ctx->x[1]) != 0 ? " object" : " empty"));
}
void fake_deinit(CpuState*) { g_ball_calls.push_back("deinit"); }
void fake_slot_position(CpuState* ctx) {  // the opponent's place, slot in w1
    CHECK_EQ(ctx->x[1], 1u);
    const float xyz[3] = {10.0f, 20.0f, 30.0f};
    for (int i = 0; i < 3; ++i)
        rt::store<float>(ctx->x[2] + 4 * i, xyz[i]);
}
void fake_res_clear(CpuState*) { g_ball_calls.push_back("res_clear"); }
void fake_free(CpuState* ctx) { g_ball_calls.push_back("free " + std::to_string(ctx->x[2])); }
void fake_release(CpuState* ctx) { g_ball_calls.push_back("release " + std::to_string(ctx->x[0] & 0xff)); }

void battle_throw_ball(const fs::path& dir) {
    rt::GuestMemory memory(rt::Layout::packed(0x0800'0000, 0x10'0000, 0x10'0000, 0x10'0000, 0x10'0000, 0x40'0000));
    const uint64_t heap = memory.layout().heap.base;
    CHECK(memory.map(heap, 4 * rt::kPageSize, rt::MemState::Normal, rt::kPermRW).ok());
    const uint64_t view = heap + 0x100;
    const uint64_t heap_obj = heap + 0x800;  // the view's heap: allocation in slot 0x10 of its vtable
    rt::store<uint64_t>(view + 0x18, heap_obj);
    rt::store<uint64_t>(heap_obj, heap + 0x880);
    rt::store<uint64_t>(heap + 0x890, kBallFns);
    rt::store<uint64_t>(heap + 0x898, kBallFns + 0x11000);  // free in slot 0x18
    g_heap_stack = heap + 0x900;
    const float pos[3] = {1.5f, 0.0f, 200.0f};
    const float pos_add[3] = {0.5f, 0.0f, -20.0f};  // the model's offset field
    for (int i = 0; i < 3; ++i) {
        rt::store<float>(view + 0x30 + 4 * i, pos[i]);
        rt::store<float>(view + 0x40 + 4 * i, pos_add[i]);
    }
    const uint64_t cmd = heap + 0x200;  // the throw command: success at +4, shakes at +5
    rt::store<uint8_t>(cmd + 5, 2);
    const uint64_t doors[] = {kThrowHandler, kThrowStart, kThrowWait};
    rt::declare_hookable(doors, 3);
    void (*const fakes[])(CpuState*) = {&fake_alloc,  &fake_ball_create, &fake_ball_init,   &fake_attach,
                                        &fake_play,   &fake_list_add,    &fake_list_remove, &fake_deinit,
                                        &fake_preload, &fake_res_anim,   &fake_res_model, &fake_path_model,
                                        &fake_path_text, &fake_res_poll, &fake_addref};
    LiftedEntry fns[23] = {{kBallFns + 0x12000, &fake_slot_position},{kRecall, &fake_recall},
                           {kSendOut, &fake_sendout},
                           {kRecallWait, &fake_recall_wait},
                           {kSendOutStart, &fake_sendout_start},
                           {kBallFns + 0xf000, &fake_release},
                           {kBallFns + 0x10000, &fake_res_clear},
                           {kBallFns + 0x11000, &fake_free}};
    for (unsigned k = 0; k < 15; ++k)
        fns[8 + k] = {kBallFns + 0x1000 * k, fakes[k]};
    rt::register_functions(fns, 23);
    g_ref_vtable = heap + 0xa00;
    rt::store<uint64_t>(g_ref_vtable + 0x10, kBallFns + 0xe000);
    rt::store<uint64_t>(g_ref_vtable + 0x18, kBallFns + 0xf000);
    const char* names[] = {"ball_alloc_slot", "ball_create",      "ball_init",   "ball_anim_attach",
                           "ball_anim_play", "ball_list_add", "ball_list_remove", "ball_deinit",
                           "res_preload", "res_anim",         "res_model",   "path_ball_model",
                           "path_from_text", "res_poll"};
    std::string cfg = "hook throw_handler 0x8017000\nhook throw_start 0x8018000\nhook throw_wait 0x8019000\n"
                      "value recall_start 0x801a000\nvalue sendout_update 0x801b000\nexpr throw_success 1 x1+4\n"
                      "value recall_wait 0x801c000\nvalue sendout_start 0x801d000\nvalue sendout_side 1\nexpr throw_shakes 1 x1+5\nvalue wait_frames 3\nexpr res_manager 0 x0+0x10\n"
                      "expr view_heap 0 x0>0x18\nexpr model_list 0 x0+0x20\nexpr ball_pos 0 x0+0x30\n"
                      "path capture_package 0 bin/test/ball.gfpak\npath capture_anim 0 bin/test/capture.anm\n"
                      "value ball_slot 1\nvalue fall_frames 2\nvalue shake_frames 2\nvalue end_frames 2\n"
                      "expr heap_stack 0 tp\nexpr ball_pos_add 0 x0+0x40\nvalue res_clear 0x8030000\n"
                      "value ball_free_slot 0x18\n";
    for (unsigned k = 1; k < 14; ++k) {  // 0: ball_alloc_slot, not a function
        char line[64];
        std::snprintf(line, sizeof line, "value %s 0x%llx\n", names[k],
                      static_cast<unsigned long long>(kBallFns + 0x1000 * k));
        cfg += line;
    }
    cfg += "value ball_alloc_slot 0x10\n";
    mods::StartOptions o;
    o.dir = dir;
    o.enabled = {"battle-throw"};
    std::string error;
    {  // a ball role missing: not started
        std::string partial = cfg;
        partial.erase(partial.find("value res_anim"), partial.find('\n', partial.find("value res_anim")) -
                                                          partial.find("value res_anim") + 1);
        write(dir / "battle-throw.cfg", partial);
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active.empty());
        CHECK(!r.messages.empty() && r.messages[0].find("res_anim") != std::string::npos);
        mods::stop();
    }
    {  // no ball_free_slot: not started either (Fang-16, PC K17)
        std::string partial = cfg;
        partial.erase(partial.find("value ball_free_slot"), std::string("value ball_free_slot 0x18\n").size());
        write(dir / "battle-throw.cfg", partial);
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active.empty());
        CHECK(!r.messages.empty() && r.messages[0].find("ball_free_slot") != std::string::npos);
        mods::stop();
    }
    {  // no res_poll: not started (the ball would not move, Fang-18)
        std::string partial = cfg;
        const size_t at = partial.find("value res_poll");
        partial.erase(at, partial.find('\n', at) - at + 1);
        write(dir / "battle-throw.cfg", partial);
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active.empty());
        CHECK(!r.messages.empty() && r.messages[0].find("res_poll") != std::string::npos);
        mods::stop();
    }
    {  // no res_clear: not started (the end of the battle would hang white, Fang-16)
        std::string partial = cfg;
        partial.erase(partial.find("value res_clear"), std::string("value res_clear 0x8030000\n").size());
        write(dir / "battle-throw.cfg", partial);
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active.empty());
        CHECK(!r.messages.empty() && r.messages[0].find("res_clear") != std::string::npos);
        mods::stop();
    }
    write(dir / "battle-throw.cfg", cfg);
    mods::StartResult r;
    CHECK(mods::start(o, &r, &error));
    CHECK_EQ(r.active.size(), 1u);
    for (int caught = 1; caught >= 0; --caught) {
        rt::store<uint8_t>(cmd + 4, static_cast<uint8_t>(caught));
        g_bump = heap + 0x1000;
        g_polls = 0;
        g_sendouts = 0;
        g_recalls = 0;
        g_ball_calls.clear();
        g_played.clear();
        CpuState cpu{};
        cpu.sp = heap + 4 * rt::kPageSize;
        cpu.tpidr_el0 = g_heap_stack;
        cpu.x[1] = cmd;
        rt::call_hooked(&cpu, kThrowHandler, &original);
        cpu.x[0] = view;
        cpu.x[1] = 2;
        rt::call_hooked(&cpu, kThrowStart, &original);
        CHECK_EQ(g_recalls, 1);
        CHECK(g_ball_calls.empty());  // the preload follows the recall
        int calls = 0;
        uint64_t done = 0;
        while (done == 0 && calls < 40) {
            cpu.x[0] = view;
            rt::call_hooked(&cpu, kThrowWait, &throw_done);
            done = cpu.x[0];
            ++calls;
        }
        // 3 held, 1 while loading, the build and fall (2 more), 2 shakes and the end (1 + 2 each), the removal;
        // a miss then sends out (3 calls, the first in the removal's call).
        CHECK_EQ(calls, caught ? 17 : 19);
        const std::vector<std::string> want_calls = {"preload bin/test/ball.gfpak model 0 pushed",
                                                     "preload bin/test/ball.gfpak bin/test/capture.anm 1 pushed",
                                                     "create 1 1",    "addref 72",    "init 34944 0",
                                                     "attach 30576",  "addref 72",    "list_add 64",
                                                     "deinit",        "list_remove object", "release 72",
                                                     "res_clear",     "free 1024"};
        CHECK(g_ball_calls == want_calls);
        CHECK_EQ(g_polls, 3);  // the third before res_clear
        const std::vector<std::string> want_played = {"to_capture01_fall", "to_capture01_yure01", "to_capture01_yure02",
                                                      caught ? "to_capture01_success" : "to_capture01_failure"};
        CHECK(g_played == want_played);
        CHECK_EQ(g_sendouts, caught ? 0 : 3);
        // The ball object (the second allocation, after the 0x400 buffer) holds the position.
        const uint64_t object = heap + 0x1000 + 0x400;
        CHECK(rt::load<float>(object + 0xb0) == 2.0f && rt::load<float>(object + 0xb8) == 180.0f);
        CHECK_EQ(rt::load<uint8_t>(object + 0xc0), 1u);
        CHECK(rt::load<uint64_t>(object + 8) == heap_obj && rt::load<uint64_t>(object + 0x10) == object &&
              rt::load<uint64_t>(object + 0x18) == 0x2c0 && rt::load<uint64_t>(object + 0x20) == 0x10);
        CHECK_EQ(rt::load<uint32_t>(g_heap_stack + 0x80), 0u);  // popped again
    }
    {  // heap stack full: the ball is left out before its preload, the buffer still goes back (Codex)
        rt::store<uint8_t>(cmd + 4, 1);
        rt::store<uint32_t>(g_heap_stack + 0x80, 16);
        g_bump = heap + 0x1000;
        g_ball_calls.clear();
        g_played.clear();
        CpuState cpu{};
        cpu.sp = heap + 4 * rt::kPageSize;
        cpu.tpidr_el0 = g_heap_stack;
        cpu.x[1] = cmd;
        rt::call_hooked(&cpu, kThrowHandler, &original);
        cpu.x[0] = view;
        cpu.x[1] = 2;
        rt::call_hooked(&cpu, kThrowStart, &original);
        int calls = 0;
        uint64_t done = 0;
        while (done == 0 && calls < 40) {
            cpu.x[0] = view;
            rt::call_hooked(&cpu, kThrowWait, &throw_done);
            done = cpu.x[0];
            ++calls;
        }
        CHECK_EQ(calls, 4);  // 3 held, then the recall wait, the left-out ball and the end in one call
        CHECK(g_ball_calls == std::vector<std::string>{"free 1024"});
        CHECK(g_played.empty());
        rt::store<uint32_t>(g_heap_stack + 0x80, 0);
    }
    mods::stop();
    {  // the position from slot_position (Fang-17), raised by ball_raise_milli, without expr ball_pos
        std::string with_slot = cfg;
        with_slot.erase(with_slot.find("expr ball_pos 0"), std::string("expr ball_pos 0 x0+0x30\n").size());
        with_slot += "value slot_position 0x8032000\nvalue ball_raise_milli 250\n";
        write(dir / "battle-throw.cfg", with_slot);
        mods::StartResult rs;
        CHECK(mods::start(o, &rs, &error));
        CHECK_EQ(rs.active.size(), 1u);
        rt::store<uint8_t>(cmd + 4, 1);
        g_bump = heap + 0x1000;
        g_polls = 0;
        g_ball_calls.clear();
        g_played.clear();
        CpuState cpu{};
        cpu.sp = heap + 4 * rt::kPageSize;
        cpu.tpidr_el0 = g_heap_stack;
        cpu.x[1] = cmd;
        rt::call_hooked(&cpu, kThrowHandler, &original);
        cpu.x[0] = view;
        cpu.x[1] = 2;
        rt::call_hooked(&cpu, kThrowStart, &original);
        const uint64_t object = heap + 0x1000 + 0x400;
        uint64_t done = 0;
        for (int calls = 0; done == 0 && calls < 40; ++calls) {
            cpu.x[0] = view;
            rt::call_hooked(&cpu, kThrowWait, &throw_done);
            done = cpu.x[0];
            if (!g_played.empty() && done == 0) {  // written again on every call while the ball plays
                CHECK(rt::load<float>(object + 0xb0) == 10.0f && rt::load<float>(object + 0xb4) == 20.25f &&
                      rt::load<float>(object + 0xb8) == 30.0f);
                rt::store<float>(object + 0xb4, 0.0f);
            }
        }
        CHECK_EQ(g_played.size(), 4u);
    }
    mods::stop();
    fs::remove(dir / "battle-throw.cfg");
}

// wild-battles (P8.2g–i): the creator, the lookup, the fill, the event's
// update and the release are host functions in the dispatch table, the
// objects live on the heap.
constexpr uint64_t kFill = 0x0800'5000;
constexpr uint64_t kLookup = 0x0800'6000;
constexpr uint64_t kGetter = 0x0800'7000;
constexpr uint64_t kCreate = 0x0800'8000;
constexpr uint64_t kUpdate = 0x0800'9000;
constexpr uint64_t kFadeIn = 0x0800'a000;
constexpr uint64_t kHandleRelease = 0x0800'b000;
constexpr uint64_t kTeam = 0x0800'd000;
constexpr uint64_t kLoseCreate = 0x0800'e000;
constexpr uint64_t kExpDoor = 0x0801'0000;
constexpr uint64_t kPartFind = 0x0801'1000;
constexpr uint64_t kLossAmount = 0x0801'8000;
constexpr uint64_t kRegistryFind = 0x0801'2000;
constexpr uint64_t kPostBattle = 0x0801'3000;
constexpr uint64_t kCapture = 0x0801'4000;
constexpr uint64_t kCatchExp = 0x0801'5000;
constexpr uint64_t kRecallEnd = 0x0801'f000;
constexpr uint64_t kEndView = 0x0802'e000;
constexpr uint64_t kFaderDone = 0x0802'f000;
constexpr uint64_t kBallChoice = 0x0803'1000;
constexpr uint64_t kItemData = 0x0803'2000;
uint32_t g_item_handled = 0;
uint64_t g_item_x3 = 0;
void fake_item_handler(CpuState* ctx) {  // the battle's item handler: the item number in w3
    ++g_item_handled;
    g_item_x3 = ctx->x[3];
    ctx->x[0] = 7;
}
void fake_item_data(CpuState* ctx) {  // item_data(item, field): field 0x10 is the kind, 5 = a ball
    ctx->x[0] = ctx->x[1] != 0x10 ? 0 : (ctx->x[0] == 4 || ctx->x[0] == 12) ? 5 : 3;
    ctx->x[3] = 0xdead;                 // clobbered: the hook puts the entry back
}
void fake_fader_done(CpuState* ctx) {  // the fader's layer x1 done: bit 0 of the byte at x0 + x1
    ctx->x[0] = rt::load<uint8_t>(ctx->x[0] + ctx->x[1]) & 1;
}

struct Filled {
    uint64_t object = 0;
    uint8_t record[0x20] = {};
} g_filled;
void fake_fill(CpuState* ctx) {
    g_filled.object = ctx->x[0];
    for (unsigned i = 0; i < sizeof(g_filled.record); ++i)
        g_filled.record[i] = rt::load<uint8_t>(ctx->x[1] + i);
}
void fake_getter(CpuState* ctx) {  // a species getter: the word at x0, plus 100
    ctx->x[0] = rt::load<uint32_t>(ctx->x[0]) + 100;
    ctx->x[24] = 0;  // clobbers what the hook must put back
}
uint64_t g_created = 0;  // the handle the creator returns
uint64_t g_create_x0 = 0, g_create_x1 = 0;
int g_create_calls = 0;
void fake_create(CpuState* ctx) {
    ++g_create_calls;
    g_create_x0 = ctx->x[0];  // the manager itself
    g_create_x1 = ctx->x[1];
    rt::store<uint64_t>(ctx->x[8], g_created);
    ctx->x[19] = 0xdead;  // a callee-saved register the hook puts back
}
uint64_t g_update_result = 0;
void fake_update(CpuState* ctx) {  // the battle event's update: 1 = done
    ctx->x[0] = g_update_result;
    ctx->x[19] = 0xbad;
}
uint64_t g_fade_x0 = 0, g_fade_x1 = 0;
int g_fade_calls = 0;
void fake_fade(CpuState* ctx) {
    ++g_fade_calls;
    g_fade_x0 = ctx->x[0];
    g_fade_x1 = ctx->x[1];
    ctx->x[0] = 0x55;
}
uint64_t g_handle_released = 0;
void fake_handle_release(CpuState* ctx) {
    g_handle_released = ctx->x[0];
}
int g_team_calls = 0;
uint64_t g_team_x1 = 0, g_team_x2 = 0;
void fake_team(CpuState* ctx) {  // a team counter: the word at x0 (fit Pokémon), mode x1, mask x2
    ++g_team_calls;
    g_team_x1 = ctx->x[1];
    g_team_x2 = ctx->x[2];
    ctx->x[0] = rt::load<uint32_t>(ctx->x[0]);
    ctx->x[19] = 0xfee;
}
int g_lose_calls = 0;
int g_fade_before_lose = 0;
uint64_t g_lose_x0 = 0, g_lose_x1 = 0, g_lose_handle = 0;
void fake_lose_create(CpuState* ctx) {  // CallBattleLose's creator
    ++g_lose_calls;
    g_fade_before_lose = g_fade_calls;
    g_lose_x0 = ctx->x[0];
    g_lose_x1 = ctx->x[1];
    rt::store<uint64_t>(ctx->x[8], g_lose_handle);
}
int g_door_calls = 0;
void fake_door(CpuState*) {  // expfix_door: a function of the battle view
    ++g_door_calls;
}
uint64_t g_recall_battle = 0;
uint32_t g_recall_flags = 0;
uint64_t g_ko_path_va = 0;  // the recall's path text (0: none)
std::string g_recall_seen;
void fake_recall_end(CpuState*) {  // recall_end_start: reads the battle's flags and its path text
    g_recall_flags = rt::load<uint32_t>(g_recall_battle + 0x10);
    g_recall_seen.clear();
    for (uint64_t at = g_ko_path_va; at != 0 && rt::load<uint8_t>(at) != 0; ++at)
        g_recall_seen += static_cast<char>(rt::load<uint8_t>(at));
}
void fake_loss(CpuState* ctx) {  // the money a defeat costs
    ctx->x[0] = 1000;
}
void fake_part_find(CpuState* ctx) {  // the part lookup: a part
    ctx->x[0] = 0x5150;
}
uint64_t g_known_key = 0, g_found = 0, g_registry_x0 = 0;
void fake_registry_find(CpuState* ctx) {  // registry_find(registry, const key*, out x8)
    g_registry_x0 = ctx->x[0];
    rt::store<uint64_t>(ctx->x[8], rt::load<uint64_t>(ctx->x[1]) == g_known_key ? g_found : 0);
    ctx->x[19] = 0xabc;
}
uint32_t g_next_state = 0;
void fake_post_battle(CpuState* ctx) {  // the process after a battle: its state at x0 + 0
    rt::store<uint32_t>(ctx->x[0], g_next_state);
}
uint32_t g_capture_seen = 0;
void fake_capture(CpuState* ctx) {  // the catch process: its state at x0 + 0
    g_capture_seen = rt::load<uint32_t>(ctx->x[0]);
    rt::store<uint32_t>(ctx->x[0], g_next_state);
}
void fake_catch_exp(CpuState* ctx) {  // the catch scene's experience
    ctx->x[0] = 500;
}
uint64_t g_template = 0;
void fake_lookup_ptr(CpuState* ctx) {  // static_battle_lookup(table, const hash*)
    ctx->x[0] = rt::load<uint64_t>(ctx->x[1]) == 0x77 ? g_template : 0;
}
void fake_lookup(CpuState* ctx) {  // static_battle_lookup(hash)
    ctx->x[0] = ctx->x[0] == 0x77 ? g_template : 0;
}

void wild_battles(const fs::path& dir) {
    rt::GuestMemory memory(rt::Layout::packed(0x0800'0000, 0x10'0000, 0x10'0000, 0x10'0000, 0x10'0000, 0x40'0000));
    const uint64_t heap = memory.layout().heap.base;
    CHECK(memory.map(heap, 4 * rt::kPageSize, rt::MemState::Normal, rt::kPermRW).ok());
    const uint64_t object_ref = heap;  // x2 of encounter_touch: a reference to the object
    const uint64_t object = heap + 0x100;
    const uint64_t global = heap + 0x200;    // a global holding a pointer to the managers
    const uint64_t managers = heap + 0x280;  // +0x8: manager for x0, +0x10: x1
    const uint64_t stack_top = heap + 4 * rt::kPageSize;
    rt::store<uint64_t>(object_ref, object);
    rt::store<uint64_t>(global, managers);
    rt::store<uint64_t>(managers + 8, 0x1111);
    rt::store<uint64_t>(managers + 0x10, 0x2222);

    const uint64_t doors[] = {kTouch,     kFill,       kLookup,  kUpdate,   kExpDoor,   kPartFind,
                              kPostBattle, kCapture, kCatchExp, kRecallEnd, kEndView, kLossAmount, kBallChoice};
    rt::declare_hookable(doors, 13);
    const LiftedEntry fns[] = {{kFill, &fake_fill},
                               {kGetter, &fake_getter},
                               {kCreate, &fake_create},
                               {kFadeIn, &fake_fade},
                               {kHandleRelease, &fake_handle_release},
                               {kTeam, &fake_team},
                               {kLoseCreate, &fake_lose_create},
                               {kRegistryFind, &fake_registry_find},
                               {kFaderDone, &fake_fader_done},
                               {kItemData, &fake_item_data}};
    rt::register_functions(fns, 10);
    const uint64_t templ = heap + 0x500;  // the template record (Lektrobal), 0x59 bytes
    for (unsigned i = 0; i < 0x59; ++i)
        rt::store<uint8_t>(templ + i, static_cast<uint8_t>(0xA0 + i));
    g_template = templ;
    rt::store<uint32_t>(object + 0x80, 41);  // the touched Pokémon: species key, level
    rt::store<uint8_t>(object + 0x84, 7);
    const uint64_t event = heap + 0x700;
    const uint64_t handle = heap + 0x780;  // what the creator leaves at x8: [handle + 0x60] = event + 0x50
    rt::store<uint64_t>(handle + 0x48, heap + 0x800);  // counter interface, its vtable
    rt::store<uint64_t>(heap + 0x818, kHandleRelease);
    const uint64_t fade = heap + 0x880;  // a fade value the end writes

    mods::StartOptions o;
    o.dir = dir;
    o.enabled = {"wild-battles"};
    std::string error;
    const std::string g = std::to_string(global);
    const std::string base = "hook encounter_touch 0x8001000\n"
                             "hook record_fill 0x8005000\nhook static_battle_lookup 0x8006000\n"
                             "value template_hash 0x77\n"
                             "hook event_create 0x8008000\n"
                             "addr start_x0 " + g + " >0>8\naddr start_x1 " + g + " >0+0x10>0\n"
                             "addr event_from_create 0 >0x60-0x50\n"
                             "value event_1_offset 0x10\nvalue event_1 0x77\nvalue event_1_size 8\n"
                             "value event_2_offset 0x18\nvalue event_2 1\nvalue event_2_size 1\n"
                             "value object_load 1\n"
                             "value record_1_offset 4\ncall record_1 0x8007000 obj+0x80\n"
                             "value record_2_offset 9\nfrom record_2 1 x2>0+0x84\n";
    auto run = [&](CpuState* cpu) {
        *cpu = CpuState{};
        cpu->x[0] = 5;
        cpu->x[2] = object_ref;
        cpu->x[19] = 19;
        cpu->x[24] = 24;
        cpu->x[30] = 0x0800'0040;
        cpu->sp = stack_top - 8;  // not aligned: the scratch is
        g_original_calls = 0;
        g_create_calls = 0;
        g_handle_released = 0;
        rt::store<uint64_t>(handle + 0x60, event + 0x50);
        rt::store<uint64_t>(event + 0x10, 0);
        rt::store<uint8_t>(event + 0x18, 0);
        rt::call_hooked(cpu, kTouch, &original);
    };
    auto lookup = [&](uint64_t hash) {
        CpuState l{};
        l.x[0] = hash;
        rt::call_hooked(&l, kLookup, &fake_lookup);
        return l.x[0];
    };
    auto fill = [&]() {
        CpuState l{};
        l.x[0] = object;
        l.x[1] = templ;
        g_filled = Filled{};
        rt::call_hooked(&l, kFill, &fake_fill);
    };

    {  // the touch makes the battle event, the lookup gets the fields
        write(dir / "wild-battles.cfg", base + "value redirect 3\n");
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CHECK_EQ(r.active.size(), 1u);
        CHECK(mods::active("wild-battles"));
        CHECK(rt::find_hook(kLookup) != nullptr && rt::find_hook(kFill) != nullptr);
        CHECK(rt::find_hook(kUpdate) == nullptr);  // no end without battle_event_update
        g_created = handle;
        CpuState cpu;
        run(&cpu);
        CHECK_EQ(g_original_calls, 0);
        CHECK_EQ(g_create_calls, 1);
        CHECK_EQ(g_create_x0, 0x1111u);
        CHECK_EQ(g_create_x1, 0x2222u);
        CHECK_EQ(rt::load<uint64_t>(event + 0x10), 0x77u);
        CHECK_EQ(rt::load<uint8_t>(event + 0x18), 1u);
        CHECK_EQ(g_handle_released, 0u);  // no release lines
        CHECK_EQ(cpu.halt, uint32_t{kHaltNone});
        CHECK_EQ(cpu.x[0], 5u);
        CHECK_EQ(cpu.x[19], 19u);
        CHECK_EQ(cpu.x[24], 24u);
        CHECK_EQ(cpu.x[30], 0x0800'0040u);
        CHECK_EQ(cpu.sp, stack_top - 8);
        CHECK_EQ(rt::load<uint32_t>(templ + 4), 0xA7A6A5A4u);  // nothing in the template yet

        CHECK_EQ(lookup(0x78), 0u);  // another hash: untouched
        CHECK_EQ(rt::load<uint32_t>(templ + 4), 0xA7A6A5A4u);
        CHECK_EQ(lookup(0x77), templ);  // the event looks the template up: the fields go in, once
        CHECK_EQ(rt::load<uint32_t>(templ + 4), 141u);  // from the getter
        CHECK_EQ(rt::load<uint8_t>(templ + 9), 7u);
        fill();  // the fill copies it, then the template gets its bytes back
        CHECK_EQ(g_filled.record[4], 141);
        CHECK_EQ(g_filled.record[9], 7);
        CHECK_EQ(rt::load<uint32_t>(templ + 4), 0xA7A6A5A4u);
        CHECK_EQ(rt::load<uint8_t>(templ + 9), 0xA9u);
        lookup(0x77);  // a real battle later: the template as it is
        CHECK_EQ(rt::load<uint32_t>(templ + 4), 0xA7A6A5A4u);

        // switched off in the menu: the catch scene, nothing requested
        CHECK(mods::set_active("wild-battles", false));
        CHECK(!mods::active("wild-battles"));
        CHECK(mods::enabled("wild-battles"));
        run(&cpu);
        CHECK_EQ(g_original_calls, 1);
        CHECK_EQ(g_create_calls, 0);
        CHECK_EQ(cpu.x[0], 6u);
        lookup(0x77);
        CHECK_EQ(rt::load<uint32_t>(templ + 4), 0xA7A6A5A4u);
        // and on again
        CHECK(mods::set_active("wild-battles", true));
        run(&cpu);
        CHECK_EQ(g_original_calls, 0);
        CHECK_EQ(g_create_calls, 1);
        lookup(0x77);
        fill();
        CHECK_EQ(g_filled.record[4], 141);

        g_created = 0;  // no handle: the catch scene as before
        run(&cpu);
        CHECK_EQ(g_original_calls, 1);
        CHECK_EQ(cpu.x[19], 19u);
        g_created = handle;  // a handle without an event object
        rt::store<uint64_t>(handle + 0x60, 0);
        g_original_calls = 0;
        cpu = CpuState{};
        cpu.x[2] = object_ref;
        cpu.sp = stack_top - 8;
        rt::call_hooked(&cpu, kTouch, &original);
        CHECK_EQ(g_original_calls, 1);
    }
    // patch battle_time: MOVZ w1, #300 (the time limit of event battles)
    const uint64_t time_site = heap + 0xa00;
    rt::store<uint32_t>(time_site, 0x52800000u | (300u << 5) | 1u);
    auto battle_time = [&]() {
        CpuState t{};
        t.x[1] = 0xdead'0000'0000'0000;
        t.pc = time_site + 4;
        CHECK(rt::run_patch(&t, time_site));
        CHECK_EQ(t.halt, uint32_t{kHaltNone});
        return t.x[1];
    };
    {  // hash by pointer, release, the end: fade and writes once, when our event is done
        write(dir / "wild-battles.cfg",
              base + "value redirect 3\nvalue lookup_hash_reg 1\nvalue lookup_hash_ptr 1\nvalue touch_return 1\n"
                     "value event_release_interface 0x48\nvalue event_release_slot 0x18\n"
                     "hook battle_event_update 0x8009000\naddr end_base " + std::to_string(fade) + "\n"
                     "value end_1_offset 0\nvalue end_1 0\n"
                     "value end_call 0x800a000\naddr end_x0 " + g + " >0>8\naddr end_x1 7\n"
                     "patch battle_time " + std::to_string(time_site) + "\n");
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CHECK_EQ(r.active.size(), 1u);
        CHECK_EQ(battle_time(), 300u);  // the game's own event battle keeps its limit
        g_created = handle;
        CpuState cpu;
        run(&cpu);
        CHECK_EQ(battle_time(), 0u);  // ours: no limit
        CHECK_EQ(battle_time(), 300u);  // once per touch: the next event battle has its limit again
        run(&cpu);  // another touch, its battle ends (done) before the limit is written
        CHECK_EQ(g_handle_released, handle + 0x48);
        CHECK_EQ(rt::load<uint64_t>(event + 0x10), 0x77u);
        CHECK_EQ(cpu.x[0], 1u);
        CHECK_EQ(cpu.x[19], 19u);

        const uint64_t hash_at = heap + 0x7f0;
        rt::store<uint64_t>(hash_at, 0x77);
        CpuState l{};
        l.x[0] = 0x1234;  // the table object
        l.x[1] = hash_at;
        rt::call_hooked(&l, kLookup, &fake_lookup_ptr);
        CHECK_EQ(l.x[0], templ);
        CHECK_EQ(rt::load<uint8_t>(templ + 9), 7u);
        fill();
        CHECK_EQ(rt::load<uint8_t>(templ + 9), 0xA9u);

        // the event's update: not done, someone else's, then ours done
        rt::store<uint32_t>(fade, 0xff);
        g_fade_calls = 0;
        auto update = [&](uint64_t self, uint64_t result) {
            g_update_result = result;
            l = CpuState{};
            l.x[0] = self;
            l.sp = stack_top - 0x200;
            rt::call_hooked(&l, kUpdate, &fake_update);
        };
        update(event, 0);
        update(heap + 0x900, 1);
        CHECK_EQ(g_fade_calls, 0);
        CHECK(mods::set_active("wild-battles", false));  // switched off mid-battle: this one still ends
        update(event, 1);
        CHECK_EQ(g_fade_calls, 1);
        CHECK_EQ(g_fade_x0, 0x1111u);
        CHECK_EQ(g_fade_x1, 7u);
        CHECK_EQ(rt::load<uint32_t>(fade), 0u);
        CHECK_EQ(l.x[0], 1u);       // the update's result stays
        CHECK_EQ(l.x[19], 0xbadu);  // as the update left it
        CHECK_EQ(l.sp, stack_top - 0x200);
        update(event, 1);  // once only
        CHECK_EQ(g_fade_calls, 1);
        CHECK_EQ(battle_time(), 300u);  // after our battle the limit is back
    }
    {  // team K.O. (P8.7–P8.10): no battle without a Pokémon that can fight;
       // with lose_create the game's defeat after our end when our battle leaves the team down
        const uint64_t team = heap + 0xa80;  // [team + 8] = number of fit Pokémon
        const uint64_t party = heap + 0xa90;
        rt::store<uint64_t>(team, party - 8);
        const std::string cfg = base + "value redirect 3\nvalue touch_return 1\n"
                                       "hook battle_event_update 0x8009000\n"
                                       "value end_call 0x800a000\naddr end_x0 " + g + " >0>8\naddr end_x1 7\n"
                                       "call team_fit 0x800d000 " + std::to_string(team) + " >0 +8\n"
                                       "value team_fit_x1 1\nvalue team_fit_x2 0x40\n";
        write(dir / "wild-battles.cfg", cfg);
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CHECK_EQ(r.active.size(), 1u);
        g_created = handle;
        CpuState cpu;
        rt::store<uint32_t>(party, 0);  // nobody can fight: the catch scene as before
        g_team_calls = 0;
        run(&cpu);
        CHECK_EQ(g_team_calls, 1);
        CHECK_EQ(g_team_x1, 1u);
        CHECK_EQ(g_team_x2, 0x40u);
        CHECK_EQ(g_original_calls, 1);
        CHECK_EQ(g_create_calls, 0);
        CHECK_EQ(cpu.x[0], 6u);
        CHECK_EQ(cpu.x[19], 19u);
        CHECK_EQ(cpu.sp, stack_top - 8);

        rt::store<uint32_t>(party, 2);  // two can: our battle
        const uint32_t started = mods::wild_battles_started();
        const uint32_t done = mods::wild_battles_done();
        run(&cpu);
        CHECK_EQ(g_original_calls, 0);
        CHECK_EQ(g_create_calls, 1);
        CHECK_EQ(mods::wild_battles_started(), started + 1);
        g_fade_calls = 0;
        CpuState l;
        auto update = [&](uint64_t self, uint64_t result) {
            g_update_result = result;
            l = CpuState{};
            l.x[0] = self;
            l.sp = stack_top - 0x200;
            rt::call_hooked(&l, kUpdate, &fake_update);
        };
        g_team_calls = 0;
        update(event, 0);  // not done: no call
        CHECK_EQ(g_team_calls, 0);
        // without lose_create: our end after a lost battle too (the field stays visible), no team check
        rt::store<uint32_t>(party, 0);  // the last one fainted
        update(event, 1);
        CHECK_EQ(g_fade_calls, 1);
        CHECK_EQ(g_team_calls, 0);
        CHECK_EQ(mods::wild_battles_done(), done + 1);
        CHECK_EQ(l.x[0], 1u);
        CHECK_EQ(l.x[19], 0xbadu);  // as the update left it
        CHECK_EQ(l.sp, stack_top - 0x200);
        update(event, 1);  // once only
        CHECK_EQ(mods::wild_battles_done(), done + 1);
        CHECK_EQ(g_fade_calls, 1);

        // lose_create (P8.10): after our end, the defeat event as CallBattleLose makes it
        const uint64_t lose_handle = heap + 0xb00;  // [lose_handle + 0x60] = object + 0x50
        const uint64_t lose_object = heap + 0xb80;
        rt::store<uint64_t>(lose_handle + 0x60, lose_object + 0x50);
        rt::store<uint64_t>(lose_handle + 0x48, heap + 0x800);  // the same counter interface
        g_lose_handle = lose_handle;
        write(dir / "wild-battles.cfg", cfg + "value event_release_interface 0x48\nvalue event_release_slot 0x18\n"
                                              "hook lose_create 0x800e000\n"
                                              "value defeat_1_offset 0x10\nvalue defeat_1 0x99\n"
                                              "value lose_release 1\n");
        r = {};
        CHECK(mods::start(o, &r, &error));
        CHECK_EQ(r.active.size(), 1u);
        rt::store<uint32_t>(party, 1);
        run(&cpu);
        rt::store<uint32_t>(party, 1);  // won: no defeat event
        g_lose_calls = 0;
        update(event, 1);
        CHECK_EQ(g_lose_calls, 0);
        run(&cpu);
        rt::store<uint32_t>(party, 0);  // lost
        g_fade_calls = 0;
        g_handle_released = 0;
        update(event, 1);
        CHECK_EQ(g_fade_calls, 1);
        CHECK_EQ(g_lose_calls, 1);
        CHECK_EQ(g_fade_before_lose, 1);  // the end first, as the script would
        CHECK_EQ(g_lose_x0, 0x1111u);
        CHECK_EQ(g_lose_x1, 0x2222u);
        CHECK_EQ(rt::load<uint32_t>(lose_object + 0x10), 0x99u);
        CHECK_EQ(g_handle_released, lose_handle + 0x48);
        CHECK_EQ(l.x[0], 1u);
        CHECK_EQ(l.sp, stack_top - 0x200);
        // as CallBattleLose: no fields, an object event_from_create cannot find, released all the same
        write(dir / "wild-battles.cfg", cfg + "value event_release_interface 0x48\nvalue event_release_slot 0x18\n"
                                              "hook lose_create 0x800e000\nvalue lose_release 1\n");
        r = {};
        CHECK(mods::start(o, &r, &error));
        CHECK_EQ(r.active.size(), 1u);
        rt::store<uint64_t>(lose_handle + 0x60, 0);
        rt::store<uint32_t>(party, 1);
        run(&cpu);
        rt::store<uint32_t>(party, 0);
        g_lose_calls = 0;
        g_handle_released = 0;
        update(event, 1);
        CHECK_EQ(g_lose_calls, 1);
        CHECK_EQ(g_handle_released, lose_handle + 0x48);

        // `value defeat` is gone: refused
        write(dir / "wild-battles.cfg", cfg + "value defeat 1\n");
        r = {};
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active.empty());
        CHECK(r.messages.size() == 1 && r.messages[0].find("lose_create") != std::string::npos);

        // lose_create without team_fit is refused
        write(dir / "wild-battles.cfg", base + "value redirect 3\nhook battle_event_update 0x8009000\n"
                                               "hook lose_create 0x800e000\n");
        r = {};
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active.empty());
        CHECK(r.messages.size() == 1 && r.messages[0].find("team_fit") != std::string::npos);
    }
    {  // addr partner_state (P8.19): the byte our battle needs, set at the touch, back at our end
        const uint64_t holder = heap + 0xc00;
        const uint64_t partner = heap + 0xc50;
        rt::store<uint64_t>(holder, heap + 0xc40);
        const std::string cfg = base + "value redirect 3\nvalue touch_return 1\n"
                                       "hook battle_event_update 0x8009000\n"
                                       "addr partner_state " + std::to_string(holder) + " >0+0x10\n";
        write(dir / "wild-battles.cfg", cfg);
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CHECK_EQ(r.active.size(), 1u);
        g_created = handle;
        CpuState cpu;
        CpuState l;
        auto update = [&](uint64_t self, uint64_t result) {
            g_update_result = result;
            l = CpuState{};
            l.x[0] = self;
            l.sp = stack_top - 0x200;
            rt::call_hooked(&l, kUpdate, &fake_update);
        };
        rt::store<uint8_t>(partner, 0);  // before the story enables the partner
        run(&cpu);
        CHECK_EQ(g_create_calls, 1);
        CHECK_EQ(rt::load<uint8_t>(partner), 1u);
        update(event, 0);  // not done yet
        CHECK_EQ(rt::load<uint8_t>(partner), 1u);
        update(event, 1);
        CHECK_EQ(rt::load<uint8_t>(partner), 0u);
        rt::store<uint8_t>(partner, 1);  // enabled: left alone
        run(&cpu);
        CHECK_EQ(rt::load<uint8_t>(partner), 1u);
        update(event, 1);
        CHECK_EQ(rt::load<uint8_t>(partner), 1u);
        // a second touch while our battle never reported done keeps the old value
        rt::store<uint8_t>(partner, 0);
        run(&cpu);
        run(&cpu);
        update(event, 1);
        CHECK_EQ(rt::load<uint8_t>(partner), 0u);
        // value partner_set
        write(dir / "wild-battles.cfg", cfg + "value partner_set 2\n");
        r = {};
        CHECK(mods::start(o, &r, &error));
        CHECK_EQ(r.active.size(), 1u);
        run(&cpu);
        CHECK_EQ(rt::load<uint8_t>(partner), 2u);
        update(event, 1);
        CHECK_EQ(rt::load<uint8_t>(partner), 0u);
        // without our end refused
        write(dir / "wild-battles.cfg", base + "value redirect 3\naddr partner_state 0x1000\n");
        r = {};
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active.empty());
        CHECK(r.messages.size() == 1 && r.messages[0].find("partner_state") != std::string::npos);
    }
    {  // expfix (P8.19): our battle from patch battle_time, its setup changed at the next expfix_door
        const uint64_t battle = heap + 0xe00;
        const std::string cfg = base + "value redirect 3\nvalue touch_return 1\n"
                                       "hook battle_event_update 0x8009000\n"
                                       "patch battle_time " + std::to_string(time_site) + "\n"
                                       "value expfix_reg 20\nvalue expfix_flags_offset 0x10\n"
                                       "value expfix_flags_clear 0x80\nvalue expfix_byte_offset 0x18\n"
                                       "hook expfix_door 0x8010000\n";
        write(dir / "wild-battles.cfg", cfg);
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CHECK_EQ(r.active.size(), 1u);
        auto start_battle = [&]() {  // the event battle's start: the limit, x20 = the battle
            CpuState t{};
            t.x[20] = battle;
            t.pc = time_site + 4;
            CHECK(rt::run_patch(&t, time_site));
            return t.x[1];
        };
        auto door = [&]() {
            CpuState d{};
            d.sp = stack_top - 0x200;
            rt::call_hooked(&d, kExpDoor, &fake_door);
        };
        auto reset = [&]() {
            rt::store<uint32_t>(battle + 0x10, 0xff);
            rt::store<uint8_t>(battle + 0x18, 1);
        };
        reset();
        g_door_calls = 0;
        CHECK_EQ(start_battle(), 300u);  // the game's own event battle: no experience as before
        door();
        CHECK_EQ(g_door_calls, 1);
        CHECK_EQ(rt::load<uint32_t>(battle + 0x10), 0xffu);
        CHECK_EQ(rt::load<uint8_t>(battle + 0x18), 1u);
        g_created = handle;
        CpuState cpu;
        run(&cpu);
        CHECK_EQ(start_battle(), 0u);  // ours
        CHECK_EQ(rt::load<uint32_t>(battle + 0x10), 0xffu);  // only at the door
        door();
        CHECK_EQ(g_door_calls, 2);
        CHECK_EQ(rt::load<uint32_t>(battle + 0x10), 0x7fu);
        CHECK_EQ(rt::load<uint8_t>(battle + 0x18), 0u);
        reset();
        door();  // once per battle
        CHECK_EQ(rt::load<uint32_t>(battle + 0x10), 0xffu);
        CHECK_EQ(rt::load<uint8_t>(battle + 0x18), 1u);
        {  // recall_end_start: the cleared bit back for the call only, in our battle
            write(dir / "wild-battles.cfg", cfg + "hook recall_end_start 0x801f000\nhook end_view_wild 0x802e000\n");
            r = {};
            CHECK(mods::start(o, &r, &error));
            CHECK_EQ(r.active.size(), 1u);
            g_recall_battle = battle;
            auto recall_end = [&](uint64_t door_va = kRecallEnd) {
                CpuState d{};
                rt::call_hooked(&d, door_va, &fake_recall_end);
                return g_recall_flags;
            };
            reset();
            run(&cpu);
            CHECK_EQ(start_battle(), 0u);
            door();
            CHECK_EQ(rt::load<uint32_t>(battle + 0x10), 0x7fu);
            CHECK_EQ(recall_end(), 0xffu);
            CHECK_EQ(rt::load<uint32_t>(battle + 0x10), 0x7fu);
            CHECK_EQ(recall_end(kEndView), 0xffu);  // the end view: white, as in a wild battle
            CHECK_EQ(rt::load<uint32_t>(battle + 0x10), 0x7fu);
            run(&cpu);  // the next touch: until its battle starts untouched
            rt::store<uint32_t>(battle + 0x10, 0x7f);
            CHECK_EQ(recall_end(), 0x7fu);
            CHECK_EQ(recall_end(kEndView), 0x7fu);
            CHECK_EQ(start_battle(), 0u);
            write(dir / "wild-battles.cfg", base + "value redirect 3\nhook battle_event_update 0x8009000\n"
                                                   "hook recall_end_start 0x801f000\n");
            r = {};
            CHECK(mods::start(o, &r, &error));
            CHECK(r.active.empty());
            CHECK(r.messages.size() == 1 && r.messages[0].find("recall_end_start") != std::string::npos);
            {  // path ko_seq (Fang-36): the opponent's K.O. plays our sequence, the bit stays cleared
                const uint64_t text = heap + 0xf00, side = heap + 0xf40;
                const char original_text[] = "bin/battle/seq/ee620.bseq";
                for (size_t i = 0; i < sizeof original_text; ++i)
                    rt::store<uint8_t>(text + i, static_cast<uint8_t>(original_text[i]));
                g_ko_path_va = text;
                write(dir / "wild-battles.cfg", cfg + "hook recall_end_start 0x801f000\nvalue ko_seq_text " +
                                                    std::to_string(text) + "\npath ko_seq 0 bin/battle/seq/ex620.bseq\n"
                                                    "from ko_side 1 x1\nvalue ko_side 1\n");
                r = {};
                CHECK(mods::start(o, &r, &error));
                CHECK_EQ(r.active.size(), 1u);
                auto ko = [&](uint64_t slot) {
                    rt::store<uint8_t>(side, static_cast<uint8_t>(slot));
                    CpuState d{};
                    d.x[1] = side;
                    rt::call_hooked(&d, kRecallEnd, &fake_recall_end);
                    return g_recall_flags;
                };
                reset();
                run(&cpu);
                CHECK_EQ(start_battle(), 0u);
                door();
                CHECK_EQ(ko(1), 0x7fu);  // the opponent: bit cleared, our sequence
                CHECK(g_recall_seen == "bin/battle/seq/ex620.bseq");
                CHECK_EQ(rt::load<uint8_t>(text + 16), static_cast<uint8_t>('e'));  // the text back
                CHECK_EQ(ko(0), 0xffu);  // the own side: as before
                CHECK(g_recall_seen == original_text);
                CHECK_EQ(rt::load<uint32_t>(battle + 0x10), 0x7fu);
                write(dir / "wild-battles.cfg", cfg + "hook recall_end_start 0x801f000\nvalue ko_seq_text " +
                                                    std::to_string(text) + "\npath ko_seq 0 bin/battle/seq/ex620.bseq\n"
                                                    "from ko_side 0 x2\nvalue ko_side 1\n");  // the slot in x2 itself
                r = {};
                CHECK(mods::start(o, &r, &error));
                CHECK_EQ(r.active.size(), 1u);
                auto ko_reg = [&](uint64_t slot) {
                    CpuState d{};
                    d.x[2] = slot;
                    rt::call_hooked(&d, kRecallEnd, &fake_recall_end);
                    return g_recall_flags;
                };
                reset();
                run(&cpu);
                CHECK_EQ(start_battle(), 0u);
                door();
                CHECK_EQ(ko_reg(1), 0x7fu);
                CHECK(g_recall_seen == "bin/battle/seq/ex620.bseq");
                CHECK_EQ(ko_reg(0), 0xffu);
                CHECK(g_recall_seen == original_text);
                write(dir / "wild-battles.cfg", cfg + "hook recall_end_start 0x801f000\npath ko_seq 0 x.bseq\n");
                r = {};
                CHECK(mods::start(o, &r, &error));
                CHECK(r.active.empty());
                CHECK(r.messages.size() == 1 && r.messages[0].find("ko_seq_text") != std::string::npos);
                g_ko_path_va = 0;
            }
            write(dir / "wild-battles.cfg", cfg);
            r = {};
            CHECK(mods::start(o, &r, &error));
        }
        // refused: without patch battle_time, without the door
        write(dir / "wild-battles.cfg", base + "value redirect 3\nhook battle_event_update 0x8009000\n"
                                               "value expfix_reg 20\nvalue expfix_flags_offset 0x10\n"
                                               "value expfix_flags_clear 0x80\nvalue expfix_byte_offset 0x18\n"
                                               "hook expfix_door 0x8010000\n");
        r = {};
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active.empty());
        CHECK(r.messages.size() == 1 && r.messages[0].find("battle_time") != std::string::npos);
        write(dir / "wild-battles.cfg", base + "value redirect 3\npatch battle_time " + std::to_string(time_site) +
                                               "\nvalue expfix_reg 20\nvalue expfix_flags_offset 0x10\n"
                                               "value expfix_flags_clear 0x80\nvalue expfix_byte_offset 0x18\n");
        r = {};
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active.empty());
        CHECK(r.messages.size() == 1 && r.messages[0].find("expfix_door") != std::string::npos);
    }
    {  // part_find_guard (P8.19): the lookup only when the registry has the character's resource
        const uint64_t ch = heap + 0xd00;
        const uint64_t holder = heap + 0xd80;
        const uint64_t registry = heap + 0xdc0;
        const uint64_t alt = heap + 0xec0;
        const uint64_t found = heap + 0xe80;  // [found + 0x48]: the counter interface, its vtable
        rt::store<uint64_t>(ch + 8, holder);
        rt::store<uint64_t>(holder + 0x10, registry);
        rt::store<uint64_t>(ch + 0x20, 0x1234);
        rt::store<uint8_t>(ch + 0x28, 0);
        rt::store<uint64_t>(ch + 0x30, alt);
        rt::store<uint32_t>(alt + 4, 1);
        rt::store<uint64_t>(found + 0x48, heap + 0x800);
        g_found = found;
        const std::string cfg = base + "value redirect 3\nvalue touch_return 1\n"
                                       "hook battle_event_update 0x8009000\n"
                                       "patch battle_time " + std::to_string(time_site) + "\n"
                                       "hook part_find_guard 0x8011000\nvalue registry_find 0x8012000\n"
                                       "value part_holder 8\nvalue part_registry 0x10\nvalue part_key 0x20\n"
                                       "value part_alt_switch 0x28\nvalue part_alt_key 0x30\nvalue part_alt_count 4\n"
                                       "value part_release_interface 0x48\nvalue part_release_slot 0x18\n"
                                       "hook loss_amount 0x8018000\n";
        write(dir / "wild-battles.cfg", cfg);
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CHECK_EQ(r.active.size(), 1u);
        CpuState f;
        auto find = [&]() {
            f = CpuState{};
            f.x[0] = ch;
            f.x[19] = 19;
            f.sp = stack_top - 0x200;
            rt::call_hooked(&f, kPartFind, &fake_part_find);
            return f.x[0];
        };
        auto loss = [&]() {
            CpuState l{};
            l.x[0] = 0x4444;
            rt::call_hooked(&l, kLossAmount, &fake_loss);
            return l.x[0];
        };
        CHECK_EQ(loss(), 1000u);  // not our battle: the game's amount
        g_known_key = 0x9999;
        g_registry_x0 = 0;
        CHECK_EQ(find(), 0x5150u);  // not our battle: the game's lookup, untouched
        CHECK_EQ(g_registry_x0, 0u);
        g_created = handle;
        CpuState cpu;
        run(&cpu);  // our battle starts
        {
            CpuState t{};
            t.pc = time_site + 4;
            CHECK(rt::run_patch(&t, time_site));
        }
        CHECK_EQ(loss(), 0u);  // a defeat in our battle: no money lost
        g_known_key = 0x1234;
        g_handle_released = 0;
        CHECK_EQ(find(), 0x5150u);  // in the registry: the game's lookup
        CHECK_EQ(g_registry_x0, registry);
        CHECK_EQ(g_handle_released, found + 0x48);  // the reference dropped
        CHECK_EQ(f.x[19], 19u);
        CHECK_EQ(f.sp, stack_top - 0x200);
        g_known_key = 0x9999;
        g_handle_released = 0;
        CHECK_EQ(find(), 0u);  // missing: 0 instead of the lookup
        CHECK_EQ(g_handle_released, 0u);
        CHECK_EQ(f.x[19], 19u);
        CHECK_EQ(f.sp, stack_top - 0x200);
        rt::store<uint8_t>(ch + 0x28, 1);  // the alternative key
        g_known_key = alt;
        CHECK_EQ(find(), 0x5150u);
        rt::store<uint32_t>(alt + 4, 0);  // an empty alternative: the key
        CHECK_EQ(find(), 0u);
        g_known_key = 0x1234;
        CHECK_EQ(find(), 0x5150u);
        {  // our battle's event done: the game's lookup again
            g_update_result = 1;
            CpuState l{};
            l.x[0] = event;
            l.sp = stack_top - 0x200;
            rt::call_hooked(&l, kUpdate, &fake_update);
        }
        g_known_key = 0x9999;
        CHECK_EQ(find(), 0x5150u);
        CHECK_EQ(loss(), 1000u);
        {  // our battle without done, then the game's own event battle starts
            g_created = handle;
            CpuState c2;
            run(&c2);
            CpuState t{};
            t.pc = time_site + 4;
            CHECK(rt::run_patch(&t, time_site));
            CHECK_EQ(loss(), 0u);
            CpuState g{};
            g.pc = time_site + 4;
            CHECK(rt::run_patch(&g, time_site));  // no touch before: the game's battle
            CHECK_EQ(loss(), 1000u);
            CHECK_EQ(find(), 0x5150u);
        }
        // refused without a value
        write(dir / "wild-battles.cfg",
              base + "value redirect 3\nhook part_find_guard 0x8011000\nvalue registry_find 0x8012000\n");
        r = {};
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active.empty());
        CHECK(r.messages.size() == 1 && r.messages[0].find("part_holder") != std::string::npos);
    }
    {  // evolution after our battles and no catch experience twice (P8.19d)
        const uint64_t battle = heap + 0xe00;
        const uint64_t post = heap + 0xf00;     // state +0, level-up bytes +0x20
        const uint64_t capture = heap + 0xf80;  // state +0, counter +8, evolve bytes +0x10
        const std::string expfix = base + "value redirect 3\nvalue touch_return 1\n"
                                          "hook battle_event_update 0x8009000\n"
                                          "patch battle_time " + std::to_string(time_site) + "\n"
                                          "value expfix_reg 20\nvalue expfix_flags_offset 0x10\n"
                                          "value expfix_flags_clear 0x80\nvalue expfix_byte_offset 0x18\n"
                                          "hook expfix_door 0x8010000\n";
        const std::string carry = "hook post_battle 0x8013000\nhook capture_process 0x8014000\n"
                                  "value post_state_offset 0\nvalue post_flags_offset 0x20\nvalue post_catch_state 5\n"
                                  "value capture_state_offset 0\nvalue capture_flags_offset 0x10\n"
                                  "value capture_counter_offset 8\nvalue capture_evolve_state 7\n"
                                  "value capture_skip_state 10\n";
        write(dir / "wild-battles.cfg", expfix + carry + "hook catch_exp_amount 0x8015000\n");
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CHECK_EQ(r.active.size(), 1u);
        auto start_battle = [&]() {
            CpuState t{};
            t.x[20] = battle;
            t.pc = time_site + 4;
            CHECK(rt::run_patch(&t, time_site));
            CpuState d{};
            d.sp = stack_top - 0x200;
            rt::call_hooked(&d, kExpDoor, &fake_door);
        };
        auto post_battle = [&](uint32_t next, bool levelled = true) {  // its first state, levelled up: members 1, 3
            for (unsigned i = 0; i < 6; ++i)
                rt::store<uint8_t>(post + 0x20 + i, levelled && (i == 1 || i == 3) ? 1 : 0);
            rt::store<uint32_t>(post, 0);
            g_next_state = next;
            CpuState p{};
            p.x[0] = post;
            rt::call_hooked(&p, kPostBattle, &fake_post_battle);
        };
        auto step = [&](uint32_t from, uint32_t to) {  // one update of the catch process
            rt::store<uint32_t>(capture, from);
            g_next_state = to;
            CpuState p{};
            p.x[0] = capture;
            p.sp = stack_top - 0x200;
            rt::call_hooked(&p, kCapture, &fake_capture);
            return rt::load<uint32_t>(capture);
        };
        auto catch_exp = [&]() {
            CpuState p{};
            rt::call_hooked(&p, kCatchExp, &fake_catch_exp);
            return p.x[0];
        };
        auto evolve_bytes = [&]() {
            uint64_t v = 0;
            for (unsigned i = 0; i < 6; ++i)
                v |= uint64_t{rt::load<uint8_t>(capture + 0x10 + i)} << (8 * i);
            for (unsigned i = 0; i < 6; ++i)
                rt::store<uint8_t>(capture + 0x10 + i, 0);
            return v;
        };
        evolve_bytes();
        // the game's own event battle: as before
        start_battle();
        post_battle(5);
        CHECK_EQ(step(7, 8), 8u);
        CHECK_EQ(evolve_bytes(), 0u);
        CHECK_EQ(catch_exp(), 500u);
        // ours, caught: the catch process evolves members 1 and 3, no catch experience
        g_created = handle;
        CpuState cpu;
        run(&cpu);
        start_battle();
        post_battle(5);
        CHECK_EQ(catch_exp(), 0u);
        CHECK_EQ(step(6, 7), 7u);
        CHECK_EQ(evolve_bytes(), 0u);
        CHECK_EQ(step(7, 8), 8u);
        CHECK_EQ(evolve_bytes(), 0x0000'0100'0100u);
        CHECK_EQ(step(7, 8), 8u);  // once
        CHECK_EQ(evolve_bytes(), 0u);
        CHECK_EQ(catch_exp(), 0u);
        run(&cpu);  // the next touch: its own experience
        CHECK_EQ(catch_exp(), 500u);
        // ours, given up: evolution first, then on with the skip
        start_battle();
        post_battle(5);
        CHECK_EQ(step(6, 10), 7u);
        CHECK_EQ(rt::load<uint32_t>(capture + 8), 0u);
        CHECK_EQ(step(7, 7), 7u);  // evolving
        CHECK_EQ(evolve_bytes(), 0x0000'0100'0100u);
        CHECK_EQ(step(7, 8), 10u);
        CHECK_EQ(step(10, 11), 11u);
        // ours without a catch scene and without post_end_state: left alone
        run(&cpu);
        start_battle();
        post_battle(4);
        CHECK_EQ(rt::load<uint32_t>(post), 4u);
        CHECK_EQ(catch_exp(), 500u);
        CHECK_EQ(step(7, 8), 8u);
        CHECK_EQ(evolve_bytes(), 0u);
        // caught in the battle (P8.20): post_end_state becomes post_evolve_state
        write(dir / "wild-battles.cfg",
              expfix + carry + "hook catch_exp_amount 0x8015000\nvalue post_end_state 4\nvalue post_evolve_state 1\n");
        r = {};
        CHECK(mods::start(o, &r, &error));
        CHECK_EQ(r.active.size(), 1u);
        start_battle();  // the game's own battle
        post_battle(4);
        CHECK_EQ(rt::load<uint32_t>(post), 4u);
        run(&cpu);
        start_battle();
        post_battle(4);
        CHECK_EQ(rt::load<uint32_t>(post), 1u);
        CHECK_EQ(catch_exp(), 500u);
        run(&cpu);
        start_battle();
        post_battle(4, false);  // nobody levelled up
        CHECK_EQ(rt::load<uint32_t>(post), 4u);
        run(&cpu);
        start_battle();
        post_battle(5);  // won: the catch scene as before
        CHECK_EQ(rt::load<uint32_t>(post), 5u);
        CHECK_EQ(catch_exp(), 0u);
        // caught in the battle, way A: the catch process for dex and box, past the throw
        const std::string caught = "value post_end_state 4\nvalue post_evolve_state 1\n"
                                   "from post_result 4 obj+0x30\nvalue post_caught_result 5\n"
                                   "value capture_result_offset 0x20\nvalue capture_caught_state 6\n";
        write(dir / "wild-battles.cfg", expfix + carry + "hook catch_exp_amount 0x8015000\n" + caught);
        r = {};
        CHECK(mods::start(o, &r, &error));
        CHECK_EQ(r.active.size(), 1u);
        rt::store<uint32_t>(post + 0x30, 1);
        start_battle();  // the game's own battle, won: as before
        post_battle(4);
        CHECK_EQ(rt::load<uint32_t>(post), 4u);
        // the game's own wild battle (a legendary), caught with a ball: catch
        // process too, without level-ups, its catch keeps its experience; also
        // with the menu's switch off
        CHECK(mods::set_active("wild-battles", false));
        rt::store<uint32_t>(post + 0x30, 5);
        start_battle();
        post_battle(4);
        CHECK_EQ(rt::load<uint32_t>(post), 5u);
        rt::store<uint32_t>(capture + 0x20, 0x77);
        CHECK_EQ(step(0, 1), 6u);
        CHECK_EQ(rt::load<uint32_t>(capture + 0x20), 0u);
        CHECK_EQ(step(7, 8), 8u);
        CHECK_EQ(evolve_bytes(), 0u);
        CHECK_EQ(catch_exp(), 500u);
        CHECK(mods::set_active("wild-battles", true));
        {  // post_as_game: ours caught as the game's own
            write(dir / "wild-battles.cfg",
                  expfix + carry + "hook catch_exp_amount 0x8015000\n" + caught + "value post_as_game 1\n");
            r = {};
            CHECK(mods::start(o, &r, &error));
            CHECK_EQ(r.active.size(), 1u);
            run(&cpu);
            start_battle();
            post_battle(4);
            CHECK_EQ(rt::load<uint32_t>(post), 5u);
            CHECK_EQ(step(0, 1), 6u);
            CHECK_EQ(step(7, 8), 8u);
            CHECK_EQ(evolve_bytes(), 0u);
            CHECK_EQ(catch_exp(), 500u);
            write(dir / "wild-battles.cfg", expfix + carry + "hook catch_exp_amount 0x8015000\n" + caught);
            r = {};
            CHECK(mods::start(o, &r, &error));
            CHECK_EQ(r.active.size(), 1u);
        }
        run(&cpu);
        start_battle();
        post_battle(4);
        CHECK_EQ(rt::load<uint32_t>(post), 5u);  // on to the catch scene
        rt::store<uint32_t>(capture + 0x20, 0x77);
        CHECK_EQ(step(0, 1), 6u);  // its first update: caught
        CHECK_EQ(rt::load<uint32_t>(capture + 0x20), 0u);
        CHECK_EQ(step(6, 7), 7u);  // once
        CHECK_EQ(step(7, 8), 8u);
        CHECK_EQ(evolve_bytes(), 0x0000'0100'0100u);
        CHECK_EQ(catch_exp(), 0u);
        rt::store<uint32_t>(post + 0x30, 3);  // fled with a level-up: the evolution state
        run(&cpu);
        start_battle();
        post_battle(4);
        CHECK_EQ(rt::load<uint32_t>(post), 1u);
        rt::store<uint32_t>(capture + 0x20, 0x77);
        CHECK_EQ(step(0, 1), 1u);
        CHECK_EQ(rt::load<uint32_t>(capture + 0x20), 0x77u);
        {  // capture_entry_state: caught once the process got past its start state
            write(dir / "wild-battles.cfg",
                  expfix + carry + "hook catch_exp_amount 0x8015000\n" + caught + "value capture_entry_state 3\n");
            r = {};
            CHECK(mods::start(o, &r, &error));
            CHECK_EQ(r.active.size(), 1u);
            rt::store<uint32_t>(post + 0x30, 5);
            run(&cpu);
            start_battle();
            post_battle(4);
            CHECK_EQ(rt::load<uint32_t>(post), 5u);
            rt::store<uint32_t>(capture + 0x20, 0x77);
            CHECK_EQ(step(0, 1), 1u);  // its start state: not yet
            CHECK_EQ(rt::load<uint32_t>(capture + 0x20), 0x77u);
            CHECK_EQ(step(1, 3), 6u);  // there: caught
            CHECK_EQ(rt::load<uint32_t>(capture + 0x20), 0u);
            CHECK_EQ(step(1, 3), 3u);  // once
            // call capture_entry_ready: there and the fader done (Fang-11)
            const uint64_t cell = heap + rt::kPageSize + 0x100;  // the global fader pointer
            const uint64_t fader = heap + rt::kPageSize + 0x140;
            rt::store<uint64_t>(cell, fader);
            rt::store<uint8_t>(fader + 2, 0);
            write(dir / "wild-battles.cfg", expfix + carry + "hook catch_exp_amount 0x8015000\n" + caught +
                                                "value capture_entry_state 3\ncall capture_entry_ready 0x802f000 " +
                                                std::to_string(cell) + " >0\nvalue capture_entry_ready_x1 2\n");
            r = {};
            CHECK(mods::start(o, &r, &error));
            CHECK_EQ(r.active.size(), 1u);
            run(&cpu);
            start_battle();
            post_battle(4);
            rt::store<uint32_t>(capture + 0x20, 0x77);
            CHECK_EQ(step(1, 3), 3u);  // there
            CHECK_EQ(step(3, 3), 3u);  // still fading
            CHECK_EQ(rt::load<uint32_t>(capture + 0x20), 0x77u);
            rt::store<uint8_t>(fader + 2, 1);
            // done: caught before the original, which would go on into the Go app
            CHECK_EQ(step(3, 4), 4u);
            CHECK_EQ(rt::load<uint32_t>(capture + 0x20), 0u);
            CHECK_EQ(g_capture_seen, 6u);
            write(dir / "wild-battles.cfg",
                  expfix + carry + caught + "call capture_entry_ready 0x802f000 " + std::to_string(cell) + " >0\n");
            r = {};
            CHECK(mods::start(o, &r, &error));
            CHECK(r.active.empty());
            CHECK(r.messages.size() == 1 && r.messages[0].find("capture_entry_ready") != std::string::npos);
            write(dir / "wild-battles.cfg", expfix + carry + "value capture_entry_state 3\n");
            r = {};
            CHECK(mods::start(o, &r, &error));
            CHECK(r.active.empty());
            CHECK(r.messages.size() == 1 && r.messages[0].find("capture_entry_state") != std::string::npos);
            rt::store<uint32_t>(post + 0x30, 3);
        }
        {  // won by K.O. with balls in the bag (knockout_always: without battle-balls): no catch scene
            write(dir / "wild-battles.cfg", expfix + carry + "hook catch_exp_amount 0x8015000\n" + caught +
                                                "value knockout_result 2\nvalue knockout_always 1\n");
            r = {};
            CHECK(mods::start(o, &r, &error));
            CHECK_EQ(r.active.size(), 1u);
            rt::store<uint32_t>(post + 0x30, 1);
            run(&cpu);
            start_battle();
            post_battle(5);  // won, levelled up: evolve first, then given up
            CHECK_EQ(rt::load<uint32_t>(post), 5u);
            CHECK_EQ(step(0, 1), 7u);
            CHECK_EQ(rt::load<uint32_t>(capture + 0x20), 2u);
            CHECK_EQ(step(7, 8), 10u);
            CHECK_EQ(evolve_bytes(), 0x0000'0100'0100u);
            CHECK_EQ(step(10, 11), 11u);
            run(&cpu);
            start_battle();
            post_battle(5, false);  // nobody levelled up: given up at once
            CHECK_EQ(step(0, 1), 10u);
            CHECK_EQ(step(1, 2), 2u);  // once
            // a K.O. still pending when the mod starts again: forgotten
            run(&cpu);
            start_battle();
            post_battle(5, false);
            r = {};
            CHECK(mods::start(o, &r, &error));
            CHECK_EQ(step(0, 1), 1u);
            rt::store<uint32_t>(post + 0x30, 5);  // caught with a ball: unchanged
            run(&cpu);
            start_battle();
            post_battle(4);
            CHECK_EQ(step(0, 1), 6u);
            // without battle-balls and knockout_always: the catch scene as before
            write(dir / "wild-battles.cfg",
                  expfix + carry + "hook catch_exp_amount 0x8015000\n" + caught + "value knockout_result 2\n");
            r = {};
            CHECK(mods::start(o, &r, &error));
            rt::store<uint32_t>(post + 0x30, 1);
            run(&cpu);
            start_battle();
            post_battle(5, false);
            CHECK_EQ(step(0, 1), 1u);
            write(dir / "wild-battles.cfg", expfix + carry + "value knockout_result 2\n");
            r = {};
            CHECK(mods::start(o, &r, &error));
            CHECK(r.active.empty());
            CHECK(r.messages.size() == 1 && r.messages[0].find("knockout_result") != std::string::npos);
        }
        {  // a ball chosen in our battle with the throw's animation off: the Let's Go catch scene
            const std::string choice = "hook ball_choice 0x8031000\nvalue ball_choice_item_reg 3\n"
                                       "value ball_choice_kind 0x8032000\nvalue ball_choice_field 0x10\n"
                                       "value ball_choice 5\n";
            auto choose = [&](uint16_t item) {  // returns x0 after the handler
                CpuState p{};
                p.x[3] = 0xffff'0000u | item;  // only the low 16 bits are the item
                p.sp = stack_top - 0x200;
                rt::call_hooked(&p, kBallChoice, &fake_item_handler);
                return p.x[0];
            };
            write(dir / "wild-battles.cfg",
                  expfix + carry + "hook catch_exp_amount 0x8015000\n" + caught + choice + "value ball_choice_always 1\n");
            r = {};
            CHECK(mods::start(o, &r, &error));
            CHECK_EQ(r.active.size(), 1u);
            rt::store<uint32_t>(post + 0x30, 3);  // fled
            run(&cpu);
            start_battle();
            g_item_handled = 0;
            CHECK_EQ(choose(30), 7u);  // not a ball: the game's handler, with its entry registers
            CHECK_EQ(g_item_handled, 1u);
            CHECK_EQ(g_item_x3, 0xffff'0000u | 30u);
            CHECK_EQ(choose(4), 1u);  // a Poké Ball: skipped, the escape item's way
            CHECK_EQ(g_item_handled, 1u);
            post_battle(4);
            CHECK_EQ(rt::load<uint32_t>(post), 5u);  // on to the catch scene, not the evolution state
            CHECK_EQ(catch_exp(), 500u);             // which keeps its experience
            rt::store<uint32_t>(capture + 0x20, 0x77);
            CHECK_EQ(step(0, 1), 1u);  // not past the throw: the player throws there
            CHECK_EQ(rt::load<uint32_t>(capture + 0x20), 0x77u);
            CHECK_EQ(step(7, 8), 8u);  // the battle's level-ups carried as after a win
            CHECK_EQ(evolve_bytes(), 0x0000'0100'0100u);
            run(&cpu);  // fled without a ball: as before
            start_battle();
            post_battle(4);
            CHECK_EQ(rt::load<uint32_t>(post), 1u);
            // the game's own battle (no touch before it): its handler
            start_battle();
            g_item_handled = 0;
            CHECK_EQ(choose(12), 7u);
            CHECK_EQ(g_item_handled, 1u);
            post_battle(4);
            CHECK_EQ(rt::load<uint32_t>(post), 4u);
            // without battle-balls (and without ball_choice_always): the game's handler
            write(dir / "wild-battles.cfg", expfix + carry + caught + choice);
            r = {};
            CHECK(mods::start(o, &r, &error));
            CHECK_EQ(r.active.size(), 1u);
            run(&cpu);
            start_battle();
            CHECK_EQ(choose(4), 7u);
            post_battle(4);
            CHECK_EQ(rt::load<uint32_t>(post), 1u);
            // refused: without the getter, without post_end_state
            write(dir / "wild-battles.cfg", expfix + carry + caught +
                                                "hook ball_choice 0x8031000\nvalue ball_choice_item_reg 3\n"
                                                "value ball_choice_field 0x10\nvalue ball_choice 5\n");
            r = {};
            CHECK(mods::start(o, &r, &error));
            CHECK(r.active.empty());
            CHECK(r.messages.size() == 1 && r.messages[0].find("ball_choice_kind") != std::string::npos);
            write(dir / "wild-battles.cfg", expfix + carry + choice);
            r = {};
            CHECK(mods::start(o, &r, &error));
            CHECK(r.active.empty());
            CHECK(r.messages.size() == 1 && r.messages[0].find("post_end_state") != std::string::npos);
        }
        write(dir / "wild-battles.cfg", expfix + carry + "from post_result 4 obj+0x30\nvalue post_caught_result 5\n"
                                        "value capture_result_offset 0x20\nvalue capture_caught_state 6\n");
        r = {};
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active.empty());
        CHECK(r.messages.size() == 1 && r.messages[0].find("post_end_state") != std::string::npos);
        write(dir / "wild-battles.cfg", expfix + carry + "value post_end_state 4\n");
        r = {};
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active.empty());
        CHECK(r.messages.size() == 1 && r.messages[0].find("post_evolve_state") != std::string::npos);
        write(dir / "wild-battles.cfg", expfix + "value post_end_state 4\nvalue post_evolve_state 1\n");
        r = {};
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active.empty());
        CHECK(r.messages.size() == 1 && r.messages[0].find("post_battle") != std::string::npos);
        // refused: one without the other, without expfix, catch_exp_amount alone
        write(dir / "wild-battles.cfg", expfix + "hook post_battle 0x8013000\n");
        r = {};
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active.empty());
        CHECK(r.messages.size() == 1 && r.messages[0].find("need each other") != std::string::npos);
        write(dir / "wild-battles.cfg", base + "value redirect 3\n" + carry);
        r = {};
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active.empty());
        CHECK(r.messages.size() == 1 && r.messages[0].find("expfix_reg") != std::string::npos);
        write(dir / "wild-battles.cfg", expfix + "hook catch_exp_amount 0x8015000\n");
        r = {};
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active.empty());
        CHECK(r.messages.size() == 1 && r.messages[0].find("catch_exp_amount") != std::string::npos);
    }
    {  // hook catch_exp is gone (P8.11: the catch gives the only experience): refused
        write(dir / "wild-battles.cfg", base + "value redirect 3\nhook battle_event_update 0x8009000\n"
                                               "hook catch_exp 0x800f000\n");
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active.empty());
        CHECK(r.messages.size() == 1 && r.messages[0].find("catch_exp is gone") != std::string::npos);
    }
    {  // redirect 0 (default): only logs
        write(dir / "wild-battles.cfg", base);
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CHECK_EQ(r.active.size(), 1u);
        CpuState cpu;
        run(&cpu);
        CHECK_EQ(g_original_calls, 1);
        CHECK_EQ(g_create_calls, 0);
        CHECK_EQ(rt::load<uint64_t>(event + 0x10), 0u);
    }
    {  // a creator without code halts the guest
        std::string text = base + "value redirect 3\n";
        text.replace(text.find("0x8008000"), 9, "0x800c000");
        write(dir / "wild-battles.cfg", text);
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CpuState cpu;
        run(&cpu);
        CHECK_EQ(cpu.halt, uint32_t{kHaltBadJump});
        CHECK_EQ(cpu.halt_info, 0x800c000u);
    }
    {  // standby: not named, installed switched off; the menu switches it on
        write(dir / "wild-battles.cfg", base + "value redirect 3\n");
        mods::StartOptions s = o;
        s.enabled.clear();
        mods::StartResult r;
        CHECK(mods::start(s, &r, &error));  // without standby: not installed
        CHECK(r.active.empty() && r.standby.empty());
        CHECK(!mods::enabled("wild-battles"));
        s.standby = true;
        r = {};
        CHECK(mods::start(s, &r, &error));
        CHECK(r.active.empty());
        CHECK(r.standby == std::vector<std::string>{"wild-battles"});
        CHECK(mods::enabled("wild-battles"));
        CHECK(!mods::active("wild-battles"));
        CpuState cpu;
        run(&cpu);
        CHECK_EQ(g_original_calls, 1);
        CHECK(mods::set_active("wild-battles", true));
        g_created = handle;
        run(&cpu);
        CHECK_EQ(g_original_calls, 0);
        CHECK_EQ(g_create_calls, 1);
        r = {};
        CHECK(mods::start(o, &r, &error));  // named and standby: on
        CHECK(r.active == std::vector<std::string>{"wild-battles"} && r.standby.empty());
        write(dir / "wild-battles.cfg", "hook encounter_touch\n");  // a broken file: a note, no error
        r = {};
        CHECK(mods::start(s, &r, &error));
        CHECK(r.standby.empty());
        CHECK(!r.messages.empty());
        fs::remove(dir / "wild-battles.cfg");  // no file: nothing to say
        r = {};
        CHECK(mods::start(s, &r, &error));
        CHECK(r.messages.empty());
        CHECK(!mods::set_active("wild-battles", true));  // not started
        CHECK(!mods::active("wild-battles"));
    }
    for (const char* bad :
         {"value redirect 1\n", "value redirect 2\n", "value redirect 4\n",
          "value redirect 3\nvalue lookup_hash_reg 8\n", "value redirect 3\nvalue event_release_slot 0x18\n",
          "value redirect 3\nhook battle_event_update 0x8009000\nvalue end_1_offset 0\nvalue end_1 0\n",
          "value redirect 3\nvalue record_3_offset 1\n", "value redirect 3\nvalue record_3_offset 1\nfrom record_3 4 y1\n",
          "value redirect 3\nvalue record_3_offset 1\nvalue record_3 1\ncall record_3 0x8007000 obj\n"}) {
        write(dir / "wild-battles.cfg", base + bad);
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active.empty());
    }
    {  // lines missing, broken expression
        mods::StartResult r;
        std::string text = base + "value redirect 3\n";
        text.replace(text.find(">0>8"), 4, ">0*8");
        write(dir / "wild-battles.cfg", text);
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active.empty());
        write(dir / "wild-battles.cfg", "hook encounter_touch 0x8001000\nvalue redirect 3\n");
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active.empty());
        CHECK(!r.messages.empty() && r.messages.back().find("event_create") != std::string::npos);
        text = base + "value redirect 3\n";
        for (const char* line : {"value event_1_offset 0x10\nvalue event_1 0x77\nvalue event_1_size 8\n"
                                 "value event_2_offset 0x18\nvalue event_2 1\nvalue event_2_size 1\n",
                                 "value template_hash 0x77\n"}) {
            std::string t = text;
            t.replace(t.find(line), std::strlen(line), "");
            write(dir / "wild-battles.cfg", t);
            r = {};
            CHECK(mods::start(o, &r, &error));
            CHECK(r.active.empty());
        }
    }
    mods::stop();
    CHECK(!mods::active("wild-battles"));
    rt::reset_dispatch();
}

// random-encounters (P8, Zufall-2): the check's object list, the type chain,
// setup/touch and the counter are host functions; the objects live on the heap.
constexpr uint64_t kCheck = 0x0802'0000;
constexpr uint64_t kCtor = 0x0802'1000;
constexpr uint64_t kType = 0x0802'2000;
constexpr uint64_t kSetup = 0x0802'3000;
constexpr uint64_t kTouchFn = 0x0802'4000;
constexpr uint64_t kRefInc = 0x0802'5000;
constexpr uint64_t kRefDec = 0x0802'6000;
constexpr uint64_t kSpawner = 0x0802'7000;
constexpr uint64_t kRateLookup = 0x0802'8000;
constexpr uint64_t kDurationLookup = 0x0802'9000;
constexpr uint64_t kModelSwitch = 0x0802'd000;
uint64_t g_model_x1 = ~uint64_t{0};  // the covered flag the model switch got
void fake_model_switch(CpuState* ctx) { g_model_x1 = ctx->x[1]; }
constexpr uint64_t kEffectUpdate = 0x0803'0000;
constexpr uint64_t kEmitterLookup = 0x0803'1000;
// The game's effect update: the aura's "on" byte from in sight, as the game does.
void fake_effect_update(CpuState* ctx) { rt::store<uint8_t>(ctx->x[0] + 0xab, 1u); }
void fake_emitter_lookup(CpuState*) {}  // handle -> emitter: the same address here
// The game's check with hide_contact 2: what the contact counts are while it runs.
uint64_t g_check_comp1 = 0, g_check_comp2 = 0;
uint32_t g_check_seen1 = 0, g_check_seen2 = 0;
void original_seeing_contact(CpuState* ctx) {
    original(ctx);
    g_check_seen1 = rt::load<uint32_t>(g_check_comp1 + 4);
    g_check_seen2 = rt::load<uint32_t>(g_check_comp2 + 4);
}

void fake_type(CpuState* ctx) {  // the type interface: its chain is the word after it
    ctx->x[0] = rt::load<uint64_t>(ctx->x[0] + 8);
    ctx->x[19] = 0x1919;
}
uint64_t g_re_handle = 0, g_re_setup_obj = 0;
void fake_setup(CpuState* ctx) {  // encounter_setup(x1 = &object, x8 = &handle)
    g_re_setup_obj = rt::load<uint64_t>(ctx->x[1]);
    rt::store<uint64_t>(ctx->x[8], g_re_handle);
}
int g_re_refs = 0;
void fake_ref_inc(CpuState* ctx) {
    ++g_re_refs;
    ctx->x[0] += 0x10;  // what the touch gets
}
void fake_ref_dec(CpuState*) { --g_re_refs; }
struct TouchArgs {
    int calls = 0;
    uint64_t x0 = 0, x1 = 0, handle = 0, zone = 0, kind = 0, x24 = 0;
} g_re_touch;
void fake_touch(CpuState* ctx) {
    ++g_re_touch.calls;
    g_re_touch.x0 = ctx->x[0];
    g_re_touch.x1 = ctx->x[1];
    g_re_touch.handle = rt::load<uint64_t>(ctx->x[2]);
    g_re_touch.zone = ctx->x[3];
    g_re_touch.kind = ctx->x[4];
    g_re_touch.x24 = ctx->x[24];  // the object, as in the game's check (wild-battles reads it)
}
void fake_ctor(CpuState* ctx) { ctx->x[0] = 0; }
uint64_t g_spawner_obj = 0, g_spawner_zone = 0;  // what the spawner sees of that object's zone
uint32_t g_duration = 0;  // what the duration lookup writes to x0 + 0x40
void fake_duration_lookup(CpuState* ctx) { rt::store<uint32_t>(ctx->x[0] + 0x40, g_duration); }
uint64_t g_rate = 0;  // what the zone's rate lookup returns
void fake_rate_lookup(CpuState* ctx) { ctx->x[0] = g_rate; }
uint64_t g_spawner_scan = 0, g_spawner_scan_n = 0, g_spawner_left_out = 0;  // objects at 0x100 steps
void fake_spawner(CpuState*) {
    g_spawner_zone = rt::load<uint64_t>(g_spawner_obj + 0x80);
    g_spawner_left_out = 0;
    for (uint64_t i = 0; i < g_spawner_scan_n; ++i)
        if (rt::load<uint64_t>(g_spawner_scan + i * 0x100 + 0x80) == ~uint64_t{0})
            ++g_spawner_left_out;
}

void random_encounters(const fs::path& dir) {
    rt::GuestMemory memory(rt::Layout::packed(0x0800'0000, 0x10'0000, 0x10'0000, 0x10'0000, 0x10'0000, 0x40'0000));
    const uint64_t heap = memory.layout().heap.base;
    CHECK(memory.map(heap, 4 * rt::kPageSize, rt::MemState::Normal, rt::kPermRW).ok());
    const uint64_t stack_top = heap + 4 * rt::kPageSize;
    // check context -> c0 -> holder (+0x10) -> manager (+0x18) -> list (+0x20), count (+0x28)
    const uint64_t context = heap, c0 = heap + 0x10, holder = heap + 0x40, mgr = heap + 0x60, list = heap + 0x90;
    rt::store<uint64_t>(context, c0);
    rt::store<uint64_t>(c0 + 0x10, holder);
    rt::store<uint64_t>(holder + 0x18, mgr);
    rt::store<uint64_t>(mgr + 0x20, list);
    rt::store<uint64_t>(mgr + 0x28, 4);
    const uint64_t type_vt = heap + 0xc0, got = heap + 0xc8, node = heap + 0xd0, other_node = heap + 0xe0;
    rt::store<uint64_t>(type_vt, kType);
    rt::store<uint64_t>(got, node);
    rt::store<uint64_t>(other_node, node + 0x100);  // a chain without the node: not a field Pokémon
    rt::store<uint64_t>(node + 0x100, 0);
    // objects: player (another type), an empty entry, two field Pokémon
    const uint64_t player = heap + 0x200, mon1 = heap + 0x300, mon2 = heap + 0x400;
    rt::store<uint64_t>(list, player);
    rt::store<uint64_t>(list + 8, 0);
    rt::store<uint64_t>(list + 16, mon1);
    rt::store<uint64_t>(list + 24, mon2);
    const uint64_t comp1 = heap + 0x500, comp2 = heap + 0x520;
    for (auto [obj, chain, comp] : {std::tuple{player, other_node, uint64_t{0}}, std::tuple{mon1, node, comp1},
                                    std::tuple{mon2, node, comp2}}) {
        rt::store<uint64_t>(obj + 0x30, type_vt);  // type_interface 0x30, its chain at +8
        rt::store<uint64_t>(obj + 0x38, chain);
        rt::store<uint64_t>(obj + 0x40, 0xabcd0000 + obj);  // zone hash
        rt::store<uint32_t>(obj + 0x48, 2);                 // area kind
        rt::store<uint8_t>(obj + 0x4c, 1);                  // touched byte
        rt::store<uint64_t>(obj + 0x50, comp);              // contact component, count at +4
        rt::store<uint8_t>(obj + 0x58, 0);                  // hidden byte
        if (comp != 0)
            rt::store<uint32_t>(comp + 4, 1);
    }
    const auto put_player = [&](float x, float z) {  // position at player + 0x60: x, y, z
        uint32_t bx, bz;
        std::memcpy(&bx, &x, 4);
        std::memcpy(&bz, &z, 4);
        rt::store<uint32_t>(player + 0x60, bx);
        rt::store<uint32_t>(player + 0x68, bz);
    };
    put_player(0, 0);
    const uint64_t handle = heap + 0x600, ref_vt = heap + 0x640;
    rt::store<uint64_t>(handle + 0x8, ref_vt);  // ref_interface 8
    rt::store<uint64_t>(ref_vt + 0x10, kRefInc);
    rt::store<uint64_t>(ref_vt + 0x18, kRefDec);
    g_re_handle = handle;

    const uint64_t doors[] = {kCheck, kCtor, kSpawner, kRateLookup, kDurationLookup, kModelSwitch, kEffectUpdate};
    rt::declare_hookable(doors, 7);
    const LiftedEntry fns[] = {{kType, &fake_type},
                               {kSetup, &fake_setup},
                               {kTouchFn, &fake_touch},
                               {kRefInc, &fake_ref_inc},
                               {kRefDec, &fake_ref_dec},
                               {kEmitterLookup, &fake_emitter_lookup}};
    rt::register_functions(fns, 6);

    const std::string base = "hook encounter_check 0x8020000\n"
                             "value seed 1\n"  // repeatable dice
                             "value check_result_encounter 9\n"
                             "value objects_holder 0x10\nvalue objects_manager 0x18\n"
                             "value objects_list 0x20\nvalue objects_count 0x28\n"
                             "value type_interface 0x30\nvalue field_pokemon_type_got " + std::to_string(got) + "\n"
                             "value encounter_setup 0x8023000\nvalue encounter_touch 0x8024000\n"
                             "value zone_hash_offset 0x40\nvalue area_kind_offset 0x48\n"
                             "value touched_byte_offset 0x4c\n"
                             "value ref_interface 8\nvalue ref_inc_slot 0x10\nvalue ref_dec_slot 0x18\n"
                             "value contact_component 0x50\nvalue contact_count 4\n";
    mods::StartOptions o;
    o.dir = dir;
    o.enabled = {"random-encounters"};
    std::string error;
    auto check = [&](CpuState* cpu) {
        *cpu = CpuState{};
        cpu->x[0] = 5;
        cpu->x[1] = context;
        cpu->x[19] = 19;
        cpu->x[30] = 0x0800'0040;
        cpu->sp = stack_top - 8;
        g_original_calls = 0;
        g_re_touch = {};
        rt::call_hooked(cpu, kCheck, &original);
    };
    auto spawn = [&](uint64_t obj) {
        CpuState cpu{};
        cpu.x[0] = obj;
        rt::call_hooked(&cpu, kCtor, &fake_ctor);
    };

    {  // a value missing: not started
        write(dir / "random-encounters.cfg", "hook encounter_check 0x8020000\n");
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active.empty());
        CHECK(!r.messages.empty() && r.messages[0].find("check_result_encounter") != std::string::npos);
        mods::stop();
    }
    {  // without spawn_ctor and steps: the die per frame picks any field Pokémon
        write(dir / "random-encounters.cfg", base + "value frame_rate 65536\n");
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active == std::vector<std::string>{"random-encounters"});
        CpuState cpu;
        check(&cpu);
        CHECK_EQ(g_original_calls, 0);
        CHECK_EQ(cpu.x[0], 9u);  // check_result_encounter
        CHECK_EQ(cpu.x[19], 19u);
        CHECK_EQ(cpu.sp, stack_top - 8);
        CHECK_EQ(cpu.halt, uint32_t{kHaltNone});
        CHECK(g_re_setup_obj == mon1 || g_re_setup_obj == mon2);
        CHECK_EQ(g_re_touch.calls, 1);
        CHECK_EQ(g_re_touch.x0, handle + 8 + 0x10);  // from ref_inc
        CHECK_EQ(g_re_touch.x1, context);
        CHECK_EQ(g_re_touch.handle, handle);
        CHECK_EQ(g_re_touch.zone, 0xabcd0000 + g_re_setup_obj);
        CHECK_EQ(g_re_touch.kind, 2u);
        CHECK_EQ(g_re_touch.x24, g_re_setup_obj);
        CHECK_EQ(g_re_refs, -1);  // one inc, two decs (the setup's reference too)
        CHECK_EQ(rt::load<uint8_t>(g_re_setup_obj + 0x4c), 0u);
        CHECK_EQ(rt::load<uint32_t>(comp1 + 4), 1u);  // nothing hidden: contact stays
        g_re_refs = 0;
        // switched off: the check as before
        CHECK(mods::set_active("random-encounters", false));
        check(&cpu);
        CHECK_EQ(g_original_calls, 1);
        CHECK_EQ(cpu.x[0], 6u);  // original: 5 + 1
        CHECK_EQ(g_re_touch.calls, 0);
        mods::stop();
    }
    {  // dry run: the die rolls, nothing starts
        write(dir / "random-encounters.cfg", base + "value frame_rate 65536\nvalue dry_run 1\n");
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CpuState cpu;
        check(&cpu);
        CHECK_EQ(g_original_calls, 1);
        CHECK_EQ(cpu.x[0], 6u);  // original: 5 + 1
        CHECK_EQ(g_re_touch.calls, 0);
        mods::stop();
    }
    {  // spawn_ctor hides spawns; the die rolls per step walked
        write(dir / "random-encounters.cfg", base + "hook spawn_ctor 0x8021000\nvalue hide_listed 0\nvalue step_rate 256\n"
                                                    "value player_position_offset 0x60\n"
                                                    "value step_length_milli 1000\n"
                                                    "value hidden_byte_offset 0x58\nvalue hide_contact 1\n"
                                                    "value return_gap_ms 50\n"
                                                    "value candidate_min_checks 2\n"
                                                    "value forget_on_return 0\n");
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active == std::vector<std::string>{"random-encounters"});
        CHECK_EQ(mods::random_encounter_rate(), 256u);  // the file's step_rate
        mods::set_random_encounter_rate(5);  // the menu's row wins
        CHECK_EQ(mods::random_encounter_rate(), 5u);
        mods::set_random_encounter_rate(300);
        CHECK_EQ(mods::random_encounter_rate(), 256u);
        mods::set_random_encounter_rate(0);  // back to the file's
        CHECK_EQ(mods::random_encounter_rate(), 256u);
        CHECK_EQ(mods::random_encounter_hidden_percent(), 100u);  // the file's (default)
        mods::set_random_encounter_hidden_percent(50);  // the menu's row "Sichtbare Pokémon" at "Mix"
        CHECK_EQ(mods::random_encounter_hidden_percent(), 50u);
        mods::set_random_encounter_hidden_percent(255);  // back to the file's
        CHECK_EQ(mods::random_encounter_hidden_percent(), 100u);
        spawn(mon1);
        CHECK_EQ(rt::load<uint8_t>(mon1 + 0x58), 1u);  // invisible right after the constructor
        CHECK_EQ(rt::load<uint32_t>(comp1 + 4), 1u);   // its contact goes once the check lists it
        rt::store<uint8_t>(mon1 + 0x58, 0);  // the game may reset it
        CpuState cpu;
        check(&cpu);  // standing: no step, but the hidden one is hidden again
        CHECK_EQ(g_original_calls, 1);
        CHECK_EQ(g_re_touch.calls, 0);
        CHECK_EQ(rt::load<uint32_t>(comp1 + 4), 0u);
        CHECK_EQ(rt::load<uint8_t>(mon1 + 0x58), 1u);
        CHECK_EQ(rt::load<uint32_t>(comp2 + 4), 1u);  // mon2 was not spawned by the ctor
        CHECK_EQ(rt::load<uint8_t>(mon2 + 0x58), 0u);
        put_player(0.6f, 0);
        check(&cpu);  // not yet a step
        CHECK_EQ(g_re_touch.calls, 0);
        spawn(mon2);  // both hidden now
        check(&cpu);
        CHECK_EQ(rt::load<uint32_t>(comp2 + 4), 0u);
        put_player(0.6f, 0.6f);
        check(&cpu);  // 1.2 walked: one step, step_rate 256 always hits; mon2 too young
        CHECK_EQ(g_re_touch.calls, 1);
        CHECK_EQ(g_re_setup_obj, mon1);
        CHECK_EQ(g_original_calls, 0);
        // the transition waits for field Pokémon without contact: all get it
        // back, the hidden byte stays
        CHECK_EQ(rt::load<uint32_t>(comp1 + 4), 1u);
        CHECK_EQ(rt::load<uint32_t>(comp2 + 4), 1u);
        CHECK_EQ(rt::load<uint8_t>(mon1 + 0x58), 1u);
        CHECK_EQ(rt::load<uint8_t>(mon2 + 0x58), 1u);
        put_player(0.6f, 2.6f);  // in the transition: no die, nothing hidden again
        check(&cpu);
        CHECK_EQ(g_original_calls, 1);
        CHECK_EQ(g_re_touch.calls, 0);
        CHECK_EQ(rt::load<uint32_t>(comp1 + 4), 1u);
        rt::store<uint8_t>(mon2 + 0x58, 0);  // a new object at mon2's address
        spawn(mon2);  // a spawn now stays visible: the game sets up the field anew
        CHECK_EQ(rt::load<uint32_t>(comp2 + 4), 1u);
        CHECK_EQ(rt::load<uint8_t>(mon2 + 0x58), 0u);
        std::this_thread::sleep_for(std::chrono::milliseconds(80));  // the check pauses (battle)
        put_player(100, 100);  // back in the field, elsewhere: no steps from the jump
        check(&cpu);
        CHECK_EQ(g_original_calls, 1);
        CHECK_EQ(g_re_touch.calls, 0);
        CHECK_EQ(rt::load<uint32_t>(comp1 + 4), 0u);  // hidden again
        CHECK_EQ(rt::load<uint32_t>(comp2 + 4), 1u);  // spawned in the transition: visible
        // the game rebuilds the field at the same addresses: ages start again
        put_player(100.6f, 100.6f);
        check(&cpu);  // a hit, but mon1 is too young now: it waits
        CHECK_EQ(g_re_touch.calls, 0);
        check(&cpu);  // old enough
        CHECK_EQ(g_re_touch.calls, 1);
        // switched off: the hidden ones get their contact back
        CHECK(mods::set_active("random-encounters", false));
        check(&cpu);
        CHECK_EQ(g_original_calls, 1);
        CHECK_EQ(rt::load<uint32_t>(comp2 + 4), 1u);
        CHECK_EQ(rt::load<uint32_t>(comp1 + 4), 1u);
        CHECK_EQ(rt::load<uint8_t>(mon2 + 0x58), 0u);
        CHECK_EQ(rt::load<uint8_t>(mon1 + 0x58), 0u);
        spawn(mon2);  // off: spawns stay visible
        CHECK(mods::set_active("random-encounters", true));
        check(&cpu);
        CHECK_EQ(rt::load<uint32_t>(comp2 + 4), 1u);
        mods::stop();
    }
    {  // forget_on_return (default): the rebuilt field is left alone
        write(dir / "random-encounters.cfg", base + "hook spawn_ctor 0x8021000\nvalue hide_listed 0\nvalue frame_rate 65536\n"
                                                    "value hidden_byte_offset 0x58\nvalue hide_contact 1\nvalue return_gap_ms 50\n"
                                                    "value candidate_min_checks 0\n");
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        rt::store<uint32_t>(comp1 + 4, 1);
        rt::store<uint32_t>(comp2 + 4, 1);
        rt::store<uint8_t>(mon1 + 0x58, 0);
        rt::store<uint8_t>(mon2 + 0x58, 0);
        spawn(mon1);
        CpuState cpu;
        check(&cpu);  // mon1 is the only hidden one: the die per frame takes it
        CHECK_EQ(g_re_touch.calls, 1);
        CHECK_EQ(g_re_setup_obj, mon1);
        std::this_thread::sleep_for(std::chrono::milliseconds(80));  // battle
        rt::store<uint32_t>(comp1 + 4, 0);  // the game rebuilds mon1 but keeps what the mod wrote
        rt::store<uint8_t>(mon1 + 0x58, 1);
        check(&cpu);  // back: mon1 is forgotten, contact and hidden byte undone, no candidate
        CHECK_EQ(g_re_touch.calls, 0);
        CHECK_EQ(g_original_calls, 1);
        CHECK_EQ(rt::load<uint32_t>(comp1 + 4), 1u);
        CHECK_EQ(rt::load<uint8_t>(mon1 + 0x58), 0u);
        rt::store<uint32_t>(comp1 + 4, 0);  // the game's own value now: left alone
        check(&cpu);
        CHECK_EQ(rt::load<uint32_t>(comp1 + 4), 0u);
        mods::stop();
        rt::store<uint32_t>(comp1 + 4, 1);
    }
    {  // the game's own touch: its transition waits for the hidden ones too
        std::string base6 = base;
        base6.replace(base6.find("check_result_encounter 9"), 24, "check_result_encounter 6");  // original: 5 + 1
        write(dir / "random-encounters.cfg", base6 + "hook spawn_ctor 0x8021000\nvalue hide_listed 0\nvalue hidden_byte_offset 0x58\nvalue hide_contact 1\n");
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        rt::store<uint8_t>(mon1 + 0x58, 0);
        spawn(mon1);
        CpuState cpu;
        check(&cpu);  // the mod hides mon1, then the original check reports a touch
        CHECK_EQ(g_original_calls, 1);
        CHECK_EQ(g_re_touch.calls, 0);
        CHECK_EQ(rt::load<uint32_t>(comp1 + 4), 1u);  // contact back
        CHECK_EQ(rt::load<uint8_t>(mon1 + 0x58), 1u);
        check(&cpu);  // in its transition: not hidden again
        CHECK_EQ(rt::load<uint32_t>(comp1 + 4), 1u);
        mods::stop();
        rt::store<uint8_t>(mon1 + 0x58, 0);
    }
    {  // default with the hidden byte: hidden by the byte only, the contact stays
        write(dir / "random-encounters.cfg", base + "hook spawn_ctor 0x8021000\nvalue hide_listed 0\nvalue hidden_byte_offset 0x58\n"
                                                    "value frame_rate 65536\nvalue candidate_min_checks 0\n");
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        rt::store<uint32_t>(comp1 + 4, 1);
        rt::store<uint8_t>(mon1 + 0x58, 0);
        spawn(mon1);
        CHECK_EQ(rt::load<uint8_t>(mon1 + 0x58), 1u);
        CpuState cpu;
        check(&cpu);  // hidden again and taken by the die per frame
        CHECK_EQ(rt::load<uint32_t>(comp1 + 4), 1u);
        CHECK_EQ(g_re_touch.calls, 1);
        CHECK_EQ(g_re_setup_obj, mon1);
        mods::stop();
        rt::store<uint8_t>(mon1 + 0x58, 0);
    }
    {  // switched off with a hidden one not listed: restored once it is listed
        write(dir / "random-encounters.cfg", base + "hook spawn_ctor 0x8021000\nvalue hide_listed 0\nvalue hidden_byte_offset 0x58\n"
                                                    "value hide_contact 1\n");
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        rt::store<uint64_t>(list + 24, 0);  // mon2 not listed yet
        spawn(mon2);
        CpuState cpu;
        check(&cpu);
        CHECK_EQ(rt::load<uint8_t>(mon2 + 0x58), 1u);
        rt::store<uint32_t>(comp2 + 4, 0);  // as with contact 0 from an earlier listing
        CHECK(mods::set_active("random-encounters", false));
        check(&cpu);  // off: mon2 is not there to restore
        CHECK_EQ(rt::load<uint8_t>(mon2 + 0x58), 1u);
        rt::store<uint64_t>(list + 24, mon2);
        check(&cpu);  // listed now: restored
        CHECK_EQ(g_original_calls, 1);
        CHECK_EQ(rt::load<uint8_t>(mon2 + 0x58), 0u);
        CHECK_EQ(rt::load<uint32_t>(comp2 + 4), 1u);
        rt::store<uint32_t>(comp2 + 4, 0);  // the game's own value now: left alone
        check(&cpu);
        CHECK_EQ(rt::load<uint32_t>(comp2 + 4), 0u);
        mods::stop();
        rt::store<uint32_t>(comp2 + 4, 1);
    }
    {  // hide_contact 2: contact 0 only while the game's check runs, nothing left at 0
        write(dir / "random-encounters.cfg", base + "hook spawn_ctor 0x8021000\nvalue hide_listed 0\n"
                                                    "value hidden_byte_offset 0x58\nvalue hide_contact 2\n");
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        g_check_comp1 = comp1;
        g_check_comp2 = comp2;
        spawn(mon1);  // hidden; mon2 stays visible
        const auto check_seeing = [&](CpuState* cpu) {
            *cpu = CpuState{};
            cpu->x[0] = 5;
            cpu->x[1] = context;
            cpu->x[30] = 0x0800'0040;
            cpu->sp = stack_top - 8;
            g_original_calls = 0;
            g_check_seen1 = g_check_seen2 = 7;
            rt::call_hooked(cpu, kCheck, &original_seeing_contact);
        };
        CpuState cpu;
        check_seeing(&cpu);
        CHECK_EQ(g_original_calls, 1);
        CHECK_EQ(g_check_seen1, 0u);  // the check skips the hidden one
        CHECK_EQ(g_check_seen2, 1u);  // a visible one keeps its contact
        CHECK_EQ(rt::load<uint32_t>(comp1 + 4), 1u);  // back right after the check
        CHECK_EQ(rt::load<uint32_t>(comp2 + 4), 1u);
        rt::store<uint32_t>(comp1 + 4, 3);  // another count of the game's: put back as it was
        check_seeing(&cpu);
        CHECK_EQ(g_check_seen1, 0u);
        CHECK_EQ(rt::load<uint32_t>(comp1 + 4), 3u);
        rt::store<uint32_t>(comp1 + 4, 1);
        CHECK(mods::set_active("random-encounters", false));
        check_seeing(&cpu);  // off: the check sees every contact
        CHECK_EQ(g_check_seen1, 1u);
        CHECK_EQ(rt::load<uint32_t>(comp1 + 4), 1u);
        mods::stop();
        rt::store<uint8_t>(mon1 + 0x58, 0);
    }
    {  // hiding only (Let's Go, the app's "Hide field Pokémon"): hidden, no die, touchable
        write(dir / "random-encounters.cfg", base + "hook spawn_ctor 0x8021000\nvalue hide_listed 0\n"
                                                    "value hidden_byte_offset 0x58\nvalue hide_contact 2\n"
                                                    "value step_rate 256\nvalue player_position_offset 0x60\n"
                                                    "value step_length_milli 1000\nvalue candidate_min_checks 0\n");
        mods::set_random_encounter_hide_only(true);
        CHECK(mods::random_encounter_hide_only());
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        g_check_comp1 = comp1;
        g_check_comp2 = comp2;
        put_player(0, 0);
        spawn(mon1);  // hidden as before
        CHECK_EQ(rt::load<uint8_t>(mon1 + 0x58), 1u);
        const auto check_seeing = [&](CpuState* cpu) {
            *cpu = CpuState{};
            cpu->x[0] = 5;
            cpu->x[1] = context;
            cpu->x[30] = 0x0800'0040;
            cpu->sp = stack_top - 8;
            g_original_calls = 0;
            g_re_touch = {};
            rt::call_hooked(cpu, kCheck, &original_seeing_contact);
        };
        CpuState cpu;
        check_seeing(&cpu);
        for (float x = 0.6f; x < 5; x += 0.6f) {  // steps that always hit at 256
            put_player(x, 0);
            check_seeing(&cpu);
            CHECK_EQ(g_re_touch.calls, 0);
        }
        CHECK_EQ(g_original_calls, 1);  // the game's check, no encounter of the mod
        CHECK_EQ(g_re_touch.calls, 0);
        CHECK_EQ(g_check_seen1, 1u);  // the hidden one stays touchable
        CHECK_EQ(rt::load<uint8_t>(mon1 + 0x58), 1u);  // and hidden
        mods::set_random_encounter_hide_only(false);  // Pokémon Gelb on again: the die
        put_player(5.4f, 0);
        check_seeing(&cpu);
        CHECK_EQ(g_re_touch.calls, 1);
        mods::stop();
        rt::store<uint8_t>(mon1 + 0x58, 0);
        rt::store<uint32_t>(comp1 + 4, 1);
        rt::store<uint32_t>(comp2 + 4, 1);
    }
    {  // hide_listed: field Pokémon no constructor decided get the die when listed
        write(dir / "random-encounters.cfg", base + "hook spawn_ctor 0x8021000\nvalue hidden_byte_offset 0x58\n"
                                                    "value hide_listed 1\n");
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        rt::store<uint8_t>(mon1 + 0x58, 0);
        rt::store<uint8_t>(mon2 + 0x58, 0);
        CpuState cpu;
        check(&cpu);  // both listed, none spawned while on: hidden by the byte, contact stays
        CHECK_EQ(g_original_calls, 1);
        CHECK_EQ(rt::load<uint8_t>(mon1 + 0x58), 1u);
        CHECK_EQ(rt::load<uint8_t>(mon2 + 0x58), 1u);
        CHECK_EQ(rt::load<uint32_t>(comp1 + 4), 1u);
        CHECK_EQ(rt::load<uint32_t>(comp2 + 4), 1u);
        CHECK(mods::set_active("random-encounters", false));
        check(&cpu);  // off: shown again
        CHECK_EQ(rt::load<uint8_t>(mon1 + 0x58), 0u);
        CHECK_EQ(rt::load<uint8_t>(mon2 + 0x58), 0u);
        check(&cpu);  // still off
        // on again (PC tog2): the ones shown while off get the die again when listed
        CHECK(mods::set_active("random-encounters", true));
        check(&cpu);
        CHECK_EQ(rt::load<uint8_t>(mon1 + 0x58), 1u);
        CHECK_EQ(rt::load<uint8_t>(mon2 + 0x58), 1u);
        mods::stop();
        rt::store<uint8_t>(mon1 + 0x58, 0);
        rt::store<uint8_t>(mon2 + 0x58, 0);
        // model_visibility: the game computes the byte anew each pass, so the
        // mod covers the hidden ones at the model switch instead
        const auto model = [&](uint64_t obj) {
            CpuState m{};
            m.x[0] = obj;
            m.x[1] = 0;  // the game: not covered
            m.x[2] = 1;  // in sight
            g_model_x1 = ~uint64_t{0};
            rt::call_hooked(&m, kModelSwitch, &fake_model_switch);
            return g_model_x1;
        };
        write(dir / "random-encounters.cfg", base + "hook spawn_ctor 0x8021000\nvalue hidden_byte_offset 0x58\n"
                                                    "value hide_listed 1\nhook model_visibility 0x802d000\n");
        r = {};
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active == std::vector<std::string>{"random-encounters"});
        check(&cpu);  // both listed and hidden, the byte left to the game
        CHECK_EQ(rt::load<uint8_t>(mon1 + 0x58), 0u);
        CHECK_EQ(rt::load<uint8_t>(mon2 + 0x58), 0u);
        CHECK_EQ(model(mon1), 1u);
        CHECK_EQ(model(mon2), 1u);
        CHECK_EQ(model(player), 0u);  // not a hidden one: the game's value
        CHECK(mods::set_active("random-encounters", false));
        check(&cpu);
        CHECK_EQ(model(mon1), 0u);  // let go: the game's model comes back
        CHECK(mods::set_active("random-encounters", true));
        check(&cpu);
        CHECK_EQ(model(mon1), 1u);
        mods::stop();
        // effect_visibility: the aura of a hidden one off after the game's
        // update, the shadow's byte set; shown ones keep the game's
        const auto effects = [&](uint64_t obj) {
            CpuState m{};
            m.x[0] = obj;
            m.sp = stack_top - 8;
            rt::call_hooked(&m, kEffectUpdate, &fake_effect_update);
            return m.sp == stack_top - 8 && m.x[0] == obj;  // registers as the game left them
        };
        rt::store<uint64_t>(mon1 + 0x90, mon1 + 0xa0);  // the aura's handle
        rt::store<uint64_t>(mon2 + 0x90, 0);            // no aura
        rt::store<uint8_t>(mon1 + 0xc0, 0);
        write(dir / "random-encounters.cfg",
              base + "hook spawn_ctor 0x8021000\nvalue hidden_byte_offset 0x58\nvalue hide_listed 1\n"
                     "hook model_visibility 0x802d000\nhook effect_visibility 0x8030000\n"
                     "value emitter_lookup 0x8031000\nvalue emitter_on_offset 0xb\n"
                     "expr emitter_1 0 x0>0x90\nexpr shadow_off 0 x0+0xc0\nvalue list_log 1\n");
        r = {};
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active == std::vector<std::string>{"random-encounters"});
        CHECK(effects(mon1));
        CHECK_EQ(rt::load<uint8_t>(mon1 + 0xab), 1u);  // not decided yet: the game's
        check(&cpu);  // both hidden
        CHECK(effects(mon1));
        CHECK_EQ(rt::load<uint8_t>(mon1 + 0xab), 0u);
        CHECK_EQ(rt::load<uint8_t>(mon1 + 0xc0), 1u);
        CHECK(effects(mon2));  // hidden, no aura: nothing to turn off
        CHECK_EQ(model(mon1), 1u);
        CHECK(mods::set_active("random-encounters", false));
        check(&cpu);
        CHECK(effects(mon1));
        CHECK_EQ(rt::load<uint8_t>(mon1 + 0xab), 1u);  // let go
        mods::stop();
        // the emitters need the model switch and the lookup
        write(dir / "random-encounters.cfg", base + "hook spawn_ctor 0x8021000\nhook effect_visibility 0x8030000\n"
                                                    "value emitter_lookup 0x8031000\nvalue emitter_on_offset 0xb\n");
        r = {};
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active.empty());
        CHECK(!r.messages.empty() && r.messages[0].find("effect_visibility needs hook model_visibility") != std::string::npos);
        mods::stop();
        // without spawn_ctor: refused
        write(dir / "random-encounters.cfg", base + "value hidden_byte_offset 0x58\nhook model_visibility 0x802d000\n");
        r = {};
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active.empty());
        CHECK(!r.messages.empty() && r.messages[0].find("model_visibility needs hook spawn_ctor") != std::string::npos);
        mods::stop();
        // hidden_percent 0: listed ones are let be, and stay so
        write(dir / "random-encounters.cfg", base + "hook spawn_ctor 0x8021000\nvalue hidden_byte_offset 0x58\n"
                                                    "value hide_listed 1\nvalue hidden_percent 0\n");
        CHECK(mods::start(o, &r, &error));
        check(&cpu);
        check(&cpu);
        CHECK_EQ(rt::load<uint8_t>(mon1 + 0x58), 0u);
        CHECK_EQ(rt::load<uint8_t>(mon2 + 0x58), 0u);
        mods::stop();
        // the menu's "An" (0 %) while some are hidden: they are shown again once
        // listed, and new ones stay visible
        write(dir / "random-encounters.cfg", base + "hook spawn_ctor 0x8021000\nvalue hidden_byte_offset 0x58\n"
                                                    "value hide_listed 1\n");
        CHECK(mods::start(o, &r, &error));
        check(&cpu);  // all of them hidden (the file's 100 %)
        CHECK_EQ(rt::load<uint8_t>(mon1 + 0x58), 1u);
        CHECK_EQ(rt::load<uint8_t>(mon2 + 0x58), 1u);
        mods::set_random_encounter_hidden_percent(0);
        check(&cpu);
        CHECK_EQ(rt::load<uint8_t>(mon1 + 0x58), 0u);
        CHECK_EQ(rt::load<uint8_t>(mon2 + 0x58), 0u);
        CHECK_EQ(rt::load<uint32_t>(comp1 + 4), 1u);
        CHECK_EQ(rt::load<uint32_t>(comp2 + 4), 1u);
        spawn(mon1);
        check(&cpu);
        CHECK_EQ(rt::load<uint8_t>(mon1 + 0x58), 0u);
        mods::set_random_encounter_hidden_percent(255);
        mods::stop();
        // list_log: a line per check here (the census only logs)
        write(dir / "random-encounters.cfg", base + "hook spawn_ctor 0x8021000\nvalue hide_listed 0\nvalue hidden_byte_offset 0x58\n"
                                                    "value list_log 1\n");
        CHECK(mods::start(o, &r, &error));
        spawn(mon1);
        check(&cpu);
        check(&cpu);
        CHECK_EQ(g_original_calls, 1);
        mods::stop();
        rt::store<uint8_t>(mon1 + 0x58, 0);
        // list_log with areas and spawn points: wrong offsets or a pointer into nothing
        // give zeros, not a fault (PC Zufall-4s crashed on one)
        {
            const std::string area_node = std::to_string(node + 0x100);  // the player's chain stands in
            write(dir / "random-encounters.cfg",
                  base + "value list_log 1\nvalue uncount_zone_offset 0x80\n"
                         "value area_type " + area_node + "\nvalue area_zone_offset 0x900000\n"
                         "value area_limit_offset 0x904\nvalue area_data_offset 0x8f8\n"
                         "value point_area_type " + area_node + "\nvalue points_offset 0x470\n"
                         "value points_count_offset 0x478\nvalue point_used_offset 0x60\n"
                         "value point_area_zone_offset 0x3c0\n");
            rt::store<uint64_t>(player + 0x470, 0xdead'0000'0000);
            rt::store<uint64_t>(player + 0x478, 5);
            r = {};
            CHECK(mods::start(o, &r, &error));
            CHECK(r.active == std::vector<std::string>{"random-encounters"});
            check(&cpu);
            mods::stop();
            rt::store<uint64_t>(player + 0x470, 0);
            rt::store<uint64_t>(player + 0x478, 0);
        }
        // default: hide_listed on
        write(dir / "random-encounters.cfg", base + "hook spawn_ctor 0x8021000\nvalue hidden_byte_offset 0x58\n");
        CHECK(mods::start(o, &r, &error));
        check(&cpu);
        CHECK_EQ(rt::load<uint8_t>(mon1 + 0x58), 1u);
        CHECK_EQ(rt::load<uint8_t>(mon2 + 0x58), 1u);
        mods::stop();
        rt::store<uint8_t>(mon1 + 0x58, 0);
        rt::store<uint8_t>(mon2 + 0x58, 0);
        // hide_listed 0: the listed ones stay as the game made them
        write(dir / "random-encounters.cfg", base + "hook spawn_ctor 0x8021000\nvalue hide_listed 0\nvalue hidden_byte_offset 0x58\n");
        CHECK(mods::start(o, &r, &error));
        check(&cpu);
        CHECK_EQ(rt::load<uint8_t>(mon1 + 0x58), 0u);
        CHECK_EQ(rt::load<uint8_t>(mon2 + 0x58), 0u);
        mods::stop();
    }
    {  // hide_listed after a battle: the rebuilt field is decided again
        write(dir / "random-encounters.cfg", base + "hook spawn_ctor 0x8021000\nvalue hidden_byte_offset 0x58\n"
                                                    "value hide_listed 1\nvalue frame_rate 65536\n"
                                                    "value return_gap_ms 50\nvalue candidate_min_checks 0\n");
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        rt::store<uint8_t>(mon1 + 0x58, 0);
        rt::store<uint8_t>(mon2 + 0x58, 0);
        CpuState cpu;
        check(&cpu);  // both hidden on listing and ready at once: the die takes one
        CHECK_EQ(g_re_touch.calls, 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(80));  // battle
        rt::store<uint8_t>(mon1 + 0x58, 0);  // the game rebuilds the field, visible
        rt::store<uint8_t>(mon2 + 0x58, 0);
        check(&cpu);  // back: forgotten, then decided again and hidden
        CHECK_EQ(rt::load<uint8_t>(mon1 + 0x58), 1u);
        CHECK_EQ(rt::load<uint8_t>(mon2 + 0x58), 1u);
        CHECK_EQ(rt::load<uint32_t>(comp1 + 4), 1u);
        CHECK_EQ(rt::load<uint32_t>(comp2 + 4), 1u);
        mods::stop();
        rt::store<uint8_t>(mon1 + 0x58, 0);
        rt::store<uint8_t>(mon2 + 0x58, 0);
    }
    {  // spawner: the hidden ones do not count against the area's limit while some are visible
        write(dir / "random-encounters.cfg", base + "hook spawn_ctor 0x8021000\nvalue hidden_byte_offset 0x58\n"
                                                    "hook spawner 0x8027000\nvalue uncount_zone_offset 0x80\n");
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active == std::vector<std::string>{"random-encounters"});
        rt::store<uint64_t>(mon1, 0x1234);  // the field Pokémon's vtable
        rt::store<uint64_t>(mon1 + 0x80, 0x5a5a);  // its zone
        spawn(mon1);
        g_spawner_obj = mon1;
        auto spawner = [&] {
            CpuState cpu{};
            cpu.sp = stack_top - 8;
            rt::call_hooked(&cpu, kSpawner, &fake_spawner);
        };
        spawner();  // all hidden (100 %): counted as the game does
        CHECK_EQ(g_spawner_zone, 0x5a5au);
        mods::set_random_encounter_hidden_percent(50);
        spawner();  // some visible: the hidden one is left out, then its zone is back
        CHECK_EQ(g_spawner_zone, ~uint64_t{0});
        CHECK_EQ(rt::load<uint64_t>(mon1 + 0x80), 0x5a5au);
        {  // many destroyed hidden ones the mod still keeps do not use up the limit of live ones (64)
            const uint64_t many = heap + 0x10000;
            CHECK(memory.map(many, 0x20000, rt::MemState::Normal, rt::kPermRW).ok());
            mods::set_random_encounter_hidden_percent(100);  // all of them hidden
            for (uint64_t i = 0; i < 270; ++i) {
                const uint64_t obj = many + i * 0x100;
                rt::store<uint64_t>(obj, 0x1234);
                rt::store<uint64_t>(obj + 0x80, 0x77);
                spawn(obj);
                if (i >= 70)
                    rt::store<uint64_t>(obj, 0x9999);
            }
            mods::set_random_encounter_hidden_percent(50);
            g_spawner_scan = many;
            g_spawner_scan_n = 70;
            spawner();
            CHECK_EQ(g_spawner_left_out + (g_spawner_zone == ~uint64_t{0} ? 1u : 0u), 64u);  // with mon1
            CHECK_EQ(rt::load<uint64_t>(many + 0x80), 0x77u);
            g_spawner_scan_n = 0;
        }
        rt::store<uint64_t>(mon1, 0x9999);  // destroyed: not written
        spawner();
        CHECK_EQ(g_spawner_zone, 0x5a5au);
        mods::set_random_encounter_hidden_percent(255);
        mods::stop();
        write(dir / "random-encounters.cfg", base + "hook spawner 0x8027000\n");  // without the offset
        r = {};
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active.empty());
        mods::stop();
        write(dir / "random-encounters.cfg", base + "value area_type 0x1000\n");  // without its offsets
        r = {};
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active.empty());
        CHECK(!r.messages.empty() && r.messages[0].find("area_zone_offset") != std::string::npos);
        mods::stop();
        rt::store<uint64_t>(mon1, 0);
        rt::store<uint64_t>(mon1 + 0x80, 0);
        rt::store<uint8_t>(mon1 + 0x58, 0);
    }
    {  // duration_lookup: the pause after a spawn times the visible share, at least 1
        write(dir / "random-encounters.cfg", base + "hook duration_lookup 0x8029000\nvalue duration_offset 0x40\n");
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active == std::vector<std::string>{"random-encounters"});
        const uint64_t spawner_obj = heap + 0x700;
        auto pause = [&](uint32_t in) {
            g_duration = in;
            CpuState cpu{};
            cpu.sp = stack_top - 8;
            cpu.x[0] = spawner_obj;
            rt::call_hooked(&cpu, kDurationLookup, &fake_duration_lookup);
            return rt::load<uint32_t>(spawner_obj + 0x40);
        };
        CHECK_EQ(pause(150), 150u);  // all hidden: unchanged
        mods::set_random_encounter_hidden_percent(50);
        CHECK_EQ(pause(150), 75u);
        CHECK_EQ(pause(1), 1u);
        CHECK_EQ(pause(0), 0u);
        mods::set_random_encounter_hidden_percent(30);
        CHECK_EQ(pause(150), 105u);
        mods::set_random_encounter_hidden_percent(255);
        mods::stop();
        rt::store<uint32_t>(spawner_obj + 0x40, 0);
        write(dir / "random-encounters.cfg", base + "hook duration_lookup 0x8029000\n");  // without the offset
        r = {};
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active.empty());
        mods::stop();
    }
    {  // rate_lookup: divided by the visible share, at most 101
        write(dir / "random-encounters.cfg", base + "hook rate_lookup 0x8028000\n");
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active == std::vector<std::string>{"random-encounters"});
        auto rate = [&](uint64_t in) {
            g_rate = in;
            CpuState cpu{};
            cpu.sp = stack_top - 8;
            rt::call_hooked(&cpu, kRateLookup, &fake_rate_lookup);
            return cpu.x[0];
        };
        CHECK_EQ(rate(2), 2u);  // all hidden (the file's 100 %): nothing to make up
        mods::set_random_encounter_hidden_percent(50);
        CHECK_EQ(rate(2), 4u);
        CHECK_EQ(rate(10), 20u);
        CHECK_EQ(rate(0), 0u);  // no spawns in this zone stays so
        CHECK_EQ(rate(60), 101u);
        mods::set_random_encounter_hidden_percent(30);
        CHECK_EQ(rate(2), 3u);  // 2 / 0.7, rounded up
        mods::set_random_encounter_hidden_percent(0);
        CHECK_EQ(rate(10), 10u);
        mods::set_random_encounter_hidden_percent(255);
        mods::stop();
    }
    {  // far_log (diagnosis): reads positions only, needs a position offset
        write(dir / "random-encounters.cfg", base + "value far_log_milli 50000\n");  // no offset
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active.empty());
        CHECK(!r.messages.empty() && r.messages[0].find("object_position_offset") != std::string::npos);
        mods::stop();
        write(dir / "random-encounters.cfg", base + "hook spawn_ctor 0x8021000\nvalue hidden_byte_offset 0x58\n"
                                                    "value far_log_milli 50000\nvalue object_position_offset 0x60\n"
                                                    "value list_log 1\n");
        r = {};
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active == std::vector<std::string>{"random-encounters"});
        mods::set_random_encounter_hidden_percent(0);  // all shown
        const auto put = [&](uint64_t obj, float x, float z) {
            uint32_t bx, bz;
            std::memcpy(&bx, &x, 4);
            std::memcpy(&bz, &z, 4);
            rt::store<uint32_t>(obj + 0x60, bx);
            rt::store<uint32_t>(obj + 0x68, bz);
        };
        put(mon1, 10, 10);
        put(mon2, 20, 20);
        CpuState cpu;
        check(&cpu);  // first listed here
        put(mon1, 200, 10);  // far: one line
        check(&cpu);
        check(&cpu);  // not again
        CHECK_EQ(g_original_calls, 1);
        CHECK_EQ(rt::load<uint8_t>(mon1 + 0x58), 0u);  // nothing written
        CHECK_EQ(rt::load<uint8_t>(mon2 + 0x58), 0u);
        CHECK_EQ(rt::load<uint32_t>(comp1 + 4), 1u);
        mods::set_random_encounter_hidden_percent(255);
        mods::stop();
        put(mon1, 0, 0);
        put(mon2, 0, 0);
    }
    {  // zone_rate: "Original" takes the rate of the zone of the field Pokémon nearest to the player
        write(dir / "random-encounters.cfg", base + "value player_position_offset 0x60\nzone_rate route_1 0xa 256\n");
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));  // no uncount_zone_offset: no zone to read
        CHECK(r.active.empty());
        CHECK(!r.messages.empty() && r.messages[0].find("uncount_zone_offset") != std::string::npos);
        mods::stop();
        write(dir / "random-encounters.cfg", base + "value player_position_offset 0x60\nvalue uncount_zone_offset 0x80\n"
                                                    "zone_rate route_1 0xa 300\n");
        r = {};
        CHECK(mods::start(o, &r, &error));  // above 256
        CHECK(r.active.empty());
        mods::stop();
        write(dir / "random-encounters.cfg", base + "hook spawn_ctor 0x8021000\nvalue hidden_byte_offset 0x58\n"
                                                    "value step_rate 0\nvalue player_position_offset 0x60\n"
                                                    "value step_length_milli 1000\nvalue warp_milli 10000\n"
                                                    "value candidate_min_checks 0\nvalue uncount_zone_offset 0x80\n"
                                                    "value uncount_zone_size 4\nzone_rate route_1 0xa 256\n");
        r = {};
        CHECK(mods::start(o, &r, &error));  // a 4-byte zone without hook spawner
        CHECK(r.active == std::vector<std::string>{"random-encounters"});
        const auto put = [&](uint64_t obj, float x, float z) {
            uint32_t bx, bz;
            std::memcpy(&bx, &x, 4);
            std::memcpy(&bz, &z, 4);
            rt::store<uint32_t>(obj + 0x60, bx);
            rt::store<uint32_t>(obj + 0x68, bz);
        };
        rt::store<uint64_t>(mon1 + 0x80, 0x12345678'0000000aull);  // Route 1: 256 of 256, other bytes above the 4
        rt::store<uint64_t>(mon2 + 0x80, 0xb);  // no zone_rate: the file's step_rate 0
        put(mon1, 0, 0);
        put(mon2, 100, 100);
        CpuState cpu;
        put_player(0, 1);
        check(&cpu);  // both hidden on listing
        put_player(0, 3);
        check(&cpu);  // walked, but without "Original" the file's 0
        CHECK_EQ(g_re_touch.calls, 0);
        mods::set_random_encounter_rate(mods::kRandomEncounterOriginal);
        CHECK_EQ(mods::random_encounter_rate(), mods::kRandomEncounterOriginal);
        put_player(100, 103);  // a jump next to mon2
        check(&cpu);
        put_player(100, 101);
        check(&cpu);  // walked in zone 0xb: no rate there
        CHECK_EQ(g_re_touch.calls, 0);
        put_player(0, 13);  // a jump back next to mon1
        check(&cpu);
        put_player(0, 11);
        check(&cpu);  // walked in zone 0xa: 256 of 256
        CHECK_EQ(g_re_touch.calls, 1);
        mods::set_random_encounter_rate(0);
        mods::stop();
        put(mon1, 0, 0);
        put(mon2, 0, 0);
        put_player(0, 0);
        for (uint64_t obj : {mon1, mon2}) {
            rt::store<uint8_t>(obj + 0x58, 0);
            rt::store<uint64_t>(obj + 0x80, 0);
        }
        rt::store<uint32_t>(comp1 + 4, 1);
        rt::store<uint32_t>(comp2 + 4, 1);
    }
    {  // grass_range: steps count and hidden ones attack only near a hidden one's spawn point
        write(dir / "random-encounters.cfg", base + "value grass_range_milli 5000\n");  // no player position
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active.empty());
        CHECK(!r.messages.empty() && r.messages[0].find("player_position_offset") != std::string::npos);
        mods::stop();
        write(dir / "random-encounters.cfg", base + "hook spawn_ctor 0x8021000\nvalue hidden_byte_offset 0x58\n"
                                                    "value step_rate 256\nvalue player_position_offset 0x60\n"
                                                    "value step_length_milli 1000\nvalue warp_milli 10000\n"
                                                    "value candidate_min_checks 0\nvalue grass_range_milli 5000\n");
        r = {};
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active == std::vector<std::string>{"random-encounters"});
        const auto put = [&](uint64_t obj, float x, float z) {
            uint32_t bx, bz;
            std::memcpy(&bx, &x, 4);
            std::memcpy(&bz, &z, 4);
            rt::store<uint32_t>(obj + 0x60, bx);
            rt::store<uint32_t>(obj + 0x68, bz);
        };
        put(mon1, 100, 100);  // first listed in the grass
        put(mon2, 101, 100);
        CpuState cpu;
        put_player(0, 0);
        check(&cpu);  // both hidden on listing
        CHECK_EQ(rt::load<uint8_t>(mon1 + 0x58), 1u);
        put(mon1, 2, 0);  // a hidden one wandered onto the path, next to the player
        for (int i = 1; i <= 4; ++i) {  // walking on the path: no steps, no attack
            put_player(2.0f * static_cast<float>(i), 0);
            check(&cpu);
        }
        CHECK_EQ(g_re_touch.calls, 0);
        put_player(100, 103);  // into the grass: steps count again
        check(&cpu);  // a jump, not walked
        put_player(100, 101);
        check(&cpu);
        CHECK_EQ(g_re_touch.calls, 1);
        CHECK(g_re_setup_obj == mon1 || g_re_setup_obj == mon2);  // first listed near, wherever it is now
        mods::stop();
        // after a battle the game rebuilds the field at the same addresses, elsewhere: their new place counts
        write(dir / "random-encounters.cfg", base + "hook spawn_ctor 0x8021000\nvalue hidden_byte_offset 0x58\n"
                                                    "value step_rate 256\nvalue player_position_offset 0x60\n"
                                                    "value step_length_milli 1000\nvalue warp_milli 10000\n"
                                                    "value candidate_min_checks 0\nvalue grass_range_milli 5000\n"
                                                    "value return_gap_ms 50\n");
        CHECK(mods::start(o, &r, &error));
        for (uint64_t obj : {mon1, mon2})
            rt::store<uint8_t>(obj + 0x58, 0);
        put(mon1, 100, 100);
        put(mon2, 101, 100);
        put_player(100, 101);
        check(&cpu);
        put_player(100, 103);
        check(&cpu);  // walked in the grass: the battle
        CHECK_EQ(g_re_touch.calls, 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        put(mon1, 300, 300);  // rebuilt in another patch, without the constructor
        put(mon2, 301, 300);
        put_player(100, 101);
        check(&cpu);  // back in the field
        put_player(100, 103);
        check(&cpu);  // the old patch: no attack
        CHECK_EQ(g_re_touch.calls, 0);
        put_player(300, 301);
        check(&cpu);
        put_player(300, 303);
        check(&cpu);  // the new patch
        CHECK_EQ(g_re_touch.calls, 1);
        mods::stop();
        put(mon1, 0, 0);
        put(mon2, 0, 0);
        put_player(0, 0);
        for (uint64_t obj : {mon1, mon2})
            rt::store<uint8_t>(obj + 0x58, 0);
        rt::store<uint32_t>(comp1 + 4, 1);
        rt::store<uint32_t>(comp2 + 4, 1);
    }
    {  // the player by its type and an active field, not by index
        const uint64_t player_got = heap + 0xf0;
        rt::store<uint64_t>(player_got, other_node);
        rt::store<uint32_t>(player + 0x70, 1);  // not active (yet)
        write(dir / "random-encounters.cfg", base + "value step_rate 256\nvalue player_position_offset 0x60\n"
                                                    "value step_length_milli 1000\nvalue player_index 1\n"
                                                    "value player_type_got " + std::to_string(player_got) + "\n"
                                                    "value player_active_offset 0x70\n");
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active == std::vector<std::string>{"random-encounters"});
        CpuState cpu;
        put_player(0, 0);
        check(&cpu);
        put_player(0.9f, 0);
        check(&cpu);
        put_player(1.8f, 0);
        check(&cpu);  // no active player: no steps
        CHECK_EQ(g_re_touch.calls, 0);
        rt::store<uint32_t>(player + 0x70, 0);
        check(&cpu);  // first reading of this player
        CHECK_EQ(g_re_touch.calls, 0);
        put_player(2.7f, 0);
        check(&cpu);
        put_player(3.6f, 0);
        check(&cpu);  // 1.8 walked: a step
        CHECK_EQ(g_re_touch.calls, 1);
        mods::stop();
    }
    fs::remove(dir / "random-encounters.cfg");  // later tests start live mods in standby from this folder
    rt::reset_dispatch();
}

// Gelb mode (Paul 2026-11-01): random-encounters rolls per step inside a spawn
// area that has slot lines and starts the battle through wild-battles with the
// slot's species and level, without a field Pokémon.
constexpr uint64_t kInside = 0x0802'a000;
int g_inside_calls = 0;
void fake_inside(CpuState* ctx) {  // the area's test: the byte at area + 0x10
    ++g_inside_calls;
    ctx->x[0] = rt::load<uint8_t>(ctx->x[0] + 0x10);
    ctx->x[19] = 0x1919;
}
constexpr uint64_t kActive = 0x0802'b000;
void fake_active(CpuState* ctx) { ctx->x[0] = 0x100 | rt::load<uint8_t>(ctx->x[0] + 0x11); }  // bit 0 only
constexpr uint64_t kOtherFn = 0x0802'c000;
void fake_other_fn(CpuState* ctx) {  // another class's function at that slot: it jumps into nowhere
    ctx->halt = kHaltBadJump;
    ctx->halt_info = 0xffff'ffff'ffff'ffb8;
}
int g_spawner_runs = 0;
void fake_spawner_count(CpuState*) { ++g_spawner_runs; }

void gelb_mode(const fs::path& dir) {
    rt::GuestMemory memory(rt::Layout::packed(0x0800'0000, 0x10'0000, 0x10'0000, 0x10'0000, 0x10'0000, 0x40'0000));
    const uint64_t heap = memory.layout().heap.base;
    CHECK(memory.map(heap, 4 * rt::kPageSize, rt::MemState::Normal, rt::kPermRW).ok());
    const uint64_t stack_top = heap + 4 * rt::kPageSize;
    // check context -> c0 -> holder -> manager -> list: player, area 1, area 2, a field Pokémon,
    // a field object of another class (PC Zufall-10: its function at the area
    // test's slot jumps into nowhere) and a broken area without a function there
    const uint64_t context = heap, c0 = heap + 0x10, holder = heap + 0x40, mgr = heap + 0x60, list = heap + 0x90;
    rt::store<uint64_t>(context, c0);
    rt::store<uint64_t>(c0 + 0x10, holder);
    rt::store<uint64_t>(holder + 0x18, mgr);
    rt::store<uint64_t>(mgr + 0x20, list);
    rt::store<uint64_t>(mgr + 0x28, 6);
    const uint64_t type_vt = heap + 0xc0, got = heap + 0xc8, node = heap + 0xd0;
    const uint64_t player_chain = heap + 0xe0, area_chain = heap + 0xf0, area_node = heap + 0x1f0;
    rt::store<uint64_t>(type_vt, kType);
    rt::store<uint64_t>(got, node);
    rt::store<uint64_t>(player_chain, node + 0x100);  // not a field Pokémon
    rt::store<uint64_t>(node + 0x100, 0);
    // types: a spawn area's type derives from the area node, whose parent is
    // the base of all field objects (its first word, Ghidra Zufall-10)
    const uint64_t base_node = heap + 0x1e0;
    rt::store<uint64_t>(area_chain, area_node);
    rt::store<uint64_t>(area_node, base_node);
    rt::store<uint64_t>(base_node, 0);
    const uint64_t player = heap + 0x200, area1 = heap + 0x600, area2 = heap + 0xa00, mon = heap + 0xe00;
    const uint64_t area_vt = heap + 0x1000;  // the areas' vtable, the test at +0xf8
    rt::store<uint64_t>(area_vt + 0xf8, kInside);
    rt::store<uint64_t>(list, player);
    rt::store<uint64_t>(list + 8, area1);
    rt::store<uint64_t>(list + 16, area2);
    rt::store<uint64_t>(list + 24, mon);
    const uint64_t other = heap + 0x1700, other_vt = heap + 0x1800, broken = heap + 0x1900;
    const uint64_t broken_vt = heap + 0x1a00;
    rt::store<uint64_t>(list + 32, other);
    rt::store<uint64_t>(list + 40, broken);
    rt::store<uint64_t>(other, other_vt);
    rt::store<uint64_t>(other_vt + 0xf8, kOtherFn);
    rt::store<uint64_t>(broken, broken_vt);
    rt::store<uint64_t>(broken_vt + 0xf8, 0xffff'ffff'ffff'ffb8);  // past the end of its vtable
    for (uint64_t a : {other, broken}) {
        rt::store<uint8_t>(a + 0x10, 1);
        rt::store<uint8_t>(a + 0x11, 1);
        rt::store<uint64_t>(a + 0x3c0, 0xa1);
    }
    for (auto [obj, chain] : {std::pair{player, player_chain}, std::pair{area1, area_chain},
                              std::pair{area2, area_chain}, std::pair{mon, node}, std::pair{other, base_node},
                              std::pair{broken, area_chain}}) {
        rt::store<uint64_t>(obj + 0x30, type_vt);
        rt::store<uint64_t>(obj + 0x38, chain);
    }
    for (uint64_t a : {area1, area2})
        rt::store<uint64_t>(a, area_vt);
    rt::store<uint64_t>(area_vt + 0x58, kActive);
    rt::store<uint64_t>(area1 + 0x3c0, 0xa1);  // Route 1's zone, slots below
    rt::store<uint64_t>(area2 + 0x3c0, 0xb2);  // a zone without slots
    const uint64_t comp = heap + 0x1100;
    rt::store<uint64_t>(mon + 0x50, comp);
    rt::store<uint32_t>(comp + 4, 1);
    const auto put_player = [&](float x, float z) {
        uint32_t bx, bz;
        std::memcpy(&bx, &x, 4);
        std::memcpy(&bz, &z, 4);
        rt::store<uint32_t>(player + 0x60, bx);
        rt::store<uint32_t>(player + 0x68, bz);
    };
    // wild-battles: the creator leaves the handle, [handle + 0x60] = event + 0x50
    const uint64_t global = heap + 0x1200, managers = heap + 0x1280;
    rt::store<uint64_t>(global, managers);
    rt::store<uint64_t>(managers + 8, 0x1111);
    rt::store<uint64_t>(managers + 0x10, 0x2222);
    const uint64_t event = heap + 0x1400, handle = heap + 0x1480, templ = heap + 0x1500;
    for (unsigned i = 0; i < 0x20; ++i)
        rt::store<uint8_t>(templ + i, static_cast<uint8_t>(0xA0 + i));
    g_template = templ;

    const uint64_t doors[] = {kCheck, kSpawner, kTouch, kFill, kLookup, kCtor, kModelSwitch};
    rt::declare_hookable(doors, 7);
    const LiftedEntry fns[] = {{kType, &fake_type},      {kInside, &fake_inside}, {kCreate, &fake_create},
                               {kGetter, &fake_getter},  {kSetup, &fake_setup},   {kTouchFn, &fake_touch},
                               {kRefInc, &fake_ref_inc}, {kRefDec, &fake_ref_dec}, {kActive, &fake_active},
                               {kOtherFn, &fake_other_fn}};
    rt::register_functions(fns, 10);

    const std::string g = std::to_string(global);
    const std::string wild = "hook encounter_touch 0x8001000\nvalue redirect 3\n"
                             "hook record_fill 0x8005000\nhook static_battle_lookup 0x8006000\n"
                             "value template_hash 0x77\nhook event_create 0x8008000\n"
                             "addr start_x0 " + g + " >0>8\naddr start_x1 " + g + " >0+0x10>0\n"
                             "addr event_from_create 0 >0x60-0x50\n"
                             "value event_1_offset 0x10\nvalue event_1 0x77\nvalue event_1_size 8\n"
                             "value object_load 1\n"
                             "value record_1_offset 4\ncall record_1 0x8007000 obj+0x80\n"
                             "value record_2_offset 9\nfrom record_2 1 x2>0+0x84\n"
                             "value record_3_offset 0x10\nvalue record_3 0xffff\nvalue record_3_size 2\n";
    const std::string re = "hook encounter_check 0x8020000\nvalue seed 2\nvalue check_result_encounter 9\n"
                           "value objects_holder 0x10\nvalue objects_manager 0x18\n"
                           "value objects_list 0x20\nvalue objects_count 0x28\n"
                           "value type_interface 0x30\nvalue field_pokemon_type_got " + std::to_string(got) + "\n"
                           "value encounter_setup 0x8023000\nvalue encounter_touch 0x8024000\n"
                           "value zone_hash_offset 0x40\nvalue area_kind_offset 0x48\n"
                           "value touched_byte_offset 0x4c\n"
                           "value ref_interface 8\nvalue ref_inc_slot 0x10\nvalue ref_dec_slot 0x18\n"
                           "value contact_component 0x50\nvalue contact_count 4\n"
                           "value player_position_offset 0x60\nvalue step_length_milli 1000\n"
                           "value warp_milli 10000\nvalue candidate_min_checks 0\nvalue step_rate 0\n"
                           "value area_type " + std::to_string(area_node) + "\nvalue area_zone_offset 0x3c0\n"
                           "value area_limit_offset 0x3d0\n";
    mods::StartOptions o;
    o.dir = dir;
    o.enabled = {"random-encounters", "wild-battles"};
    std::string error;
    auto check = [&](CpuState* cpu) {
        *cpu = CpuState{};
        cpu->x[0] = 5;
        cpu->x[1] = context;
        cpu->x[19] = 19;
        cpu->sp = stack_top - 8;
        g_original_calls = 0;
        g_create_calls = 0;
        g_re_touch = {};
        rt::store<uint64_t>(handle + 0x60, event + 0x50);
        rt::call_hooked(cpu, kCheck, &original);
    };
    auto lookup = [&]() {
        CpuState l{};
        l.x[0] = 0x77;
        rt::call_hooked(&l, kLookup, &fake_lookup);
        return l.x[0];
    };

    {  // slot lines need the area test; a bad slot; direct needs constants in wild-battles
        write(dir / "wild-battles.cfg", wild + "value direct_species 1\nvalue direct_level 2\n");
        write(dir / "random-encounters.cfg", re + "slot route_1 0xa1 1 25 5\n");
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active == std::vector<std::string>{"wild-battles"});
        CHECK(!r.messages.empty() && r.messages[0].find("area_inside_slot") != std::string::npos);
        mods::stop();
        write(dir / "random-encounters.cfg", re + "value area_inside_slot 0xf8\nslot route_1 0xa1 1 25 0\n");
        r = {};
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active == std::vector<std::string>{"wild-battles"});  // level 0
        mods::stop();
        write(dir / "random-encounters.cfg", re + "value area_inside_slot 0xf8\nslot route_1 0xa1 1 25 5-4\n");
        r = {};
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active == std::vector<std::string>{"wild-battles"});  // a range backwards
        mods::stop();
        write(dir / "random-encounters.cfg", re + "value area_inside_slot 0xf8\nslot route_1 0xa1 1 25 5\n");
        write(dir / "wild-battles.cfg", wild + "value direct_species 1\n");  // record_2 reads the touched one
        r = {};
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active == std::vector<std::string>{"random-encounters"});
        CHECK(!r.messages.empty() && r.messages[0].find("direct_record_K") != std::string::npos);
        mods::stop();
        write(dir / "wild-battles.cfg", wild + "value direct_species 9\nvalue direct_level 2\n");
        r = {};
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active == std::vector<std::string>{"random-encounters"});  // no record_9
        mods::stop();
        write(dir / "wild-battles.cfg", wild + "value direct_species 1\nvalue direct_level 2\n");
        write(dir / "random-encounters.cfg", re + "value spawns_off 1\nvalue area_inside_slot 0xf8\n"
                                                  "slot route_1 0xa1 1 25 5\n");  // spawns_off without the spawner
        r = {};
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active == std::vector<std::string>{"wild-battles"});
        mods::stop();
    }
    {  // steps count only inside an area with slots; a hit is the slot's battle, no field Pokémon touched
        g_created = handle;
        write(dir / "wild-battles.cfg",
              wild + "value direct_species 1\nvalue direct_level 2\nvalue direct_record_2 0\nvalue direct_record_3 5\n");
        write(dir / "random-encounters.cfg",
              re + "value area_inside_slot 0xf8\nzone_rate route_1 0xa1 256\nvalue uncount_zone_offset 0x80\n"
                   "hook spawner 0x8027000\nvalue spawns_off 1\n"
                   "value transition_checks 1\nvalue hidden_byte_offset 0x58\nhook spawn_ctor 0x8021000\n"
                   "slot route_1 0xa1 3 16 3\nslot route_1 0xa1 1 19 4-6 2\n");
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active.size() == 2u);
        CpuState cpu;
        // Not "Original": the die as before (no hidden one here, no battle), the spawner runs
        rt::store<uint8_t>(area1 + 0x10, 1);
        put_player(0, 0);
        check(&cpu);
        put_player(0, 3);
        check(&cpu);
        CHECK_EQ(g_create_calls, 0);
        g_spawner_runs = 0;
        CpuState sp{};
        rt::call_hooked(&sp, kSpawner, &fake_spawner_count);
        CHECK_EQ(g_spawner_runs, 1);

        mods::set_random_encounter_rate(mods::kRandomEncounterOriginal);
        rt::call_hooked(&sp, kSpawner, &fake_spawner_count);
        CHECK_EQ(g_spawner_runs, 1);  // Gelb mode: no new field Pokémon
        rt::store<uint8_t>(area1 + 0x10, 0);
        rt::store<uint8_t>(area2 + 0x10, 1);  // inside an area without slots: no steps
        put_player(0, 0);
        check(&cpu);
        put_player(0, 5);
        check(&cpu);
        CHECK_EQ(g_create_calls, 0);
        CHECK_EQ(g_original_calls, 1);
        CHECK_EQ(cpu.x[19], 19u);
        rt::store<uint8_t>(area1 + 0x10, 1);  // into Route 1's grass: 256 of 256 per step
        put_player(0, 7);
        check(&cpu);
        CHECK_EQ(g_create_calls, 1);
        CHECK_EQ(g_re_touch.calls, 0);  // no field Pokémon touched
        CHECK_EQ(g_original_calls, 1);  // the check runs on as the game's
        CHECK_EQ(cpu.x[0], 6u);
        CHECK_EQ(cpu.x[19], 19u);
        CHECK_EQ(cpu.sp, stack_top - 8);
        CHECK_EQ(g_create_x0, 0x1111u);
        CHECK_EQ(rt::load<uint64_t>(event + 0x10), 0x77u);
        CHECK_EQ(lookup(), templ);  // the template gets the slot's species and level
        const uint32_t species = rt::load<uint32_t>(templ + 4);
        const uint8_t level = rt::load<uint8_t>(templ + 9);
        CHECK((species == 16 && level == 3) || (species == 19 && level >= 4 && level <= 6));
        CHECK_EQ(rt::load<uint16_t>(templ + 0x10), 5u);  // direct_record_3 instead of 0xffff
        CHECK_EQ(rt::load<uint8_t>(mon + 0x58), 1u);  // the field Pokémon on screen is hidden
        rt::store<uint8_t>(templ + 4, 0xA4);
        put_player(0, 9);
        check(&cpu);  // the transition ends here (transition_checks 1), no step since
        CHECK_EQ(g_create_calls, 0);
        // wild-battles switched off: no battle, the check as the game's
        CHECK(mods::set_active("wild-battles", false));
        put_player(0, 12);
        check(&cpu);
        CHECK_EQ(g_create_calls, 0);
        CHECK_EQ(g_original_calls, 1);
        CHECK(mods::set_active("wild-battles", true));
        // the slots' weights, 3 to 1
        unsigned sixteen = 0, nineteen = 0;
        bool levels_19[7] = {};
        float z = 12;
        for (int i = 0; i < 400; ++i) {
            z += 1;
            put_player(0, z);
            check(&cpu);
            CHECK_EQ(g_create_calls, 1);
            CHECK_EQ(lookup(), templ);
            const uint32_t sp_now = rt::load<uint32_t>(templ + 4);
            sixteen += sp_now == 16;
            nineteen += sp_now == 19;
            if (sp_now == 19) {
                const uint8_t lv = rt::load<uint8_t>(templ + 9);
                CHECK(lv >= 4 && lv <= 6);
                levels_19[lv <= 6 ? lv : 0] = true;
            }
            CpuState f{};
            f.x[1] = templ;
            rt::call_hooked(&f, kFill, &fake_fill);  // the template gets its bytes back
            check(&cpu);  // the transition ends
        }
        CHECK_EQ(sixteen + nineteen, 400u);
        CHECK(sixteen > 250 && sixteen < 350);
        CHECK(levels_19[4] && levels_19[5] && levels_19[6]);  // the range is rolled, not its minimum
        mods::set_random_encounter_rate(0);
        mods::stop();
    }
    {  // Gelb's battle: the hidden field Pokémon stay covered through its transition
       // (Paul, iPhone 02.11.: they showed for about a second as the battle began)
        g_created = handle;
        write(dir / "wild-battles.cfg",
              wild + "value direct_species 1\nvalue direct_level 2\nvalue direct_record_2 0\nvalue direct_record_3 5\n");
        write(dir / "random-encounters.cfg",
              re + "value area_inside_slot 0xf8\nzone_rate route_1 0xa1 256\nvalue uncount_zone_offset 0x80\n"
                   "value hidden_byte_offset 0x58\nhook spawn_ctor 0x8021000\nvalue hide_listed 1\n"
                   "hook model_visibility 0x802d000\nslot route_1 0xa1 1 16 3\n");
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active.size() == 2u);
        const auto model = [&](uint64_t obj) {
            CpuState m{};
            m.x[0] = obj;
            m.x[1] = 0;  // the game: not covered
            m.x[2] = 1;  // in sight
            g_model_x1 = ~uint64_t{0};
            rt::call_hooked(&m, kModelSwitch, &fake_model_switch);
            return g_model_x1;
        };
        mods::set_random_encounter_rate(mods::kRandomEncounterOriginal);
        rt::store<uint8_t>(mon + 0x58, 0);
        rt::store<uint8_t>(area1 + 0x10, 1);
        rt::store<uint8_t>(area2 + 0x10, 0);
        CpuState cpu;
        put_player(0, 100);
        check(&cpu);
        CHECK_EQ(g_create_calls, 0);
        CHECK_EQ(model(mon), 1u);  // listed and covered
        put_player(0, 103);
        check(&cpu);
        CHECK_EQ(g_create_calls, 1);  // Gelb's battle, its transition runs
        CHECK_EQ(model(mon), 1u);     // still covered
        CHECK_EQ(rt::load<uint8_t>(mon + 0x58), 0u);  // the byte left to the game
        CHECK(mods::set_active("random-encounters", false));
        CHECK_EQ(model(mon), 0u);  // off: the game's model
        mods::set_random_encounter_rate(0);
        mods::stop();
    }
    {  // the area's active test first, as the spawner: an inactive area counts no steps
        write(dir / "random-encounters.cfg",
              re + "value area_inside_slot 0xf8\nvalue area_active_slot 0x58\nzone_rate route_1 0xa1 256\n"
                   "value uncount_zone_offset 0x80\nvalue transition_checks 1\nslot route_1 0xa1 1 16 3\n");
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active.size() == 2u);
        mods::set_random_encounter_rate(mods::kRandomEncounterOriginal);
        CpuState cpu;
        rt::store<uint8_t>(area1 + 0x10, 1);
        rt::store<uint8_t>(area1 + 0x11, 0);
        for (float z : {40.0f, 43.0f, 46.0f}) {
            put_player(0, z);
            check(&cpu);
            CHECK_EQ(cpu.halt, uint32_t{kHaltNone});
            CHECK_EQ(g_create_calls, 0);
            CHECK_EQ(g_original_calls, 1);
        }
        rt::store<uint8_t>(area1 + 0x11, 1);
        put_player(0, 49);
        check(&cpu);
        CHECK_EQ(g_create_calls, 1);
        CHECK_EQ(lookup(), templ);
        CHECK_EQ(rt::load<uint32_t>(templ + 4), 16u);
        mods::set_random_encounter_rate(0);
        mods::stop();
    }
    {  // only on the grass: the player's grass byte, a cave zone counts everywhere in its area
        g_created = handle;
        const std::string grass = re + "value area_inside_slot 0xf8\nvalue player_grass_offset 0x70\n"
                                       "zone_rate route_1 0xa1 256\nvalue uncount_zone_offset 0x80\n"
                                       "value transition_checks 1\nslot route_1 0xa1 1 16 3\n";
        write(dir / "random-encounters.cfg", grass);
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active.size() == 2u);
        mods::set_random_encounter_rate(mods::kRandomEncounterOriginal);
        CpuState cpu;
        rt::store<uint8_t>(area1 + 0x10, 1);
        rt::store<uint8_t>(player + 0x70, 0);  // inside the area's trigger, on the path
        for (float z : {60.0f, 63.0f, 66.0f}) {
            put_player(0, z);
            check(&cpu);
            CHECK_EQ(g_create_calls, 0);
        }
        rt::store<uint8_t>(player + 0x70, 1);  // the step into the grass counts, as in Gelb
        put_player(0, 67);
        check(&cpu);
        CHECK_EQ(g_create_calls, 1);
        check(&cpu);  // the transition ends
        mods::set_random_encounter_rate(0);
        mods::stop();
        // a cave: every step inside the area
        write(dir / "random-encounters.cfg", grass + "cave route_1 0xa1\n");
        r = {};
        CHECK(mods::start(o, &r, &error));
        mods::set_random_encounter_rate(mods::kRandomEncounterOriginal);
        rt::store<uint8_t>(player + 0x70, 0);
        put_player(0, 80);
        check(&cpu);
        put_player(0, 83);
        check(&cpu);
        CHECK_EQ(g_create_calls, 1);
        check(&cpu);  // the transition ends
        // no manager (a scene change): the areas of the last field are not asked again
        rt::store<uint64_t>(holder + 0x18, 0);
        g_inside_calls = 0;
        for (float z : {90.0f, 93.0f}) {
            put_player(0, z);
            check(&cpu);
            CHECK_EQ(cpu.halt, uint32_t{kHaltNone});
            CHECK_EQ(g_create_calls, 0);
        }
        CHECK_EQ(g_inside_calls, 0);  // the objects of the last field may be gone
        rt::store<uint64_t>(holder + 0x18, mgr);
        mods::set_random_encounter_rate(0);
        mods::stop();
        write(dir / "random-encounters.cfg", re + "value player_grass_offset 0x70\n");  // without Gelb mode
        r = {};
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active == std::vector<std::string>{"wild-battles"});
        mods::stop();
    }
    fs::remove(dir / "random-encounters.cfg");
    fs::remove(dir / "wild-battles.cfg");
    rt::reset_dispatch();
}

// Data mods: RomFS files from <dir>/<id>/romfs/, no .cfg, no hooks.
constexpr uint64_t kExpFormula = 0x0801'6000;
constexpr uint64_t kExpCaught = 0x0801'7000;

void fake_formula(CpuState* ctx) {  // the per-member entry: amount at x1 + 0, flags at + 4 (fought 1, bonus 2)
    rt::store<uint32_t>(ctx->x[1], 999);
    rt::store<uint8_t>(ctx->x[1] + 4, 3);
    ctx->x[0] = ctx->x[1] = 0;
}

uint64_t g_formula_in = 0, g_formula_out = 0;
void fake_caught(CpuState* ctx) {  // the server's catch experience: runs the formula once
    CpuState c{};
    c.x[0] = g_formula_in;
    c.x[1] = g_formula_out;
    rt::call_hooked(&c, kExpFormula, &fake_formula);
    ctx->x[0] = 0;
}

void classic_exp(const fs::path& dir) {
    const mods::ModInfo* info = mods::find_mod("classic-exp");
    CHECK(info != nullptr && info->live && info->ready && !info->data);
    rt::GuestMemory memory(rt::Layout::packed(0x0800'0000, 0x10'0000, 0x10'0000, 0x10'0000, 0x10'0000, 0x40'0000));
    const uint64_t heap = memory.layout().heap.base;
    CHECK(memory.map(heap, rt::kPageSize, rt::MemState::Normal, rt::kPermRW).ok());
    const uint64_t in = heap, out = heap + 0x100;
    g_formula_in = in;
    g_formula_out = out;
    const uint64_t doors[] = {kExpFormula, kCatchExp, kExpCaught};
    rt::declare_hookable(doors, 3);

    const std::string cfg = "hook exp_formula 0x8016000\nvalue exp_in_reg 0\nvalue exp_out_reg 1\n"
                            "value exp_base_offset 0\nvalue exp_base_size 2\nvalue exp_level_offset 2\n"
                            "value exp_level_size 1\nvalue exp_kind_offset 4\nvalue exp_kind_size 1\n"
                            "value exp_ot_offset 5\nvalue exp_ot_size 1\nvalue exp_fighters_offset 6\n"
                            "value exp_fighters_size 1\nvalue exp_fought_offset 7\nvalue exp_fought_size 1\n"
                            "value exp_amount_offset 0\nvalue exp_bonus_offset 4\nvalue exp_bonus_size 1\n"
                            "value exp_bonus_mask 2\n";
    mods::StartOptions o;
    o.dir = dir;
    o.enabled = {"classic-exp"};
    std::string error;
    {  // without the catch scene's door: refused, nothing left
        write(dir / "classic-exp.cfg", cfg);
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active.empty());
        CHECK(!r.messages.empty() && r.messages[0].find("catch_exp_amount") != std::string::npos);
        CHECK_EQ(rt::hook_count(), 0u);
    }
    {  // a field missing
        write(dir / "classic-exp.cfg", "hook catch_exp_amount 0x8015000\nhook exp_formula 0x8016000\n"
                                       "value exp_in_reg 0\nvalue exp_out_reg 1\n");
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active.empty());
        CHECK(!r.messages.empty() && r.messages[0].find("exp_base_offset") != std::string::npos);
    }
    write(dir / "classic-exp.cfg", cfg + "hook catch_exp_amount 0x8015000\nhook exp_caught_door 0x8017000\n");
    mods::StartResult r;
    CHECK(mods::start(o, &r, &error));
    CHECK(r.active == std::vector<std::string>{"classic-exp"});
    CHECK(mods::active("classic-exp"));
    auto member = [&](uint16_t base, uint8_t level, uint8_t kind, bool own, uint8_t count, bool fought,
                      bool caught = false) {  // caught: inside the server's catch experience
        rt::store<uint16_t>(in, base);
        rt::store<uint8_t>(in + 2, level);
        rt::store<uint8_t>(in + 4, kind);
        rt::store<uint8_t>(in + 5, own);
        rt::store<uint8_t>(in + 6, count);
        rt::store<uint8_t>(in + 7, fought);
        CpuState c{};
        c.x[0] = in;
        c.x[1] = out;
        if (caught)
            rt::call_hooked(&c, kExpCaught, &fake_caught);
        else
            rt::call_hooked(&c, kExpFormula, &fake_formula);
        return rt::load<uint32_t>(out);
    };
    auto catch_exp = [&]() {
        CpuState p{};
        rt::call_hooked(&p, kCatchExp, &fake_catch_exp);
        return p.x[0];
    };
    CHECK_EQ(member(50, 10, 0, true, 2, true), 35u);  // floor(500 / 7) = 71, two fought
    CHECK_EQ(rt::load<uint8_t>(out + 4), 1u);       // bonus cleared, fought kept
    CHECK_EQ(member(50, 10, 1, false, 2, true), 78u);  // traded and a trainer: 35 × 3/2 × 3/2
    CHECK_EQ(member(50, 10, 0, true, 0, true), 71u);   // count 0 counts as 1
    CHECK_EQ(member(50, 10, 0, true, 2, false), 0u);   // did not fight
    CHECK_EQ(member(50, 10, 0, true, 1, true, true), 0u);  // caught in the battle
    CHECK_EQ(member(50, 10, 0, true, 1, true), 71u);        // and after it as before
    CHECK_EQ(member(1, 1, 0, true, 3, true), 1u);          // at least 1
    CHECK_EQ(catch_exp(), 0u);
    // Switched off in the menu: the game's own amounts.
    CHECK(mods::set_active("classic-exp", false));
    CHECK_EQ(member(50, 10, 0, true, 2, true), 999u);
    CHECK_EQ(rt::load<uint8_t>(out + 4), 3u);
    CHECK_EQ(catch_exp(), 500u);
    CHECK_EQ(member(50, 10, 0, true, 1, true, true), 999u);
    mods::stop();

    {  // exp_trainer_kind: only that kind is a trainer battle
        write(dir / "classic-exp.cfg", cfg + "hook catch_exp_amount 0x8015000\nvalue exp_trainer_kind 3\n");
        r = {};
        CHECK(mods::start(o, &r, &error));
        CHECK_EQ(r.active.size(), 1u);
        CHECK_EQ(member(50, 10, 1, true, 1, true), 71u);
        CHECK_EQ(member(50, 10, 3, true, 1, true), 106u);
        mods::stop();
    }
    {  // standby (live): installed, off until the menu
        o.enabled.clear();
        o.standby = true;
        r = {};
        CHECK(mods::start(o, &r, &error));
        CHECK(r.standby == std::vector<std::string>{"classic-exp"});
        CHECK_EQ(member(50, 10, 0, true, 2, true), 999u);
        CHECK(mods::set_active("classic-exp", true));
        CHECK_EQ(member(50, 10, 0, true, 2, true), 35u);
        mods::stop();
        CHECK_EQ(catch_exp(), 500u);  // no hook after stop
    }
}

bool g_route_called = false;
bool g_route_server = false;
void record_route(bool server) {
    g_route_called = true;
    g_route_server = server;
}

void move_immediates() {
    unsigned rd = 0;
    uint64_t v = 0;
    CHECK(rt::decode_move_immediate(0x320007E8u, &rd, &v) && rd == 8 && v == 3);         // mov w8, #3 (bitmask)
    CHECK(rt::decode_move_immediate(0x321F03E8u, &rd, &v) && rd == 8 && v == 2);         // mov w8, #2 (bitmask)
    CHECK(rt::decode_move_immediate(0x52800049u, &rd, &v) && rd == 9 && v == 2);         // movz w9, #2
    CHECK(rt::decode_move_immediate(0x12800000u, &rd, &v) && v == 0xFFFF'FFFFu);         // movn w0, #0
    CHECK(rt::decode_move_immediate(0x92800000u, &rd, &v) && v == ~uint64_t{0});         // movn x0, #0
    CHECK(rt::decode_move_immediate(0xD2A00020u, &rd, &v) && v == 0x10000u);             // movz x0, #1, lsl #16
    CHECK(rt::decode_move_immediate(0xB200C3E0u, &rd, &v) && v == 0x0101'0101'0101'0101u);  // mov x0, #0x0101…
    CHECK(rt::decode_move_immediate(0x32003FE1u, &rd, &v) && rd == 1 && v == 0xFFFFu);   // mov w1, #0xffff
    CHECK(!rt::decode_move_immediate(0x72800000u, &rd, &v));  // movk
    CHECK(!rt::decode_move_immediate(0x32000421u, &rd, &v));  // orr w1, w1, #3 (not from wzr)
    CHECK(!rt::decode_move_immediate(0xD503201Fu, &rd, &v));  // nop
}

void online_button(const fs::path& dir) {
    const mods::ModInfo* info = mods::find_mod("online-button");
    CHECK(info != nullptr && info->ready && !info->live && !info->data && info->hidden);
    rt::GuestMemory memory(rt::Layout::packed(0x0800'0000, 0x10'0000, 0x10'0000, 0x10'0000, 0x10'0000, 0x40'0000));
    const uint64_t heap = memory.layout().heap.base;
    CHECK(memory.map(heap, rt::kPageSize, rt::MemState::Normal, rt::kPermRW).ok());
    const uint64_t far_state = heap, far_flag = heap + 4, near_state = heap + 8;
    rt::store<uint32_t>(far_state, 0x320007E8u);   // mov w8, #3
    rt::store<uint32_t>(far_flag, 0x320003E9u);    // mov w9, #1
    rt::store<uint32_t>(near_state, 0x52800048u);  // movz w8, #2
    auto pass = [](uint64_t site, unsigned reg, bool replaced = true) {
        CpuState t{};
        t.x[reg] = 0xdead'0000'0000'0000;
        t.pc = site + 4;
        CHECK_EQ(rt::run_patch(&t, site), replaced);
        CHECK_EQ(t.halt, uint32_t{kHaltNone});
        return t.x[reg];
    };
    mods::set_ldn_route_handler(&record_route);
    mods::StartOptions o;
    o.dir = dir;
    o.enabled = {"online-button"};
    std::string error;
    {  // without far_flag: refused, nothing installed
        write(dir / "online-button.cfg", "patch far_state " + std::to_string(far_state) + "\n");
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active.empty());
        CHECK(!r.messages.empty() && r.messages[0].find("far_flag") != std::string::npos);
        CHECK_EQ(rt::patch_count(), 0u);
    }
    write(dir / "online-button.cfg", "patch far_state " + std::to_string(far_state) + "\npatch far_flag " +
                                         std::to_string(far_flag) + "\npatch near_state " +
                                         std::to_string(near_state) + "\n");
    mods::StartResult r;
    CHECK(mods::start(o, &r, &error));
    CHECK(r.active == std::vector<std::string>{"online-button"});
    CHECK_EQ(rt::patch_count(), 3u);
    // Far away: the nearby state and flag, the session to the server.
    CHECK_EQ(pass(far_state, 8), 2u);
    CHECK(g_route_called && g_route_server);
    CHECK_EQ(pass(far_flag, 9), 0u);
    // Nearby: observed (the instruction still runs), back to the LAN.
    g_route_called = false;
    pass(near_state, 8, false);
    CHECK(g_route_called && !g_route_server);
    mods::stop();
    {  // other values, without near_state
        write(dir / "online-button.cfg", "patch far_state " + std::to_string(far_state) + "\npatch far_flag " +
                                             std::to_string(far_flag) + "\nvalue far_state 5\nvalue far_flag 7\n");
        r = {};
        CHECK(mods::start(o, &r, &error));
        CHECK_EQ(rt::patch_count(), 2u);
        CHECK_EQ(pass(far_state, 8), 5u);
        CHECK_EQ(pass(far_flag, 9), 7u);
        mods::stop();
    }
    {  // not named: not installed (not live, no standby)
        o.enabled.clear();
        o.standby = true;
        r = {};
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active.empty());
        CHECK(std::find(r.standby.begin(), r.standby.end(), "online-button") == r.standby.end());
        CHECK(!mods::enabled("online-button") && !rt::detail::patch_installed(far_state));
        mods::stop();
    }
    {  // a site that is no MOV halts the guest
        write(dir / "online-button.cfg", "patch far_state " + std::to_string(far_flag + 8) + "\npatch far_flag " +
                                             std::to_string(far_flag) + "\n");
        rt::store<uint32_t>(far_flag + 8, 0xD503201Fu);  // nop
        o.enabled = {"online-button"};
        r = {};
        CHECK(mods::start(o, &r, &error));
        CpuState t{};
        t.pc = far_flag + 12;
        CHECK(rt::run_patch(&t, far_flag + 8));
        CHECK_EQ(t.halt, uint32_t{kHaltUndefined});
        mods::stop();
    }
    mods::set_ldn_route_handler(nullptr);
}

// skip-controller-screen: the boot's controller check gets mode 0 (the quiet
// check) instead of the game's; switched off, the game's own mode.
void skip_controller_screen(const fs::path& dir) {
    const mods::ModInfo* info = mods::find_mod("skip-controller-screen");
    CHECK(info != nullptr && info->ready && !info->live && !info->data && info->hidden);
    rt::GuestMemory memory(rt::Layout::packed(0x0800'0000, 0x10'0000, 0x10'0000, 0x10'0000, 0x10'0000, 0x40'0000));
    const uint64_t heap = memory.layout().heap.base;
    CHECK(memory.map(heap, rt::kPageSize, rt::MemState::Normal, rt::kPermRW).ok());
    const uint64_t site = heap;
    rt::store<uint32_t>(site, 0x52800028u);  // movz w8, #1
    auto pass = [&] {
        CpuState t{};
        t.x[8] = 0xdead'0000'0000'0000;
        t.pc = site + 4;
        CHECK(rt::run_patch(&t, site));
        CHECK_EQ(t.halt, uint32_t{kHaltNone});
        return t.x[8];
    };
    mods::StartOptions o;
    o.dir = dir;
    o.enabled = {"skip-controller-screen"};
    std::string error;
    {  // without the line: refused, nothing installed
        write(dir / "skip-controller-screen.cfg", "value boot_mode 0\n");
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active.empty());
        CHECK(!r.messages.empty() && r.messages[0].find("boot_mode") != std::string::npos);
        CHECK_EQ(rt::patch_count(), 0u);
    }
    write(dir / "skip-controller-screen.cfg", "patch boot_mode " + std::to_string(site) + "\n");
    mods::StartResult r;
    CHECK(mods::start(o, &r, &error));
    CHECK(r.active == std::vector<std::string>{"skip-controller-screen"});
    CHECK_EQ(pass(), 0u);
    mods::stop();
    {  // another mode
        write(dir / "skip-controller-screen.cfg", "patch boot_mode " + std::to_string(site) + "\nvalue boot_mode 3\n");
        r = {};
        CHECK(mods::start(o, &r, &error));
        CHECK_EQ(pass(), 3u);
        mods::stop();
    }
}

// audio-free-guard: a free count that wrapped (more buffers queued than the
// output holds) reads 0 at the site, any other value passes unchanged; the
// instruction itself still runs (an observing patch).
void audio_free_guard(const fs::path& dir) {
    const mods::ModInfo* info = mods::find_mod("audio-free-guard");
    CHECK(info != nullptr && info->ready && !info->live && !info->data && info->hidden);
    const uint64_t site = 0x0800'1234'0000;
    auto pass = [&](uint64_t w8) {
        CpuState t{};
        t.x[8] = w8;
        t.pc = site + 4;
        CHECK(!rt::run_patch(&t, site));  // observing: the STR still runs
        CHECK_EQ(t.halt, uint32_t{kHaltNone});
        return t.x[8];
    };
    mods::StartOptions o;
    o.dir = dir;
    o.enabled = {"audio-free-guard"};
    std::string error;
    {  // without the line: refused, nothing installed
        write(dir / "audio-free-guard.cfg", "value free_reg 8\n");
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active.empty());
        CHECK(!r.messages.empty() && r.messages[0].find("patch free") != std::string::npos);
        CHECK_EQ(rt::patch_count(), 0u);
    }
    {  // a register that does not exist
        write(dir / "audio-free-guard.cfg", "patch free " + std::to_string(site) + "\nvalue free_reg 31\n");
        mods::StartResult r;
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active.empty());
        CHECK_EQ(rt::patch_count(), 0u);
    }
    write(dir / "audio-free-guard.cfg", "patch free " + std::to_string(site) + "\n");
    mods::StartResult r;
    CHECK(mods::start(o, &r, &error));
    CHECK(r.active == std::vector<std::string>{"audio-free-guard"});
    CHECK_EQ(pass(0xffff'fffau), 0u);                // 4 - 10: wrapped
    CHECK_EQ(pass(0x1234'5678'ffff'fffau), 0u);      // upper half of x8 is not part of w8
    CHECK_EQ(pass(0), 0u);
    CHECK_EQ(pass(3), 3u);                           // normal counts pass
    CHECK_EQ(pass(4), 4u);
    CHECK(!mods::set_active("audio-free-guard", false));  // not live: always on while started
    mods::stop();
    {  // another register
        write(dir / "audio-free-guard.cfg", "patch free " + std::to_string(site) + "\nvalue free_reg 9\n");
        r = {};
        CHECK(mods::start(o, &r, &error));
        CpuState t{};
        t.x[8] = 0xffff'fffau;
        t.x[9] = 0xffff'fff0u;
        t.pc = site + 4;
        rt::run_patch(&t, site);
        CHECK_EQ(t.x[8], 0xffff'fffau);
        CHECK_EQ(t.x[9], 0u);
        mods::stop();
    }
}

void data_mods(const fs::path& dir) {
    const mods::ModInfo* info = mods::find_mod("original-encounters");
    CHECK(info != nullptr && info->data && !info->live);

    mods::StartOptions o;
    o.dir = dir;
    o.enabled = {"original-encounters"};
    o.standby = true;
    mods::StartResult r;
    std::string error;
    CHECK(mods::start(o, &r, &error));
    CHECK(r.active.empty());  // no folder yet
    CHECK(!r.messages.empty() && r.messages[0].find("original-encounters: not started") == 0);

    fs::create_directories(dir / "original-encounters" / "romfs" / "bin" / "field");
    write(dir / "original-encounters" / "romfs" / "bin" / "field" / "table.bin", "yellow");
    write(dir / "original-encounters" / "romfs" / "top.bin", "top");
    fs::create_directories(dir / "other" / "romfs" / "bin" / "field");
    write(dir / "other" / "romfs" / "bin" / "field" / "table.bin", "other");
    r = {};
    CHECK(mods::start(o, &r, &error));
    CHECK(r.active == std::vector<std::string>{"original-encounters"});
    CHECK(mods::enabled("original-encounters"));
    CHECK(!mods::set_active("original-encounters", false));  // not live
    // Not named: not started, even with standby.
    o.enabled.clear();
    r = {};
    CHECK(mods::start(o, &r, &error));
    CHECK(r.active.empty() && r.standby.empty());
    mods::stop();

    std::vector<std::string> notes;
    auto files = mods::romfs_files(dir, {"original-encounters"}, &notes);
    CHECK_EQ(files.size(), 2u);
    CHECK(files["/bin/field/table.bin"] == std::vector<uint8_t>({'y', 'e', 'l', 'l', 'o', 'w'}));
    CHECK(files["/top.bin"] == std::vector<uint8_t>({'t', 'o', 'p'}));
    CHECK(notes.empty());
    // A path two mods replace: the later one wins, with a note.
    files = mods::romfs_files(dir, {"original-encounters", "other", "missing"}, &notes);
    CHECK(files["/bin/field/table.bin"] == std::vector<uint8_t>({'o', 't', 'h', 'e', 'r'}));
    CHECK_EQ(notes.size(), 1u);
    CHECK(mods::romfs_files(dir, {}, &notes).empty());

    // battle-balls is a data mod as well: started only with its romfs folder,
    // and only with wild-battles (a catch is lost without it).
    const mods::ModInfo* balls = mods::find_mod("battle-balls");
    CHECK(balls != nullptr && balls->data && !balls->live && balls->ready);
    CHECK(std::string(mods::needed_mod("battle-balls")) == "wild-battles");
    CHECK(mods::needed_mod("wild-battles") == nullptr);
    fs::create_directories(dir / "battle-balls" / "romfs" / "bin" / "item");
    write(dir / "battle-balls" / "romfs" / "bin" / "item" / "battle_pocket.dat", "pages");
    o.enabled = {"battle-balls"};
    r = {};
    CHECK(mods::start(o, &r, &error));
    CHECK(r.active.empty());
    CHECK(!r.messages.empty() && r.messages[0] == "battle-balls: not started, it needs wild-battles");
    notes.clear();
    files = mods::romfs_files(dir, {"original-encounters", "battle-balls"}, &notes);
    CHECK_EQ(files.size(), 2u);
    CHECK(notes.size() == 1 && notes[0] == "battle-balls: files left out, it needs wild-battles");
    fs::remove_all(dir / "battle-balls" / "romfs");
    o.enabled = {"wild-battles", "battle-balls"};
    r = {};
    CHECK(mods::start(o, &r, &error));
    CHECK(r.active.empty());  // no wild-battles.cfg here, no romfs folder for battle-balls
    bool no_folder = false;
    for (const std::string& m : r.messages)
        no_folder |= m.find("battle-balls: not started, no ") == 0;
    CHECK(no_folder);
    fs::create_directories(dir / "battle-balls" / "romfs" / "bin" / "item");
    write(dir / "battle-balls" / "romfs" / "bin" / "item" / "battle_pocket.dat", "pages");
    r = {};
    CHECK(mods::start(o, &r, &error));
    // Named but not started (no wild-battles.cfg): battle-balls goes again, its files too (romfs_keep).
    CHECK(r.active.empty());
    bool not_started = false;
    for (const std::string& m : r.messages)
        not_started |= m == "battle-balls: not started, wild-battles did not start";
    CHECK(not_started);
    CHECK(!mods::enabled("battle-balls"));
    CHECK(!mods::needs_met("battle-balls"));
    CHECK(mods::needs_met("original-encounters"));
    mods::stop();
    {  // wild-battles started, but without `from post_result` it stores no catch: no balls
        const uint64_t doors[] = {kTouch};
        rt::declare_hookable(doors, 1);
        write(dir / "wild-battles.cfg", "hook encounter_touch 0x8001000\n");
        r = {};
        CHECK(mods::start(o, &r, &error));
        CHECK(r.active.size() == 1 && r.active[0] == "wild-battles");
        bool no_catch = false;
        for (const std::string& m : r.messages)
            no_catch |= m == "battle-balls: not started, wild-battles stores no catch (from post_result missing)";
        CHECK(no_catch);
        CHECK(!mods::needs_met("battle-balls"));
        mods::stop();
        fs::remove(dir / "wild-battles.cfg");
    }
    files = mods::romfs_files(dir, {"original-encounters", "wild-battles", "battle-balls"}, &notes);
    CHECK_EQ(files.size(), 3u);
    CHECK(files["/bin/item/battle_pocket.dat"] == std::vector<uint8_t>({'p', 'a', 'g', 'e', 's'}));
    // The same path from a mod without a needed mod stays: battle-balls may be left out after start().
    fs::create_directories(dir / "original-encounters" / "romfs" / "bin" / "item");
    write(dir / "original-encounters" / "romfs" / "bin" / "item" / "battle_pocket.dat", "first");
    notes.clear();
    std::map<std::string, std::string> owners;
    files = mods::romfs_files(dir, {"original-encounters", "wild-battles", "battle-balls"}, &notes, &owners);
    CHECK(files["/bin/item/battle_pocket.dat"] == std::vector<uint8_t>({'f', 'i', 'r', 's', 't'}));
    CHECK(owners["/bin/item/battle_pocket.dat"] == "original-encounters");
    CHECK(notes.size() == 1 &&
          notes[0] == "battle-balls: /bin/item/battle_pocket.dat left out, original-encounters replaces it already");
    fs::remove(dir / "original-encounters" / "romfs" / "bin" / "item" / "battle_pocket.dat");
}

}  // namespace

int main() {
    parse();
    dumps();
    const fs::path dir = fs::temp_directory_path() / ("lgp_test_mods_" + std::to_string(std::rand()));
    // std::rand() is unseeded, so the name repeats: a run killed before its
    // clean-up (a cancelled CI run on a kept runner) leaves its files there.
    fs::remove_all(dir);
    fs::create_directories(dir);
    start(dir);
    acts(dir);
    battle_throw(dir);
    battle_throw_ball(dir);
    widescreen_sizes();
    widescreen_scales();
    widescreen_projections();
    widescreen_start(dir);
    widescreen_layouts(dir);
    widescreen_file_scale(dir);
    widescreen_touches();
    wild_battles(dir);
    random_encounters(dir);
    gelb_mode(dir);
    classic_exp(dir);
    move_immediates();
    online_button(dir);
    skip_controller_screen(dir);
    audio_free_guard(dir);
    data_mods(dir / "data");
    fs::remove_all(dir);
    return finish();
}
