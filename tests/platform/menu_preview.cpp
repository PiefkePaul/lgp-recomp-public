// P7: pictures of the app's menus without a device or GPU (SDL's dummy video
// driver, the triangle lists drawn in software), to look at a layout on the
// screen sizes of the target devices:
//
//   lgp_menu_preview --screen start|slots|settings|pause|text|loading|hud-bounds --size WxH
//                    --out FILE.ppm [--in-game] [--hints] [--page N] [--down N] [--right N] [--up N] [--click X,Y]
//                    [--then-down N] [--system-settings] [--touch]
//                    [--font FILE] [--skin ARCHIVE.arc]... [--layout ARCHIVE:LAYOUT] [--language en|de]
//
// --size in window points (iPad Pro 11" landscape 1194x834, iPhone landscape
// 844x390), --in-game puts a grey stand-in for the paused game image behind
// it, --hints shows the controller hints, --page picks the settings page,
// --down/--right/--up press the arrow keys N times (cursor, values; on
// hud-bounds the lines, which open on the window's aspect), --click taps
// with the mouse at a point (after the keys), --then-down presses down N times
// after the tap (in an opened choice list), --font draws with a font file (the game's
// BFOTF from the user's RomFS; such pictures stay in $LGP_PRIVATE_DIR),
// --skin loads a layout archive of the game (SARC with blyt/ and timg/) and
// --layout draws one of its layouts alone over the whole picture, text panes
// labelled with their names (to compare with the game, locally).
// --system-settings: as on iPhone/iPad, the restart settings in the Settings app.
// --touch: as on a touch screen (the settings' round back button in the corner).
// --language: the interface language (lgp/platform/ui_text.hpp), default English.
// Without --out it checks that every screen draws (ctest platform.menu_preview).
#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "lgp/platform/ui/app_menu.hpp"
#include "lgp/platform/ui/layout_skin.hpp"
#include "lgp/platform/ui_text.hpp"
#include "ui_fixtures.hpp"

using namespace lgp::platform;

namespace {

struct Image {
    int w = 0, h = 0;
    std::vector<float> rgb;  // 0..1
};

void fill_background(Image& img, bool in_game) {
    img.rgb.assign(size_t(img.w) * size_t(img.h) * 3, 0.0f);
    for (int y = 0; y < img.h; ++y)
        for (int x = 0; x < img.w; ++x) {
            // A calm checker as the stand-in for the game image.
            const float v = in_game ? (((x / 48) + (y / 48)) % 2 ? 0.55f : 0.45f) : 0.0f;
            float* p = &img.rgb[(size_t(y) * size_t(img.w) + size_t(x)) * 3];
            p[0] = v * 0.8f, p[1] = v * 0.95f, p[2] = v;
        }
}

float srgb(uint8_t b) { return float(b) / 255.0f; }

// Barycentric fill of each triangle with interpolated colour times the
// nearest texel of the command's texture, blended over the image (straight alpha, as the
// renderer does).
void draw(Image& img, const UiDrawData& d) {
    for (const UiCommand& c : d.commands) {
        const UiTexture* tex = c.image == 0                 ? d.texture.get()
                               : c.image <= d.images.size() ? d.images[c.image - 1].get()
                                                            : nullptr;
        const int cx0 = std::max(0, int(std::floor(c.clip[0]))), cy0 = std::max(0, int(std::floor(c.clip[1])));
        const int cx1 = std::min(img.w, int(std::ceil(c.clip[2]))), cy1 = std::min(img.h, int(std::ceil(c.clip[3])));
        for (uint32_t t = 0; t + 2 < c.index_count; t += 3) {
            const UiVertex* v[3];
            for (int k = 0; k < 3; ++k)
                v[k] = &d.vertices[c.base_vertex + d.indices[c.first_index + t + uint32_t(k)]];
            const float area = (v[1]->x - v[0]->x) * (v[2]->y - v[0]->y) - (v[2]->x - v[0]->x) * (v[1]->y - v[0]->y);
            if (std::abs(area) < 1e-6f)
                continue;
            const int x0 = std::max(cx0, int(std::floor(std::min({v[0]->x, v[1]->x, v[2]->x}))));
            const int x1 = std::min(cx1, int(std::ceil(std::max({v[0]->x, v[1]->x, v[2]->x}))));
            const int y0 = std::max(cy0, int(std::floor(std::min({v[0]->y, v[1]->y, v[2]->y}))));
            const int y1 = std::min(cy1, int(std::ceil(std::max({v[0]->y, v[1]->y, v[2]->y}))));
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x) {
                    const float px = float(x) + 0.5f, py = float(y) + 0.5f;
                    const float w0 = ((v[1]->x - px) * (v[2]->y - py) - (v[2]->x - px) * (v[1]->y - py)) / area;
                    const float w1 = ((v[2]->x - px) * (v[0]->y - py) - (v[0]->x - px) * (v[2]->y - py)) / area;
                    const float w2 = 1.0f - w0 - w1;
                    if (w0 < 0 || w1 < 0 || w2 < 0)
                        continue;
                    float rgba[4];
                    for (int ch = 0; ch < 4; ++ch)
                        rgba[ch] = w0 * srgb(uint8_t(v[0]->rgba >> (8 * ch))) +
                                   w1 * srgb(uint8_t(v[1]->rgba >> (8 * ch))) +
                                   w2 * srgb(uint8_t(v[2]->rgba >> (8 * ch)));
                    if (tex != nullptr && tex->width != 0) {
                        const float tu = w0 * v[0]->u + w1 * v[1]->u + w2 * v[2]->u;
                        const float tv = w0 * v[0]->v + w1 * v[1]->v + w2 * v[2]->v;
                        const uint32_t ix = std::min(tex->width - 1, uint32_t(std::max(0.0f, tu * float(tex->width))));
                        const uint32_t iy = std::min(tex->height - 1, uint32_t(std::max(0.0f, tv * float(tex->height))));
                        const uint8_t* texel = &tex->rgba[(size_t(iy) * tex->width + ix) * 4];
                        for (int ch = 0; ch < 4; ++ch)
                            rgba[ch] *= srgb(texel[ch]);
                    }
                    float* p = &img.rgb[(size_t(y) * size_t(img.w) + size_t(x)) * 3];
                    for (int ch = 0; ch < 3; ++ch)
                        p[ch] = c.blend == 1 ? p[ch] * rgba[ch] : p[ch] * (1.0f - rgba[3]) + rgba[ch] * rgba[3];
                }
        }
    }
}

