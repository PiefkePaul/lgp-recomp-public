// P7: the app's menus (platform/ui, AppMenu) driven by SDL events, as a user
// would: the pause button and Escape over the game, back navigation, and
// which events the game still sees while a menu is open. SDL's dummy video
// driver, no renderer (frame() only builds the triangle lists).
#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include "../runtime/check.hpp"
#include "imgui_internal.h"  // NavWindowingTarget: ImGui's window switcher
#include "lgp/platform/ui/app_menu.hpp"
#include "lgp/platform/ui_text.hpp"

using namespace lgp::platform;

namespace {

SDL_Window* g_window = nullptr;

SDL_Event key(SDL_EventType type, SDL_Keycode code) {
    SDL_Event e{};
    e.type = type;
    e.key.key = code;
    e.key.down = type == SDL_EVENT_KEY_DOWN;
    e.key.windowID = SDL_GetWindowID(g_window);
    return e;
}

SDL_Event finger(SDL_EventType type, float x, float y) {
    SDL_Event e{};
    e.type = type;
    e.tfinger.fingerID = 1;
    e.tfinger.x = x;
    e.tfinger.y = y;
    e.tfinger.windowID = SDL_GetWindowID(g_window);
    return e;
}

std::unique_ptr<ui::AppMenu> in_game_menu() {
    std::string error;
    auto menu = ui::AppMenu::create(g_window, &error);
    CHECK(menu != nullptr);
    if (!menu)
        return nullptr;
    menu->set_slots({{"Save 1", "empty", true}}, 0);
    menu->set_in_game(true);
    menu->show(ui::Screen::None);
    CHECK(menu->frame() == ui::Action::None);  // draws the pause button once
    CHECK(!menu->wants_frames());               // then idles while the game runs
    return menu;
}

// A controller plugged in before the start menu shows up there: the menu
// starts the gamepad subsystem itself and quits it again.
void test_menu_starts_gamepads() {
    CHECK(SDL_WasInit(SDL_INIT_GAMEPAD) == 0);
    {
        std::string error;
        auto menu = ui::AppMenu::create(g_window, &error);
        CHECK(menu != nullptr);
        CHECK(SDL_WasInit(SDL_INIT_GAMEPAD) != 0);
    }
    CHECK(SDL_WasInit(SDL_INIT_GAMEPAD) == 0);
}

void test_escape_opens_pause() {
    auto menu = in_game_menu();
    if (!menu)
        return;
    const SDL_Event down = key(SDL_EVENT_KEY_DOWN, SDLK_ESCAPE);
    CHECK(menu->handle_event(down));
    CHECK(menu->wants_frames());  // the host loop must run frame() now
    CHECK(menu->frame() == ui::Action::OpenPause);
    CHECK(menu->screen() == ui::Screen::Pause);
    // Escape again: back into the game.
    CHECK(!menu->handle_event(key(SDL_EVENT_KEY_UP, SDLK_ESCAPE)));
    CHECK(menu->handle_event(down));
    CHECK(menu->frame() == ui::Action::Resume);
    CHECK(menu->screen() == ui::Screen::None);
}

// The Android TV remote has no Home key: a short press of its menu key reaches
// the game (Plus), a long one opens the pause, and a press closes it again.
void test_remote_menu_key() {
    auto menu = in_game_menu();
    if (!menu)
        return;
    SDL_Event down = key(SDL_EVENT_KEY_DOWN, SDLK_MENU);
    down.key.timestamp = SDL_GetTicksNS();
    CHECK(!menu->handle_event(down));
    CHECK(menu->frame() == ui::Action::None);
    CHECK(!menu->handle_event(key(SDL_EVENT_KEY_UP, SDLK_MENU)));
    CHECK(menu->frame() == ui::Action::None);
    CHECK(menu->screen() == ui::Screen::None);
    CHECK(!menu->wants_frames());
    down.key.timestamp = SDL_GetTicksNS() - 1'000'000'000;  // held for a second
    CHECK(!menu->handle_event(down));
    CHECK(menu->wants_frames());  // the app only calls frame() when asked to
    CHECK(menu->frame() == ui::Action::OpenPause);
    CHECK(menu->screen() == ui::Screen::Pause);
    CHECK(!menu->handle_event(key(SDL_EVENT_KEY_UP, SDLK_MENU)));
    CHECK(menu->handle_event(key(SDL_EVENT_KEY_DOWN, SDLK_MENU)));
    CHECK(menu->frame() == ui::Action::Resume);
    CHECK(menu->screen() == ui::Screen::None);
}

void test_pause_button() {
    auto menu = in_game_menu();
    if (!menu)
        return;
    // Elsewhere on the screen: for the game (touch controller).
    CHECK(!menu->handle_event(finger(SDL_EVENT_FINGER_DOWN, 0.2f, 0.7f)));
    CHECK(!menu->wants_frames());
    // Top centre: the pause button.
    CHECK(menu->handle_event(finger(SDL_EVENT_FINGER_DOWN, 0.5f, 0.08f)));
    CHECK(menu->wants_frames());
    CHECK(menu->frame() == ui::Action::OpenPause);
    CHECK(menu->screen() == ui::Screen::Pause);
}

void test_releases_reach_the_game() {
    auto menu = in_game_menu();
    if (!menu)
        return;
    menu->handle_event(key(SDL_EVENT_KEY_DOWN, SDLK_ESCAPE));
    menu->frame();
    CHECK(menu->screen() == ui::Screen::Pause);
    // Presses belong to the menu, releases and stick motion also to the game.
    CHECK(menu->handle_event(key(SDL_EVENT_KEY_DOWN, SDLK_A)));
    CHECK(!menu->handle_event(key(SDL_EVENT_KEY_UP, SDLK_A)));
    CHECK(menu->handle_event(finger(SDL_EVENT_FINGER_DOWN, 0.2f, 0.7f)));
    CHECK(!menu->handle_event(finger(SDL_EVENT_FINGER_UP, 0.2f, 0.7f)));
    SDL_Event e{};
    e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    e.gbutton.button = SDL_GAMEPAD_BUTTON_SOUTH;
    CHECK(menu->handle_event(e));
    e.type = SDL_EVENT_GAMEPAD_BUTTON_UP;
    CHECK(!menu->handle_event(e));
    e = SDL_Event{};
    e.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
    CHECK(!menu->handle_event(e));
}

void test_back_navigation() {
    auto menu = in_game_menu();
    if (!menu)
        return;
    menu->show(ui::Screen::Pause);
    menu->show(ui::Screen::Settings);
    CHECK(menu->handle_event(key(SDL_EVENT_KEY_DOWN, SDLK_AC_BACK)));
    CHECK(menu->frame() == ui::Action::None);
    CHECK(menu->screen() == ui::Screen::Pause);
    SDL_Event home{};
    home.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    home.gbutton.button = SDL_GAMEPAD_BUTTON_GUIDE;
    CHECK(menu->handle_event(home));
    CHECK(menu->frame() == ui::Action::Resume);
    CHECK(menu->screen() == ui::Screen::None);
}

// The graphics screen before the game: progress, "Start now" plays, the
// back key does not leave it (there is nothing to go back to).
void test_loading_screen() {
    auto menu = in_game_menu();
    if (!menu)
        return;
    menu->show(ui::Screen::Loading);
    menu->set_progress(3, 10);
    CHECK(menu->wants_frames());
    CHECK(menu->frame() == ui::Action::None);
    menu->handle_event(key(SDL_EVENT_KEY_DOWN, SDLK_ESCAPE));
    menu->handle_event(key(SDL_EVENT_KEY_UP, SDLK_ESCAPE));
    for (int i = 0; i < 3; ++i)
        CHECK(menu->frame() == ui::Action::None);
    CHECK(menu->screen() == ui::Screen::Loading);
    menu->press(ui::Action::Play);
    CHECK(menu->frame() == ui::Action::Play);
}

// The software keyboard drawn by the menu (iOS, Android): Enter confirms
// the text in the field, back cancels, a message closes with OK; the screen
// returns to where it was.
void test_text_screen() {
    auto menu = in_game_menu();
    if (!menu)
        return;
    ui::TextPrompt p;
    p.header = "Wie hei\xC3\x9Ft du?";
    p.initial = "Red";
    p.max_length = 10;
    menu->show_text(p);
    CHECK(menu->wants_frames());
    CHECK(menu->frame() == ui::Action::None);  // asks for the field's focus
    CHECK(menu->frame() == ui::Action::None);  // the field is active
    CHECK(menu->frame() == ui::Action::None);  // ... and owns Enter (ImGui routes keys a frame later)
    CHECK(menu->screen() == ui::Screen::Text);
    menu->handle_event(key(SDL_EVENT_KEY_DOWN, SDLK_RETURN));
    menu->handle_event(key(SDL_EVENT_KEY_UP, SDLK_RETURN));
    ui::Action a = menu->frame();
    for (int i = 0; i < 3 && a == ui::Action::None; ++i)
        a = menu->frame();
    CHECK(a == ui::Action::TextOk);
    CHECK(menu->text_value() == "Red");
    CHECK(menu->screen() == ui::Screen::None);
    CHECK(menu->wants_frames());  // the dialog must still be drawn away
    CHECK(menu->frame() == ui::Action::None);
    CHECK(!menu->wants_frames());
    // A refused answer reopens the same field with its text and the note.
    menu->reopen_text("zu kurz");
    CHECK(menu->screen() == ui::Screen::Text);
    for (int i = 0; i < 3; ++i)
        menu->frame();
    menu->press(ui::Action::TextOk);
    CHECK(menu->frame() == ui::Action::TextOk);
    CHECK(menu->text_value() == "Red");
    menu->frame();
    // A pause request while the game waits for the text is dropped: the
    // pause screen would hide the request and the game would wait forever.
    menu->show_text(p);
    menu->frame();
    menu->press(ui::Action::OpenPause);
    CHECK(menu->frame() == ui::Action::None);
    CHECK(menu->screen() == ui::Screen::Text);
    menu->press(ui::Action::TextCancel);
    CHECK(menu->frame() == ui::Action::TextCancel);
    // Typing past the limit: the field itself stops at it, so it shows what
    // the count says and a backspace takes the last kept character.
    ui::TextPrompt shorter;
    shorter.max_length = 4;
    menu->show_text(shorter);
    for (int i = 0; i < 3; ++i)
        menu->frame();
    SDL_Event typed{};
    typed.type = SDL_EVENT_TEXT_INPUT;
    typed.text.windowID = SDL_GetWindowID(g_window);
    typed.text.text = "abcdefgh";
    menu->handle_event(typed);
    menu->frame();
    menu->frame();
    CHECK(menu->text_value() == "abcd");
    menu->handle_event(key(SDL_EVENT_KEY_DOWN, SDLK_BACKSPACE));
    menu->handle_event(key(SDL_EVENT_KEY_UP, SDLK_BACKSPACE));
    menu->frame();
    menu->frame();
    menu->press(ui::Action::TextOk);
    CHECK(menu->frame() == ui::Action::TextOk);
    CHECK(menu->text_value() == "abc");
    menu->frame();
    // Texts longer than any fixed field buffer stay whole.
    ui::TextPrompt longer;
    longer.initial = std::string(600, 'a') + "\xC3\xA4";
    menu->show_text(longer);
    for (int i = 0; i < 3; ++i)
        menu->frame();
    menu->press(ui::Action::TextOk);
    CHECK(menu->frame() == ui::Action::TextOk);
    CHECK(menu->text_value() == longer.initial);
    menu->frame();
    menu->show_text(p);
    menu->frame();
    CHECK(menu->handle_event(key(SDL_EVENT_KEY_DOWN, SDLK_ESCAPE)));
    CHECK(menu->frame() == ui::Action::TextCancel);
    CHECK(menu->screen() == ui::Screen::None);
    ui::TextPrompt m;
    m.message = true;
    m.header = "error 2110-1100";
    menu->show_text(m);
    menu->frame();
    menu->press(ui::Action::TextOk);
    CHECK(menu->frame() == ui::Action::TextOk);
    CHECK(menu->screen() == ui::Screen::None);
}

SDL_Event pad_button(SDL_EventType type, SDL_GamepadButton button) {
    SDL_Event e{};
    e.type = type;
    e.gbutton.button = static_cast<Uint8>(button);
    e.gbutton.down = type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    return e;
}

// Android keeps a controller's Home for itself (NVIDIA Shield) and sends a
// TV remote's menu key as Start: held, Start opens the pause; a short press
// stays the game's Plus. In the menus Back, Start and B (by its label; an
// unknown pad has Xbox labels, B east) go back.
void test_pad_start_and_back() {
    auto menu = in_game_menu();
    if (!menu)
        return;
    // Elsewhere (desktop, iOS) holding Start stays the game's Plus only.
    SDL_Event held = pad_button(SDL_EVENT_GAMEPAD_BUTTON_DOWN, SDL_GAMEPAD_BUTTON_START);
    held.gbutton.timestamp = SDL_GetTicksNS() - 1'000'000'000;
    CHECK(!menu->handle_event(held));
    CHECK(!menu->wants_frames());
    CHECK(menu->frame() == ui::Action::None);
    CHECK(!menu->handle_event(pad_button(SDL_EVENT_GAMEPAD_BUTTON_UP, SDL_GAMEPAD_BUTTON_START)));
    menu->set_start_hold_pause(true);  // the default on Android
    SDL_Event down = pad_button(SDL_EVENT_GAMEPAD_BUTTON_DOWN, SDL_GAMEPAD_BUTTON_START);
    const SDL_Event up = pad_button(SDL_EVENT_GAMEPAD_BUTTON_UP, SDL_GAMEPAD_BUTTON_START);
    down.gbutton.timestamp = SDL_GetTicksNS();
    CHECK(!menu->handle_event(down));
    CHECK(menu->frame() == ui::Action::None);
    CHECK(!menu->handle_event(up));
    CHECK(menu->screen() == ui::Screen::None);
    CHECK(!menu->wants_frames());
    down.gbutton.timestamp = SDL_GetTicksNS() - 1'000'000'000;  // held for a second
    CHECK(!menu->handle_event(down));
    CHECK(menu->wants_frames());
    CHECK(menu->frame() == ui::Action::OpenPause);
    CHECK(menu->screen() == ui::Screen::Pause);
    CHECK(!menu->handle_event(up));
    // Start again closes the pause.
    CHECK(menu->handle_event(pad_button(SDL_EVENT_GAMEPAD_BUTTON_DOWN, SDL_GAMEPAD_BUTTON_START)));
    CHECK(menu->frame() == ui::Action::Resume);
    CHECK(menu->screen() == ui::Screen::None);
    // Back and B go one screen back.
    menu->show(ui::Screen::Pause);
    menu->show(ui::Screen::Settings);
    menu->frame();
    CHECK(menu->handle_event(pad_button(SDL_EVENT_GAMEPAD_BUTTON_DOWN, SDL_GAMEPAD_BUTTON_BACK)));
    CHECK(menu->frame() == ui::Action::None);
    CHECK(menu->screen() == ui::Screen::Pause);
    CHECK(!menu->handle_event(pad_button(SDL_EVENT_GAMEPAD_BUTTON_UP, SDL_GAMEPAD_BUTTON_BACK)));
    CHECK(menu->handle_event(pad_button(SDL_EVENT_GAMEPAD_BUTTON_DOWN, SDL_GAMEPAD_BUTTON_EAST)));
    CHECK(menu->frame() == ui::Action::Resume);
    CHECK(menu->screen() == ui::Screen::None);
}

// The Siri Remote has no Start (SDL: click A, Back B, Play/Pause X; Back only
// as short pulses): on tvOS holding X opens the pause instead.
void test_remote_without_start() {
    auto menu = in_game_menu();
    if (!menu)
        return;
    menu->set_start_hold_pause(true);  // the default on Android and tvOS
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = 2;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_LEFTX) | (1u << SDL_GAMEPAD_AXIS_LEFTY);
    desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_SOUTH) | (1u << SDL_GAMEPAD_BUTTON_EAST) |
                       (1u << SDL_GAMEPAD_BUTTON_WEST);
    desc.name = "lgp virtual siri remote";
    desc.vendor_id = 0x1209;
    desc.product_id = 0x0013;
    const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
    CHECK(id != 0);
    SDL_Gamepad* pad = SDL_OpenGamepad(id);
    CHECK(pad != nullptr);
    if (pad == nullptr)
        return;
    SDL_Event down = pad_button(SDL_EVENT_GAMEPAD_BUTTON_DOWN, SDL_GAMEPAD_BUTTON_WEST);
    down.gbutton.which = id;
    SDL_Event up = pad_button(SDL_EVENT_GAMEPAD_BUTTON_UP, SDL_GAMEPAD_BUTTON_WEST);
    up.gbutton.which = id;
    down.gbutton.timestamp = SDL_GetTicksNS();
    CHECK(!menu->handle_event(down));  // a short press stays the game's X
    CHECK(!menu->handle_event(up));
    CHECK(menu->frame() == ui::Action::None);
    down.gbutton.timestamp = SDL_GetTicksNS() - 1'000'000'000;
    CHECK(!menu->handle_event(down));
    CHECK(menu->frame() == ui::Action::OpenPause);
    CHECK(!menu->handle_event(up));
    // Back (east, labelled B) closes it.
    SDL_Event back = pad_button(SDL_EVENT_GAMEPAD_BUTTON_DOWN, SDL_GAMEPAD_BUTTON_EAST);
    back.gbutton.which = id;
    CHECK(menu->handle_event(back));
    CHECK(menu->frame() == ui::Action::Resume);
    menu->frame();  // draws the pause screen away
    // A controller with Start: X held stays X.
    SDL_Event x = pad_button(SDL_EVENT_GAMEPAD_BUTTON_DOWN, SDL_GAMEPAD_BUTTON_WEST);
    x.gbutton.timestamp = SDL_GetTicksNS() - 1'000'000'000;
    CHECK(!menu->handle_event(x));
    CHECK(!menu->wants_frames());
    SDL_CloseGamepad(pad);
    SDL_DetachVirtualJoystick(id);
}

