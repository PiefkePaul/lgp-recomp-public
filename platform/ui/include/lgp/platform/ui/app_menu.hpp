// The app's own interface around the game (P7, docs/analysis/27-p7-app-oberflaeche.md):
// start screen, save slot choice, settings and the pause menu, the same on
// iPad, iPhone, Android and the desktop. Built with Dear ImGui on SDL3 events;
// the result is a neutral triangle list (lgp/platform/ui_draw.hpp) that the
// WebGPU renderer draws over the game image. Texts are English in the code
// and German on a German device (lgp/platform/ui_text.hpp); texts the caller
// passes in (slots, notes, mods) are already in the interface language.
//
// Everything here runs on the host's main thread (the one pumping SDL
// events). The menu holds no game state: the caller fills in the slots and
// settings, reacts to the returned actions and stores what changed.
#pragma once

#include <functional>
#include <memory>
#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

#include "lgp/platform/ui_draw.hpp"
#include "lgp/platform/ui/layout_skin.hpp"

struct SDL_Window;
union SDL_Event;
struct ImGuiContext;
struct ImDrawData;

struct ImDrawList;
struct ImVec2;

namespace lgp::platform::ui {

struct LayoutDraw;

struct MenuSlot {
    std::string name;    // "Save 1"
    std::string detail;  // "last played on Oct 14 at 08:12", "empty"
    bool empty = false;  // no save data yet: "New game" instead of "Continue"
};

// A game mod on the settings screen (lgp/mods/mods.hpp; the app fills in
// the list, the menu only flips `on`).
struct MenuMod {
    std::string id;
    std::string title;        // "Wild battles"
    std::string description;  // one line under the switch
    bool on = false;
    bool live = false;  // switched while the game runs (else in game: "from the next start")
};

// What the settings screen edits (the app stores it in settings.ini). A new
// option: a field here, one row in settings_pages() (app_menu.cpp; a page per
// topic, switched with L/R) and its line in settings.ini
// (runtime/boot/src/main.cpp: menu_settings, store_menu_changes).
struct MenuSettings {
    bool touch = true;
    int touch_size = 100;     // percent
    int touch_opacity = 100;  // percent
    bool touch_mirrored = false;
    bool touch_throw = true;  // a swipe upwards throws a Poké Ball
    bool touch_throw_pad = false;  // … also while a gamepad plays player 1
    int volume = 100;         // percent
    bool docked = false;      // TV mode (the game renders 1080p) instead of handheld (720p)
    int controller = 0;       // 0 handheld, 1 Joy-Con right, 2 Joy-Con left
    bool player1_by_press = true;  // player 1: the first gamepad pressed (else connection order)
    int aspect = 0;           // picture format: 0 fit, 1 fill, 2 stretch (lgp::platform::AspectMode)
    // With the widescreen mod in `mods` the picture format row shows
    // 0 widescreen (the mod on, aspect fit), 1-3 aspect 0-2 with the mod off;
    // the menu keeps both in step (only it uses these two).
    int picture_format = 0;
    int picture_format_seen = -1;
    bool ldn_lan = true;      // trade and battle over the Wi-Fi (--ldn lan); from the next start in game
    // Online is always on through a RyuLDN server (with the mod online-button):
    // its address ("" or the build's: the default) and an optional room text
    // (only devices with the same text find each other); from the next start in game.
    std::string ldn_server;
    std::string ldn_room;
    // Graphics (WebGPU renderer); in game from the next start:
    bool effects_half = false;  // the expensive light effects at half resolution (--effects half)
    int render_scale = 0;       // render targets at 0: 100 %, 1: 75 %, 2: 50 % (--render-scale)
    bool async_execute = false;  // graphics work on a thread of its own (--async-execute)
    int smoothing = 0;  // scaling to the screen: 0 sharp, 1 soft, 2 FXAA (--smoothing; at once)
    // The battle's HUD bounds with the widescreen mod (Screen::HudBounds):
    // how far the displays on the battle screen move from the 16:9 frame
    // towards the edge of a taller (height) or wider (width) picture, in
    // percent of the way, opposite sides alike; 0 = the 16:9 frame (default,
    // the least). Everything else always goes to the edge. --hud-sides
    // H,H,W,W; from the next start.
    int hud_height = 0;
    int hud_width = 0;
    std::vector<MenuMod> mods;  // in game: live ones at once, the rest from the next start
    // The game's language, from the next start: 0 the device's, else
    // kMenuLanguages[language] (settings.ini language =, the same order as
    // lgp::boot::kGameLanguages after the device row).
    int language = 0;
    // The main menu before the game, from the next start (settings.ini
    // start_screen); off: the game starts with the last save.
    bool start_screen = false;
    // The game's controller screen at every start (settings.ini
    // controller_screen); off: the mod skip-controller-screen, from the next start.
    bool controller_screen = false;
    // Let's Go without visible field Pokémon (settings.ini hide_field_pokemon):
    // a row under the mod random-encounters ("Pokémon Yellow"), only while that
    // is off; the hidden ones can still be touched. Takes effect at once.
    bool hide_field_pokemon = false;
};

// Rows of the language choice: "Device", then the game's languages (English;
// the menu shows them through lgp/platform/ui_text.hpp).
extern const char* const kMenuLanguages[13];

enum class Screen {
    None,      // only the pause button over the game (in_game), else nothing
    Start,     // before the game: continue, slots, settings, quit
    Slots,     // save slot choice
    Settings,
    Pause,     // the game stands still
    Text,      // the game asks for text (software keyboard) or shows a message
    Loading,   // before the game: graphics prepared ahead (set_progress()); "Start now" = Play
    HudBounds,  // from the settings: the battle's HUD bounds as lines over the picture
};

// What the game's software keyboard (swkbd) or error applet shows, for
// Screen::Text where the platform has no keyboard window of its own (iOS,
// Android: one full-screen window).
struct TextPrompt {
    std::string header, sub, guide;  // guide: the grey hint in an empty field
    std::string initial;
    std::string note;  // why the last answer was not taken
    uint32_t max_length = 0;  // UTF-16 units, 0 = none
    bool password = false;
    bool message = false;  // a message with OK only (header = code, sub = text)
};

enum class Action {
    None,
    Play,             // start the game with slot()
    NewSlot,          // the caller creates a slot and calls set_slots()
    OpenPause,        // the pause button was tapped: pause the game
    Resume,           // close the pause menu and let the game run
    Backup,           // copy the current slot (set_status() with the result)
    QuitGame,         // end the game (the app ends with it)
    Quit,             // leave the app from the start screen
    SettingsChanged,  // settings() changed; apply what can change at once
    TextOk,           // Screen::Text confirmed: text_value() (the caller may show_text() again with a note)
    TextCancel,       // Screen::Text cancelled
    PickPlayer1,      // in the game: the next button press on a gamepad chooses player 1
    LoadCloudSaves,   // start screen: take the other iCloud state (set_cloud_choice())
    OpenSystemSettings,  // the system's Settings app at the app's page (set_system_settings())
};

class AppMenu {
public:
    // nullptr and *error if the UI cannot start (no SDL window).
    static std::unique_ptr<AppMenu> create(SDL_Window* window, std::string* error);
    ~AppMenu();
    AppMenu(const AppMenu&) = delete;
    AppMenu& operator=(const AppMenu&) = delete;