bool write_ppm(const Image& img, const char* path) {
    FILE* f = std::fopen(path, "wb");
    if (f == nullptr)
        return false;
    std::fprintf(f, "P6\n%d %d\n255\n", img.w, img.h);
    std::vector<uint8_t> row(size_t(img.w) * 3);
    for (int y = 0; y < img.h; ++y) {
        for (size_t i = 0; i < row.size(); ++i)
            row[i] = uint8_t(std::clamp(img.rgb[size_t(y) * row.size() + i], 0.0f, 1.0f) * 255.0f + 0.5f);
        std::fwrite(row.data(), 1, row.size(), f);
    }
    return std::fclose(f) == 0;
}

void press_key(ui::AppMenu& menu, SDL_Window* window, SDL_Keycode key, SDL_Scancode scancode) {
    for (const SDL_EventType type : {SDL_EVENT_KEY_DOWN, SDL_EVENT_KEY_UP}) {
        SDL_Event e{};
        e.type = type;
        e.key.key = key;
        e.key.scancode = scancode;
        e.key.down = type == SDL_EVENT_KEY_DOWN;
        e.key.windowID = SDL_GetWindowID(window);
        menu.handle_event(e);
        menu.frame();
    }
}

void click(ui::AppMenu& menu, SDL_Window* window, float x, float y) {
    for (const SDL_EventType type : {SDL_EVENT_MOUSE_MOTION, SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_UP}) {
        SDL_Event e{};
        e.type = type;
        if (type == SDL_EVENT_MOUSE_MOTION) {
            e.motion.windowID = SDL_GetWindowID(window);
            e.motion.x = x, e.motion.y = y;
        } else {
            e.button.windowID = SDL_GetWindowID(window);
            e.button.button = SDL_BUTTON_LEFT;
            e.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            e.button.clicks = 1;
            e.button.x = x, e.button.y = y;
        }
        menu.handle_event(e);
        menu.frame();
    }
}