// "Catch style in battle" only with "Catch in battle": switched off and
// greyed out while that one is off (Paul 2026-11-02).
void test_throw_needs_balls() {
    std::string error;
    auto menu = ui::AppMenu::create(g_window, &error);
    CHECK(menu != nullptr);
    if (!menu)
        return;
    menu->settings().mods.push_back({"battle-balls", "Catch in battle", "", false, false});
    menu->settings().mods.push_back({"battle-throw", "Catch style in battle", "", true, true});
    menu->show(ui::Screen::Settings);
    menu->frame();
    CHECK(!menu->settings().mods[1].on);  // off with Catch in battle
    menu->settings().mods[0].on = true;
    menu->settings().mods[1].on = true;
    menu->frame();
    CHECK(menu->settings().mods[1].on);  // stays on with it
    menu->settings().mods[0].on = false;
    menu->frame();
    CHECK(!menu->settings().mods[1].on);
    // Switched off on its row: the throw is off in that change already, so
    // whoever stores and applies the settings sees both off.
    menu->settings().mods[0].on = true;
    menu->settings().mods[1].on = true;
    menu->frame();
    const auto press = [&](SDL_Keycode code, SDL_Scancode scancode) {
        SDL_Event down = key(SDL_EVENT_KEY_DOWN, code);
        down.key.scancode = scancode;
        menu->handle_event(down);
        const ui::Action action = menu->frame();
        SDL_Event up = key(SDL_EVENT_KEY_UP, code);
        up.key.scancode = scancode;
        menu->handle_event(up);
        return action;
    };
    const auto tap = [&](SDL_Keycode code, SDL_Scancode scancode) {
        const ui::Action action = press(code, scancode);
        menu->frame();
        return action;
    };
    for (int i = 0; i < 3; ++i)
        tap(SDLK_E, SDL_SCANCODE_E);
    CHECK_EQ(menu->settings_page(), 3);  // "Game": Wi-Fi, server, room, language, main menu, the mods
    for (int i = 0; i < 5; ++i)
        tap(SDLK_DOWN, SDL_SCANCODE_DOWN);
    CHECK(press(SDLK_RIGHT, SDL_SCANCODE_RIGHT) == ui::Action::SettingsChanged);
    CHECK(!menu->settings().mods[0].on);
    CHECK(!menu->settings().mods[1].on);
    // A change from outside (--menu-script: the flag, then SettingsChanged),
    // also with the settings screen closed.
    menu->show(ui::Screen::Pause);
    menu->frame();
    menu->settings().mods[0].on = true;
    menu->settings().mods[1].on = true;
    menu->frame();
    menu->settings().mods[0].on = false;
    menu->press(ui::Action::SettingsChanged);
    CHECK(menu->frame() == ui::Action::SettingsChanged);
    CHECK(!menu->settings().mods[1].on);
}