    void show(Screen screen);
    Screen screen() const { return screen_; }
    // In the game: Screen::None shows the pause button, Settings returns to
    // Pause instead of Start.
    void set_in_game(bool in_game) { in_game_ = in_game; }

    void set_slots(std::vector<MenuSlot> slots, int current);
    int slot() const { return slot_; }
    // A tap on a slot (also on the one already selected): the user's choice.
    void choose_slot(int index);
    bool slot_chosen() const { return slot_chosen_; }
    MenuSettings& settings() { return settings_; }
    // The settings screen greys out what the build cannot change yet.
    void set_touch_layout_supported(bool supported) { touch_layout_supported_ = supported; }
    // iPhone/iPad: controller, graphics and the link rows (the settings that
    // need a restart) live in the Settings app (lgp/platform/system_settings.hpp);
    // the settings screen leaves them out and offers a button there instead.
    void set_system_settings(bool on) { system_settings_ = on; }
    // "Back up save" only where the caller can stop the whole game first.
    void set_backup_available(bool available) { backup_available_ = available; }
    // The settings screen in the game: the gamepad that plays player 1 ("": none).
    void set_player1_name(std::string name) { player1_name_ = std::move(name); }
    // Opens Screen::Text over whatever is shown; OK or cancel return to it.
    void show_text(TextPrompt prompt);
    // Checks the server a user typed on the settings screen: "" when it is
    // fine, else the note shown under the field (unset: everything goes).
    void set_server_check(std::function<std::string(const std::string&)> check) { server_check_ = std::move(check); }
    const std::string& text_value() const { return text_; }
    // After a TextOk the caller refused (too short or too long): back to the
    // same field with its text and a note, without a fresh request (a fresh
    // field restarts the system keyboard).
    void reopen_text(std::string note);
    // Screen::Loading: how many of the pipelines to prepare are done.
    void set_progress(size_t done, size_t total) {
        if (done != progress_done_ || total != progress_total_)
            dirty_ = true;
        progress_done_ = done;
        progress_total_ = total;
    }
    // One line under the current screen ("Backed up to …").
    void set_status(std::string text) { status_ = std::move(text); }
    // A grey line at the foot of the start screen ("Game: included in the app").
    void set_game_source(std::string text) { game_source_ = std::move(text); }
    // Start screen: whether the saves are backed up (tvOS iCloud); empty: no line.
    void set_cloud_note(std::string text) { cloud_note_ = std::move(text); }
    // Start screen: iCloud has another state than this device kept; the text
    // under "Load iCloud state" (empty: no button).
    void set_cloud_choice(std::string text) { cloud_choice_ = std::move(text); }