struct Options {
    ui::Screen screen = ui::Screen::Start;
    int w = 1194, h = 834;
    bool in_game = false, hints = false, system_settings = false, touch = false;
    int page = 0, down = 0, right = 0, up = 0, then_down = 0;
    float click_x = -1, click_y = -1;
    std::string font;
    std::vector<std::pair<std::string, std::vector<uint8_t>>> skins;  // archive stem, bytes
    std::string layout;  // ARCHIVE:LAYOUT
};

std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Draws one screen; false if nothing was drawn.
bool render(const Options& o, const char* out) {
    SDL_Window* window = SDL_CreateWindow("lgp menu preview", o.w, o.h, 0);
    if (window == nullptr) {
        std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return false;
    }
    std::string error;
    auto menu = ui::AppMenu::create(window, &error);
    if (!menu) {
        std::fprintf(stderr, "menu: %s\n", error.c_str());
        SDL_DestroyWindow(window);
        return false;
    }
    if (!o.skins.empty()) {
        auto skin = std::make_shared<ui::LayoutSkin>();
        for (const auto& [stem, bytes] : o.skins) {
            std::vector<std::string> warnings;
            auto a = ui::load_skin_archive(stem, bytes, &error, &warnings);
            for (const std::string& w : warnings)
                std::printf("skin warning: %s\n", w.c_str());
            if (!a) {
                std::fprintf(stderr, "skin %s: %s\n", stem.c_str(), error.c_str());
                return false;
            }
            std::printf("skin %s: %zu layouts, %zu textures\n", stem.c_str(), a->layouts.size(), a->images.size());
            skin->archives.push_back(std::move(*a));
        }
        menu->set_skin(std::move(skin));
        if (!o.layout.empty()) {
            const size_t colon = o.layout.find(':');
            menu->set_layout_preview(o.layout.substr(0, colon),
                                     colon == std::string::npos ? std::string() : o.layout.substr(colon + 1));
        }
    }
    if (!o.font.empty()) {
        std::vector<uint8_t> bytes = read_file(o.font);
        const size_t size = bytes.size();
        if (!menu->set_game_font(std::move(bytes), &error)) {
            std::fprintf(stderr, "font %s: %s\n", o.font.c_str(), error.c_str());
            return false;
        }
        // The "interface font" line above is the system font loaded first;
        // this one replaces it.
        std::printf("game font %s: loaded (%zu bytes)\n", o.font.c_str(), size);
    }
    menu->set_slots({{trf("Save {}", {"1"}), "Oct 20, 18:12", false}, {trf("Save {}", {"2"}), tr("empty"), true}}, 0);
    menu->set_touch_layout_supported(true);
    // Two mods as the app lists them (lgp/mods/mods.hpp), for the page "Game".
    menu->settings().mods = {
        {"wild-battles", tr("Wild battles"), "Wild Pok\xC3\xA9mon you touch battle first.", true, true},
        {"random-encounters", tr("Pok\xC3\xA9mon Yellow"), "Random battles as in Pok\xC3\xA9mon Yellow.", true, true},
    };
    menu->set_system_settings(o.system_settings);
    menu->set_game_source(tr("Game: included in the app"));
    menu->set_button_hints(o.hints ? 1 : 0);
    menu->set_back_button(o.touch ? 1 : 0);
    menu->set_settings_page(o.page);
    menu->set_in_game(o.in_game);
    if (o.screen == ui::Screen::Settings && o.in_game)
        menu->show(ui::Screen::Pause);
    if (o.screen == ui::Screen::Text) {
        ui::TextPrompt prompt;
        prompt.header = "Wie hei\xC3\x9Ft du?";
        prompt.guide = "Name";
        prompt.initial = "Paul";
        prompt.max_length = 10;
        menu->show_text(prompt);
    } else {
        menu->show(o.screen);
        menu->set_progress(412, 1453);
    }
    for (int i = 0; i < 10; ++i)  // the panel sizes itself (and its rows) over the first frames
        menu->frame();
    for (int i = 0; i < o.down; ++i)
        press_key(*menu, window, SDLK_DOWN, SDL_SCANCODE_DOWN);
    for (int i = 0; i < o.right; ++i)
        press_key(*menu, window, SDLK_RIGHT, SDL_SCANCODE_RIGHT);
    for (int i = 0; i < o.up; ++i)
        press_key(*menu, window, SDLK_UP, SDL_SCANCODE_UP);
    if (o.click_x >= 0)
        click(*menu, window, o.click_x, o.click_y);
    for (int i = 0; i < o.then_down; ++i)
        press_key(*menu, window, SDLK_DOWN, SDL_SCANCODE_DOWN);
    for (int i = 0; i < 2; ++i)
        menu->frame();
    const auto data = menu->draw_data();
    bool ok = data != nullptr && !data->commands.empty();
    if (ok && out != nullptr) {
        Image img;
        img.w = o.w, img.h = o.h;
        fill_background(img, o.in_game);
        draw(img, *data);
        ok = write_ppm(img, out);
        if (!ok)
            std::fprintf(stderr, "cannot write %s\n", out);
    }
    menu.reset();
    SDL_DestroyWindow(window);
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    Options o;
    const char* out = nullptr;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const char* next = i + 1 < argc ? argv[i + 1] : "";
        if (a == "--screen") {
            const std::string s = next;
            o.screen = s == "slots"      ? ui::Screen::Slots
                       : s == "settings" ? ui::Screen::Settings
                       : s == "pause"    ? ui::Screen::Pause
                       : s == "text"     ? ui::Screen::Text
                       : s == "loading"  ? ui::Screen::Loading
                       : s == "hud-bounds" ? ui::Screen::HudBounds
                                         : ui::Screen::Start;
            ++i;
        } else if (a == "--size") {
            std::sscanf(next, "%dx%d", &o.w, &o.h);
            ++i;
        } else if (a == "--out") {
            out = next;
            ++i;
        } else if (a == "--in-game") {
            o.in_game = true;
        } else if (a == "--hints") {
            o.hints = true;
        } else if (a == "--system-settings") {
            o.system_settings = true;
        } else if (a == "--touch") {
            o.touch = true;
        } else if (a == "--language") {
            set_ui_language(ui_language_from_name(next));
            ++i;
        } else if (a == "--page") {
            o.page = std::atoi(next);
            ++i;
        } else if (a == "--down") {
            o.down = std::atoi(next);
            ++i;
        } else if (a == "--then-down") {
            o.then_down = std::atoi(next);
            ++i;
        } else if (a == "--right") {
            o.right = std::atoi(next);
            ++i;
        } else if (a == "--up") {
            o.up = std::atoi(next);
            ++i;
        } else if (a == "--font") {
            o.font = next;
            ++i;
        } else if (a == "--skin") {
            std::string stem = next;
            stem = stem.substr(stem.find_last_of("/\\") + 1);
            stem = stem.substr(0, stem.find('.'));
            o.skins.emplace_back(stem, read_file(next));
            ++i;
        } else if (a == "--layout") {
            o.layout = next;
            ++i;
        } else if (a == "--click") {
            std::sscanf(next, "%f,%f", &o.click_x, &o.click_y);
            ++i;
        } else {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return 2;
        }
    }
    int failed = 0;
    if (out != nullptr) {
        failed = render(o, out) ? 0 : 1;
    } else {
        // Every screen on a tablet and a phone, before and in the game, in
        // the own look and with stand-ins for the game's parts.
        Options skinned = o;
        skinned.skins = {{"setting_list_00", lgp::test::make_settings_list_archive()},
                         {"setting_bg_00", lgp::test::make_settings_bg_archive()}};
        for (const ui::Screen s : {ui::Screen::Start, ui::Screen::Slots, ui::Screen::Settings, ui::Screen::Pause,
                                   ui::Screen::Text, ui::Screen::Loading, ui::Screen::HudBounds})
            for (const auto& size : {std::pair{1194, 834}, std::pair{844, 390}})
                for (const int variant : {0, 1, 2}) {
                    Options each = variant == 2 ? skinned : o;
                    const bool in_game = variant == 1;
                    each.screen = s, each.w = size.first, each.h = size.second, each.in_game = in_game;
                    each.hints = true;
                    if (!render(each, nullptr)) {
                        std::fprintf(stderr, "screen %d at %dx%d drew nothing\n", int(s), size.first, size.second);
                        ++failed;
                    }
                }
        if (failed == 0)
            std::printf("all screens drawn\n");
    }
    SDL_Quit();
    return failed == 0 ? 0 : 1;
}