// The settings rows as in the game's options: the first row is selected
// when the screen opens, left/right change the selected row's value, E (R
// on the controller) turns to the next page.
void test_settings_rows() {
    std::string error;
    auto menu = ui::AppMenu::create(g_window, &error);
    CHECK(menu != nullptr);
    if (!menu)
        return;
    menu->set_touch_layout_supported(true);
    // With the widescreen mod chosen as the picture format (the HUD row needs it).
    menu->settings().mods.push_back({"widescreen", "Widescreen", "", true, false});
    menu->show(ui::Screen::Settings);
    menu->frame();
    menu->frame();
    const auto tap = [&](SDL_Keycode code, SDL_Scancode scancode) {
        SDL_Event down = key(SDL_EVENT_KEY_DOWN, code);
        down.key.scancode = scancode;
        CHECK(menu->handle_event(down));
        const ui::Action action = menu->frame();
        SDL_Event up = key(SDL_EVENT_KEY_UP, code);
        up.key.scancode = scancode;
        menu->handle_event(up);
        menu->frame();
        return action;
    };
    CHECK(menu->settings().touch);
    CHECK(tap(SDLK_RIGHT, SDL_SCANCODE_RIGHT) == ui::Action::SettingsChanged);  // "Touch-Steuerung" off
    CHECK(!menu->settings().touch);
    CHECK(tap(SDLK_LEFT, SDL_SCANCODE_LEFT) == ui::Action::SettingsChanged);
    CHECK(menu->settings().touch);
    CHECK(tap(SDLK_DOWN, SDL_SCANCODE_DOWN) == ui::Action::None);  // to "Size"
    CHECK(tap(SDLK_RIGHT, SDL_SCANCODE_RIGHT) == ui::Action::SettingsChanged);
    CHECK_EQ(menu->settings().touch_size, 110);
    CHECK(menu->settings().touch);
    CHECK(tap(SDLK_E, SDL_SCANCODE_E) == ui::Action::None);  // page "Ton"
    CHECK_EQ(menu->settings_page(), 1);
    CHECK(tap(SDLK_LEFT, SDL_SCANCODE_LEFT) == ui::Action::SettingsChanged);  // its first row: the volume
    CHECK_EQ(menu->settings().volume, 95);
    CHECK_EQ(menu->settings().touch_size, 110);
    // Page "Bild": format, TV mode, smoothing, the battle's HUD bounds, then the graphics rows.
    menu->set_picture_aspect(4.0f / 3.0f);  // the iPad's: the height pair
    CHECK(tap(SDLK_E, SDL_SCANCODE_E) == ui::Action::None);
    CHECK_EQ(menu->settings_page(), 2);
    CHECK(tap(SDLK_DOWN, SDL_SCANCODE_DOWN) == ui::Action::None);
    CHECK(tap(SDLK_DOWN, SDL_SCANCODE_DOWN) == ui::Action::None);  // "Smoothing"
    CHECK(tap(SDLK_RIGHT, SDL_SCANCODE_RIGHT) == ui::Action::SettingsChanged);
    CHECK_EQ(menu->settings().smoothing, 1);  // "Weich"
    CHECK(tap(SDLK_DOWN, SDL_SCANCODE_DOWN) == ui::Action::None);  // "Battle HUD bounds"
    CHECK(tap(SDLK_RIGHT, SDL_SCANCODE_RIGHT) == ui::Action::None);  // opens its screen
    CHECK(menu->screen() == ui::Screen::HudBounds);
    CHECK(tap(SDLK_UP, SDL_SCANCODE_UP) == ui::Action::None);  // both lines 5 % outwards
    CHECK(tap(SDLK_UP, SDL_SCANCODE_UP) == ui::Action::None);
    CHECK(tap(SDLK_RIGHT, SDL_SCANCODE_RIGHT) == ui::Action::None);  // 4:3 has no width pair
    CHECK(tap(SDLK_RETURN, SDL_SCANCODE_RETURN) == ui::Action::SettingsChanged);  // apply
    CHECK(menu->screen() == ui::Screen::Settings);
    CHECK_EQ(menu->settings().hud_height, 10);
    CHECK_EQ(menu->settings().hud_width, 0);
    // Back on the row: open again, move, discard with Escape (B).
    CHECK(tap(SDLK_RIGHT, SDL_SCANCODE_RIGHT) == ui::Action::None);
    CHECK(menu->screen() == ui::Screen::HudBounds);
    CHECK(tap(SDLK_UP, SDL_SCANCODE_UP) == ui::Action::None);
    CHECK(tap(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE) == ui::Action::None);
    CHECK(menu->screen() == ui::Screen::Settings);
    CHECK_EQ(menu->settings().hud_height, 10);
    // Default (Y) goes back to the 16:9 frame; down never goes inside it.
    CHECK(tap(SDLK_RIGHT, SDL_SCANCODE_RIGHT) == ui::Action::None);
    CHECK(tap(SDLK_DOWN, SDL_SCANCODE_DOWN) == ui::Action::None);
    CHECK(tap(SDLK_DOWN, SDL_SCANCODE_DOWN) == ui::Action::None);
    CHECK(tap(SDLK_DOWN, SDL_SCANCODE_DOWN) == ui::Action::None);
    CHECK(tap(SDLK_UP, SDL_SCANCODE_UP) == ui::Action::None);
    CHECK(tap(SDLK_Y, SDL_SCANCODE_Y) == ui::Action::None);
    CHECK(tap(SDLK_RETURN, SDL_SCANCODE_RETURN) == ui::Action::SettingsChanged);
    CHECK_EQ(menu->settings().hud_height, 0);
    CHECK(menu->screen() == ui::Screen::Settings);
    CHECK(tap(SDLK_DOWN, SDL_SCANCODE_DOWN) == ui::Action::None);  // "Lichteffekte"
    CHECK(!menu->settings().effects_half);
    CHECK(tap(SDLK_RIGHT, SDL_SCANCODE_RIGHT) == ui::Action::SettingsChanged);
    CHECK(menu->settings().effects_half);
    CHECK(tap(SDLK_DOWN, SDL_SCANCODE_DOWN) == ui::Action::None);  // "Resolution"
    CHECK(tap(SDLK_RIGHT, SDL_SCANCODE_RIGHT) == ui::Action::SettingsChanged);
    CHECK_EQ(menu->settings().render_scale, 1);  // 75 %
    CHECK(tap(SDLK_DOWN, SDL_SCANCODE_DOWN) == ui::Action::None);  // "Grafik-Thread"
    CHECK(tap(SDLK_RIGHT, SDL_SCANCODE_RIGHT) == ui::Action::SettingsChanged);
    CHECK(menu->settings().async_execute);
    CHECK(!menu->settings().docked);
    CHECK(tap(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE) == ui::Action::None);
    CHECK(menu->screen() == ui::Screen::Start);
}