    // Every SDL event goes here first. True: the menu used it and the game
    // must not see it (events while a screen is open; over the game only
    // touches and clicks on the pause button, and Escape or the controller's
    // Home button, which open the pause menu, as does holding the TV
    // remote's menu key, or on Android and tvOS the controller's Start (X
    // without Start), for
    // kMenuHoldNs). With a screen open, Escape, the back key (Android), Home,
    // the controller's Back and Start, and the button labelled B outside
    // popups and edited widgets go one screen back; from the pause menu that
    // resumes the game.
    // Releases (keys, buttons, fingers) and stick motion are used and still
    // returned as false, so the game never keeps a button held across the menu.
    bool handle_event(const SDL_Event& event);
    static bool passes_through(const SDL_Event& event);
    // Builds this frame's interface; returns what the user chose.
    Action frame();
    // Scripted runs and tests: the next frame() returns `action` as if its
    // button had been tapped on the current screen.
    void press(Action action) { pressed_ = action; dirty_ = true; }
    // Holding the controller's Start (X on a pad without Start, the Siri
    // Remote) opens the pause (default on Android and tvOS, where the
    // controller's Home stays with the system).
    void set_start_hold_pause(bool on) { start_hold_pause_ = on; }
    // Touches are a remote's touch surface, not the screen (default on
    // tvOS): finger events and the mouse clicks SDL makes from them never
    // reach the menu, which the remote drives as a gamepad instead.
    void set_touch_is_remote(bool on) { touch_is_remote_ = on; }
    // The game's own font (a BFOTF/BFTTF from its RomFS, or a plain
    // OpenType/TrueType file) for all text from the next frame on; false and
    // *error if it is none. Without it the menus keep the system font.
    bool set_game_font(std::vector<uint8_t> file, std::string* error);
    // The line of controller hints (A, B, L/R) under each screen: -1 while a
    // controller is connected and always in the game's look, where "B" also
    // takes a tap (default), 0 never, 1 always (previews).
    void set_button_hints(int mode) { hints_ = mode; dirty_ = true; }
    // The settings' round back button in the top left corner while the hint
    // bar stands in for the "Back" row: -1 on a touch screen (default), 0
    // never, 1 always (tests, previews).
    void set_back_button(int mode) { back_button_ = mode; dirty_ = true; }
    // The aspect ratio (width / height) of the game's picture, for the HUD
    // bounds screen: in the game the widescreen mod's (16/9 without it),
    // before it 0 = the window's, as the mod will size it.
    void set_picture_aspect(float aspect) { picture_aspect_ = aspect; }
    // While the widescreen mod runs the TV mode row applies from the next
    // start (its help says so; the app does not switch).
    void set_docked_next_start(bool next_start) { docked_next_start_ = next_start; }
    // The settings screen's page (tab), from 0.
    void set_settings_page(int page) { settings_page_ = page; dirty_ = true; }
    int settings_page() const { return settings_page_; }
    // A picture for the menus' own drawing (one of the game's menu graphics,
    // RGBA8): its index for image_id(). The menu keeps it until clear_images().
    int add_image(uint32_t width, uint32_t height, std::vector<uint8_t> rgba);
    void clear_images();
    // The game's menu parts (from its RomFS, layout_skin.hpp); nullptr: the
    // menus' own look.
    void set_skin(std::shared_ptr<const LayoutSkin> skin);
    // Diagnostics: shows only layout `layout` of the skin's archive
    // `archive`, filling the screen, its text panes labelled with their names.
    void set_layout_preview(std::string archive, std::string layout);
    // The id ImGui draws add_image()'s picture `index` with.
    static uint64_t image_id(int index) { return uint64_t(index) + 2; }
    // The last frame's triangles (nullptr: nothing to draw).
    std::shared_ptr<const UiDrawData> draw_data() const { return draw_data_; }
    // True while frame() should run every display frame (a screen is open,
    // the pause button needs drawing once, or the menu key or Start is held).
    bool wants_frames() const { return screen_ != Screen::None || dirty_ || menu_down_ns_ != 0; }

private:
    AppMenu() = default;
    bool over_pause_button(float x, float y) const;  // window points
    // The window's safe area in window points (SDL_GetWindowSafeArea): panels
    // and the pause button stay inside it, the dimmed background does not.
    struct SafeRect {
        float x0, y0, x1, y1;
    };
    SafeRect safe_rect() const;
    void corner_back(const SafeRect& safe);
    void tap_back();
    Action draw_screen();
    // Screen::HudBounds over the whole window; Settings again on apply/discard.
    Action draw_hud_bounds(const ImVec2& size, const SafeRect& safe);
    void open_hud_bounds();
    void leave_hud_bounds();
    void convert(ImDrawData* data);