// With the widescreen mod's file the picture format row holds four choices:
// "Widescreen" (the mod on) and the three 16:9 ones (the mod off, aspect);
// the battle's HUD bounds row only with "Widescreen", the mod not among the
// game mods.
void test_picture_format() {
    std::string error;
    auto menu = ui::AppMenu::create(g_window, &error);
    CHECK(menu != nullptr);
    if (!menu)
        return;
    menu->settings().mods.push_back({"widescreen", "Widescreen", "", true, false});
    menu->settings().aspect = 1;
    menu->set_picture_aspect(4.0f / 3.0f);
    menu->show(ui::Screen::Settings);
    menu->frame();
    menu->frame();
    const auto tap = [&](SDL_Keycode code, SDL_Scancode scancode) {
        SDL_Event down = key(SDL_EVENT_KEY_DOWN, code);
        down.key.scancode = scancode;
        menu->handle_event(down);
        ui::Action action = menu->frame();
        SDL_Event up = key(SDL_EVENT_KEY_UP, code);
        up.key.scancode = scancode;
        menu->handle_event(up);
        if (const ui::Action a = menu->frame(); action == ui::Action::None)
            action = a;
        return action;
    };
    tap(SDLK_E, SDL_SCANCODE_E);
    tap(SDLK_E, SDL_SCANCODE_E);
    CHECK_EQ(menu->settings_page(), 2);
    CHECK_EQ(menu->settings().picture_format, 0);  // the mod on: "Widescreen"
    CHECK(tap(SDLK_RIGHT, SDL_SCANCODE_RIGHT) == ui::Action::SettingsChanged);  // "16:9 with borders"
    CHECK(!menu->settings().mods[0].on);
    CHECK_EQ(menu->settings().aspect, 0);
    CHECK(tap(SDLK_RIGHT, SDL_SCANCODE_RIGHT) == ui::Action::SettingsChanged);  // "16:9 fill"
    CHECK_EQ(menu->settings().aspect, 1);
    CHECK(!menu->settings().mods[0].on);
    // Without the HUD row: format, TV mode, smoothing, light effects.
    for (int i = 0; i < 3; ++i)
        CHECK(tap(SDLK_DOWN, SDL_SCANCODE_DOWN) == ui::Action::None);
    CHECK(tap(SDLK_RIGHT, SDL_SCANCODE_RIGHT) == ui::Action::SettingsChanged);
    CHECK(menu->settings().effects_half);
    for (int i = 0; i < 3; ++i)
        tap(SDLK_UP, SDL_SCANCODE_UP);
    CHECK(tap(SDLK_LEFT, SDL_SCANCODE_LEFT) == ui::Action::SettingsChanged);
    CHECK(tap(SDLK_LEFT, SDL_SCANCODE_LEFT) == ui::Action::SettingsChanged);  // "Widescreen" again
    CHECK(menu->settings().mods[0].on);
    CHECK_EQ(menu->settings().aspect, 0);
    CHECK_EQ(menu->settings().picture_format, 0);
    // A switch from elsewhere (--menu-script mod:widescreen=off): the row follows.
    menu->settings().mods[0].on = false;
    menu->settings().aspect = 2;
    menu->frame();
    CHECK_EQ(menu->settings().picture_format, 3);
    CHECK_EQ(menu->settings().aspect, 2);
}

SDL_Event touch_mouse(SDL_EventType type, float x, float y) {
    SDL_Event e{};
    e.type = type;
    e.button.which = SDL_TOUCH_MOUSEID;
    e.button.button = SDL_BUTTON_LEFT;
    e.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
    e.button.x = x;
    e.button.y = y;
    e.button.windowID = SDL_GetWindowID(g_window);
    return e;
}

// Taps the centre of the start screen with a mouse click made from a touch;
// returns the first action within a few frames.
ui::Action tap_centre(ui::AppMenu& menu) {
    int w = 0, h = 0;
    SDL_GetWindowSize(g_window, &w, &h);
    ui::Action a = ui::Action::None;
    SDL_Event move{};
    move.type = SDL_EVENT_MOUSE_MOTION;  // SDL sends the position first
    move.motion.which = SDL_TOUCH_MOUSEID;
    move.motion.x = float(w) * 0.5f;
    move.motion.y = float(h) * 0.5f;
    move.motion.windowID = SDL_GetWindowID(g_window);
    menu.handle_event(move);
    menu.frame();
    menu.handle_event(touch_mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, float(w) * 0.5f, float(h) * 0.5f));
    for (int i = 0; i < 3 && a == ui::Action::None; ++i)
        a = menu.frame();
    menu.handle_event(touch_mouse(SDL_EVENT_MOUSE_BUTTON_UP, float(w) * 0.5f, float(h) * 0.5f));
    for (int i = 0; i < 3 && a == ui::Action::None; ++i)
        a = menu.frame();
    return a;
}