    SDL_Window* window_ = nullptr;
    ImGuiContext* context_ = nullptr;
    Screen screen_ = Screen::None;
    Screen settings_return_ = Screen::Start;
    bool in_game_ = false;
    bool dirty_ = true;
    size_t progress_done_ = 0, progress_total_ = 0;
    bool pause_requested_ = false;
    // A value chosen from a list over the settings (the game's choice
    // window): the option (page, index), the list's cursor, the row it
    // opened from; cancel from Back/Escape (handle_event).
    bool choice_open_ = false, choice_cancel_ = false, choice_focus_ = false;
    int choice_page_ = 0, choice_option_ = 0, choice_cursor_ = 0;
    float choice_row_[4] = {0, 0, 0, 0};
    std::vector<Layout> choice_states_;  // button_option_00: unselected, selected
    // `tab`: a page tab (label centred, no cursor). `fit_like`: shrink the
    // label as far as this (longer) one would be, so a row of buttons keeps
    // one text size.
    void draw_choice(ImDrawList* dl, float x0, float y0, float x1, float y1, bool selected, const char* label,
                     bool tab = false, const char* fit_like = nullptr);
    // The game's choice button: its width per unit of height and its
    // text's share of that width (false: the skin has none).
    bool choice_shape(float* width, float* text) const;
    bool back_requested_ = false;  // Escape, back key, Home, pad Back/Start/B while a screen is open
    // TV remote menu key or controller Start: a short press is Plus for the
    // game, a long one the pause.
    static constexpr uint64_t kMenuHoldNs = 700'000'000;
    uint64_t menu_down_ns_ = 0;
    bool start_hold_pause_ = false;
    bool touch_is_remote_ = false;
    bool gamepads_ = false;  // SDL_INIT_GAMEPAD started by create(), quit by the destructor
    Action pressed_ = Action::None;
    std::vector<MenuSlot> slots_;
    int slot_ = 0;
    bool slot_chosen_ = false;
    MenuSettings settings_;
    bool touch_layout_supported_ = false;
    bool system_settings_ = false;
    bool backup_available_ = true;
    bool pointer_seen_ = false;  // "menu: first touch" logged
    TextPrompt prompt_;
    int prompt_id_ = 0;  // a fresh field per request: ImGui keeps an active field's own text
    int text_log_lines_ = 0;  // keyboard diagnostics printed so far
    int back_log_lines_ = 0;  // "menu: back by …" lines printed so far
    std::string text_;
    Screen text_return_ = Screen::None;
    // A text row of the settings screen being edited on Screen::Text (OK
    // stores text_ there and returns SettingsChanged instead of TextOk).
    std::string* text_edit_ = nullptr;
    bool text_edit_server_ = false;
    const std::string* text_focus_row_ = nullptr;  // that row takes the cursor back
    int text_focus_frames_ = 0;
    bool text_focus_seen_ = false;
    std::function<std::string(const std::string&)> server_check_;
    bool text_focus_ = false;  // focus the field (and so the on-screen keyboard) next frame
    std::string status_;
    std::string game_source_;
    std::string cloud_note_;
    std::string cloud_choice_;
    std::string player1_name_;
    std::shared_ptr<UiTexture> texture_;
    std::vector<std::shared_ptr<const UiTexture>> images_;
    std::shared_ptr<const LayoutSkin> skin_;
    std::vector<std::vector<int>> skin_images_;  // [archive][image] -> add_image() index
    std::vector<std::pair<std::string, int>> tinted_;  // key -> add_image() index
    std::string preview_archive_, preview_layout_;
    bool draw_layout_preview();
    bool preview_warned_ = false;
    std::vector<Layout> row_states_;  // the skin's row: unselected, selected
    // Rows and list frame wider than the game's (widened(), by the screen),
    // kept while the width stays.
    std::vector<Layout> wide_rows_;
    std::vector<std::tuple<float, int, Layout>> wide_tabs_;  // width change, state, layout
    float wide_rows_dx_ = -1;
    Layout wide_list_;
    float wide_list_dx_ = -1;
    // The background's looping animations (its gears), run while it shows.
    std::vector<const LayoutAnimation*> bg_loops_;
    // The background as its "in" animation leaves it (empty: no such animation).
    std::vector<Layout> bg_base_;
    int skin_image(std::string_view name) const;
    // The game's light band behind a selection (system_window_button_00,
    // P_BG_gradation_00) from x0 to x1 around y0..y1; false without it.
    bool draw_band(ImDrawList* dl, float x0, float x1, float y0, float y1);
    // The game's context menu (context_00: frame, fill, the marked row with
    // its arrow) cut to `rows` choices: its layout, the distance between its
    // rows (layout units); nullptr without it.
    const Layout* context_layout(int rows, float* pitch);
    Layout context_base_;  // context_00 after its in-animation (empty: none)
    Layout context_cut_;
    int context_cut_rows_ = -1;
    LayoutDraw skin_painter(ImDrawList* list);
    int skin_texture(const Layout& layout, const LayoutMaterial& material, float tint[4]);
    std::shared_ptr<const UiDrawData> draw_data_;
    float scale_ = 1.0f;  // UI size factor (points per design unit)
    float font_size_ = 18.0f;
    bool overflow_ = false;  // the panel did not fit last frame: show a scroll bar
    int hints_ = -1;
    int back_button_ = -1;
    bool press_in_choice_ = false;  // the last press began with a choice list open
    int settings_page_ = 0;
    float picture_aspect_ = 0;
    bool docked_next_start_ = false;
    int hud_edit_[2] = {0, 0};  // height, width while Screen::HudBounds is open
    int hud_drag_ = -1;         // the line pair a finger or the mouse moves: 0 height, 1 width
};

}  // namespace lgp::platform::ui