std::unique_ptr<ui::AppMenu> start_menu() {
    std::string error;
    auto menu = ui::AppMenu::create(g_window, &error);
    CHECK(menu != nullptr);
    if (!menu)
        return nullptr;
    menu->set_slots({{"Spielstand 1", "leer", true}, {"Spielstand 2", "leer", true}}, 0);
    menu->show(ui::Screen::Start);
    for (int i = 0; i < 3; ++i)
        menu->frame();
    return menu;
}

// tvOS: the Siri Remote's surface sends touches that SDL turns into mouse
// clicks mid-screen, which pressed "Spielstand wählen" on every click (Apple
// TV, 15.10.2026). With set_touch_is_remote they never reach the menu.
void test_remote_touch_is_no_click() {
    auto menu = start_menu();
    if (!menu)
        return;
    const ui::Action plain = tap_centre(*menu);
    // A screen tap at the centre presses "Spielstand wählen", as on the Apple TV.
    CHECK(plain == ui::Action::None && menu->screen() == ui::Screen::Slots);
    menu = start_menu();
    if (!menu)
        return;
    menu->set_touch_is_remote(true);
    CHECK(tap_centre(*menu) == ui::Action::None);
    CHECK(menu->screen() == ui::Screen::Start);
    CHECK(menu->handle_event(finger(SDL_EVENT_FINGER_DOWN, 0.5f, 0.08f)));  // menu open: swallowed
}

void pump_into(ui::AppMenu& menu) {
    SDL_Event e;
    while (SDL_PollEvent(&e))
        menu.handle_event(e);
}

// The menus follow every controller, not only the first SDL lists (tvOS:
// the Siri Remote is always first, a controller did nothing in the menus).
// Holding X (Play/Pause on the Siri Remote) in a menu does not open ImGui's
// window switcher ("(Untitled)" mid-screen on the Apple TV, 2026-10-28).
void test_x_hold_no_window_switcher() {
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.name = "lgp virtual controller";
    desc.vendor_id = 0x1209;
    desc.product_id = 0x0014;
    const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
    SDL_Joystick* joy = SDL_OpenJoystick(id);
    CHECK(joy != nullptr);
    auto menu = in_game_menu();
    if (!menu || joy == nullptr)
        return;
    pump_into(*menu);
    menu->show(ui::Screen::Pause);
    for (int i = 0; i < 3; ++i)
        menu->frame();
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_WEST, true);
    bool switcher = false;
    for (int i = 0; i < 30; ++i) {  // longer than ImGui's list delay (0.15 s)
        pump_into(*menu);
        menu->frame();
        SDL_Delay(10);
        switcher = switcher || ImGui::GetCurrentContext()->NavWindowingTarget != nullptr;
    }
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_WEST, false);
    pump_into(*menu);
    menu->frame();
    CHECK(!switcher);
    CHECK(menu->screen() == ui::Screen::Pause);
    menu.reset();
    SDL_CloseJoystick(joy);
    SDL_DetachVirtualJoystick(id);
}

void test_second_pad_drives_menu() {
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = 2;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_LEFTX) | (1u << SDL_GAMEPAD_AXIS_LEFTY);
    desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_SOUTH) | (1u << SDL_GAMEPAD_BUTTON_EAST) |
                       (1u << SDL_GAMEPAD_BUTTON_WEST);
    desc.name = "lgp virtual siri remote";
    desc.vendor_id = 0x1209;
    desc.product_id = 0x0013;
    const SDL_JoystickID remote = SDL_AttachVirtualJoystick(&desc);
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.name = "lgp virtual controller";
    desc.vendor_id = 0x1209;
    desc.product_id = 0x0014;
    SDL_JoystickID controller = SDL_AttachVirtualJoystick(&desc);
    CHECK(remote != 0 && controller != 0);
    SDL_Joystick* joy = SDL_OpenJoystick(controller);
    CHECK(joy != nullptr);
    auto menu = start_menu();
    if (!menu || joy == nullptr)
        return;
    pump_into(*menu);
    ui::Action a = ui::Action::None;
    // Down twice (Weiterspielen -> Spielstand wählen -> Einstellungen), then A.
    for (int step = 0; step < 2; ++step) {
        SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_DPAD_DOWN, true);
        pump_into(*menu);
        menu->frame();
        SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_DPAD_DOWN, false);
        pump_into(*menu);
        menu->frame();
    }
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_SOUTH, true);
    pump_into(*menu);
    for (int i = 0; i < 3 && a == ui::Action::None; ++i)
        a = menu->frame();
    SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_SOUTH, false);
    pump_into(*menu);
    for (int i = 0; i < 3 && a == ui::Action::None; ++i)
        a = menu->frame();
    CHECK(a == ui::Action::None);
    CHECK(menu->screen() == ui::Screen::Settings);
    menu.reset();
    // A controller that connects while the game runs drives the pause too.
    SDL_CloseJoystick(joy);
    SDL_DetachVirtualJoystick(controller);
    menu = in_game_menu();
    if (!menu)
        return;
    pump_into(*menu);
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.name = "lgp virtual controller";
    desc.vendor_id = 0x1209;
    desc.product_id = 0x0014;
    controller = SDL_AttachVirtualJoystick(&desc);
    joy = SDL_OpenJoystick(controller);
    CHECK(joy != nullptr);
    if (joy == nullptr)
        return;
    pump_into(*menu);  // the game runs: the menu only notes the new pad
    menu->show(ui::Screen::Pause);
    for (int i = 0; i < 3; ++i)
        menu->frame();
    a = ui::Action::None;
    // Down and up put the nav cursor on "Fortsetzen", then A.
    for (SDL_GamepadButton b : {SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_SOUTH}) {
        SDL_SetJoystickVirtualButton(joy, b, true);
        pump_into(*menu);
        for (int i = 0; i < 3 && a == ui::Action::None; ++i)
            a = menu->frame();
        SDL_SetJoystickVirtualButton(joy, b, false);
        pump_into(*menu);
        for (int i = 0; i < 3 && a == ui::Action::None; ++i)
            a = menu->frame();
    }
    CHECK(a == ui::Action::Resume);  // first item of the pause screen
    menu.reset();
    SDL_CloseJoystick(joy);
    SDL_DetachVirtualJoystick(controller);
    SDL_DetachVirtualJoystick(remote);
}

// A tap (here a mouse click) on a settings row only selects it: values
// change by the arrows or left/right, not by a tap on the row's name.
void test_settings_tap_selects() {
    std::string error;
    auto menu = ui::AppMenu::create(g_window, &error);
    CHECK(menu != nullptr);
    if (!menu)
        return;
    menu->set_touch_layout_supported(true);
    menu->show(ui::Screen::Settings);
    menu->frame();
    menu->frame();
    const auto click = [&](float x, float y) {
        ui::Action seen = ui::Action::None;
        for (const SDL_EventType type : {SDL_EVENT_MOUSE_MOTION, SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_UP}) {
            SDL_Event e{};
            e.type = type;
            if (type == SDL_EVENT_MOUSE_MOTION) {
                e.motion.windowID = SDL_GetWindowID(g_window);
                e.motion.x = x, e.motion.y = y;
            } else {
                e.button.windowID = SDL_GetWindowID(g_window);
                e.button.button = SDL_BUTTON_LEFT;
                e.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
                e.button.clicks = 1;
                e.button.x = x, e.button.y = y;
            }
            menu->handle_event(e);
            if (const ui::Action a = menu->frame(); a != ui::Action::None)
                seen = a;
        }
        return seen;
    };
    // Down the left half of the panel (the names), on every row of page 1
    // ("Steuerung": on/off rows and choices) until a click leaves the screen.
    int clicks = 0;
    for (float y = 0; y < 720 && menu->screen() == ui::Screen::Settings; y += 6, ++clicks)
        CHECK(click(1280 * 0.4f, y) != ui::Action::SettingsChanged);
    CHECK(clicks > 20);
    CHECK(menu->settings().touch);
    CHECK(!menu->settings().touch_mirrored);
}

// Page "Game": the Wi-Fi and the server's and the room's text rows (online is
// always on, no switch of its own); A or an arrow on a text row
// opens the text screen, its OK goes back to the settings as SettingsChanged
// (a server the check refuses reopens the field with the note), cancel keeps
// the old text.
void test_settings_online_rows() {
    std::string error;
    auto menu = ui::AppMenu::create(g_window, &error);
    CHECK(menu != nullptr);
    if (!menu)
        return;
    menu->set_server_check([](const std::string& t) { return t == "bad" ? std::string("so nicht") : std::string(); });
    menu->settings().ldn_server = "bad";
    menu->show(ui::Screen::Settings);
    menu->frame();
    menu->frame();
    const auto tap = [&](SDL_Keycode code, SDL_Scancode scancode) {
        SDL_Event down = key(SDL_EVENT_KEY_DOWN, code);
        down.key.scancode = scancode;
        menu->handle_event(down);
        const ui::Action action = menu->frame();
        SDL_Event up = key(SDL_EVENT_KEY_UP, code);
        up.key.scancode = scancode;
        menu->handle_event(up);
        menu->frame();
        return action;
    };
    for (int i = 0; i < 3; ++i)
        tap(SDLK_E, SDL_SCANCODE_E);
    CHECK_EQ(menu->settings_page(), 3);
    CHECK(menu->settings().ldn_lan);
    CHECK(tap(SDLK_DOWN, SDL_SCANCODE_DOWN) == ui::Action::None);  // "Server"
    CHECK(tap(SDLK_RIGHT, SDL_SCANCODE_RIGHT) == ui::Action::None);
    CHECK(menu->screen() == ui::Screen::Text);
    for (int i = 0; i < 3; ++i)
        menu->frame();
    menu->press(ui::Action::TextOk);
    CHECK(menu->frame() == ui::Action::None);  // refused: the same field again
    CHECK(menu->screen() == ui::Screen::Text);
    for (int i = 0; i < 3; ++i)
        menu->frame();
    SDL_Event typed{};
    typed.type = SDL_EVENT_TEXT_INPUT;
    typed.text.windowID = SDL_GetWindowID(g_window);
    typed.text.text = "x";
    menu->handle_event(typed);
    menu->frame();
    menu->frame();
    menu->press(ui::Action::TextOk);
    CHECK(menu->frame() == ui::Action::SettingsChanged);
    CHECK(menu->screen() == ui::Screen::Settings);
    CHECK(menu->settings().ldn_server == "x");  // the focused field selects its text: typing replaces it
    menu->frame();  // the cursor back on "Server"
    menu->frame();
    CHECK(tap(SDLK_DOWN, SDL_SCANCODE_DOWN) == ui::Action::None);  // "Raum"
    CHECK(tap(SDLK_LEFT, SDL_SCANCODE_LEFT) == ui::Action::None);
    CHECK(menu->screen() == ui::Screen::Text);
    menu->frame();
    menu->press(ui::Action::TextCancel);
    CHECK(menu->frame() == ui::Action::None);
    CHECK(menu->screen() == ui::Screen::Settings);
    CHECK(menu->settings().ldn_room.empty());
    // A request of the game replaces an open settings field: its OK is the game's.
    CHECK(tap(SDLK_RIGHT, SDL_SCANCODE_RIGHT) == ui::Action::None);
    ui::TextPrompt p;
    p.initial = "Red";
    menu->show_text(p);
    menu->frame();
    menu->press(ui::Action::TextOk);
    CHECK(menu->frame() == ui::Action::TextOk);
    CHECK(menu->settings().ldn_room.empty());
}

// iPhone/iPad (set_system_settings): controller, graphics and link rows are
// left out; "Picture" and "Game" offer the Settings app instead.
void test_settings_in_system_app() {
    std::string error;
    auto menu = ui::AppMenu::create(g_window, &error);
    CHECK(menu != nullptr);
    if (!menu)
        return;
    menu->set_touch_layout_supported(true);
    menu->set_system_settings(true);
    menu->settings().mods.push_back({"widescreen", "Widescreen", "", true, false});
    menu->show(ui::Screen::Settings);
    menu->frame();
    menu->frame();
    const auto tap = [&](SDL_Keycode code, SDL_Scancode scancode) {
        SDL_Event down = key(SDL_EVENT_KEY_DOWN, code);
        down.key.scancode = scancode;
        menu->handle_event(down);
        ui::Action action = menu->frame();
        SDL_Event up = key(SDL_EVENT_KEY_UP, code);
        up.key.scancode = scancode;
        menu->handle_event(up);
        if (const ui::Action a = menu->frame(); action == ui::Action::None)
            action = a;
        return action;
    };
    // "Picture": format, TV mode, smoothing, the battle's HUD bounds, then the button.
    menu->set_picture_aspect(1568.0f / 720.0f);  // the iPhone's
    tap(SDLK_E, SDL_SCANCODE_E);
    tap(SDLK_E, SDL_SCANCODE_E);
    CHECK_EQ(menu->settings_page(), 2);
    for (int i = 0; i < 4; ++i)
        CHECK(tap(SDLK_DOWN, SDL_SCANCODE_DOWN) == ui::Action::None);
    CHECK(tap(SDLK_RETURN, SDL_SCANCODE_RETURN) == ui::Action::OpenSystemSettings);
    CHECK(!menu->settings().effects_half && menu->settings().render_scale == 0 && !menu->settings().async_execute);
    // "Game" without other mods (widescreen is a picture format): the game's language, the main menu at start, then the button.
    tap(SDLK_E, SDL_SCANCODE_E);
    CHECK_EQ(menu->settings_page(), 3);
    for (int i = 0; i < 2; ++i)
        CHECK(tap(SDLK_DOWN, SDL_SCANCODE_DOWN) == ui::Action::None);
    CHECK(tap(SDLK_RETURN, SDL_SCANCODE_RETURN) == ui::Action::OpenSystemSettings);
    CHECK(menu->settings().ldn_lan);
}

}  // namespace

// On a touch screen the settings with the hint bar (no "Back" row) have a
// round back button in the top left corner: a tap there goes back, or only
// closes an open choice list (press and release in separate frames, as on a
// device: the list closes on the press already).
void test_settings_corner_back() {
    int w = 0, h = 0;
    SDL_GetWindowSize(g_window, &w, &h);
    const float r = std::max(18.0f, 0.04f * float(h));
    const float x = r + std::max(12.0f, 0.02f * float(w)), y = 0.08f * float(h);
    const auto tap_corner = [&](ui::AppMenu& menu) {
        for (const SDL_EventType type : {SDL_EVENT_MOUSE_MOTION, SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_UP}) {
            SDL_Event e{};
            e.type = type;
            if (type == SDL_EVENT_MOUSE_MOTION) {
                e.motion.windowID = SDL_GetWindowID(g_window);
                e.motion.x = x, e.motion.y = y;
            } else {
                e.button.windowID = SDL_GetWindowID(g_window);
                e.button.button = SDL_BUTTON_LEFT;
                e.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
                e.button.clicks = 1;
                e.button.x = x, e.button.y = y;
            }
            menu.handle_event(e);
            menu.frame();
        }
        menu.frame();
    };
    const auto press = [&](ui::AppMenu& menu, SDL_Keycode code) {
        menu.handle_event(key(SDL_EVENT_KEY_DOWN, code));
        menu.frame();
        menu.handle_event(key(SDL_EVENT_KEY_UP, code));
        menu.frame();
    };
    std::string error;
    auto menu = ui::AppMenu::create(g_window, &error);
    CHECK(menu != nullptr);
    if (!menu)
        return;
    menu->set_touch_layout_supported(true);
    menu->set_button_hints(1);
    menu->set_back_button(1);
    menu->show(ui::Screen::Start);
    menu->show(ui::Screen::Settings);
    menu->frame();
    menu->frame();
    for (int i = 0; i < 6; ++i)
        press(*menu, SDLK_DOWN);  // "Controller" (three choices)
    press(*menu, SDLK_RETURN);     // its choice list
    tap_corner(*menu);
    CHECK(menu->screen() == ui::Screen::Settings);  // only the list closed
    tap_corner(*menu);
    CHECK(menu->screen() != ui::Screen::Settings);
    // Without it (default off a touch screen, as here) the same tap stays.
    auto plain = ui::AppMenu::create(g_window, &error);
    CHECK(plain != nullptr);
    if (!plain)
        return;
    plain->set_button_hints(1);
    plain->set_back_button(0);
    plain->show(ui::Screen::Settings);
    plain->frame();
    plain->frame();
    tap_corner(*plain);
    CHECK(plain->screen() == ui::Screen::Settings);
}

int main() {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    // Only video: the menu has to start the gamepad subsystem itself, as in
    // the app, where it runs before the game's input.
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::printf("skipped: no dummy video driver (%s)\n", SDL_GetError());
        return 0;
    }
    g_window = SDL_CreateWindow("menu", 1280, 720, 0);
    CHECK(g_window != nullptr);
    // Every test in both interface languages (lgp/platform/ui_text.hpp): the
    // menus must work the same with the German texts.
    for (const UiLanguage ui_language : {UiLanguage::English, UiLanguage::German}) {
        if (g_window == nullptr)
            break;
        set_ui_language(ui_language);
        std::printf("interface language: %s\n", ui_language_name(ui_language));
        RUN(test_menu_starts_gamepads);
        RUN(test_escape_opens_pause);
        RUN(test_remote_menu_key);
        RUN(test_pause_button);
        RUN(test_releases_reach_the_game);
        RUN(test_back_navigation);
        RUN(test_pad_start_and_back);
        RUN(test_remote_without_start);
        RUN(test_text_screen);
        RUN(test_remote_touch_is_no_click);
        RUN(test_second_pad_drives_menu);
        RUN(test_x_hold_no_window_switcher);
        RUN(test_loading_screen);
        RUN(test_settings_rows);
        RUN(test_throw_needs_balls);
        RUN(test_picture_format);
        RUN(test_settings_tap_selects);
        RUN(test_settings_online_rows);
        RUN(test_settings_in_system_app);
        RUN(test_settings_corner_back);
    }
    if (g_window != nullptr)
        SDL_DestroyWindow(g_window);
    SDL_Quit();
    return finish();
}
