#include "lgp/platform/ui/app_menu.hpp"

#include "lgp/platform/ui/game_font.hpp"
#include "lgp/platform/screen_fit.hpp"
#include "lgp/platform/ui_text.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <iterator>

#define IMGUI_DEFINE_MATH_OPERATORS
#include "backends/imgui_impl_sdl3.h"
#include "imgui.h"
#include "imgui_internal.h"  // ClearActiveID (a scrolling finger lets go of the button under it), SetFocusID
#include "layout_draw.hpp"

namespace lgp::platform::ui {

const char* const kMenuLanguages[13] = {
    "Device",  "German",          "English (US)", "English (UK)", "French", "French (Canada)", "Italian", "Spanish",
    "Spanish (Latin America)", "Japanese", "Korean", "Chinese (simplified)", "Chinese (traditional)"};

namespace {

// Colours (straight alpha), after the menus of Let's Go Pikachu (own values,
// no game assets): white panels with a dark outline under a Poke Ball red
// header, Pikachu yellow behind the selected row, dark ink for text.
constexpr ImVec4 kPaper{1.0f, 1.0f, 1.0f, 1.0f};
constexpr ImVec4 kRow{0.95f, 0.95f, 0.93f, 1.0f};
constexpr ImVec4 kInk{0.16f, 0.16f, 0.19f, 1.0f};
constexpr ImVec4 kTextDim{0.42f, 0.42f, 0.46f, 1.0f};
constexpr ImVec4 kRed{0.89f, 0.21f, 0.05f, 1.0f};
constexpr ImVec4 kYellow{1.0f, 0.84f, 0.20f, 1.0f};
constexpr ImVec4 kNote{0.72f, 0.17f, 0.03f, 1.0f};  // status lines and notes: dark red, readable on white

// Text input as the keyboard request wants it. SDL starts text input with
// auto-correction, suggestions and multi-line on; on phones and tablets (also
// with a hardware keyboard) the system then rewrites typed names. The
// backend's IME hook is wrapped: text input is started here with plain
// properties first, the backend only places the input area.
using ImeFn = void (*)(ImGuiContext*, ImGuiViewport*, ImGuiPlatformImeData*);
ImeFn g_backend_ime = nullptr;
bool g_ime_password = false;  // the open request hides its text

bool g_ime_wanted = false;    // the last IME data asked for text input

// Bytes of the longest whole-character prefix of UTF-8 text that fits in
// max UTF-16 units (as the game counts them).
size_t utf16_prefix(const char* text, size_t size, uint32_t max) {
    size_t units = 0, i = 0;
    while (i < size) {
        const auto b = static_cast<uint8_t>(text[i]);
        const size_t n = b < 0x80 ? 1 : b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : 2;
        const size_t u16 = n == 4 ? 2 : 1;
        if (units + u16 > max)
            break;
        units += u16;
        i += n;
    }
    return std::min(i, size);
}

// Starts text input with the plain properties when the last IME data wants it
// and it is off. Also runs before the backend's NewFrame: that one restarts
// input stopped by a focus change or a type switch with SDL's defaults
// (auto-correction on) unless it is already active again. Hiding the iOS
// keyboard keeps text input on; on Android hiding it with Back stops it, and
// while the field is open the keyboard then comes back (Verdacht, not tried).
void start_text_input() {
    SDL_Window* window = SDL_GetKeyboardFocus();
    if (window == nullptr || !g_ime_wanted || SDL_TextInputActive(window))
        return;
    const SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetNumberProperty(props, SDL_PROP_TEXTINPUT_TYPE_NUMBER,
                          g_ime_password ? SDL_TEXTINPUT_TYPE_TEXT_PASSWORD_HIDDEN : SDL_TEXTINPUT_TYPE_TEXT);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTINPUT_CAPITALIZATION_NUMBER, SDL_CAPITALIZE_NONE);
    SDL_SetBooleanProperty(props, SDL_PROP_TEXTINPUT_AUTOCORRECT_BOOLEAN, false);
    SDL_SetBooleanProperty(props, SDL_PROP_TEXTINPUT_MULTILINE_BOOLEAN, false);  // Return = done
    SDL_StartTextInputWithProperties(window, props);
    SDL_DestroyProperties(props);
    std::printf("menu: text input started (hardware keyboard: %s)\n", SDL_HasKeyboard() ? "yes" : "no");
}

void set_ime_data(ImGuiContext* ctx, ImGuiViewport* viewport, ImGuiPlatformImeData* data) {
    g_ime_wanted = data->WantVisible || data->WantTextInput;
    start_text_input();
    if (g_backend_ime != nullptr)
        g_backend_ime(ctx, viewport, data);
#if defined(SDL_PLATFORM_IOS) || defined(SDL_PLATFORM_ANDROID)
    // The field sits at the top of the screen, above any keyboard. Report the
    // input area there too: from the area SDL shifts the whole view up while
    // the system keyboard (or the shortcut bar of a hardware keyboard) shows,
    // and back down when it hides; on the iPad the dialog jumped up and down.
    SDL_Window* window = SDL_GetKeyboardFocus();
    if (window != nullptr && g_ime_wanted) {
        const SDL_Rect top{0, 0, 1, 1};
        SDL_SetTextInputArea(window, &top, 0);
    }
#endif
}

// Fonts with German umlauts, by platform; the first one that loads wins,
// else ImGui's own scalable font. Bold faces first: the game's menus use a
// heavy, rounded type. Paths on iOS and Android are not checked on a device
// yet (Verdacht: readable from the app sandbox).
const char* const kFontFiles[] = {
#if defined(__ANDROID__)
    "/system/fonts/Roboto-Bold.ttf",
    "/system/fonts/Roboto-Regular.ttf",
#elif defined(__APPLE__)
    "/System/Library/Fonts/Supplemental/Arial Rounded Bold.ttf",
    "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
    "/System/Library/Fonts/Supplemental/Arial.ttf",
    "/System/Library/Fonts/Helvetica.ttc",
    "/System/Library/Fonts/Core/Helvetica.ttc",
    "/System/Library/Fonts/CoreUI/SFUI.ttf",
#elif defined(_WIN32)
    "C:/Windows/Fonts/segoeuib.ttf",
    "C:/Windows/Fonts/arialbd.ttf",
    "C:/Windows/Fonts/segoeui.ttf",
    "C:/Windows/Fonts/arial.ttf",
#else
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/dejavu/DejaVuSans.ttf",
#endif
};

bool file_exists(const char* path) {
    if (FILE* f = std::fopen(path, "rb")) {
        std::fclose(f);
        return true;
    }
    return false;
}

void apply_style(float font_size) {
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();
    ImGui::StyleColorsLight(&style);
    const float u = font_size / 18.0f;
    style.WindowRounding = 18 * u;
    style.FrameRounding = 10 * u;
    style.GrabRounding = 10 * u;
    style.WindowPadding = ImVec2(18 * u, 16 * u);
    style.FramePadding = ImVec2(12 * u, 9 * u);
    style.ItemSpacing = ImVec2(10 * u, 9 * u);
    style.ItemInnerSpacing = ImVec2(8 * u, 6 * u);
    style.GrabMinSize = 20 * u;
    style.ScrollbarSize = 12 * u;
    style.ScrollbarRounding = 6 * u;
    style.WindowBorderSize = 0;  // panel_frame() draws the outline
    style.FrameBorderSize = 2 * u;
    style.DisabledAlpha = 0.45f;
    style.FontSizeBase = font_size;
    ImVec4* c = style.Colors;
    c[ImGuiCol_WindowBg] = kPaper;
    c[ImGuiCol_Text] = kInk;
    c[ImGuiCol_TextDisabled] = kTextDim;
    c[ImGuiCol_Border] = kInk;
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_Button] = kRow;
    c[ImGuiCol_ButtonHovered] = kYellow;
    c[ImGuiCol_ButtonActive] = kYellow;
    c[ImGuiCol_FrameBg] = kPaper;
    c[ImGuiCol_FrameBgHovered] = kPaper;
    c[ImGuiCol_FrameBgActive] = kPaper;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(kTextDim.x, kTextDim.y, kTextDim.z, 0.5f);
    c[ImGuiCol_ScrollbarGrabHovered] = kTextDim;
    c[ImGuiCol_ScrollbarGrabActive] = kInk;
    c[ImGuiCol_TextSelectedBg] = ImVec4(kYellow.x, kYellow.y, kYellow.z, 0.7f);
    c[ImGuiCol_InputTextCursor] = kInk;
    c[ImGuiCol_NavCursor] = ImVec4(0, 0, 0, 0);  // the selected row is yellow instead
}

ImU32 col(const ImVec4& c, float alpha = 1.0f) {
    return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * alpha));  // also fades with BeginDisabled
}

float unit() { return ImGui::GetFontSize() / 18.0f; }

bool contains(const ImVec2& a, const ImVec2& b, const ImVec2& p) {
    return p.x >= a.x && p.x < b.x && p.y >= a.y && p.y < b.y;
}

bool has_touch_screen() {
    int n = 0;
    SDL_TouchID* ids = SDL_GetTouchDevices(&n);
    SDL_free(ids);
    return n > 0;
}

// A Poke Ball: red top, white bottom, the dark band and the button. `outline`
// only: one colour, for the backdrop.
void poke_ball(ImDrawList* dl, ImVec2 c, float r, ImU32 outline_col, bool outline) {
    const float t = std::max(1.5f, r * (outline ? 0.07f : 0.11f));
    if (!outline) {
        dl->PathArcTo(c, r, IM_PI, 2 * IM_PI, 24);
        dl->PathFillConvex(col(kRed));
        dl->PathArcTo(c, r, 0, IM_PI, 24);
        dl->PathFillConvex(col(kPaper));
    }
    dl->AddCircle(c, r - t * 0.5f, outline_col, 0, t);
    dl->AddLine(ImVec2(c.x - r + t * 0.5f, c.y), ImVec2(c.x - r * 0.36f, c.y), outline_col, t);
    dl->AddLine(ImVec2(c.x + r * 0.36f, c.y), ImVec2(c.x + r - t * 0.5f, c.y), outline_col, t);
    if (!outline)
        dl->AddCircleFilled(c, r * 0.3f, col(kPaper));
    dl->AddCircle(c, r * 0.3f, outline_col, 0, t);
    dl->AddCircleFilled(c, r * 0.13f, outline ? outline_col : col(kPaper, 1.0f));
}

// The backdrop before the game: Pikachu yellow in diagonal stripes, two faint
// Poke Balls.
void draw_backdrop(ImDrawList* bg, ImVec2 size, float u) {
    bg->AddRectFilled(ImVec2(0, 0), size, IM_COL32(255, 210, 40, 255));
    const float w = 46 * u;
    for (float x = 0; x < size.x + size.y; x += 2 * w)
        bg->AddQuadFilled(ImVec2(x, 0), ImVec2(x + w, 0), ImVec2(x + w - size.y, size.y), ImVec2(x - size.y, size.y),
                          IM_COL32(255, 224, 90, 255));
    poke_ball(bg, ImVec2(size.x * 0.88f, size.y * 0.82f), size.y * 0.42f, IM_COL32(255, 255, 255, 110), true);
    poke_ball(bg, ImVec2(size.x * 0.08f, size.y * 0.12f), size.y * 0.22f, IM_COL32(255, 255, 255, 90), true);
}

// Rows drawn with the game's own parts (AppMenu::set_skin): the row's
// rectangle, whether it has the cursor, its label and value (nullptr: a
// plain button). nullptr: the menus' own look.
struct SkinRows {
    std::function<void(ImDrawList*, ImVec2 a, ImVec2 b, bool selected, const char* label, const std::string* value)>
        draw;
    float height = 44;
    float width = 0;  // of the panel
    // The value's span between the arrows of a row drawn at a..b (after
    // draw); false: none.
    std::function<bool(ImVec2 a, ImVec2 b, float* x0, float* x1)> value_span;
    // A page tab a..b as the game's row frame with the label in the middle;
    // the other pages' tabs faded (the yellow frame stays the row cursor's).
    std::function<void(ImDrawList*, ImVec2 a, ImVec2 b, bool selected, const char* label)> tab;
    // The controller hints (button, text) in the game's hint bar; false:
    // the menus' own line.
    std::function<bool(const std::vector<std::pair<std::string, std::string>>&)> hints;
    // The hint bar has the game's description field: the settings' help
    // goes there (g_bar_help) instead of under the rows.
    bool bar_help = false;
};
const SkinRows* g_rows = nullptr;
std::string g_bar_help;  // the hint bar's description this frame (empty: none)
// Texture ids with this bit draw multiplying (UiCommand::blend 1).
constexpr ImTextureID kMultiplyBit = ImTextureID(1) << 40;
// How much wider than the game the menu's rows and list are.
constexpr float kWideRows = 1.25f;
float g_tab_label = 0;  // the settings tabs' label size that fits the widest (0: no limit)
bool g_title_in_layout = false;  // the screen's title sits in the game's header box
// A choice list is open over the panel: rows take no left/right.
bool g_choice_open = false;
// The last option_row was activated (A or a tap off its arrows), and where.
bool g_row_activated = false;
ImVec2 g_row_min, g_row_max;
// The last option_row has the cursor.
bool g_row_selected = false;


// The panel's header: a red band with the title in white and a Poke Ball at
// its right end, above a dark line, as the tops of the game's windows. Sets
// the cursor below it.
void panel_header(const char* title) {
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    if (g_rows != nullptr && g_title_in_layout)
        return;
    if (g_rows != nullptr) {  // over the game's background: the title alone
        const float size = ImGui::GetStyle().FontSizeBase * 1.3f;
        ImGui::PushFont(nullptr, size);
        ImGui::TextUnformatted(title);
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, 6 * unit()));
        return;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float u = unit();
    const float size = ImGui::GetStyle().FontSizeBase * 1.3f;
    const float h = size + 22 * u;
    // Inside the outline (panel_frame), so no red shows past its corners.
    const float t = 1.5f * u;
    const ImVec2 a(w->Pos.x + t, w->Pos.y - w->Scroll.y + t);
    const ImVec2 b(w->Pos.x + w->Size.x - t, a.y + h - t);
    ImGui::PushClipRect(w->Rect().Min, w->Rect().Max, false);
    dl->AddRectFilled(a, b, col(kRed), ImGui::GetStyle().WindowRounding - t, ImDrawFlags_RoundCornersTop);
    dl->AddLine(ImVec2(a.x, b.y), b, col(kInk), 3 * u);
    poke_ball(dl, ImVec2(b.x - h * 0.75f, a.y + h * 0.5f), h * 0.3f, col(kInk), false);
    ImGui::PushFont(nullptr, size);
    dl->AddText(ImVec2(a.x + ImGui::GetStyle().WindowPadding.x + 4 * u, a.y + (h - size) * 0.5f), col(kPaper), title);
    ImGui::PopFont();
    ImGui::PopClipRect();
    ImGui::SetCursorPosY(h + 14 * u);
}

// The outline around the panel and its shadow (after the content, from the
// window's size this frame).
void panel_frame() {
    if (g_rows != nullptr)
        return;
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    const float u = unit(), r = ImGui::GetStyle().WindowRounding;
    ImGui::GetBackgroundDrawList()->AddRectFilled(w->Pos + ImVec2(0, 7 * u), w->Pos + w->Size + ImVec2(0, 7 * u),
                                                  col(kInk, 0.35f), r);
    ImGui::PushClipRect(w->Rect().Min, w->Rect().Max, false);
    const float t = 3 * u;
    ImGui::GetWindowDrawList()->AddRect(w->Pos + ImVec2(t, t) * 0.5f, w->Pos + w->Size - ImVec2(t, t) * 0.5f, col(kInk),
                                        r, t);
    ImGui::PopClipRect();
}

// The next row takes the cursor (a screen or page just opened), as in the
// game's menus, where one entry is always selected.
bool g_focus_first = false;
// The next row takes the focus (a text row back from its field).
bool g_focus_next = false;
// The text row that edited this text gets the cursor back (AppMenu::text_focus_row_).
const std::string* g_focus_text = nullptr;
bool g_focus_text_reached = false;  // … and has it (ImGui's own focus on an appearing window may come first)
// Stands for the row that opened Screen::HudBounds in g_focus_text (it has no text).
const std::string g_hud_bounds_row;

// After a row: takes the cursor when asked to, or when a real mouse moves
// over it (one cursor for mouse, keys and controller).
void row_focus(bool enabled) {
    if (!enabled)
        return;
    const ImGuiIO& io = ImGui::GetIO();
    const bool mouse_moved = ImGui::IsItemHovered() && io.MouseSource == ImGuiMouseSource_Mouse &&
                             (io.MouseDelta.x != 0 || io.MouseDelta.y != 0);
    if ((g_focus_first || mouse_moved || g_focus_next) && !ImGui::IsItemFocused())
        ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
    g_focus_first = false;
    g_focus_next = false;
}

// The red triangle in front of the selected row.
void draw_cursor(ImDrawList* dl, ImVec2 tip, float s) {
    const ImVec2 p1(tip.x - s, tip.y - s * 0.8f), p2(tip.x - s, tip.y + s * 0.8f);
    dl->AddTriangleFilled(p1, tip, p2, col(kRed));
    dl->AddTriangle(p1, tip, p2, col(kInk), std::max(1.0f, s * 0.18f));
}

// A row's pill: shadow, fill (yellow when selected) and outline.
void draw_row(ImDrawList* dl, ImVec2 a, ImVec2 b, bool selected) {
    const float u = unit(), r = (b.y - a.y) * 0.5f;
    dl->AddRectFilled(a + ImVec2(0, 3 * u), b + ImVec2(0, 3 * u), col(kInk, 0.16f), r);
    dl->AddRectFilled(a, b, col(selected ? kYellow : kRow), r);
    dl->AddRect(a, b, col(kInk, selected ? 1.0f : 0.22f), r, selected ? 2.5f * u : 1.5f * u);
}

// A menu row as in the game's lists: full width, touch height (at least 44
// points); `detail` a second, grey line, `marked` a red dot at the right end
// (the current save slot).
bool menu_button(const char* label, bool enabled = true, const std::string& detail = {}, bool marked = false) {
    const float font = ImGui::GetFontSize();
    const float h = g_rows != nullptr ? g_rows->height : std::max(44.0f, font * (detail.empty() ? 2.2f : 3.3f));
    ImGui::BeginDisabled(!enabled);
    const ImVec2 a = ImGui::GetCursorScreenPos();
    const bool pressed =
        ImGui::InvisibleButton(label, ImVec2(ImGui::GetContentRegionAvail().x, h), ImGuiButtonFlags_EnableNav);
    row_focus(enabled);
    const bool selected = enabled && ImGui::IsItemFocused();
    const ImVec2 b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (g_rows != nullptr) {
        std::string text(label, ImGui::FindRenderedTextEnd(label));
        if (!detail.empty())
            text += " \xC2\xB7 " + detail;
        g_rows->draw(dl, a, b, selected, text.c_str(), nullptr);
        ImGui::EndDisabled();
        return pressed;
    }
    draw_row(dl, a, b, selected);
    if (selected)
        draw_cursor(dl, ImVec2(a.x + font * 1.15f, a.y + h * 0.5f), font * 0.42f);
    const float x = a.x + font * 1.55f;
    const char* end = ImGui::FindRenderedTextEnd(label);
    if (detail.empty()) {
        dl->AddText(ImVec2(x, a.y + (h - font) * 0.5f), col(kInk), label, end);
    } else {
        dl->AddText(ImVec2(x, a.y + h * 0.5f - font * 1.1f), col(kInk), label, end);
        ImGui::PushFont(nullptr, font * 0.85f);
        dl->AddText(ImVec2(x, a.y + h * 0.5f + font * 0.1f), col(kTextDim), detail.c_str());
        ImGui::PopFont();
    }
    if (marked)
        dl->AddCircleFilled(ImVec2(b.x - h * 0.5f, a.y + h * 0.5f), font * 0.3f, col(kRed));
    ImGui::EndDisabled();
    return pressed;
}

bool key_pressed(ImGuiKey a, ImGuiKey b, ImGuiKey c) {
    return ImGui::IsKeyPressed(a, true) || ImGui::IsKeyPressed(b, true) || ImGui::IsKeyPressed(c, true);
}

// One settings row as in the game's options: the name on the left, the value
// on the right between two arrows. Returns -1 or +1 for a step: an arrow
// tapped, left/right (pad, stick, keys) on the selected row, or for `cycles`
// (on/off, choices) A. A tap elsewhere on the row only selects it. `fraction` >= 0 draws a
// bar under the value; a finger or the mouse dragging on it sets *drag_to
// (0..1) and *dragging.
int option_row(const char* label, const std::string& value, bool enabled, bool cycles, float fraction,
               float* drag_to, bool* dragging) {
    const float font = ImGui::GetFontSize(), u = unit();
    const float h = g_rows != nullptr ? g_rows->height : std::max(44.0f, font * 2.3f);
    ImGui::BeginDisabled(!enabled);
    const ImVec2 a = ImGui::GetCursorScreenPos();
    const bool pressed =
        ImGui::InvisibleButton(label, ImVec2(ImGui::GetContentRegionAvail().x, h), ImGuiButtonFlags_EnableNav);
    row_focus(enabled);
    const bool selected = enabled && ImGui::IsItemFocused();
    g_row_selected = selected;
    const ImVec2 b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (g_rows != nullptr) {
        g_rows->draw(dl, a, b, selected, std::string(label, ImGui::FindRenderedTextEnd(label)).c_str(), &value);
    } else {
        draw_row(dl, a, b, selected);
        if (selected)
            draw_cursor(dl, ImVec2(a.x + font * 1.15f, a.y + h * 0.5f), font * 0.42f);
        dl->AddText(ImVec2(a.x + font * 1.55f, a.y + (h - font) * 0.5f), col(kInk), label,
                    ImGui::FindRenderedTextEnd(label));
    }

    // The value area: right part of the row, arrows at both ends.
    const float vw = std::min((b.x - a.x) * 0.5f, font * 11.0f);
    const float arrow = std::min(h, font * 2.2f);
    const ImVec2 va(b.x - vw - h * 0.25f, a.y), vb(b.x - h * 0.25f, b.y);
    const ImVec2 left_b(va.x + arrow, vb.y), right_a(vb.x - arrow, va.y);
    const float s = font * 0.36f, cy = a.y + h * 0.5f;
    const float lx = va.x + arrow * 0.5f, rx = vb.x - arrow * 0.5f;
    if (g_rows == nullptr) {
        dl->AddTriangleFilled(ImVec2(lx - s, cy), ImVec2(lx + s * 0.7f, cy - s), ImVec2(lx + s * 0.7f, cy + s),
                              col(kInk));
        dl->AddTriangleFilled(ImVec2(rx + s, cy), ImVec2(rx - s * 0.7f, cy + s), ImVec2(rx - s * 0.7f, cy - s),
                              col(kInk));
        const ImVec2 ts = ImGui::CalcTextSize(value.c_str());
        const float mid = (left_b.x + right_a.x) * 0.5f;
        const float ty = fraction >= 0 ? a.y + h * 0.5f - font * 0.75f : a.y + (h - font) * 0.5f;
        dl->AddText(ImVec2(mid - ts.x * 0.5f, ty), col(kInk), value.c_str());
    }
    // With the game's rows the bar sits at their bottom edge, between the arrows.
    float span0 = left_b.x + 4 * u, span1 = right_a.x - 4 * u;
    if (g_rows != nullptr && g_rows->value_span)
        g_rows->value_span(a, b, &span0, &span1);
    const ImVec2 bar_a = g_rows != nullptr ? ImVec2(span0, b.y - font * 0.35f)
                                           : ImVec2(span0, a.y + h * 0.5f + font * 0.45f);
    const ImVec2 bar_b(span1, bar_a.y + font * (g_rows != nullptr ? 0.18f : 0.3f));
    if (fraction >= 0) {
        dl->AddRectFilled(bar_a, bar_b, col(kInk, 0.15f), font);
        dl->AddRectFilled(bar_a, ImVec2(bar_a.x + (bar_b.x - bar_a.x) * std::clamp(fraction, 0.0f, 1.0f), bar_b.y),
                          col(kRed), font);
    }

    int step = 0;
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 at = io.MouseClickedPos[ImGuiMouseButton_Left];
    const bool by_pointer = pressed && ImGui::IsMouseReleased(ImGuiMouseButton_Left);
    // Tap targets: the arrows (the game's sit just outside the value's span).
    const bool skin_arrows = g_rows != nullptr && g_rows->value_span;
    const ImVec2 left0 = skin_arrows ? ImVec2(span0 - h, a.y) : va, left1 = skin_arrows ? ImVec2(span0, b.y) : left_b;
    const ImVec2 right0 = skin_arrows ? ImVec2(span1, a.y) : right_a, right1 = skin_arrows ? ImVec2(span1 + h, b.y) : vb;
    const bool on_bar = fraction >= 0 && contains(ImVec2(left1.x, a.y), ImVec2(right0.x, b.y), at);
    g_row_activated = false;
    if (pressed) {
        if (by_pointer && contains(left0, left1, at)) {
            step = -1;
        } else if (by_pointer && contains(right0, right1, at)) {
            step = 1;
        } else if (cycles && !by_pointer) {  // a tap on the row only selects it
            step = 1;
            g_row_activated = true;
            // The choice list opens under the value (arrows included).
            g_row_min = ImVec2(span0 - h, a.y);
            g_row_max = ImVec2(span1 + h, b.y);
        }
    }
    if (selected && !g_choice_open) {
        if (key_pressed(ImGuiKey_LeftArrow, ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadLStickLeft))
            step = -1;
        if (key_pressed(ImGuiKey_RightArrow, ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickRight))
            step = 1;
    }
    if (enabled && on_bar && ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        *drag_to = std::clamp((io.MousePos.x - bar_a.x) / std::max(1.0f, bar_b.x - bar_a.x), 0.0f, 1.0f);
        *dragging = true;
    }
    ImGui::EndDisabled();
    return enabled ? step : 0;
}

// A button's symbol in the hint line: a dark circle (A, B) or pill (L, R)
// with the letter in white. Returns its width.
float hint_glyph(ImDrawList* dl, ImVec2 p, const char* letter) {
    const float font = ImGui::GetFontSize();
    ImGui::PushFont(nullptr, font * 0.8f);
    const ImVec2 ts = ImGui::CalcTextSize(letter);
    const float r = font * 0.62f;
    const float w = std::max(2 * r, ts.x + r);
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + 2 * r), col(kInk), r);
    dl->AddText(ImVec2(p.x + (w - ts.x) * 0.5f, p.y + r - ts.y * 0.5f), col(kPaper), letter);
    ImGui::PopFont();
    return w;
}

struct Hint {
    const char* glyph;  // "A", "B", "L R", or "<>" for the two arrows
    const char* text;
};

// The line of controller hints at the foot of the panel, right-aligned.
void hint_line(std::initializer_list<Hint> hints) {
    if (g_rows != nullptr && g_rows->hints) {
        std::vector<std::pair<std::string, std::string>> list;
        for (const Hint& h : hints)
            list.emplace_back(h.glyph, h.text);
        if (g_rows->hints(list))
            return;
    }
    const float font = ImGui::GetFontSize(), u = unit();
    float width = 0;
    for (const Hint& h : hints)
        width += font * 1.5f + 6 * u + ImGui::CalcTextSize(h.text).x + 16 * u;
    ImGui::Spacing();
    const ImVec2 start = ImGui::GetCursorScreenPos();
    float x = start.x + std::max(0.0f, ImGui::GetContentRegionAvail().x - width);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (const Hint& h : hints) {
        float w = 0;
        if (std::strcmp(h.glyph, "<>") == 0) {
            const float s = font * 0.36f, cy = start.y + font * 0.62f;
            dl->AddTriangleFilled(ImVec2(x, cy), ImVec2(x + s * 1.6f, cy - s), ImVec2(x + s * 1.6f, cy + s), col(kInk));
            dl->AddTriangleFilled(ImVec2(x + s * 4.0f, cy), ImVec2(x + s * 2.4f, cy + s), ImVec2(x + s * 2.4f, cy - s),
                                  col(kInk));
            w = s * 4.0f;
        } else {
            w = hint_glyph(dl, start + ImVec2(x - start.x, 0), h.glyph);
        }
        x += w + 6 * u;
        dl->AddText(ImVec2(x, start.y + font * 0.62f - font * 0.5f), col(kTextDim), h.text);
        x += ImGui::CalcTextSize(h.text).x + 16 * u;
    }
    ImGui::Dummy(ImVec2(width, font * 1.3f));
}

void dim_text(const std::string& text) {
    if (text.empty())
        return;
    ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
}

void note_text(const std::string& text) {
    if (text.empty())
        return;
    ImGui::PushStyleColor(ImGuiCol_Text, kNote);
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
}

// The game picture's width / height (AppMenu::set_picture_aspect, else the window's).
float g_picture_aspect = 16.0f / 9.0f;
bool picture_not_16_9() { return std::fabs(g_picture_aspect - 16.0f / 9.0f) > 0.01f; }
// AppMenu::set_docked_next_start: the TV mode switch waits for the next start.
bool g_docked_next_start = false;

// The settings screen as pages of rows. A new option is a field in
// MenuSettings, one Option here (on its page, or a new page) and its line in
// settings.ini (runtime/boot/src/main.cpp: menu_settings, store_menu_changes).
struct Option {
    const char* label;
    bool* flag = nullptr;             // on/off; `choices` may name both states (off first)
    int* value = nullptr;             // a choice (index into `choices`) or a number lo..hi
    std::vector<const char*> choices = {};  // empty with `value`: a number
    int lo = 0, hi = 100, step = 5;
    const char* format = "%d %%";
    const char* zero = nullptr;       // shown instead of 0 ("Stumm")
    bool enabled = true;
    std::string help = {};            // shown under the list while the row has the cursor
    std::string* text = nullptr;      // a text, edited on Screen::Text (A, either arrow)
    const char* empty_text = nullptr;  // shown for an empty text
    uint32_t max_length = 0;          // of the text (UTF-16 units, 0: none)
    bool server = false;              // the text is a server address (AppMenu::set_server_check)
    Screen opens = Screen::None;      // a screen of its own (A, either arrow), `shown` as the value
    std::string shown = {};
};

struct Page {
    const char* title;
    std::vector<Option> options;
    std::string note;  // under the rows while the selected one has no help of its own
    const char* button = nullptr;  // a button under the note
    Action button_action = Action::None;
    bool button_enabled = true;
};

// The widescreen mod among the menu's mods (only listed with its mod file), else null.
MenuMod* widescreen_mod(MenuSettings& s) {
    for (MenuMod& mod : s.mods)
        if (mod.id == "widescreen")
            return &mod;
    return nullptr;
}

// With the widescreen mod the picture format row has four choices,
// "Widescreen" (the mod on) and the three 16:9 ones (aspect, the mod off):
// a choice made on the row since the last call goes to both, else the row
// shows what they hold (settings.ini, a --menu-script mod switch).
void sync_picture_format(MenuSettings& s) {
    MenuMod* wide = widescreen_mod(s);
    if (wide == nullptr) {
        s.picture_format_seen = -1;
        return;
    }
    if (s.picture_format_seen >= 0 && s.picture_format != s.picture_format_seen) {
        wide->on = s.picture_format == 0;
        s.aspect = s.picture_format == 0 ? 0 : std::clamp(s.picture_format - 1, 0, 2);
    }
    s.picture_format = s.picture_format_seen = wide->on ? 0 : std::clamp(s.aspect, 0, 2) + 1;
}

// The throw's animation only with Catch in battle (Paul 2026-11-02): off
// with it. Called before the menu reports a change, so the consumers see the
// throw already off in the same SettingsChanged.
void sync_mod_needs(MenuSettings& s) {
    bool balls_on = false;
    for (const MenuMod& mod : s.mods)
        if (mod.id == "battle-balls")
            balls_on = mod.on;
    if (!balls_on)
        for (MenuMod& mod : s.mods)
            if (mod.id == "battle-throw")
                mod.on = false;
}

std::vector<Page> settings_pages(MenuSettings& s, bool touch_layout, bool in_game, const std::string& player1,
                                 bool system) {
    sync_picture_format(s);
    sync_mod_needs(s);
    MenuMod* const wide = widescreen_mod(s);
    std::vector<Page> pages;
    // `system`: the rows that need a restart are in the Settings app.
    const char* const system_button_text = tr("Open iOS Settings");
    const char* const system_note = tr("Controller, graphics and connection are in Settings > Apps > LGP and apply "
                                       "from the next start.");
    const auto system_button = [&](Page& page) {
        page.note = system_note;
        page.button = system_button_text;
        page.button_action = Action::OpenSystemSettings;
    };
    const bool layout = touch_layout && s.touch;
    Page control{tr("Controls"), {}, {}};
    control.options.push_back({.label = tr("Touch controls"), .flag = &s.touch});
    control.options.push_back(
        {.label = tr("Size"), .value = &s.touch_size, .lo = 50, .hi = 140, .step = 10, .enabled = layout});
    control.options.push_back(
        {.label = tr("Opacity"), .value = &s.touch_opacity, .lo = 25, .hi = 100, .step = 5, .enabled = layout});
    control.options.push_back(
        {.label = tr("Stick"), .flag = &s.touch_mirrored, .choices = {tr("Left"), tr("Right")}, .enabled = layout});
    control.options.push_back(
        {.label = tr("Throw Pok\xC3\xA9 Balls by swiping"), .flag = &s.touch_throw, .enabled = layout});
    control.options.push_back({.label = tr("Swipe with a controller too"),
                               .flag = &s.touch_throw_pad,
                               .enabled = layout && s.touch_throw});
    if (!system)
        control.options.push_back({.label = tr("Controller"),
                                   .value = &s.controller,
                                   .choices = {tr("Handheld"), tr("Joy-Con right"), tr("Joy-Con left")}});
    const std::string touch_help =
        !touch_layout ? tr("Size, opacity and side come with the next update.")
        : !s.touch    ? tr("The on-screen controls are off. Size, opacity and side apply once they are on.")
                      : "";
    for (size_t i = 0; i < 4 && i < control.options.size(); ++i)
        control.options[i].help = touch_help;
    control.options[0].help =
        touch_help.empty() ? tr("Stick and buttons on the screen, for playing without a controller.") : touch_help;
    if (touch_layout && s.touch) {
        control.options[1].help = tr("How large the stick and buttons on the screen are.");
        control.options[2].help = tr("How clearly the stick and buttons show.");
        control.options[3].help = tr("Which side the stick is on; right for left-handers.");
        control.options[4].help = tr("A swipe upwards throws a Pok\xC3\xA9 Ball while catching.");
        control.options[5].help = tr("Swiping throws even while player 1 plays with a controller.");
    } else {
        control.options[4].help = control.options[5].help = touch_help;
    }
    const std::string next_start = in_game ? std::string(" ") + tr("Applies from the next start.") : std::string();
    if (!system)
        control.options.back().help =
            tr("Which controller the game is told about.") +
            (in_game ? std::string(" ") + tr("Takes effect at once, as if the controller were swapped.") : "");
    control.options.push_back({.label = tr("Player 1"),
                               .flag = &s.player1_by_press,
                               .choices = {tr("Connection"), tr("Button press")}});
    std::string& player_help = control.options.back().help;
    player_help = s.player1_by_press ? tr("Player 1 is the controller on which a button is pressed first.")
                                     : tr("Player 1 is the controller connected first.");
    if (in_game) {
        if (!player1.empty())
            player_help += " " + trf("Now: {}.", {player1});
        control.button = tr("Choose player 1 again");
        control.button_action = Action::PickPlayer1;
        control.button_enabled = s.player1_by_press;
    } else if (system) {
        system_button(control);
    }
    control.options.push_back({.label = tr("Controller screen at start"), .flag = &s.controller_screen});
    control.options.back().help =
        tr("On: the game asks for the controller at every start, as on the console. Off: it starts with the "
           "controller from these settings (needs the mod file skip-controller-screen.cfg); the game's options "
           "still show the screen. Applies from the next start.");
    pages.push_back(std::move(control));
    Page sound{tr("Sound"), {}, {}};
    sound.options.push_back({.label = tr("Volume"), .value = &s.volume, .lo = 0, .hi = 100, .step = 5,
                             .zero = tr("Mute")});
    sound.options.back().help = tr("Volume of the game and its videos.");
    pages.push_back(std::move(sound));
    Page picture{tr("Picture"), {}, {}};
    if (wide != nullptr) {
        // The widescreen mod is a picture format, not a row among the game mods.
        picture.options.push_back({.label = tr("Picture format"),
                                   .value = &s.picture_format,
                                   .choices = {tr("Widescreen"), tr("16:9 with borders"), tr("16:9 fill"),
                                               tr("16:9 stretched")}});
        picture.options.back().help = tr("Widescreen: more of the game world to fit the screen, nothing stretched; "
                                         "applies from the next start. 16:9 with borders: as on the console. 16:9 "
                                         "fill: no borders, cropped. 16:9 stretched: no borders, distorted.");
    } else {
        picture.options.push_back({.label = tr("Picture format"),
                                   .value = &s.aspect,
                                   .choices = {tr("Fit"), tr("Fill"), tr("Stretch")}});
        picture.options.back().help = tr("Fit: 16:9 with borders. Fill: no borders, cropped. Stretch: no borders, "
                                         "distorted.");
    }
    picture.options.push_back({.label = tr("TV mode (1080p)"), .flag = &s.docked});
    std::string& docked_help = picture.options.back().help;
    docked_help = s.docked ? tr("TV mode: the game renders at 1920\xC3\x97" "1080 as in the dock, sharper, more load "
                                "on the GPU.")
                           : tr("Handheld mode: the game renders at 1280\xC3\x97" "720, easier on GPU and battery.");
    // The widescreen mod does not follow a switch while the game runs.
    if (in_game && g_docked_next_start)
        docked_help += std::string(" ") + tr("With widescreen the TV mode applies from the next start.");
#if defined(SDL_PLATFORM_TVOS)
    // Apple TV: about 440 MiB left before tvOS ends the app (docs/agents/tvos.md);
    // larger render targets are unmeasured there.
    if (s.docked)
        docked_help += std::string(" ") + tr("Apple TV: untested, the larger pictures can exceed the memory (the app "
                                             "is ended).");
#endif
    picture.options.push_back({.label = tr("Smoothing"),
                               .value = &s.smoothing,
                               .choices = {tr("Off"), tr("Soft"), "FXAA"}});
    picture.options.back().help = tr("How the picture is scaled to the screen. Off: sharp, pixels of unequal size. "
                                     "Soft: even, a little softer. FXAA: smoothed edges as well, text gets a little "
                                     "softer too.");
    // In the app on iPhone and iPad too: the widescreen mod matters most there.
    // Only with "Widescreen" as the picture format.
    if (wide != nullptr && wide->on) {
        // Only the pair the picture has room for: taller (iPad) the height,
        // wider (iPhone) the width.
        const int v = g_picture_aspect < 16.0f / 9.0f ? s.hud_height : s.hud_width;
        picture.options.push_back({.label = tr("Battle HUD bounds"),
                                   .enabled = picture_not_16_9(),
                                   .opens = Screen::HudBounds,
                                   .shown = v <= 0 ? std::string("16:9") : std::to_string(v) + " %"});
        picture.options.back().help = tr("With the widescreen mod: where the displays on the battle screen sit on "
                                         "a taller or wider screen, between the 16:9 frame and the edge. Everything "
                                         "else always goes to the edge. Nothing is stretched.") +
                                      (in_game ? std::string(" ") + tr("Applies from the next battle.") : std::string());
    }
    if (system) {
        system_button(picture);
    } else {
        picture.options.push_back(
            {.label = tr("Light effects"), .flag = &s.effects_half, .choices = {tr("Full"), tr("Half")}});
        picture.options.back().help = tr("Half: expensive light and glow effects at half resolution, a little "
                                         "softer, less load on the GPU.") + next_start;
        picture.options.push_back(
            {.label = tr("Resolution"), .value = &s.render_scale, .choices = {"100 %", "75 %", "50 %"}});
        picture.options.back().help = tr("How many pixels the game renders. Fewer is blurrier and eases the GPU; "
                                         "after a change it may stutter more often at first, until the shaders are "
                                         "translated again.") + next_start;
        picture.options.push_back({.label = tr("Graphics thread"), .flag = &s.async_execute});
        picture.options.back().help = tr("On: graphics on a thread of their own, one frame ahead. Whether that "
                                         "brings more frames per second depends on the device.") + next_start;
    }
    pages.push_back(std::move(picture));
    {
        Page game{tr("Game"), {}, {}};
        if (system) {
            system_button(game);
        } else {
            game.options.push_back({.label = tr("Trade and battle over Wi-Fi"), .flag = &s.ldn_lan});
            game.options.back().help = tr("Link trades and link battles with other devices on the same Wi-Fi (in "
                                          "the game: players nearby).");
            game.options.back().help += next_start;
            // Online is always on with our server; the field only names another.
            game.options.push_back({.label = tr("Server"),
                                    .help = tr("Online (in the game: players far away) goes over this server. "
                                               "Another server's address, HOST[:PORT] or "
                                               "ws[s]://HOST[:PORT][/PATH]. Empty: the server of this build.") +
                                            next_start,
                                    .text = &s.ldn_server,
                                    .empty_text = tr("Default"),
                                    .max_length = 120,
                                    .server = true});
            game.options.push_back({.label = tr("Room"),
                                    .help = tr("Only devices with the same room find each other. Empty: everyone "
                                               "on the server.") + next_start,
                                    .text = &s.ldn_room,
                                    .empty_text = tr("Public"),
                                    .max_length = 32});
        }
        std::vector<const char*> languages;
        for (const char* name : kMenuLanguages)
            languages.push_back(tr(name));
        game.options.push_back({.label = tr("Language of the game"), .value = &s.language, .choices = languages});
        game.options.back().help = tr("Device: this device's language if the game has it, else English. Applies "
                                      "from the next start.");
        game.options.push_back({.label = tr("Main menu at start"), .flag = &s.start_screen});
        game.options.back().help = tr("On: this menu comes before the game, to choose a save. Off: the game starts "
                                      "at once with the last save; the pause button opens the menu. Applies from the "
                                      "next start.");
        // The throw's animation only with Catch in battle: greyed out without
        // it (sync_mod_needs() has switched it off).
        bool balls_on = false;
        for (const MenuMod& mod : s.mods)
            if (mod.id == "battle-balls")
                balls_on = mod.on;
        for (MenuMod& mod : s.mods) {
            if (&mod == wide)
                continue;  // on the picture page as a picture format
            const bool throw_row = mod.id == "battle-throw";
            game.options.push_back({.label = mod.title.c_str(), .flag = &mod.on, .enabled = !throw_row || balls_on,
                                    .help = mod.description});
            if (throw_row)  // "Catch style in battle": off = Let's Go, on = the classic throw
                game.options.back().choices = {tr("Let's Go"), tr("Classic")};
            if (in_game)
                game.options.back().help += std::string(mod.description.empty() ? "" : " ") +
                                            (mod.live ? tr("Takes effect at once.") : tr("Applies from the next start."));
            // Only in Let's Go (Paul 2026-11-02): with Pokémon Yellow on, battles come from the die alone.
            if (mod.id == "random-encounters" && !mod.on) {
                game.options.push_back({.label = tr("Hide field Pokémon"), .flag = &s.hide_field_pokemon});
                game.options.back().help = tr("Let's Go without wild Pokémon in sight; walking into a hidden one "
                                              "still starts its battle. Takes effect at once.");
            }
        }
        pages.push_back(std::move(game));
    }
    return pages;
}

// The option whose list A or a tap asked for this frame.
Option* g_choice_request = nullptr;
// The text row whose field A or an arrow asked for this frame.
Option* g_text_request = nullptr;
// The screen a row asked to open this frame (Option::opens).
Screen g_open_request = Screen::None;

// A text row's value: without ws:// or wss:// and the trailing slash, at most 16
// characters (the value box between the arrows is narrow).
std::string short_text(const Option& o) {
    std::string t = *o.text;
    if (t.empty())
        return o.empty_text != nullptr ? o.empty_text : "";
    if (o.server && t.rfind("ws://", 0) == 0)
        t.erase(0, 5);
    else if (o.server && t.rfind("wss://", 0) == 0)
        t.erase(0, 6);
    if (o.server && t.size() > 1 && t.back() == '/')
        t.pop_back();
    constexpr size_t kMax = 16;
    size_t cut = 0, chars = 0;
    for (; cut < t.size() && chars < kMax; ++chars)
        for (++cut; cut < t.size() && (static_cast<unsigned char>(t[cut]) & 0xC0) == 0x80;)
            ++cut;
    if (cut < t.size())
        t = t.substr(0, cut) + "\xE2\x80\xA6";  // …
    return t;
}

// Draws one option; true if its value changed.
bool option(Option& o, bool* dragging) {
    std::string text;
    float fraction = -1;
    const bool number = o.value != nullptr && o.choices.empty();
    if (o.opens != Screen::None) {
        float drag_to = -1;
        const bool refocus = o.opens == Screen::HudBounds && g_focus_text == &g_hud_bounds_row;
        g_focus_next = refocus;
        if (option_row(o.label, o.shown, o.enabled, true, -1, &drag_to, dragging) != 0)
            g_open_request = o.opens;
        // Also ImGui's own first focus of the settings window as it comes
        // back (else the first row has it for a frame and takes a key).
        if (refocus)
            ImGui::SetItemDefaultFocus();
        if (refocus && g_row_selected)
            g_focus_text_reached = true;
        return false;
    }
    if (o.text != nullptr) {
        float drag_to = -1;
        const bool refocus = o.text == g_focus_text;
        g_focus_next = refocus;
        if (option_row(o.label, short_text(o), o.enabled, true, -1, &drag_to, dragging) != 0)
            g_text_request = &o;
        if (refocus && g_row_selected)
            g_focus_text_reached = true;
        return false;
    }
    if (o.flag != nullptr) {
        text = o.choices.size() == 2 ? o.choices[*o.flag ? 1 : 0] : (*o.flag ? tr("On") : tr("Off"));
    } else if (number) {
        char buf[48];
        if (*o.value == 0 && o.zero != nullptr)
            std::snprintf(buf, sizeof buf, "%s", o.zero);
        else
            std::snprintf(buf, sizeof buf, o.format, *o.value);
        text = buf;
        fraction = float(*o.value - o.lo) / float(std::max(1, o.hi - o.lo));
    } else {
        text = o.choices[size_t(std::clamp(*o.value, 0, int(o.choices.size()) - 1))];
    }
    float drag_to = -1;
    const int step = option_row(o.label, text, o.enabled, !number, fraction, &drag_to, dragging);
    // Three choices or more: A opens the list (the arrows still step).
    if (g_row_activated && o.value != nullptr && o.choices.size() >= 3) {
        g_choice_request = &o;
        return false;
    }
    if (o.flag != nullptr) {
        if (step == 0)
            return false;
        *o.flag = !*o.flag;
        return true;
    }
    const int was = *o.value;
    if (number) {
        if (drag_to >= 0) {
            const int v = o.lo + int(std::lround(drag_to * float(o.hi - o.lo) / float(o.step))) * o.step;
            *o.value = std::clamp(v, o.lo, o.hi);
        } else if (step != 0) {
            // From an odd value (settings.ini) onto the grid first.
            const int grid = o.lo + (*o.value - o.lo) / o.step * o.step;
            *o.value = std::clamp(step > 0 ? grid + o.step : (grid == *o.value ? grid - o.step : grid), o.lo, o.hi);
        }
    } else if (step != 0) {
        const int n = int(o.choices.size());
        *o.value = ((*o.value + step) % n + n) % n;  // choices wrap around, as in the game
    }
    return *o.value != was;
}

}  // namespace

std::unique_ptr<AppMenu> AppMenu::create(SDL_Window* window, std::string* error) {
    if (window == nullptr) {
        *error = "no window";
        return nullptr;
    }
    std::unique_ptr<AppMenu> menu(new AppMenu());
    menu->window_ = window;
    // The start menu runs before the game opens its input (SdlInput); without
    // the subsystem SDL reports no gamepads, and only a TV remote's keys
    // reached the menu (NVIDIA Shield, 20.10.2026). The subsystem counts its
    // users, so SdlInput's own init later is harmless.
    menu->gamepads_ = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    if (!menu->gamepads_)
        SDL_Log("lgp: menu: no gamepads (%s)", SDL_GetError());
#if defined(SDL_PLATFORM_ANDROID) || defined(SDL_PLATFORM_TVOS)
    menu->start_hold_pause_ = true;  // a controller's Home stays with the system there
#endif
#if defined(SDL_PLATFORM_TVOS)
    // The Siri Remote's surface sends indirect touches that start mid-screen;
    // as mouse clicks they would press whatever button sits in the centre.
    menu->touch_is_remote_ = true;
#endif
    IMGUI_CHECKVERSION();
    menu->context_ = ImGui::CreateContext();
    ImGui::SetCurrentContext(menu->context_);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // no imgui.ini next to the game
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
    // No window switcher: holding X (Play/Pause on the Siri Remote) opened
    // ImGui's Ctrl+Tab list mid-screen as "(Untitled)" (Paul, 2026-10-28).
    // The menus show one window at a time.
    ImGui::GetCurrentContext()->ConfigNavWindowingWithGamepad = false;
    ImGui::GetCurrentContext()->ConfigNavWindowingKeyNext = 0;
    ImGui::GetCurrentContext()->ConfigNavWindowingKeyPrev = 0;
    io.BackendRendererName = "lgp_webgpu_renderer";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
    if (!ImGui_ImplSDL3_InitForOther(window)) {
        *error = "ImGui SDL3 backend did not start";
        ImGui::DestroyContext(menu->context_);
        menu->context_ = nullptr;
        return nullptr;
    }
    // Every controller drives the menus, not only the first one SDL lists
    // (on tvOS that is the Siri Remote, connected before any controller).
    ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_AutoAll);
    ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
    if (pio.Platform_SetImeDataFn != set_ime_data) {
        g_backend_ime = pio.Platform_SetImeDataFn;
        pio.Platform_SetImeDataFn = set_ime_data;
    }

    int w = 0, h = 0;
    SDL_GetWindowSize(window, &w, &h);
    // Window points: an iPad Pro 11" in landscape is 834 high (24.5), an
    // iPhone in landscape about 390 (16), a 720p desktop window 21.
    const float font_size = std::clamp(float(std::min(w, h)) / 34.0f, 16.0f, 26.0f);
    menu->scale_ = font_size / 18.0f;
    menu->font_size_ = font_size;
    ImFont* font = nullptr;
    for (const char* path : kFontFiles)
        if (file_exists(path) && (font = io.Fonts->AddFontFromFileTTF(path, font_size)) != nullptr) {
            SDL_Log("lgp: interface font %s", path);
            break;
        }
    if (font == nullptr)
        io.Fonts->AddFontDefaultVector();
    apply_style(font_size);
    return menu;
}

bool AppMenu::set_game_font(std::vector<uint8_t> file, std::string* error) {
    auto font = unwrap_game_font(file);
    if (!font) {
        *error = "no OpenType/TrueType font (nor a wrapped one)";
        return false;
    }
    ImGui::SetCurrentContext(context_);
    ImGuiIO& io = ImGui::GetIO();
    // The atlas owns (and frees) the data.
    void* data = IM_ALLOC(font->size());
    std::memcpy(data, font->data(), font->size());
    ImFontConfig config;
    config.FontDataOwnedByAtlas = true;
    ImFont* f = io.Fonts->AddFontFromMemoryTTF(data, int(font->size()), font_size_, &config);
    if (f == nullptr) {
        *error = "the font did not load";
        return false;
    }
    io.FontDefault = f;
    dirty_ = true;
    return true;
}

namespace {
std::atomic<uint64_t> g_image_version{1u << 20};  // apart from the atlas's own count
}  // namespace

int AppMenu::add_image(uint32_t width, uint32_t height, std::vector<uint8_t> rgba) {
    auto image = std::make_shared<UiTexture>();
    image->width = width;
    image->height = height;
    image->version = g_image_version++;
    image->rgba = std::move(rgba);
    image->rgba.resize(size_t(width) * height * 4);
    images_.push_back(std::move(image));
    dirty_ = true;
    return int(images_.size()) - 1;
}

void AppMenu::clear_images() {
    images_.clear();
    skin_images_.clear();
    tinted_.clear();
    dirty_ = true;
}

void AppMenu::set_skin(std::shared_ptr<const LayoutSkin> skin) {
    clear_images();
    skin_ = std::move(skin);
    if (!skin_)
        return;
    for (const SkinArchive& a : skin_->archives) {
        std::vector<int>& ids = skin_images_.emplace_back();
        for (const UiImage& img : a.images)
            ids.push_back(add_image(img.width, img.height, img.rgba));
    }
    wide_rows_.clear();
    wide_tabs_.clear();
    wide_rows_dx_ = wide_list_dx_ = -1;
    // The background's loops (setting_bg_00_loop_*), all at once.
    bg_loops_.clear();
    bg_base_.clear();
    if (const SkinArchive* a = skin_->archive_with("setting_bg_00")) {
        for (const auto& [stem, anim] : a->animations)
            if (stem.rfind("setting_bg_00_loop", 0) == 0 && anim.frames > 0)
                bg_loops_.push_back(&anim);
        if (const LayoutAnimation* in = a->animation("setting_bg_00_in"); in != nullptr && a->layout("setting_bg_00"))
            bg_base_.push_back(animated(*a->layout("setting_bg_00"), *in));
    }
    // A row as it stands after its select and unselect animations.
    row_states_.clear();
    if (const SkinArchive* a = skin_->archive_with("setting_button_00")) {
        const LayoutAnimation* select = a->animation("setting_button_00_select");
        const LayoutAnimation* unselect = a->animation("setting_button_00_unselect");
        if (select != nullptr && unselect != nullptr) {
            row_states_.push_back(animated(*a->layout("setting_button_00"), *unselect));
            row_states_.push_back(animated(*a->layout("setting_button_00"), *select));
        }
    }
    // The choice window's button likewise.
    choice_states_.clear();
    if (const SkinArchive* a = skin_->archive_with("button_option_00")) {
        const LayoutAnimation* select = a->animation("button_option_00_select");
        const LayoutAnimation* unselect = a->animation("button_option_00_unselect");
        if (select != nullptr && unselect != nullptr) {
            choice_states_.push_back(animated(*a->layout("button_option_00"), *unselect));
            choice_states_.push_back(animated(*a->layout("button_option_00"), *select));
        }
    }
    // The context menu (the game's yes/no and choice windows).
    context_base_ = Layout{};
    context_cut_rows_ = -1;
    if (const SkinArchive* a = skin_->archive_with("context_00")) {
        const LayoutAnimation* in = a->animation("context_00_in");
        context_base_ = in != nullptr ? animated(*a->layout("context_00"), *in) : *a->layout("context_00");
    }
}

const Layout* AppMenu::context_layout(int rows, float* pitch) {
    const Layout& b = context_base_;
    const int c0 = b.find("N_cursor_00"), c1 = b.find("N_cursor_01"), pos = b.find("N_curosr_pos_00");
    if (b.panes.empty() || c0 < 0 || c1 < 0 || pos < 0)
        return nullptr;
    *pitch = std::fabs(b.panes[size_t(c1)].translate[1] - b.panes[size_t(c0)].translate[1]);
    if (*pitch < 1.0f)
        return nullptr;
    if (rows == context_cut_rows_)
        return &context_cut_;
    context_cut_ = b;
    // The windows as tall as the rows used plus their margin above the
    // first row, below the last too (the layout's height does not fit its
    // seven slots exactly).
    ImVec2 rmn, rmx;
    if (!layout_pane_rect(b, c0, ImVec2(0, 0), 1.0f, &rmn, &rmx))
        return nullptr;
    for (const char* w : {"W_window_00", "W_window_01"}) {
        const int i = context_cut_.find(w);
        ImVec2 wmn, wmx;
        if (i < 0 || !layout_pane_rect(b, i, ImVec2(0, 0), 1.0f, &wmn, &wmx))
            continue;
        LayoutPane& p = context_cut_.panes[size_t(i)];
        const float margin = std::max(0.0f, rmn.y - wmn.y);
        const float d = p.height - (2 * margin + *pitch * float(rows));
        p.height -= d;
        const int oy = (p.origin >> 2) & 3;
        p.translate[1] += oy == 0 ? d * 0.5f : oy == 2 ? d : 0.0f;
    }
    context_cut_rows_ = rows;
    return &context_cut_;
}

void AppMenu::set_layout_preview(std::string archive, std::string layout) {
    preview_archive_ = std::move(archive);
    preview_layout_ = std::move(layout);
    dirty_ = true;
}

// The picture for a material of one of the skin's layouts (-1: none) and
// the colour to multiply it with. The game maps a texture's colour from the
// material's black to its white; where black is not black that needs a
// tinted copy of the picture, made once.
namespace {
bool multiplies(const LayoutMaterial& m) {
    return m.blend_op == 1 && ((m.blend_src == 2 && m.blend_dst == 0) || (m.blend_src == 0 && m.blend_dst == 8));
}
}  // namespace

int AppMenu::skin_texture(const Layout& layout, const LayoutMaterial& m, float tint[4]) {
    if (!skin_ || m.maps.empty() || m.maps[0].texture < 0)
        return -1;
    for (size_t a = 0; a < skin_->archives.size(); ++a) {
        const SkinArchive& arch = skin_->archives[a];
        bool mine = !layout.archive.empty() && layout.archive == arch.name;
        for (const auto& entry : arch.layouts)
            mine |= &entry.second == &layout;
        if (!mine)
            continue;
        const std::string& name = layout.textures[size_t(m.maps[0].texture)];
        int img = arch.image(name);
        // Shared textures (the game's dummy_white_*) can sit in another
        // archive; white ones are made when none has them.
        size_t from = a;
        for (size_t o = 0; img < 0 && o < skin_->archives.size(); ++o)
            if (o != a && (img = skin_->archives[o].image(name)) >= 0)
                from = o;
        if (img < 0 && name.rfind("dummy_white", 0) == 0) {
            tint[0] = m.white.r / 255.0f, tint[1] = m.white.g / 255.0f, tint[2] = m.white.b / 255.0f;
            tint[3] = m.white.a / 255.0f;
            for (const auto& [k, id] : tinted_)
                if (k == "white")
                    return id;
            const int id = add_image(4, 4, std::vector<uint8_t>(4 * 4 * 4, 255));
            tinted_.emplace_back("white", id);
            return id;
        }
        if (img < 0)
            return -1;
        a = from;
        const SkinArchive& src_arch = skin_->archives[a];
        // Colour from black to white by the texture; alpha the texture's
        // times white's (black's alpha only counts with the material's
        // alpha interpolation, which the menus leave out).
        const LayoutColor& b = m.black;
        // Adding (src + dst), which the UI does not blend, as the colour
        // with alpha = its brightness; multiplying the renderer does
        // (skin_painter).
        const bool add = m.blend_op == 1 && !multiplies(m) && m.blend_dst == 1;
        if (b.r == 0 && b.g == 0 && b.b == 0 && !add) {
            tint[0] = m.white.r / 255.0f, tint[1] = m.white.g / 255.0f, tint[2] = m.white.b / 255.0f;
            tint[3] = m.white.a / 255.0f;
            return skin_images_[a][size_t(img)];
        }
        tint[0] = tint[1] = tint[2] = tint[3] = 1.0f;
        char key[96];
        std::snprintf(key, sizeof key, "%zu/%d/%02x%02x%02x/%02x%02x%02x%02x/%d", a, img, b.r, b.g, b.b, m.white.r,
                      m.white.g, m.white.b, m.white.a, add ? 2 : 0);
        for (const auto& [k, id] : tinted_)
            if (k == key)
                return id;
        const UiImage& src = src_arch.images[size_t(img)];
        std::vector<uint8_t> out(src.rgba.size());
        const uint8_t lo[4] = {b.r, b.g, b.b, 0}, hi[4] = {m.white.r, m.white.g, m.white.b, m.white.a};
        for (size_t i = 0; i < out.size(); ++i) {
            const int c = int(i & 3);
            out[i] = uint8_t(lo[c] + (int(hi[c]) - int(lo[c])) * int(src.rgba[i]) / 255);
        }
        if (add)
            for (size_t i = 0; i + 3 < out.size(); i += 4) {
                const int lum = (out[i] * 77 + out[i + 1] * 150 + out[i + 2] * 29) >> 8;
                out[i + 3] = uint8_t(lum * out[i + 3] / 255);
            }
        const int id = add_image(src.width, src.height, std::move(out));
        tinted_.emplace_back(key, id);
        return id;
    }
    return -1;
}

// Callbacks that draw the skin's layouts (textures, parts) into `list`.
LayoutDraw AppMenu::skin_painter(ImDrawList* list) {
    LayoutDraw d;
    d.list = list;
    // The layouts' text sizes drawn as ImGui font sizes come out far too
    // large against the game (titles and rows filling their boxes); six
    // tenths gives the sizes of the target picture Paul chose from.
    // Suspicion, not proven: the game's font size counts a larger line
    // height than the font's em.
    d.text_scale = 0.6f;
    d.texture = [this](const Layout& l, const LayoutMaterial& m, ImTextureID* id, ImVec4* tint) {
        float t[4];
        const int img = skin_texture(l, m, t);
        if (img < 0)
            return false;
        *id = ImTextureID(image_id(img)) | (multiplies(m) ? kMultiplyBit : 0);
        *tint = ImVec4(t[0], t[1], t[2], t[3]);
        return true;
    };
    d.part = [this](std::string_view part) -> const Layout* {
        const SkinArchive* a = skin_ ? skin_->archive_with(part) : nullptr;
        return a != nullptr ? a->layout(part) : nullptr;
    };
    return d;
}

bool AppMenu::choice_shape(float* width, float* text) const {
    const SkinArchive* arch = skin_ ? skin_->archive_with("button_option_00") : nullptr;
    const Layout* base = arch != nullptr ? arch->layout("button_option_00") : nullptr;
    if (base == nullptr)
        return false;
    ImVec2 mn, mx, tmn, tmx;
    int frame = base->find("W_glare_00");
    if (frame < 0)
        frame = base->find("W_window_fill");
    if (!layout_pane_rect(*base, frame, ImVec2(0, 0), 1.0f, &mn, &mx) || mx.x - mn.x < 1 || mx.y - mn.y < 1 ||
        !layout_pane_rect(*base, base->find("T_text_00"), ImVec2(0, 0), 1.0f, &tmn, &tmx) || tmx.x - tmn.x < 1)
        return false;
    *width = (mx.x - mn.x) / (mx.y - mn.y);
    *text = std::min(1.0f, (tmx.x - tmn.x) / (mx.x - mn.x));
    return true;
}

void AppMenu::draw_choice(ImDrawList* dl, float x0, float y0, float x1, float y1, bool selected, const char* label,
                          bool tab, const char* fit_like) {
    const ImVec2 a(x0, y0), b(x1, y1);
    const SkinArchive* arch = skin_ ? skin_->archive_with("button_option_00") : nullptr;
    const Layout* base = arch != nullptr ? arch->layout("button_option_00") : nullptr;
    ImVec2 mn, mx;
    int frame = base != nullptr ? base->find("W_glare_00") : -1;
    if (base != nullptr && frame < 0)
        frame = base->find("W_window_fill");
    if (base == nullptr || !layout_pane_rect(*base, frame, ImVec2(0, 0), 1.0f, &mn, &mx) || mx.x - mn.x < 1 ||
        mx.y - mn.y < 1) {
        // Without the game's choice buttons: its row frames, the selected
        // one outlined.
        if (g_rows != nullptr && g_rows->tab) {
            g_rows->tab(dl, a, b, true, label);
            if (selected)
                dl->AddRect(a, b, IM_COL32(255, 210, 40, 255), 0.0f, std::max(2.0f, (b.y - a.y) * 0.08f));
        } else {
            draw_row(dl, a, b, selected);
            dl->AddText(ImVec2(a.x + ImGui::GetFontSize() * 1.55f, (a.y + b.y - ImGui::GetFontSize()) * 0.5f),
                        col(kInk), label);
        }
        return;
    }
    const float rw = mx.x - mn.x, rh = mx.y - mn.y;
    const bool states = choice_states_.size() == 2;
    const Layout& state = states ? choice_states_[selected ? 1 : 0] : *base;
    const float sc = (b.y - a.y) / rh;
    const float dx = std::round((b.x - a.x) / sc - rw);
    const Layout* layout = nullptr;
    const int key = 2 + (selected ? 1 : 0);  // apart from the tabs' states
    for (const auto& [kdx, kstate, l] : wide_tabs_)
        if (kdx == dx && kstate == key)
            layout = &l;
    if (layout == nullptr) {
        if (wide_tabs_.size() > 16)
            wide_tabs_.clear();
        wide_tabs_.emplace_back(dx, key, widened(state, dx, rw * 0.5f));
        layout = &std::get<2>(wide_tabs_.back());
    }
    LayoutDraw d = skin_painter(dl);
    const std::string text = label;
    d.text = [&](const Layout& l, const LayoutPane& p) -> const std::string* {
        return &l == layout && !tab && p.name == "T_text_00" ? &text : nullptr;
    };
    // With `fit_like` the box shrinks with the label against that one, so
    // both get the same factor (text width goes with the size).
    float fit = 1.0f;
    if (fit_like != nullptr) {
        const float own = ImGui::CalcTextSize(label).x, like = ImGui::CalcTextSize(fit_like).x;
        if (like > own && like > 0.0f)
            fit = own / like;
    }
    d.text_fit = [&](const Layout& l, const LayoutPane& p) {
        return &l == layout && p.name == "T_text_00" ? fit : 0.0f;
    };
    d.hidden = [&](const Layout& l, const LayoutPane& p) {
        return &l == layout && !states && !selected && p.name == "N_select_00";
    };
    const ImVec2 c((a.x + b.x) * 0.5f - (mn.x + rw * 0.5f) * sc, (a.y + b.y) * 0.5f - (mn.y + rh * 0.5f) * sc);
    draw_layout(d, *layout, c, sc);
    if (tab) {
        // The label centred in the button's text colour.
        ImU32 ink = col(kInk);
        if (const int t = layout->find("T_text_00"); t >= 0) {
            const LayoutColor& tc = layout->panes[size_t(t)].text_top;
            ink = IM_COL32(tc.r, tc.g, tc.b, 255);
        }
        // As tall as the rows' labels, but all tabs' labels inside their tab
        // with some room (g_tab_label: a small screen's tabs are narrow for
        // that height, the label ran up to the L beside them).
        float fs = std::max(ImGui::GetFontSize() * 0.8f, (b.y - a.y) * 0.6f);
        if (g_tab_label > 0)
            fs = std::min(fs, g_tab_label);
        const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0.0f, label);
        dl->AddText(ImGui::GetFont(), fs, ImVec2(std::round((a.x + b.x - ts.x) * 0.5f), std::round((a.y + b.y - ts.y) * 0.5f)),
                    ink, label);
        return;
    }
    // The game's code puts its cursor into N_cursor (the layout leaves it
    // empty): an orange arrow pointing at the selected choice.
    ImVec2 cmn, cmx;
    if (selected && layout_pane_rect(*layout, layout->find("N_cursor"), c, sc, &cmn, &cmx)) {
        const float s = std::max(8.0f, std::min(cmx.x - cmn.x, cmx.y - cmn.y));
        const ImVec2 m((cmn.x + cmx.x) * 0.5f, (cmn.y + cmx.y) * 0.5f);
        const ImVec2 p0(m.x - s * 0.35f, m.y - s * 0.4f), p1(m.x + s * 0.4f, m.y), p2(m.x - s * 0.35f, m.y + s * 0.4f);
        dl->AddTriangleFilled(p0, p1, p2, IM_COL32(255, 255, 255, 255));
        const float k = s * 0.12f;
        dl->AddTriangleFilled(ImVec2(p0.x + k, p0.y + k * 1.8f), ImVec2(p1.x - k * 1.8f, p1.y),
                              ImVec2(p2.x + k, p2.y - k * 1.8f), IM_COL32(245, 120, 30, 255));
    }
}

bool AppMenu::draw_band(ImDrawList* dl, float x0, float x1, float y0, float y1) {
    const SkinArchive* a = skin_ ? skin_->archive_with("system_window_button_00") : nullptr;
    const Layout* l = a != nullptr ? a->layout("system_window_button_00") : nullptr;
    const int band = l != nullptr ? l->find("P_BG_gradation_00") : -1;
    ImVec2 mn, mx;
    if (band < 0 || y1 - y0 < 1 || !layout_pane_rect(*l, band, ImVec2(0, 0), 1.0f, &mn, &mx) || mx.x - mn.x < 1 ||
        mx.y - mn.y < 1)
        return false;
    // The band, its children and the panes above it; nothing else.
    std::vector<char> keep(l->panes.size(), 0);
    for (int i = band; i >= 0; i = l->panes[size_t(i)].parent)
        keep[size_t(i)] = 1;
    for (size_t i = size_t(band) + 1; i < l->panes.size(); ++i)
        if (l->panes[i].parent >= 0 && keep[size_t(l->panes[i].parent)] && l->panes[i].parent >= band)
            keep[i] = 2;
    LayoutDraw d = skin_painter(dl);
    d.hidden = [&](const Layout& layout, const LayoutPane& p) {
        if (&layout != l)
            return false;
        const size_t i = size_t(&p - l->panes.data());
        return i < keep.size() && keep[i] == 0;
    };
    // The band may be hidden in the layout (an animation shows it). Its own
    // picture (a mint gradient) does not show where the game puts its
    // windows on the band (the controller screen): only its white fill,
    // lines and shadow.
    Layout shown = *l;
    for (int i = band; i >= 0; i = shown.panes[size_t(i)].parent)
        shown.panes[size_t(i)].flags |= 1;
    shown.panes[size_t(band)].kind = PaneKind::Null;
    l = &shown;
    // Stretched across, but its lines and shadow keep the game's thickness:
    // the fill spans y0..y1 with the lines on its edges, the shadow below.
    const ImVec2 view = ImGui::GetMainViewport()->Size;
    const float sx = (x1 - x0) / (mx.x - mn.x), sy = std::min(view.x / 1280.0f, view.y / 720.0f);
    const float cy = (mn.y + mx.y) * 0.5f, top = cy - (y1 - y0) * 0.5f / sy, bottom = cy + (y1 - y0) * 0.5f / sy;
    // Moves (and with resize stretches) pane i to top..bottom in the
    // layout's units, whatever its origin and anchor.
    const auto place = [&](int i, float to_top, float to_bottom, bool resize) {
        LayoutPane& p = shown.panes[size_t(i)];
        ImVec2 a0, a1;
        if (!layout_pane_rect(shown, i, ImVec2(0, 0), 1.0f, &a0, &a1) || a1.y - a0.y < 1e-3f)
            return;
        if (resize) {
            p.height *= (to_bottom - to_top) / (a1.y - a0.y);
            layout_pane_rect(shown, i, ImVec2(0, 0), 1.0f, &a0, &a1);
        }
        ImVec2 b0, b1;
        p.translate[1] += 1.0f;
        layout_pane_rect(shown, i, ImVec2(0, 0), 1.0f, &b0, &b1);
        p.translate[1] -= 1.0f;
        if (std::fabs(b0.y - a0.y) > 1e-4f)
            p.translate[1] += (to_top - a0.y) / (b0.y - a0.y);
    };
    for (const int i : l->panes[size_t(band)].children) {
        const LayoutPane& p = shown.panes[size_t(i)];
        ImVec2 a0, a1;
        if (!layout_pane_rect(shown, i, ImVec2(0, 0), 1.0f, &a0, &a1))
            continue;
        const float h = a1.y - a0.y;
        if (p.name.find("line_top") != std::string::npos)
            place(i, top, top + h, false);
        else if (p.name.find("line_btm") != std::string::npos)
            place(i, bottom - h, bottom, false);
        else if (p.name.find("shadow") != std::string::npos)
            place(i, bottom + (a0.y - mx.y), bottom + (a0.y - mx.y) + h, false);
        else
            place(i, top, bottom, true);
    }
    const ImVec2 c((x0 + x1) * 0.5f - (mn.x + mx.x) * 0.5f * sx, (y0 + y1) * 0.5f - cy * sy);
    draw_layout(d, shown, c, sx, 1.0f, sy);
    return true;
}

// add_image()'s index of the skin's picture `name` (-1: none).
int AppMenu::skin_image(std::string_view name) const {
    if (!skin_)
        return -1;
    for (size_t a = 0; a < skin_->archives.size() && a < skin_images_.size(); ++a)
        if (const int i = skin_->archives[a].image(name); i >= 0)
            return skin_images_[a][size_t(i)];
    return -1;
}

bool AppMenu::draw_layout_preview() {
    if (!skin_ || preview_layout_.empty())
        return false;
    const SkinArchive* arch = skin_->archive(preview_archive_);
    if (arch == nullptr && preview_archive_.size() > 4 &&
        preview_archive_.compare(preview_archive_.size() - 4, 4, ".arc") == 0)
        arch = skin_->archive(preview_archive_.substr(0, preview_archive_.size() - 4));
    const Layout* layout = arch != nullptr ? arch->layout(preview_layout_) : nullptr;
    if (layout == nullptr) {
        if (!preview_warned_)
            SDL_Log("lgp: no layout %s in archive %s", preview_layout_.c_str(), preview_archive_.c_str());
        preview_warned_ = true;
        return false;
    }
    const ImVec2 size = ImGui::GetMainViewport()->Size;
    LayoutDraw d = skin_painter(ImGui::GetBackgroundDrawList());
    d.text = [](const Layout&, const LayoutPane& p) { return &p.name; };
    const float lw = layout->width > 0 ? layout->width : 1280.0f, lh = layout->height > 0 ? layout->height : 720.0f;
    draw_layout(d, *layout, ImVec2(size.x * 0.5f, size.y * 0.5f), std::min(size.x / lw, size.y / lh));
    return true;
}

AppMenu::~AppMenu() {
    if (context_ != nullptr) {
        ImGui::SetCurrentContext(context_);
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext(context_);
    }
    if (gamepads_)
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}

void AppMenu::show(Screen screen) {
    if (screen == Screen::Settings && screen_ != Screen::Settings)
        settings_return_ = in_game_ ? Screen::Pause : Screen::Start;
    if (screen != screen_)
        status_.clear();
    screen_ = screen;
    dirty_ = true;
}

void AppMenu::show_text(TextPrompt prompt) {
    if (screen_ != Screen::Text)
        text_return_ = screen_;
    if (prompt.password != g_ime_password && SDL_TextInputActive(window_))
        SDL_StopTextInput(window_);  // restarted with the other input type
    prompt_ = std::move(prompt);
    text_edit_ = nullptr;  // the game's request replaces a settings field
    g_ime_password = prompt_.password;
    ++prompt_id_;
    text_ = prompt_.initial;
    text_focus_ = !prompt_.message;
    screen_ = Screen::Text;
    status_.clear();
    dirty_ = true;
}

void AppMenu::reopen_text(std::string note) {
    if (screen_ != Screen::Text)
        text_return_ = screen_;
    prompt_.note = std::move(note);
    screen_ = Screen::Text;
    text_focus_ = !prompt_.message;  // Enter let go of the field; same ID, same text
    status_.clear();
    dirty_ = true;
}

void AppMenu::choose_slot(int index) {
    if (slots_.empty())
        return;
    slot_ = std::clamp(index, 0, int(slots_.size()) - 1);
    slot_chosen_ = true;
    show(Screen::Start);
}

void AppMenu::set_slots(std::vector<MenuSlot> slots, int current) {
    slots_ = std::move(slots);
    slot_ = std::clamp(current, 0, std::max(0, int(slots_.size()) - 1));
    dirty_ = true;
}

AppMenu::SafeRect AppMenu::safe_rect() const {
    int w = 0, h = 0;
    SDL_GetWindowSize(window_, &w, &h);
    SDL_Rect safe{};
    if (!SDL_GetWindowSafeArea(window_, &safe) || safe.w <= 0 || safe.h <= 0)
        return {0, 0, float(w), float(h)};
    return {float(safe.x), float(safe.y), float(safe.x + safe.w), float(safe.y + safe.h)};
}

// A tap on a back button: closes an open choice list, else goes back. A tap
// that began with the list open only closes it, also when the list already
// closed on the press (its outside tap).
void AppMenu::tap_back() {
    if (choice_open_)
        choice_cancel_ = true;
    else if (!press_in_choice_)
        back_requested_ = true;
    dirty_ = true;
}

// The settings' back button in the top left corner, in the style of the
// pause button: a released tap inside it (pressed there too) goes back, or
// closes an open choice list.
void AppMenu::corner_back(const SafeRect& safe) {
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    const float r = std::max(18.0f, 0.04f * size.y);
    const ImVec2 c(safe.x0 + r + std::max(12.0f, 0.02f * size.x), safe.y0 + 0.08f * size.y);
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    fg->AddCircleFilled(c, r, IM_COL32(40, 40, 40, 150));
    fg->AddCircle(c, r, IM_COL32(255, 255, 255, 200), 0, std::max(2.0f, r * 0.09f));
    const float a = r * 0.32f, t = std::max(2.5f, r * 0.14f);  // a left-pointing chevron
    fg->AddLine(ImVec2(c.x + a * 0.5f, c.y - a), ImVec2(c.x - a * 0.5f, c.y), IM_COL32(255, 255, 255, 230), t);
    fg->AddLine(ImVec2(c.x - a * 0.5f, c.y), ImVec2(c.x + a * 0.5f, c.y + a), IM_COL32(255, 255, 255, 230), t);
    const ImGuiIO& io = ImGui::GetIO();
    const auto inside = [&](const ImVec2& p) {
        const float dx = p.x - c.x, dy = p.y - c.y, hit = r * 1.3f;  // a bit larger to hit
        return dx * dx + dy * dy <= hit * hit;
    };
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && inside(io.MouseClickedPos[ImGuiMouseButton_Left]) &&
        inside(io.MousePos)) {
        std::printf("menu: back from the corner button\n");
        tap_back();
    }
}

bool AppMenu::over_pause_button(float x, float y) const {
    int w = 0, h = 0;
    SDL_GetWindowSize(window_, &w, &h);
    // Same place as drawn in draw_screen(): top centre below the safe area's
    // top edge, between Minus and Plus of the touch controller
    // (lgp/platform/touch_pad.hpp, TouchPad::set_insets), a bit larger to hit.
    const float r = std::max(22.0f, 0.05f * float(h));
    const float dx = x - float(w) * 0.5f, dy = y - (safe_rect().y0 + 0.08f * float(h));
    return dx * dx + dy * dy <= r * r;
}

namespace {

// The button that, held, opens the pause: Start, or X (west) on a pad
// without Start, such as the Siri Remote under SDL (click A, Back B,
// Play/Pause X; its Back only arrives as short pulses and cannot be held).
SDL_GamepadButton hold_button(SDL_JoystickID which) {
    SDL_Gamepad* pad = SDL_GetGamepadFromID(which);
    if (pad != nullptr && !SDL_GamepadHasButton(pad, SDL_GAMEPAD_BUTTON_START))
        return SDL_GAMEPAD_BUTTON_WEST;
    return SDL_GAMEPAD_BUTTON_START;
}

}  // namespace

// Events the game still sees while a menu screen is open: window and app
// events, controllers coming and going, and every release (keys, buttons,
// fingers, sticks), so nothing held when the menu opened stays pressed in the
// game after it closes.
bool AppMenu::passes_through(const SDL_Event& e) {
    if (e.type >= SDL_EVENT_WINDOW_FIRST && e.type <= SDL_EVENT_WINDOW_LAST)
        return true;
    switch (e.type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
    case SDL_EVENT_DID_ENTER_BACKGROUND:
    case SDL_EVENT_WILL_ENTER_FOREGROUND:
    case SDL_EVENT_DID_ENTER_FOREGROUND:
    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMOVED:
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:  // the game stands; its stick follows the real one
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        return true;
    default:
        return false;
    }
}

bool AppMenu::handle_event(const SDL_Event& e) {
    ImGui::SetCurrentContext(context_);
    if (touch_is_remote_) {
        switch (e.type) {
        case SDL_EVENT_FINGER_DOWN:
        case SDL_EVENT_FINGER_MOTION:
            return screen_ != Screen::None;  // not a tap on the screen, never the pause button
        case SDL_EVENT_MOUSE_MOTION:
            if (e.motion.which == SDL_TOUCH_MOUSEID)
                return screen_ != Screen::None;
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (e.button.which == SDL_TOUCH_MOUSEID)
                return screen_ != Screen::None;  // the game never saw the press either
            break;
        default:
            break;
        }
    }
    // Diagnostics for a menu that took no touches (iPad, first launch right
    // after an install): the first pointer press and focus/foreground changes.
    switch (e.type) {
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (!pointer_seen_)
            std::printf("menu: first %s\n", e.type == SDL_EVENT_FINGER_DOWN ? "touch" : "click");
        pointer_seen_ = true;
        break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED: std::printf("menu: window focus gained\n"); break;
    case SDL_EVENT_WINDOW_FOCUS_LOST: std::printf("menu: window focus lost\n"); break;
    case SDL_EVENT_DID_ENTER_FOREGROUND: std::printf("menu: app in the foreground\n"); break;
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
        // tvOS: Menu on the Siri Remote sent the app home although SDL keeps
        // UIKit out of the remote's presses (controllerUserInteractionEnabled
        // off) unless text input is active (Paul's test, 15.10.2026).
        std::printf("menu: app leaves the foreground (screen %d, text input %s, controller UI events %s)\n",
                    static_cast<int>(screen_), window_ != nullptr && SDL_TextInputActive(window_) ? "on" : "off",
                    SDL_GetHint(SDL_HINT_APPLE_TV_CONTROLLER_UI_EVENTS) != nullptr
                        ? SDL_GetHint(SDL_HINT_APPLE_TV_CONTROLLER_UI_EVENTS)
                        : "unset");
        break;
    case SDL_EVENT_DID_ENTER_BACKGROUND: std::printf("menu: app in the background\n"); break;
    case SDL_EVENT_GAMEPAD_ADDED:
        if (const char* name = SDL_GetGamepadNameForID(e.gdevice.which); name != nullptr)
            std::printf("menu: gamepad \"%s\"\n", name);
        else
            std::printf("menu: gamepad %u\n", unsigned(e.gdevice.which));
        break;
    default: break;
    }
    // Diagnostics for the software keyboard screen (iPad with a keyboard cover:
    // the dialog jumped and reported empty input), capped.
    if (screen_ == Screen::Text && text_log_lines_ < 200) {
        switch (e.type) {
        case SDL_EVENT_SCREEN_KEYBOARD_SHOWN: std::printf("menu: system keyboard shown\n"); ++text_log_lines_; break;
        case SDL_EVENT_SCREEN_KEYBOARD_HIDDEN: std::printf("menu: system keyboard hidden\n"); ++text_log_lines_; break;
        case SDL_EVENT_TEXT_INPUT:
            std::printf("menu: text input event (%zu bytes)\n", std::strlen(e.text.text));
            ++text_log_lines_;
            break;
        case SDL_EVENT_KEY_DOWN:
            if (e.key.key == SDLK_RETURN || e.key.key == SDLK_BACKSPACE || e.key.key == SDLK_ESCAPE) {
                std::printf("menu: key %s%s\n", SDL_GetKeyName(e.key.key), e.key.repeat ? " (repeat)" : "");
                ++text_log_lines_;
            }
            break;
        default: break;
        }
    }
    if (e.type == SDL_EVENT_WINDOW_RESIZED || e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
        e.type == SDL_EVENT_WINDOW_SAFE_AREA_CHANGED)
        dirty_ = true;
    if (screen_ != Screen::None) {
        // Back: Escape, Android's back key, or Home on the controller / the TV
        // remote's menu key (which opened the pause menu, so they also close it).
        // On a controller also Back and Start (the Shield's Home never reaches
        // the app; Start held opened the pause), and B by its label (ImGui's
        // cancel; east on Xbox layouts, south on Nintendo ones) unless a popup
        // or a widget being edited takes it.
        menu_down_ns_ = 0;
        bool pad_back = false;
        if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
            ImGui::SetCurrentContext(context_);
            const SDL_GamepadType type = SDL_GetGamepadTypeForID(e.gbutton.which);
            // Nintendo layout: A is east, so ImGui's activate (south) and
            // cancel (east) swap.
            ImGui::GetIO().ConfigNavSwapGamepadButtons =
                SDL_GetGamepadButtonLabelForType(type, SDL_GAMEPAD_BUTTON_SOUTH) == SDL_GAMEPAD_BUTTON_LABEL_B;
            const SDL_GamepadButtonLabel label =
                SDL_GetGamepadButtonLabelForType(type, static_cast<SDL_GamepadButton>(e.gbutton.button));
            switch (e.gbutton.button) {
            case SDL_GAMEPAD_BUTTON_GUIDE:
            case SDL_GAMEPAD_BUTTON_BACK:
            case SDL_GAMEPAD_BUTTON_START: pad_back = true; break;
            default:
                if (label == SDL_GAMEPAD_BUTTON_LABEL_B || label == SDL_GAMEPAD_BUTTON_LABEL_CIRCLE)
                    pad_back = !ImGui::IsAnyItemActive() &&
                               !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
                break;
            }
        }
        if ((e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat &&
             (e.key.key == SDLK_ESCAPE || e.key.key == SDLK_AC_BACK || e.key.key == SDLK_MENU)) ||
            pad_back) {
            if (back_log_lines_ < 50) {
                ++back_log_lines_;
                if (e.type == SDL_EVENT_KEY_DOWN)
                    std::printf("menu: back by key %s on screen %d\n", SDL_GetKeyName(e.key.key),
                                static_cast<int>(screen_));
                else
                    std::printf("menu: back by pad button %d on screen %d\n", int(e.gbutton.button),
                                static_cast<int>(screen_));
            }
            if (choice_open_) {
                choice_cancel_ = true;
                dirty_ = true;
                return true;
            }
            back_requested_ = true;
            dirty_ = true;
            return true;
        }
        ImGui_ImplSDL3_ProcessEvent(&e);
        return !passes_through(e);
    }
    // ImGui opens its own pads (AutoAll) and refreshes that list on these
    // events only: a controller connected during the game drives the pause too.
    if (e.type == SDL_EVENT_GAMEPAD_ADDED || e.type == SDL_EVENT_GAMEPAD_REMOVED)
        ImGui_ImplSDL3_ProcessEvent(&e);
    if (!in_game_)
        return false;
    switch (e.type) {
    case SDL_EVENT_FINGER_DOWN: {
        int w = 0, h = 0;
        SDL_GetWindowSize(window_, &w, &h);
        if (over_pause_button(e.tfinger.x * float(w), e.tfinger.y * float(h))) {
            pause_requested_ = true;
            dirty_ = true;
            return true;
        }
        return false;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (e.button.which != SDL_TOUCH_MOUSEID && over_pause_button(e.button.x, e.button.y)) {
            pause_requested_ = true;
            dirty_ = true;
            return true;
        }
        return false;
    case SDL_EVENT_KEY_DOWN:
        if (e.key.key == SDLK_ESCAPE && !e.key.repeat) {
            pause_requested_ = true;
            dirty_ = true;
            return true;
        }
        // The menu key of an Android TV remote (no Home key there) is the
        // game's Plus (SdlInput); held for kMenuHoldNs it opens the pause.
        if (e.key.key == SDLK_MENU && !e.key.repeat)
            menu_down_ns_ = e.key.timestamp != 0 ? e.key.timestamp : SDL_GetTicksNS();
        return false;
    case SDL_EVENT_KEY_UP:
        if (e.key.key == SDLK_MENU)
            menu_down_ns_ = 0;
        return false;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        if (e.gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE) {  // Home: the Switch's system button
            pause_requested_ = true;
            dirty_ = true;
            return true;
        }
        // Start is the game's Plus; held (start_hold_pause_, Android and tvOS)
        // it opens the pause like the remote's menu key. Android keeps a
        // controller's Home for itself (Shield), and a TV remote that SDL made
        // a gamepad sends its menu key as Start.
        if (start_hold_pause_ && e.gbutton.button == hold_button(e.gbutton.which))
            menu_down_ns_ = e.gbutton.timestamp != 0 ? e.gbutton.timestamp : SDL_GetTicksNS();
        return false;
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        if (e.gbutton.button == hold_button(e.gbutton.which))
            menu_down_ns_ = 0;
        return false;
    default:
        return false;
    }
}

Action AppMenu::frame() {
    ImGui::SetCurrentContext(context_);
    if (menu_down_ns_ != 0 && screen_ == Screen::None && SDL_GetTicksNS() - menu_down_ns_ >= kMenuHoldNs) {
        menu_down_ns_ = 0;
        pause_requested_ = true;
    }
    if (screen_ == Screen::Text && (pause_requested_ || pressed_ == Action::OpenPause)) {
        // The game waits for the text; the pause screen would hide the request.
        pause_requested_ = false;
        if (pressed_ == Action::OpenPause)
            pressed_ = Action::None;
    }
    if (pause_requested_ || pressed_ == Action::OpenPause) {
        pause_requested_ = false;
        pressed_ = Action::None;
        show(Screen::Pause);
        return Action::OpenPause;
    }
    if (back_requested_) {
        back_requested_ = false;
        switch (screen_) {
        case Screen::Slots: show(Screen::Start); break;
        case Screen::Settings: show(settings_return_); break;
        case Screen::HudBounds: leave_hud_bounds(); break;  // discards the lines' changes
        case Screen::Pause:
            if (pressed_ == Action::None)
                pressed_ = Action::Resume;
            break;
        case Screen::Text:
            if (pressed_ == Action::None)
                pressed_ = prompt_.message ? Action::TextOk : Action::TextCancel;
            break;
        default: break;  // the start screen stays (leaving is "Beenden")
        }
    }
    start_text_input();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    const Screen drawn = screen_;
    Action action = draw_screen();
    if (pressed_ != Action::None) {
        action = pressed_;
        pressed_ = Action::None;
        if (action == Action::Resume)
            show(Screen::None);
        if (action == Action::SettingsChanged)
            sync_mod_needs(settings_);  // a change made from outside (--menu-script), too
    }
    // A settings text row: the field's answer stays in the menu.
    if (text_edit_ != nullptr && screen_ == Screen::Text && action == Action::TextOk) {
        const std::string note = text_edit_server_ && server_check_ && !text_.empty() ? server_check_(text_) : "";
        if (!note.empty()) {
            reopen_text(note);
            action = Action::None;
        } else {
            const bool changed = *text_edit_ != text_;
            *text_edit_ = text_;
            text_focus_row_ = text_edit_;
            text_focus_frames_ = 0, text_focus_seen_ = false;
            text_edit_ = nullptr;
            show(text_return_);
            action = changed ? Action::SettingsChanged : Action::None;
        }
    } else if (text_edit_ != nullptr && screen_ == Screen::Text && action == Action::TextCancel) {
        text_focus_row_ = text_edit_;
        text_focus_frames_ = 0, text_focus_seen_ = false;
        text_edit_ = nullptr;
        show(text_return_);
        action = Action::None;
    }
    if ((action == Action::TextOk || action == Action::TextCancel) && screen_ == Screen::Text)
        show(text_return_);
    if (drawn == Screen::HudBounds && screen_ == Screen::Settings) {
        // The settings once more without new input: ImGui gives an appearing
        // window's first row the cursor for its first frame, and a key in
        // that frame changed that row instead of the one we came from.
        ImGui::Render();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        draw_screen();
    }
    ImGui::Render();
    convert(ImGui::GetDrawData());
    dirty_ = screen_ != drawn;  // a screen change during this frame still needs drawing
    return action;
}

// Back to the settings, the cursor on the row that opened the screen.
void AppMenu::leave_hud_bounds() {
    hud_drag_ = -1;
    text_focus_row_ = &g_hud_bounds_row;
    text_focus_frames_ = 0, text_focus_seen_ = false;
    show(Screen::Settings);
}

void AppMenu::open_hud_bounds() {
    hud_edit_[0] = std::clamp(settings_.hud_height, 0, 100);
    hud_edit_[1] = std::clamp(settings_.hud_width, 0, 100);
    hud_drag_ = -1;
    show(Screen::HudBounds);
}

// The battle's HUD bounds as on a shooter's HUD screen: the game's picture
// (paused in a battle: its displays behind the lines) with a pair of lines
// where the bounds lie and the band outside them shaded. Up/down or a drag
// on a line moves the height pair, left/right the width pair; the lines of a
// pair move mirrored, never inside the 16:9 frame. A applies, B discards, Y
// goes back to 16:9. Only the pair the picture has room for shows.
Action AppMenu::draw_hud_bounds(const ImVec2& size, const SafeRect& safe) {
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    const ImGuiIO& io = ImGui::GetIO();
    const float font = ImGui::GetFontSize(), u = unit();
    const float aspect = g_picture_aspect;
    const platform::ImageRect r = platform::place_image(platform::AspectMode(std::clamp(settings_.aspect, 0, 2)),
                                                        aspect * 720.0f, 720.0f, size.x, size.y);
    // Half the picture's height and width in layout units (1280x720 at 16:9).
    const float half[2] = {std::max(360.0f, 640.0f / aspect), std::max(640.0f, 360.0f * aspect)};
    const float base[2] = {360.0f, 640.0f};
    const bool pair[2] = {half[0] > 360.5f, half[1] > 640.5f};
    const ImVec2 c(r.x + r.width * 0.5f, r.y + r.height * 0.5f);
    const float screen_half[2] = {r.height * 0.5f, r.width * 0.5f};
    // A pair's distance from the middle in window points at v percent, and back.
    const auto reach = [&](int k, int v) {
        return (base[k] + (half[k] - base[k]) * float(v) / 100.0f) / half[k] * screen_half[k];
    };
    const auto value_at = [&](int k, ImVec2 p) {
        const float d = (k == 0 ? std::fabs(p.y - c.y) : std::fabs(p.x - c.x)) / screen_half[k] * half[k];
        return std::clamp(int(std::lround((d - base[k]) / (half[k] - base[k]) * 100.0f)), 0, 100);
    };

    // Keys, pad and stick: 5 % a step, outwards with up and right.
    const auto step = [&](int k, int by) {
        if (pair[k])
            hud_edit_[k] = std::clamp(by > 0 ? (hud_edit_[k] / 5 + 1) * 5 : ((hud_edit_[k] + 4) / 5 - 1) * 5, 0, 100);
    };
    if (key_pressed(ImGuiKey_UpArrow, ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadLStickUp))
        step(0, 1);
    if (key_pressed(ImGuiKey_DownArrow, ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadLStickDown))
        step(0, -1);
    if (key_pressed(ImGuiKey_RightArrow, ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickRight))
        step(1, 1);
    if (key_pressed(ImGuiKey_LeftArrow, ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadLStickLeft))
        step(1, -1);
    // A finger or the mouse takes the nearest line within reach and drags its pair.
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow)) {
        float best = std::max(32.0f, font * 2.0f);
        hud_drag_ = -1;
        for (int k = 0; k < 2; ++k) {
            if (!pair[k])
                continue;
            const float from_middle = k == 0 ? std::fabs(io.MousePos.y - c.y) : std::fabs(io.MousePos.x - c.x);
            const float off = std::fabs(from_middle - reach(k, hud_edit_[k]));
            if (off < best) {
                best = off;
                hud_drag_ = k;
            }
        }
    }
    if (hud_drag_ >= 0) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
            hud_edit_[hud_drag_] = value_at(hud_drag_, io.MousePos);
        else
            hud_drag_ = -1;
    }

    // The band outside the bounds shaded, the lines yellow, inside the
    // picture with their grips whole (at 100 % on its edge).
    const ImVec2 r0(r.x, r.y), r1(r.x + r.width, r.y + r.height);
    const ImU32 shade = IM_COL32(0, 0, 0, 110);
    const float thick = std::max(3.0f, 3.0f * u), grip = std::max(9.0f, font * 0.55f);
    if (pair[0]) {
        const float d = reach(0, hud_edit_[0]);
        bg->AddRectFilled(r0, ImVec2(r1.x, c.y - d), shade);
        bg->AddRectFilled(ImVec2(r0.x, c.y + d), r1, shade);
        for (const float y : {c.y - d, c.y + d}) {
            const float ly = std::clamp(y, r0.y + thick * 0.5f, r1.y - thick * 0.5f);
            const float gy = std::clamp(y, r0.y + grip + 2.0f * u, r1.y - grip - 2.0f * u);
            bg->AddLine(ImVec2(r0.x, ly), ImVec2(r1.x, ly), col(kYellow), thick);
            bg->AddCircleFilled(ImVec2(c.x, gy), grip, col(kYellow));
            bg->AddCircle(ImVec2(c.x, gy), grip, col(kInk), 0, 2.0f * u);
        }
    }
    if (pair[1]) {
        const float d = reach(1, hud_edit_[1]);
        bg->AddRectFilled(r0, ImVec2(c.x - d, r1.y), shade);
        bg->AddRectFilled(ImVec2(c.x + d, r0.y), r1, shade);
        for (const float x : {c.x - d, c.x + d}) {
            const float lx = std::clamp(x, r0.x + thick * 0.5f, r1.x - thick * 0.5f);
            const float gx = std::clamp(x, r0.x + grip + 2.0f * u, r1.x - grip - 2.0f * u);
            bg->AddLine(ImVec2(lx, r0.y), ImVec2(lx, r1.y), col(kYellow), thick);
            bg->AddCircleFilled(ImVec2(gx, c.y), grip, col(kYellow));
            bg->AddCircle(ImVec2(gx, c.y), grip, col(kInk), 0, 2.0f * u);
        }
    }

    // A small panel in the middle: the values and the three buttons. Not in
    // the pad's navigation: the pad moves the lines.
    Action action = Action::None;
    const float width = std::min(safe.x1 - safe.x0 - 32.0f, font * 27.0f);
    ImGui::SetNextWindowPos(ImVec2((safe.x0 + safe.x1) * 0.5f, (safe.y0 + safe.y1) * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0), ImVec2(width, safe.y1 - safe.y0));
    // With the game's skin the panel lies on its white band across the
    // screen, as the settings' choice lists do; else the menus' own panel.
    const bool skinned = g_rows != nullptr;
    ImGui::SetNextWindowBgAlpha(skinned ? 0.0f : 0.88f);
    ImGui::Begin("##hud_bounds", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav);
    if (skinned) {
        const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
        ImDrawList* wl = ImGui::GetWindowDrawList();
        wl->PushClipRectFullScreen();
        if (!draw_band(wl, 0, size.x, wp.y, wp.y + ws.y))
            wl->AddRectFilled(wp, wp + ws, IM_COL32(255, 255, 255, 235));
        wl->PopClipRect();
    }
    panel_header(tr("Battle HUD bounds"));
    const auto value_text = [](int v) { return v <= 0 ? std::string("16:9") : std::to_string(v) + " %"; };
    if (pair[0])
        ImGui::TextUnformatted(trf("Height: {}", {value_text(hud_edit_[0])}).c_str());
    if (pair[1])
        ImGui::TextUnformatted(trf("Width: {}", {value_text(hud_edit_[1])}).c_str());
    if (!pair[0] && !pair[1])
        dim_text(tr("The picture is 16:9: there is no room beyond the 16:9 frame."));
    else
        dim_text(tr("The battle's displays stay inside the yellow lines. Arrows or a drag move them, opposite "
                    "lines together."));
    if (!in_game_)
        dim_text(tr("Open this screen during a battle to see its displays behind the lines."));
    else
        dim_text(tr("Applies from the next battle."));
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float bw = (ImGui::GetContentRegionAvail().x - 2 * gap) / 3.0f, bh = std::max(40.0f, font * 2.0f);
    // The game's choice buttons with the skin, the menus' own without it; all
    // three labels in the size the longest one fits in.
    const char* labels[3] = {tr("Apply"), tr("Discard"), tr("Default")};
    const char* longest = labels[0];
    for (const char* l : labels)
        if (ImGui::CalcTextSize(l).x > ImGui::CalcTextSize(longest).x)
            longest = l;
    const auto button = [&](const char* label) {
        if (!skinned)
            return ImGui::Button(label, ImVec2(bw, bh));
        const bool pressed = ImGui::InvisibleButton(label, ImVec2(bw, bh));
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        draw_choice(ImGui::GetWindowDrawList(), a.x, a.y, b.x, b.y, ImGui::IsItemHovered() && ImGui::IsMouseDown(0),
                    label, false, longest);
        return pressed;
    };
    const bool apply = button(labels[0]);
    ImGui::SameLine();
    const bool discard = button(labels[1]);
    ImGui::SameLine();
    const bool reset = button(labels[2]);
    const bool hints = hints_ < 0 ? SDL_HasGamepad() : hints_ > 0;
    if (hints)
        hint_line({{"A", tr("Apply")}, {"B", tr("Discard")}, {"Y", tr("Default")}});
    panel_frame();
    ImGui::End();

    // A and Y by their labels (Nintendo layout: A east, Y west).
    const bool swap = io.ConfigNavSwapGamepadButtons;
    const ImGuiKey a_key = swap ? ImGuiKey_GamepadFaceRight : ImGuiKey_GamepadFaceDown;
    const ImGuiKey y_key = swap ? ImGuiKey_GamepadFaceLeft : ImGuiKey_GamepadFaceUp;
    if (reset || ImGui::IsKeyPressed(y_key, false) || ImGui::IsKeyPressed(ImGuiKey_Y, false)) {
        hud_edit_[0] = hud_edit_[1] = 0;
    } else if (apply || ImGui::IsKeyPressed(a_key, false) || ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
               ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
        if (settings_.hud_height != hud_edit_[0] || settings_.hud_width != hud_edit_[1]) {
            settings_.hud_height = hud_edit_[0];
            settings_.hud_width = hud_edit_[1];
            action = Action::SettingsChanged;
        }
        leave_hud_bounds();
    } else if (discard) {
        leave_hud_bounds();
    }
    return action;
}

Action AppMenu::draw_screen() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 size = vp->Size;
    // Panels and the pause button keep clear of notch, corners and the home
    // indicator; backgrounds cover the whole window.
    const SafeRect safe = safe_rect();
    const ImVec2 safe_size(safe.x1 - safe.x0, safe.y1 - safe.y0);
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    Action action = Action::None;
    if (draw_layout_preview())
        return action;
    g_picture_aspect = picture_aspect_ > 0 ? picture_aspect_ : size.x / std::max(1.0f, size.y);
    g_docked_next_start = docked_next_start_;
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        press_in_choice_ = choice_open_;  // before anything closes the list this frame
    // The game's own rows and background where the skin has them.
    const SkinArchive* rows_arch = skin_ ? skin_->archive_with("setting_button_00") : nullptr;
    const Layout* row_layout = rows_arch != nullptr ? rows_arch->layout("setting_button_00") : nullptr;
    const SkinArchive* bg_arch = skin_ ? skin_->archive_with("setting_bg_00") : nullptr;
    const Layout* bg_layout = bg_arch != nullptr ? bg_arch->layout("setting_bg_00") : nullptr;
    const float k = std::min(size.x / 1280.0f, size.y / 720.0f);  // screen points per game pixel
    // The menus' text is at least 16 points (create()); on a phone (390
    // points high) that is about 40 % larger than the game's text beside it,
    // and the settings list kept only two rows. There it goes with the
    // screen as on larger ones (a 34th of its height), down to 13 points.
    const bool small_list = screen_ == Screen::Settings && row_layout != nullptr && skin_ &&
                            skin_->archive_with("setting_list_00") != nullptr;
    const float list_font = std::max(13.0f, std::min(ImGui::GetStyle().FontSizeBase, std::min(size.x, size.y) / 34.0f));
    const bool push_font = small_list && list_font < ImGui::GetStyle().FontSizeBase;
    if (push_font)
        ImGui::PushFont(nullptr, list_font);
    struct PopFont {
        bool on;
        ~PopFont() {
            if (on)
                ImGui::PopFont();
        }
    } pop_font{push_font};
    SkinRows rows;
    ImVec2 row_min, row_max;
    int row_pane = row_layout != nullptr ? row_layout->find("W_glare_00") : -1;
    if (row_layout != nullptr && row_pane < 0)
        row_pane = row_layout->find("W_window_fill");
    if (row_layout != nullptr && layout_pane_rect(*row_layout, row_pane, ImVec2(0, 0), 1.0f, &row_min, &row_max) &&
        row_max.x - row_min.x > 1 && row_max.y - row_min.y > 1) {
        const float rw = row_max.x - row_min.x, rh = row_max.y - row_min.y;
        // At least a finger's height (phones), the width following, a
        // quarter wider than the game's rows (the window panes stretch).
        rows.height = std::max(40.0f, rh * k);
        rows.width = std::min(safe_size.x - 32.0f, kWideRows * rw * rows.height / rh);
        rows.height = std::min(rows.height, rows.width * rh / rw);
        rows.draw = [this, row_layout, row_min, rw, rh](ImDrawList* dl, ImVec2 a, ImVec2 b, bool selected,
                                                         const char* label, const std::string* value) {
            // With the game's animations the row's states as they set them,
            // else the selection pane shown or not; as wide as asked.
            const bool animated_states = row_states_.size() == 2;
            // At the row's height, but never wider than the row: a row that
            // is a finger high on a small screen (40 points) is narrower than
            // the game's row at that scale, and its label was cut off.
            const float sc = std::min((b.y - a.y) / rh, (b.x - a.x) / rw);
            const float dx = std::max(0.0f, std::round((b.x - a.x) / sc - rw));
            if (dx != wide_rows_dx_) {
                wide_rows_.clear();
                if (animated_states)
                    for (const Layout& state : row_states_)
                        wide_rows_.push_back(widened(state, dx, rw * 0.5f));
                else
                    wide_rows_.push_back(widened(*row_layout, dx, rw * 0.5f));
                wide_rows_dx_ = dx;
            }
            const Layout* layout = &wide_rows_[animated_states && selected ? 1 : 0];
            LayoutDraw d = skin_painter(dl);
            const std::string label_text = label;
            d.text = [&](const Layout&, const LayoutPane& p) -> const std::string* {
                // Buttons too in the option row's pattern (the game shows
                // N_pattern_01 only through an animation): the label alone.
                if (p.name == "T_text_00" || p.name == "T_text_02")
                    return &label_text;
                if (p.name == "T_text_01")
                    return value;
                return nullptr;
            };
            const ImVec2 c((a.x + b.x) * 0.5f - (row_min.x + rw * 0.5f) * sc, (a.y + b.y) * 0.5f - (row_min.y + rh * 0.5f) * sc);
            // The label may take its pane up to the value's box (where the
            // left arrow sits), not more: a long one (as "Tauschen und
            // Kämpfen im WLAN") ran into the arrow on narrow rows.
            float label_fit = 0;
            ImVec2 l0, l1, v0, v1;
            if (value != nullptr && layout_pane_rect(*layout, layout->find("T_text_00"), c, sc, &l0, &l1) &&
                layout_pane_rect(*layout, layout->find("T_text_01"), c, sc, &v0, &v1) && l1.x - l0.x > 1)
                label_fit = std::max(0.0f, v0.x - 6.0f * sc - l0.x) / (l1.x - l0.x);
            // The value's box reaches under the arrows (they sit inside it,
            // about an eighth of its width each): the value keeps clear.
            d.text_fit = [&](const Layout&, const LayoutPane& p) {
                if (value != nullptr && p.name == "T_text_01")
                    return 0.72f;
                return p.name == "T_text_00" ? label_fit : 0.0f;
            };
            d.hidden = [&](const Layout& l, const LayoutPane& p) {
                if (&l != layout)
                    return false;
                return (!animated_states && p.name == "N_select_00" && !selected) ||
                       p.name == "N_pattern_01" ||
                       (value == nullptr && (p.name == "L_cursor_00" || p.name == "L_cursor_01"));
            };
            draw_layout(d, *layout, c, sc);
        };
        rows.tab = [this, row_layout, row_min, rw, rh](ImDrawList* dl, ImVec2 a, ImVec2 b, bool selected,
                                                        const char* label) {
            const bool animated_states = row_states_.size() == 2;
            const int state = 0;
            const float fade = selected ? 1.0f : 0.55f;
            const float sc = (b.y - a.y) / rh;
            const float dx = std::round((b.x - a.x) / sc - rw);
            const Layout* layout = nullptr;
            for (const auto& [key_dx, key_state, l] : wide_tabs_)
                if (key_dx == dx && key_state == state)
                    layout = &l;
            if (layout == nullptr) {
                if (wide_tabs_.size() > 16)
                    wide_tabs_.clear();
                wide_tabs_.emplace_back(dx, state, widened(animated_states ? row_states_[size_t(state)] : *row_layout,
                                                           dx, rw * 0.5f));
                layout = &std::get<2>(wide_tabs_.back());
            }
            LayoutDraw d = skin_painter(dl);
            d.hidden = [&](const Layout& l, const LayoutPane& p) {
                return &l == layout && ((!animated_states && p.name == "N_select_00") || p.name == "N_pattern_00" ||
                                        p.name == "N_pattern_01");
            };
            const ImVec2 c((a.x + b.x) * 0.5f - (row_min.x + rw * 0.5f) * sc, (a.y + b.y) * 0.5f - (row_min.y + rh * 0.5f) * sc);
            draw_layout(d, *layout, c, sc, fade);
            // The label in the row's text colour, centred.
            ImU32 ink = col(kInk);
            if (const int t = layout->find("T_text_00"); t >= 0) {
                const LayoutColor& tc = layout->panes[size_t(t)].text_top;
                ink = IM_COL32(tc.r, tc.g, tc.b, int(255 * fade));
            }
            const ImVec2 ts = ImGui::CalcTextSize(label);
            dl->AddText(ImVec2(std::round((a.x + b.x - ts.x) * 0.5f), std::round((a.y + b.y - ts.y) * 0.5f)), ink, label);
        };
        rows.value_span = [this, row_min, rw, rh](ImVec2 a, ImVec2 b, float* x0, float* x1) {
            if (wide_rows_.empty())
                return false;
            const Layout& l = wide_rows_[0];
            const float sc = (b.y - a.y) / rh;
            const ImVec2 c((a.x + b.x) * 0.5f - (row_min.x + rw * 0.5f) * sc, (a.y + b.y) * 0.5f - (row_min.y + rh * 0.5f) * sc);
            ImVec2 mn, mx;
            if (!layout_pane_rect(l, l.find("T_text_01"), c, sc, &mn, &mx) || mx.x - mn.x < 1)
                return false;
            // As much as text_fit gives the value (the arrows sit inside).
            const float mid = (mn.x + mx.x) * 0.5f, half = (mx.x - mn.x) * 0.36f;
            *x0 = mid - half;
            *x1 = mid + half;
            return true;
        };
    }
    const SkinArchive* bar_arch = skin_ ? skin_->archive_with("optionbar_00") : nullptr;
    const Layout* bar_layout = bar_arch != nullptr ? bar_arch->layout("optionbar_00") : nullptr;
    float bar_height = 0;  // screen points the game's hint bar takes at the bottom
    float bar_top = 0;     // where the bar begins (0: no bar)
    // The game's bar shows also without a controller ("B Zurück" taps back),
    // so every device has "Zurück" in the same place.
    if (rows.draw && bar_layout != nullptr && hints_ != 0) {
        const float bs = size.x / std::max(1.0f, bar_layout->width);
        ImVec2 bmin, bmax;
        const int bar_pane = bar_layout->find("P_bar");
        if (layout_pane_rect(*bar_layout, bar_pane, ImVec2(size.x * 0.5f, size.y - bar_layout->height * 0.5f * bs), bs,
                             &bmin, &bmax))
            bar_height = std::max(0.0f, size.y - bmin.y);
        bar_top = size.y - bar_height;
        rows.bar_help = bar_layout->find("T_descript_01") >= 0;
        rows.hints = [this, bar_layout, bs, size, safe_x0 = safe.x0, bar_top_y = bmin.y, bar_bottom = bmax.y](const std::vector<std::pair<std::string, std::string>>& hints) {
            // Button icons by name in the skin; a button without one goes
            // into the text.
            static const std::pair<const char*, const char*> kIcons[] = {
                {"A", "button_icon_A_00"}, {"B", "button_icon_B_00"}, {"L R", "button_icon_R_00"},
                {"<>", "button_icon_stick_00"}};
            std::vector<std::string> texts;
            std::vector<int> icons;
            for (const auto& [glyph, text] : hints) {
                int icon = -1;
                for (const auto& [g, name] : kIcons)
                    if (glyph == g)
                        icon = skin_image(name);
                icons.push_back(icon);
                texts.push_back(icon >= 0 ? text : (glyph == "<>" ? std::string("\xE2\x97\x80\xE2\x96\xB6") : glyph) + " " + text);
            }
            LayoutDraw d = skin_painter(ImGui::GetForegroundDrawList());
            const auto set_of = [](const std::string& name, const char* prefix) -> int {
                const size_t n = std::strlen(prefix);
                if (name.size() != n + 2 || name.compare(0, n, prefix) != 0)
                    return -1;
                return std::atoi(name.c_str() + n);
            };
            d.hidden = [&](const Layout&, const LayoutPane& p) {
                const int set = set_of(p.name, "N_optionSet_");
                return (set >= 0 && set >= int(hints.size())) || p.name == "T_descript_01" || p.name == "N_bag_00";
            };
            d.text = [&](const Layout&, const LayoutPane& p) -> const std::string* {
                const int set = set_of(p.name, "T_option_");
                return set >= 0 && set < int(texts.size()) ? &texts[size_t(set)] : nullptr;
            };
            d.picture = [&](const Layout&, const LayoutPane& p, ImTextureID* id, ImVec4* tint) {
                const int set = set_of(p.name, "P_option_");
                if (set < 0 || set >= int(icons.size()))
                    return false;
                if (icons[size_t(set)] < 0) {
                    *tint = ImVec4(0, 0, 0, 0);  // no icon: an invisible quad
                    *id = ImTextureID(image_id(0));
                    return !images_.empty();
                }
                *id = ImTextureID(image_id(icons[size_t(set)]));
                *tint = ImVec4(1, 1, 1, 1);
                return true;
            };
            const ImVec2 at(size.x * 0.5f, size.y - bar_layout->height * 0.5f * bs);
            draw_layout(d, *bar_layout, at, bs);
            // The settings' help on the bar's left, where the game puts its
            // description (T_descript_01): at the menu's text size, wrapped to
            // two lines, smaller (down to 60 %) only where it needs to be.
            if (!g_bar_help.empty()) {
                float right = size.x;
                for (size_t i = 0; i < hints.size(); ++i) {
                    char name[24];
                    std::snprintf(name, sizeof name, "N_optionSet_%02d", int(i));
                    ImVec2 mn, mx;
                    if (layout_pane_rect(*bar_layout, bar_layout->find(name), at, bs, &mn, &mx))
                        right = std::min(right, mn.x);
                }
                const float margin = std::max(12.0f, size.x * 0.02f);
                const float x0 = safe_x0 + margin, x1 = right - margin;
                const float h = (bar_bottom - bar_top_y) * 0.8f;
                ImFont* font = ImGui::GetFont();
                float fs = ImGui::GetFontSize();
                ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, std::max(1.0f, x1 - x0), g_bar_help.c_str());
                // Two lines at most (a taller bar would take a third, which
                // ran past its foot), and inside the bar.
                const auto fits = [&] { return ts.y <= std::min(h, fs * 2.2f) + 0.5f; };
                for (const float scale : {0.9f, 0.8f, 0.7f, 0.6f}) {
                    if (fits())
                        break;
                    fs = ImGui::GetFontSize() * scale;
                    ts = font->CalcTextSizeA(fs, FLT_MAX, std::max(1.0f, x1 - x0), g_bar_help.c_str());
                }
                ImU32 ink = IM_COL32(40, 40, 40, 255);
                if (const int t = bar_layout->find("T_descript_01"); t >= 0) {
                    const LayoutColor& tc = bar_layout->panes[size_t(t)].text_top;
                    ink = IM_COL32(tc.r, tc.g, tc.b, 255);
                }
                if (x1 - x0 > 20)
                    ImGui::GetForegroundDrawList()->AddText(font, fs,
                                                           ImVec2(x0, std::round((bar_top_y + bar_bottom - ts.y) * 0.5f)),
                                                           ink, g_bar_help.c_str(), nullptr, x1 - x0);
            }
            // A tap on "B ..." goes back like B (the bar also shows without a
            // controller, in place of a "Zurück" row). The pane only spans the
            // set's own box, which can be smaller than its icon and text, so
            // the whole bar from the set's left edge to the screen's right
            // counts (B is the last hint).
            const ImGuiIO& io = ImGui::GetIO();
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
                for (size_t i = 0; i < hints.size(); ++i) {
                    char name[24];
                    std::snprintf(name, sizeof name, "N_optionSet_%02d", int(i));
                    ImVec2 mn, mx;
                    if (hints[i].first != "B" || !layout_pane_rect(*bar_layout, bar_layout->find(name), at, bs, &mn, &mx))
                        continue;
                    const ImVec2 zmin(std::min(mn.x, size.x * 0.75f), std::min(bar_top_y, mn.y)), zmax(size.x, size.y);
                    if (contains(zmin, zmax, io.MouseClickedPos[ImGuiMouseButton_Left]) && contains(zmin, zmax, io.MousePos)) {
                        std::printf("menu: back from the hint bar\n");
                        tap_back();
                    }
                }
            ImGui::Dummy(ImVec2(0, 1));
            return true;
        };
    }
    g_rows = rows.draw ? &rows : nullptr;
    g_bar_help.clear();
    struct ResetRows {
        ~ResetRows() { g_rows = nullptr; }
    } reset_rows;

    if (screen_ == Screen::None) {
        if (in_game_) {
            const float r = std::max(18.0f, 0.04f * size.y);
            const ImVec2 c(size.x * 0.5f, safe.y0 + 0.08f * size.y);
            bg->AddCircleFilled(c, r, IM_COL32(255, 255, 255, 40));
            bg->AddCircle(c, r, IM_COL32(255, 255, 255, 150), 0, std::max(2.0f, r * 0.09f));
            const float bw = r * 0.18f, bh = r * 0.8f, gap = r * 0.2f;
            bg->AddRectFilled(ImVec2(c.x - gap - bw, c.y - bh / 2), ImVec2(c.x - gap, c.y + bh / 2),
                              IM_COL32(255, 255, 255, 170), bw * 0.3f);
            bg->AddRectFilled(ImVec2(c.x + gap, c.y - bh / 2), ImVec2(c.x + gap + bw, c.y + bh / 2),
                              IM_COL32(255, 255, 255, 170), bw * 0.3f);
        }
        return action;
    }

    // Before the game the yellow backdrop, in the game the paused image dimmed.
    const float font = ImGui::GetFontSize();
    const float u = unit();
    if (bg_layout != nullptr && (!in_game_ || screen_ == Screen::Settings || screen_ == Screen::Loading)) {
        LayoutDraw d = skin_painter(bg);
        // Its loops (the gears) at 60 frames a second.
        Layout moving;
        if (!bg_base_.empty())
            bg_layout = &bg_base_[0];
        if (!bg_loops_.empty()) {
            moving = *bg_layout;
            const double frames = ImGui::GetTime() * 60.0;
            for (const LayoutAnimation* a : bg_loops_)
                moving = animated(moving, *a, float(std::fmod(frames, double(a->frames))));
            bg_layout = &moving;
        }
        draw_layout(d, *bg_layout, ImVec2(size.x * 0.5f, size.y * 0.5f),
                    std::max(size.x / std::max(1.0f, bg_layout->width), size.y / std::max(1.0f, bg_layout->height)));
    } else if (!in_game_ || screen_ == Screen::Loading) {  // Loading: no game picture behind it yet
        draw_backdrop(bg, size, u);
    } else if (screen_ != Screen::HudBounds) {  // the battle shows undimmed behind the lines
        bg->AddRectFilled(ImVec2(0, 0), size, IM_COL32(0, 0, 0, 140));
    }
    // In the settings' look (the game's background before the game, its
    // band, buttons and hint bar where the skin has them).
    if (screen_ == Screen::HudBounds)
        return draw_hud_bounds(size, safe);

    // One panel in the middle of the safe area (clear of notch, corners and
    // the home indicator) that never leaves the screen and scrolls.
    // Settings in the game's list screen (setting_list_00): its header box
    // with the title, the rows where its list is.
    const SkinArchive* list_arch = g_rows != nullptr && screen_ == Screen::Settings ? skin_->archive_with("setting_list_00")
                                                                                     : nullptr;
    const Layout* list_layout = list_arch != nullptr ? list_arch->layout("setting_list_00") : nullptr;
    ImVec2 list_min, list_max;
    const float list_scale = std::min(safe_size.x / std::max(1.0f, list_layout ? list_layout->width : 1.0f),
                                      (safe_size.y - bar_height) / std::max(1.0f, list_layout ? list_layout->height : 1.0f));
    // Taller than 16:9 (4:3, tablets): the header stays near the top and the
    // list takes the height down to the hint bar (below).
    const float list_extra = list_layout != nullptr
                                 ? std::max(0.0f, safe_size.y - bar_height - list_layout->height * list_scale)
                                 : 0.0f;
    const ImVec2 list_center(size.x * 0.5f, (safe.y0 + safe.y1 - bar_height) * 0.5f - list_extra * 0.4f);
    bool list_frame = list_layout != nullptr &&
                      layout_pane_rect(*list_layout, list_layout->find("scroll_00"), list_center, list_scale, &list_min,
                                       &list_max) &&
                      list_max.x - list_min.x > 100 && list_max.y - list_min.y > 50;
    // The rows at the game's height (its row pane, glow included: the frame
    // inside it is what shows), however much wider the list gets; taller
    // than 16:9 they grow with the free height, up to a fifth.
    const float list_grow =
        list_layout != nullptr ? std::min(1.2f, 1.0f + list_extra / std::max(1.0f, list_layout->height * list_scale)) : 1.0f;
    const float list_rows_height =
        list_frame && row_max.y - row_min.y > 1 ? std::max(40.0f, (row_max.y - row_min.y) * list_scale * list_grow) : 0.0f;
    if (list_frame) {
        const float sw = list_max.x - list_min.x;
        const float dx = std::round((std::min(sw * kWideRows, safe_size.x - 32.0f) - sw) / list_scale);
        if (dx > 0) {
            if (dx != wide_list_dx_) {
                wide_list_ = widened(*list_layout, dx, sw / list_scale * 0.5f);
                // Only the list grows: what sits above it (the header box
                // with its title, its middle above the list's top) keeps the
                // game's size.
                ImVec2 smn, smx;
                const Layout& orig = *list_layout;
                if (layout_pane_rect(orig, orig.find("scroll_00"), ImVec2(0, 0), 1.0f, &smn, &smx)) {
                    std::vector<int> todo(orig.panes.empty() ? std::vector<int>{} : orig.panes[0].children);
                    while (!todo.empty()) {
                        const int i = todo.back();
                        todo.pop_back();
                        ImVec2 mn, mx;
                        if (layout_pane_rect(orig, i, ImVec2(0, 0), 1.0f, &mn, &mx) && mx.y > mn.y &&
                            (mn.y + mx.y) * 0.5f < smn.y) {
                            std::vector<int> sub{i};
                            while (!sub.empty()) {
                                const int j = sub.back();
                                sub.pop_back();
                                wide_list_.panes[size_t(j)] = orig.panes[size_t(j)];
                                for (int c : orig.panes[size_t(j)].children)
                                    sub.push_back(c);
                            }
                            continue;
                        }
                        for (int c : orig.panes[size_t(i)].children)
                            todo.push_back(c);
                    }
                }
                wide_list_dx_ = dx;
            }
            list_layout = &wide_list_;
            list_frame = layout_pane_rect(*list_layout, list_layout->find("scroll_00"), list_center, list_scale,
                                          &list_min, &list_max);
        }
    }
    // The band reaches down to the hint bar (its shadow, 40 game pixels,
    // ending on it) or near the screen's foot; the list (help, "Zurück")
    // uses it all.
    const float band_end = (bar_height > 0 ? bar_top : safe.y1) - 40.0f * k;
    if (list_frame)
        list_max.y = std::max(list_max.y, band_end - 8.0f * k);
    g_title_in_layout = list_frame;
    struct ResetTitle {
        ~ResetTitle() { g_title_in_layout = false; }
    } reset_title;
    if (list_frame) {
        const auto pages = settings_pages(settings_, touch_layout_supported_, in_game_, player1_name_, system_settings_);
        const std::string title = std::string(tr("Settings")) + " \xC2\xB7 " +
                                  pages[size_t(std::clamp(settings_page_, 0, int(pages.size()) - 1))].title;
        LayoutDraw d = skin_painter(bg);
        d.hidden = [&](const Layout& l, const LayoutPane& p) {
            return &l == list_layout &&
                   (p.name == "N_list" || p.name == "N_scroll_bar_00" || p.name == "L_optionbar_00");
        };
        d.text = [&](const Layout& l, const LayoutPane& p) -> const std::string* {
            return &l == list_layout && p.name == "T_text_00" ? &title : nullptr;
        };
        // The game's light band across the screen behind the whole list
        // (from the tabs on), as on its controller screen.
        draw_band(bg, 0, size.x, list_min.y - 10.0f * k, band_end);
        draw_layout(d, *list_layout, list_center, list_scale);
        const float pad = ImGui::GetStyle().WindowPadding.x;
        rows.width = list_max.x - list_min.x - 2 * pad;
        // Rows keep their size; a longer page scrolls as the game's lists do.
        if (list_rows_height > 0)
            rows.height = list_rows_height;
    }
    const float width = g_rows != nullptr ? rows.width + 2 * ImGui::GetStyle().WindowPadding.x
                                          : std::min(safe_size.x - 32.0f, font * (screen_ == Screen::Settings ? 30.0f : 22.0f));
    const float margin = std::min(16.0f, size.y * 0.02f);
    // Text: in the upper half, above the on-screen keyboard.
    const bool text_screen = screen_ == Screen::Text;
    if (list_frame) {
        ImGui::SetNextWindowPos(list_min, ImGuiCond_Always);
        ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0), ImVec2(width, list_max.y - list_min.y));
    } else {
        ImGui::SetNextWindowPos(ImVec2((safe.x0 + safe.x1) * 0.5f,
                                       text_screen ? safe.y0 + margin : (safe.y0 + safe.y1 - bar_height) * 0.5f),
                                ImGuiCond_Always, ImVec2(0.5f, text_screen ? 0.0f : 0.5f));
        // Above the game's hint bar, where it shows.
        ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0), ImVec2(width, safe_size.y - 2 * margin - bar_height));
    }
    // A window per screen: each keeps its own selected row, and a screen
    // opened for the first time selects its first one.
    char name[24];
    std::snprintf(name, sizeof name, "##menu%d", int(screen_));
    if (g_rows != nullptr)
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::Begin(name, nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
                     (overflow_ && !list_frame ? 0 : ImGuiWindowFlags_NoScrollbar));
    if (g_rows != nullptr)
        ImGui::PopStyleColor();
    if (ImGui::GetCurrentWindow()->Appearing && ImGui::GetCurrentWindow()->NavLastIds[0] == 0)
        g_focus_first = true;
    // Tried for a few frames: the window's own first focus may come later.
    if (screen_ != Screen::Settings || ++text_focus_frames_ > 4)
        text_focus_row_ = nullptr;
    g_focus_text = text_focus_row_;
    g_focus_text_reached = false;
    if (g_focus_text != nullptr)
        g_focus_first = false;
    const bool hints = hints_ < 0 ? SDL_HasGamepad() || rows.hints != nullptr : hints_ > 0;
    bool dragging = false;
    // Fingers scroll the panel (ImGui itself only scrolls with a wheel): a
    // vertical drag that starts anywhere but on a value bar; a row under the
    // finger is let go, so scrolling never taps it. With cues (the game's
    // list, which has no scroll bar) orange arrows above and below say there
    // is more.
    const auto scroll_panel = [&](bool cues, float pitch = 0, float pad = 0) {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.MouseSource == ImGuiMouseSource_TouchScreen && !dragging && ImGui::GetScrollMaxY() > 1.0f &&
            ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
            ImGui::IsMouseDragging(ImGuiMouseButton_Left, font * 0.6f)) {
            const ImVec2 drag = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
            if (std::abs(drag.y) > std::abs(drag.x)) {
                ImGui::SetScrollY(ImGui::GetScrollY() - io.MouseDelta.y);
                if (ImGui::IsAnyItemActive())
                    ImGui::ClearActiveID();
            }
        }
        if (!cues || ImGui::GetScrollMaxY() <= 1.0f)
            return;
        // Not while a finger or the mouse holds it: the list rests on its
        // rows' grid (the cursor's scrolling stops a few points off it),
        // and at its end where only the padding below the last row is left.
        float sy = ImGui::GetScrollY();
        const float max_y = ImGui::GetScrollMaxY();
        if (pitch > 1 && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            float to = std::clamp(std::round(sy / pitch) * pitch, 0.0f, max_y);
            // (Not from the top: a list scrolling by less than its padding
            // jumped to its end when it opened.)
            if (sy > 0.5f && max_y - sy <= pad + 0.5f)
                to = max_y;
            if (std::abs(to - sy) > 0.5f) {
                ImGui::SetScrollY(to);
                sy = to;
                dirty_ = true;
            }
        }
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
        const float s = std::max(10.0f, 14.0f * k), x = wp.x + ws.x * 0.5f;
        const auto arrow = [&](float y, float dir) {  // tip at y, pointing up (-1) or down (+1)
            const ImVec2 tip(x, y), l(x - s, y - dir * s * 0.9f), r(x + s, y - dir * s * 0.9f);
            fg->AddTriangleFilled(l, tip, r, IM_COL32(255, 255, 255, 255));
            const float e = s * 0.2f;
            fg->AddTriangleFilled(ImVec2(l.x + e * 1.8f, l.y + dir * e), ImVec2(tip.x, tip.y - dir * e * 1.8f),
                                  ImVec2(r.x - e * 1.8f, r.y + dir * e), IM_COL32(245, 120, 30, 255));
        };
        if (sy > 1.0f)
            arrow(wp.y - s * 0.9f, -1.0f);
        if (sy < max_y - pad - 1.0f)
            arrow(wp.y + ws.y + s * 0.9f, 1.0f);
    };
    switch (screen_) {
    case Screen::Start: {
        panel_header(tr("Main menu"));
        const std::string cont = slots_.empty()        ? std::string(tr("New game"))
                                 : slots_[slot_].empty ? tr("New game") + ("  \xC2\xB7  " + slots_[slot_].name)
                                                       : tr("Continue") + ("  \xC2\xB7  " + slots_[slot_].name);
        if (menu_button((cont + "##play").c_str(), true, slots_.empty() ? std::string() : slots_[slot_].detail))
            action = Action::Play;
        if (menu_button(tr("Choose save")))
            show(Screen::Slots);
        if (!cloud_choice_.empty() && menu_button(tr("Load iCloud state"), true, cloud_choice_))
            action = Action::LoadCloudSaves;
        if (menu_button(tr("Settings")))
            show(Screen::Settings);
        if (menu_button(tr("Quit")))
            action = Action::Quit;
        if (!game_source_.empty())
            dim_text(game_source_);
        if (!cloud_note_.empty())
            dim_text(cloud_note_);
        if (hints)
            hint_line({{"A", tr("Select")}});
        break;
    }
    case Screen::Slots: {
        panel_header(tr("Choose save"));
        for (int i = 0; i < int(slots_.size()); ++i) {
            ImGui::PushID(i);
            if (menu_button((slots_[i].name + "##slot").c_str(), true, slots_[i].detail, i == slot_))
                choose_slot(i);
            ImGui::PopID();
        }
        if (menu_button(tr("New save")))
            action = Action::NewSlot;
        if (menu_button(tr("Back")))
            show(Screen::Start);
        if (hints)
            hint_line({{"A", tr("Select")}, {"B", tr("Back")}});
        break;
    }
    case Screen::Settings: {
        panel_header(tr("Settings"));
        std::vector<Page> pages = settings_pages(settings_, touch_layout_supported_, in_game_, player1_name_, system_settings_);
        settings_page_ = std::clamp(settings_page_, 0, int(pages.size()) - 1);
        int page = settings_page_;
        // Pages as tabs: tapped, or L and R on the controller (Q and E on a
        // keyboard; Page Up/Down would move ImGui's cursor).
        g_choice_open = choice_open_;
        if (pages.size() > 1) {
            if (!choice_open_ && (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || ImGui::IsKeyPressed(ImGuiKey_Q, false)))
                page = (page + int(pages.size()) - 1) % int(pages.size());
            if (!choice_open_ && (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || ImGui::IsKeyPressed(ImGuiKey_E, false)))
                page = (page + 1) % int(pages.size());
            // With the game's rows the tabs are its choice buttons (else its
            // row frames), equally wide across the list with L and R beside
            // them, nine tenths of a row tall.
            const bool skin_tabs = g_rows != nullptr && g_rows->tab;
            float shape_w = 0, shape_text = 0;
            const bool choice_tabs = skin_tabs && choice_shape(&shape_w, &shape_text);
            const float gap = (skin_tabs ? 12 : 8) * u;
            const float th = skin_tabs ? std::max(40.0f, g_rows->height * 0.9f) : std::max(40.0f, font * 2.0f);
            float widest = 0;
            for (const Page& p : pages)
                widest = std::max(widest, ImGui::CalcTextSize(p.title).x);
            const float n = float(pages.size());
            const float across = (g_rows != nullptr ? g_rows->width : 0.0f) - (hints ? 2 * (font * 1.24f + gap) : 0.0f);
            const float even = std::max(widest + font * 3.0f, (across - gap * (n - 1)) / n);
            const auto tab_width = [&](const Page& p) {
                return skin_tabs ? even : ImGui::CalcTextSize(p.title).x + font * 2.0f;
            };
            g_tab_label = 0;
            if (choice_tabs)
                for (const Page& p : pages) {
                    const float fs = std::max(font * 0.8f, th * 0.6f);
                    const float w = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0.0f, p.title).x;
                    const float fit = w > tab_width(p) * 0.85f ? fs * tab_width(p) * 0.85f / w : fs;
                    g_tab_label = g_tab_label > 0 ? std::min(g_tab_label, fit) : fit;
                }
            float tabs_w = 0;
            for (const Page& p : pages)
                tabs_w += tab_width(p) + gap;
            tabs_w -= gap;
            ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), (ImGui::GetWindowWidth() - tabs_w) * 0.5f));
            ImDrawList* dl = ImGui::GetWindowDrawList();
            if (hints) {
                const ImVec2 p = ImGui::GetCursorScreenPos();
                float l = ImGui::GetWindowPos().x + ImGui::GetStyle().WindowPadding.x;
                float r = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;
                if (skin_tabs) {
                    l = p.x - font * 1.24f - gap;
                    r = p.x + tabs_w + font * 1.24f + gap;
                }
                hint_glyph(dl, ImVec2(l, p.y + th * 0.5f - font * 0.62f), "L");
                hint_glyph(dl, ImVec2(r - font * 1.24f, p.y + th * 0.5f - font * 0.62f), "R");
            }
            for (int i = 0; i < int(pages.size()); ++i) {
                ImGui::PushID(i);
                const float tw = tab_width(pages[i]);
                const ImVec2 a = ImGui::GetCursorScreenPos();
                if (ImGui::InvisibleButton("##tab", ImVec2(tw, th)))
                    page = i;
                const ImVec2 b = ImGui::GetItemRectMax();
                const bool on = i == settings_page_;
                if (skin_tabs) {
                    if (choice_tabs)
                        draw_choice(dl, a.x, a.y, b.x, b.y, on, pages[i].title, true);
                    else
                        g_rows->tab(dl, a, b, on, pages[i].title);
                    ImGui::PopID();
                    if (i + 1 < int(pages.size()))
                        ImGui::SameLine(0, gap);
                    continue;
                }
                dl->AddRectFilled(a, b, col(on ? kRed : kRow), th * 0.5f);
                dl->AddRect(a, b, col(kInk, on ? 1.0f : 0.22f), th * 0.5f, on ? 2.5f * u : 1.5f * u);
                const ImVec2 ts = ImGui::CalcTextSize(pages[i].title);
                dl->AddText(ImVec2(a.x + (tw - ts.x) * 0.5f, a.y + (th - ts.y) * 0.5f), col(on ? kPaper : kInk),
                            pages[i].title);
                ImGui::PopID();
                if (i + 1 < int(pages.size()))
                    ImGui::SameLine(0, gap);
            }
            ImGui::Dummy(ImVec2(0, (skin_tabs ? 10 : 4) * u));
            g_tab_label = 0;
        }
        if (page != settings_page_) {
            settings_page_ = page;
            g_focus_first = true;  // the new page starts on its first row
        }
        // The selected row's help (else the page's note) in a fixed place
        // under the rows, as tall as the page's longest but at most two lines:
        // a longer one is set smaller (down to 70 %), so the help never takes
        // the rows' room (small phones).
        const float help_inset = rows.height * 0.45f;
        const float help_wrap = std::max(100.0f, rows.width - 2 * help_inset);
        const float help_font = ImGui::GetFontSize(), help_max = 2 * help_font;
        const auto help_size = [&](const std::string& text, float scale) {
            ImGui::PushFont(nullptr, help_font * scale);
            const float h = ImGui::CalcTextSize(text.c_str(), nullptr, false, help_wrap).y;
            ImGui::PopFont();
            return h;
        };
        const auto help_scale = [&](const std::string& text) {
            for (const float scale : {1.0f, 0.9f, 0.8f})
                if (help_size(text, scale) <= help_max + 0.5f)
                    return scale;
            return 0.7f;
        };
        float help_h = 0;
        const auto fit_help = [&](const std::string& text) {
            if (!text.empty())
                help_h = std::max(help_h, help_size(text, help_scale(text)));
        };
        // With the game's hint bar the help sits in its description field
        // (as in the game's own menus), and the rows have that room.
        const bool help_in_bar = list_frame && hints && g_rows->bar_help;
        if (!help_in_bar) {
            fit_help(pages[size_t(settings_page_)].note);
            for (const Option& o : pages[size_t(settings_page_)].options)
                fit_help(o.help);
        }
        // In the game's list the tabs stay and the rows below them scroll.
        if (list_frame) {
            // Whole rows only: no row cut off at the edges once the cursor
            // has scrolled the list (rows sit on a grid of their pitch).
            const float spacing = ImGui::GetStyle().ItemSpacing.y, pitch = rows.height + spacing;
            float most = list_max.y - ImGui::GetCursorScreenPos().y - ImGui::GetStyle().WindowPadding.y;
            // "Zurück" sits below the scrolling rows (with the arrow's room)
            // unless the hint bar shows B for it.
            if (!hints)
                most -= rows.height + 3 * ImGui::GetStyle().ItemSpacing.y + std::max(10.0f, 14.0f * k) * 2;
            if (help_h > 0)
                most -= help_h + ImGui::GetStyle().ItemSpacing.y + (hints ? std::max(10.0f, 14.0f * k) * 2 : 0.0f);
            if (g_rows != nullptr)
                most = std::max(rows.height, std::floor((most + spacing) / pitch) * pitch - spacing);
            ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, most));
            ImGui::BeginChild("##rows", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_NavFlattened,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
        } else {
            // The own look: the rows scroll on their own, the help (as tall as
            // the page's longest) and the buttons under it stay in view, so a
            // two-line help never pushes "Back" out of the panel.
            const ImGuiStyle& style = ImGui::GetStyle();
            const float button_h = g_rows != nullptr ? g_rows->height : std::max(44.0f, font * 2.2f);
            float foot = button_h + 2 * style.ItemSpacing.y;  // Spacing() and "Back"
            if (pages[size_t(settings_page_)].button)
                foot += button_h + style.ItemSpacing.y;
            // In the panel's own font, wrapped at its width.
            help_h = 0;
            const float wrap = ImGui::GetContentRegionAvail().x;
            const auto own_help = [&](const std::string& text) {
                if (!text.empty())
                    help_h = std::max(help_h, ImGui::CalcTextSize(text.c_str(), nullptr, false, wrap).y);
            };
            own_help(pages[size_t(settings_page_)].note);
            for (const Option& o : pages[size_t(settings_page_)].options)
                own_help(o.help);
            if (help_h > 0)
                foot += help_h + style.ItemSpacing.y;
            if (hints && !(g_rows != nullptr && g_rows->hints))
                foot += font * 1.3f + 2 * style.ItemSpacing.y;  // hint_line()
            const float panel = safe_size.y - 2 * margin - bar_height - 2 * style.WindowPadding.y;
            const float most = std::max(button_h, panel - (ImGui::GetCursorPosY() - style.WindowPadding.y) - foot);
            // The rows' height from their count (option_row()), not ImGui's
            // auto-resize: that lags a frame or more behind a page change, and
            // the cursor keys then jumped over rows still cut off.
            const float row_h = g_rows != nullptr ? g_rows->height : std::max(44.0f, font * 2.3f);
            const float n = float(pages[size_t(settings_page_)].options.size());
            const float rows_h = std::max(0.0f, n * (row_h + style.ItemSpacing.y) - style.ItemSpacing.y);
            ImGui::BeginChild("##rows", ImVec2(0, std::min(most, rows_h)), ImGuiChildFlags_NavFlattened,
                              ImGuiWindowFlags_NoBackground);
        }
        bool changed = false, open_choice = false;
        Page& current = pages[size_t(settings_page_)];
        ImGui::PushID(settings_page_);
        g_choice_request = nullptr;
        g_text_request = nullptr;
        g_open_request = Screen::None;
        const Option* selected_option = nullptr;
        for (Option& o : current.options) {
            g_row_selected = false;
            changed |= option(o, &dragging);
            if (g_row_selected)
                selected_option = &o;
        }
        ImGui::PopID();
        const std::string& help =
            selected_option != nullptr && !selected_option->help.empty() ? selected_option->help : current.note;
        if (g_text_request != nullptr && !choice_open_) {
            TextPrompt prompt;
            prompt.header = g_text_request->label;
            prompt.sub = g_text_request->help;
            prompt.guide = g_text_request->empty_text != nullptr ? g_text_request->empty_text : "";
            prompt.initial = *g_text_request->text;
            prompt.max_length = g_text_request->max_length;
            std::string* target = g_text_request->text;
            const bool server = g_text_request->server;
            show_text(std::move(prompt));
            text_edit_ = target;  // after show_text(), which forgets an earlier edit
            text_edit_server_ = server;
        }
        if (g_open_request == Screen::HudBounds && !choice_open_)
            open_hud_bounds();
        if (g_choice_request != nullptr && !choice_open_) {
            choice_open_ = true;
            choice_cancel_ = false;
            choice_focus_ = true;
            choice_page_ = settings_page_;
            choice_option_ = int(g_choice_request - current.options.data());
            choice_cursor_ = std::clamp(*g_choice_request->value, 0, int(g_choice_request->choices.size()) - 1);
            choice_row_[0] = g_row_min.x, choice_row_[1] = g_row_min.y;
            choice_row_[2] = g_row_max.x, choice_row_[3] = g_row_max.y;
            open_choice = true;
        }
        if (!list_frame) {
            scroll_panel(false);
            ImGui::EndChild();
            if (help_h > 0) {
                const ImVec2 at = ImGui::GetCursorScreenPos();
                dim_text(help);
                ImGui::SetCursorScreenPos(at);
                ImGui::Dummy(ImVec2(0, help_h));
            }
        }
        if (current.button) {
            ImGui::BeginDisabled(!current.button_enabled);
            if (menu_button(current.button))
                action = current.button_action;
            ImGui::EndDisabled();
        }
        if (!list_frame) {
            ImGui::Spacing();
            if (menu_button(tr("Back")))
                show(settings_return_);
        }
        if (list_frame) {
            // A list that scrolls ends on the rows' grid too (the note and
            // spacing above the last button are not a whole row): scrolled
            // to the end, the top row is whole as well.
            float pitch = 0, pad = 0;
            if (g_rows != nullptr && ImGui::GetScrollMaxY() > 1.0f) {
                const float spacing = ImGui::GetStyle().ItemSpacing.y;
                pitch = g_rows->height + spacing;
                const float y = ImGui::GetCursorPosY(), end = y - spacing;  // a Dummy starts at y
                float to = std::ceil((end + spacing - 0.5f) / pitch) * pitch - spacing;
                if (to > end + 0.5f) {
                    if (to < y)
                        to += pitch;
                    ImGui::Dummy(ImVec2(0, to - y));
                    pad = to - end;
                }
            }
            scroll_panel(true, pitch, pad);
            ImGui::EndChild();
            // The help above the foot of the list, set in from the rows'
            // edges like their labels, in their ink.
            const float foot = list_max.y - ImGui::GetStyle().WindowPadding.y - (hints ? 0.0f : rows.height);
            if (help_h > 0) {
                const float y = foot - help_h - (hints ? 0.0f : ImGui::GetStyle().ItemSpacing.y);
                if (ImGui::GetCursorScreenPos().y < y)
                    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, y));
                const ImVec2 at = ImGui::GetCursorScreenPos();
                ImGui::SetCursorScreenPos(ImVec2(at.x + help_inset, at.y));
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + help_wrap);
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(col(kInk, 0.85f)));
                ImGui::PushFont(nullptr, help_font * help_scale(help));
                ImGui::TextUnformatted(help.c_str());
                ImGui::PopFont();
                ImGui::PopStyleColor();
                ImGui::PopTextWrapPos();
                ImGui::SetCursorScreenPos(at);
                ImGui::Dummy(ImVec2(0, help_h));
            }
            // At the foot of the list, whatever the page; with the hint bar
            // its "B Zurück" does it.
            if (!hints) {
                const float y = foot;
                if (ImGui::GetCursorScreenPos().y < y)
                    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, y));
                if (menu_button(tr("Back")))
                    show(settings_return_);
            }
        }
        if (open_choice)  // in the panel's ID scope, where the popup begins
            ImGui::OpenPopup("##choice");
        if (help_in_bar && !choice_open_)
            g_bar_help = help;
        if (hints) {
            if (choice_open_)
                hint_line({{"A", tr("Select")}, {"B", tr("Cancel")}});
            else if (pages.size() > 1)
                hint_line({{"<>", tr("Change")}, {"L R", tr("Page")}, {"B", tr("Back")}});
            else
                hint_line({{"<>", tr("Change")}, {"B", tr("Back")}});
            // On a touch screen also a round back button in the top left
            // corner, the "Zurück" row's place being taken by the bar: its
            // "B Zurück" is not seen as a button (iPhone, 23.10.).
            if (back_button_ > 0 || (back_button_ < 0 && has_touch_screen()))
                corner_back(safe);
        }
        // The choice list: the game's band across the screen with its
        // buttons beside the row it opened from.
        if (choice_open_) {
            Option* o = choice_page_ == settings_page_ && choice_option_ >= 0 &&
                                choice_option_ < int(current.options.size())
                            ? &current.options[size_t(choice_option_)]
                            : nullptr;
            if (o == nullptr || o->value == nullptr || o->choices.empty()) {
                choice_open_ = false;
            } else {
                const float rh = g_rows != nullptr ? std::max(40.0f, g_rows->height * 0.9f) : std::max(44.0f, font * 2.3f);
                float pitch = 0;
                const int n_choices = int(o->choices.size());
                if (const Layout* ctx = context_layout(n_choices, &pitch)) {
                    // The game's context menu, cut to the choices, beside the
                    // row: below it, else above it, its right edge under the
                    // value.
                    // Its rows a little lower than the list's, its text the
                    // size of the rows' labels.
                    const float sc = rh * 0.85f / pitch;
                    int frame = ctx->find("W_window_01");
                    if (frame < 0)
                        frame = ctx->find("W_window_00");
                    ImVec2 fmn(0, 0), fmx(rh * 4, rh * float(n_choices));
                    layout_pane_rect(*ctx, frame, ImVec2(0, 0), sc, &fmn, &fmx);
                    const float w = fmx.x - fmn.x, h = fmx.y - fmn.y, pad = rh * 0.35f;
                    const float bottom = safe.y1 - bar_height - pad;
                    float y = choice_row_[3] + pad;
                    if (y + h > bottom)
                        y = choice_row_[1] - pad - h;
                    y = std::clamp(y, safe.y0 + pad, std::max(safe.y0 + pad, bottom - h));
                    const float x = std::clamp((choice_row_[0] + choice_row_[2] - w) * 0.5f, safe.x0 + 16.0f,
                                               std::max(safe.x0 + 16.0f, safe.x1 - 16.0f - w));
                    const ImVec2 cc(x - fmn.x, y - fmn.y);
                    ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always);
                    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
                    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0, 0, 0, 0.3f));
                    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0, 0, 0, 0));
                    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
                    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
                    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
                    if (ImGui::BeginPopupModal("##choice", nullptr,
                                               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                                   ImGuiWindowFlags_NoScrollbar)) {
                        ImDrawList* pl = ImGui::GetWindowDrawList();
                        pl->PushClipRectFullScreen();
                        draw_band(pl, 0, size.x, y - pad, y + h + pad);
                        int chosen = -1;
                        char cname[24];
                        for (int i = 0; i < n_choices; ++i) {
                            std::snprintf(cname, sizeof cname, "N_cursor_%02d", i);
                            ImVec2 ra, rb;
                            if (!layout_pane_rect(*ctx, ctx->find(cname), cc, sc, &ra, &rb))
                                continue;
                            ImGui::PushID(i);
                            ImGui::SetCursorScreenPos(ImVec2(x, ra.y));
                            if (ImGui::InvisibleButton("##c", ImVec2(w, std::max(1.0f, rb.y - ra.y)),
                                                       ImGuiButtonFlags_EnableNav))
                                chosen = i;
                            const ImGuiIO& mio = ImGui::GetIO();
                            const bool mouse_moved = mio.MouseSource == ImGuiMouseSource_Mouse &&
                                                     (mio.MouseDelta.x != 0 || mio.MouseDelta.y != 0);
                            if ((choice_focus_ && i == choice_cursor_) ||
                                (!choice_focus_ && mouse_moved && ImGui::IsItemHovered() && !ImGui::IsItemFocused())) {
                                ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
                                if (choice_focus_ && i == choice_cursor_)
                                    choice_focus_ = false;
                                choice_cursor_ = i;
                            }
                            if (ImGui::IsItemFocused())
                                choice_cursor_ = i;
                            ImGui::PopID();
                        }
                        // The marked row and its arrow on the cursor's row.
                        const int pos = context_cut_.find("N_curosr_pos_00");
                        std::snprintf(cname, sizeof cname, "N_cursor_%02d", choice_cursor_);
                        const int ck = context_base_.find(cname), c0 = context_base_.find("N_cursor_00");
                        if (pos >= 0 && ck >= 0 && c0 >= 0)
                            context_cut_.panes[size_t(pos)].translate[1] =
                                context_base_.panes[size_t(pos)].translate[1] +
                                context_base_.panes[size_t(ck)].translate[1] - context_base_.panes[size_t(c0)].translate[1];
                        std::vector<std::string> labels(o->choices.begin(), o->choices.end());
                        LayoutDraw d = skin_painter(pl);
                        d.text = [&](const Layout& l, const LayoutPane& p) -> const std::string* {
                            int k = -1;
                            if (&l != &context_cut_ || std::sscanf(p.name.c_str(), "T_text_%d", &k) != 1 || k < 0 ||
                                k >= int(labels.size()))
                                return nullptr;
                            return &labels[size_t(k)];
                        };
                        draw_layout(d, context_cut_, cc, sc);
                        pl->PopClipRect();
                        const ImGuiIO& io = ImGui::GetIO();
                        const ImGuiKey cancel_key =
                            io.ConfigNavSwapGamepadButtons ? ImGuiKey_GamepadFaceDown : ImGuiKey_GamepadFaceRight;
                        const bool outside = !ImGui::IsWindowAppearing() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                                             !ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
                        if (chosen >= 0) {
                            if (*o->value != chosen) {
                                *o->value = chosen;
                                changed = true;
                            }
                            choice_open_ = false;
                        } else if (choice_cancel_ || ImGui::IsKeyPressed(cancel_key, false) || outside) {
                            choice_open_ = false;
                        }
                        if (!choice_open_)
                            ImGui::CloseCurrentPopup();
                        ImGui::EndPopup();
                    } else {
                        choice_open_ = false;
                    }
                    ImGui::PopStyleVar(2);
                    ImGui::PopStyleColor(3);
                    choice_cancel_ = false;
                } else {
                const float gap = 4 * u;
                float tw = 0;
                for (const char* c : o->choices)
                    tw = std::max(tw, ImGui::CalcTextSize(c).x);
                // As wide as the game's choice buttons where they fit, at
                // least the longest choice and the cursor before it.
                float w = std::max(rh * 3.0f, tw + font * 5.0f);
                if (float natural = 0, text = 0; choice_shape(&natural, &text))
                    w = std::max(std::min(natural * rh, std::max(tw / text + rh * 1.5f, natural * rh * 0.55f)), w);
                w = std::min(w, safe_size.x - 32.0f);
                const float h = float(o->choices.size()) * (rh + gap) - gap;
                const float pad = rh * 0.35f;
                // Beside the row it opened from: below it, else above it,
                // its right edge under the value.
                const float bottom = safe.y1 - bar_height - pad;
                float y = choice_row_[3] + pad;
                if (y + h > bottom)
                    y = choice_row_[1] - pad - h;
                y = std::clamp(y, safe.y0 + pad, std::max(safe.y0 + pad, bottom - h));
                const float x = std::clamp(choice_row_[2] - w, safe.x0 + 16.0f, std::max(safe.x0 + 16.0f, safe.x1 - 16.0f - w));
                ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always);
                ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
                ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0, 0, 0, 0.3f));
                ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, gap));
                if (ImGui::BeginPopupModal("##choice", nullptr,
                                           ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                               ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar)) {
                    ImDrawList* pl = ImGui::GetWindowDrawList();
                    // The game's band across the screen behind the buttons,
                    // over the dimmed list.
                    pl->PushClipRectFullScreen();
                    if (!draw_band(pl, 0, size.x, y - pad, y + h + pad)) {
                        pl->AddRectFilled(ImVec2(0, y - pad), ImVec2(size.x, y + h + pad), IM_COL32(255, 255, 255, 110));
                        pl->AddRectFilled(ImVec2(0, y - pad), ImVec2(size.x, y - pad + 2), IM_COL32(255, 255, 255, 230));
                        pl->AddRectFilled(ImVec2(0, y + h + pad - 2), ImVec2(size.x, y + h + pad),
                                          IM_COL32(255, 255, 255, 230));
                    }
                    pl->PopClipRect();
                    int chosen = -1;
                    for (int i = 0; i < int(o->choices.size()); ++i) {
                        ImGui::PushID(i);
                        const ImVec2 a = ImGui::GetCursorScreenPos();
                        if (ImGui::InvisibleButton("##c", ImVec2(w, rh), ImGuiButtonFlags_EnableNav))
                            chosen = i;
                        // One row selected: the cursor's; a moving mouse
                        // takes it along.
                        const ImGuiIO& mio = ImGui::GetIO();
                        const bool mouse_moved = mio.MouseSource == ImGuiMouseSource_Mouse &&
                                                 (mio.MouseDelta.x != 0 || mio.MouseDelta.y != 0);
                        if ((choice_focus_ && i == choice_cursor_) ||
                            (!choice_focus_ && mouse_moved && ImGui::IsItemHovered() && !ImGui::IsItemFocused())) {
                            ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
                            if (choice_focus_ && i == choice_cursor_)
                                choice_focus_ = false;
                            choice_cursor_ = i;
                        }
                        if (ImGui::IsItemFocused())
                            choice_cursor_ = i;
                        const ImVec2 e = ImGui::GetItemRectMax();
                        draw_choice(pl, a.x, a.y, e.x, e.y, i == choice_cursor_, o->choices[size_t(i)]);
                        ImGui::PopID();
                    }
                    const ImGuiIO& io = ImGui::GetIO();
                    const ImGuiKey cancel_key = io.ConfigNavSwapGamepadButtons ? ImGuiKey_GamepadFaceDown : ImGuiKey_GamepadFaceRight;
                    // A tap beside the list closes it (not the tap that opened it).
                    const bool outside = !ImGui::IsWindowAppearing() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                                         !ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
                    if (chosen >= 0) {
                        if (*o->value != chosen) {
                            *o->value = chosen;
                            changed = true;
                        }
                        choice_open_ = false;
                    } else if (choice_cancel_ || ImGui::IsKeyPressed(cancel_key, false) || outside) {
                        choice_open_ = false;
                    }
                    if (!choice_open_)
                        ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                } else {
                    choice_open_ = false;
                }
                ImGui::PopStyleVar(2);
                ImGui::PopStyleColor(3);
                choice_cancel_ = false;
                }
            }
        }
        if (changed) {
            sync_picture_format(settings_);  // a picture format row's choice into aspect and the mod
            sync_mod_needs(settings_);       // the throw off with the balls, in this change already
            action = Action::SettingsChanged;
        }
        break;
    }
    case Screen::Pause: {
        panel_header(tr("Pause"));
        if (menu_button(tr("Resume")))
            action = Action::Resume;
        if (menu_button(tr("Back up save"), backup_available_))
            action = Action::Backup;
        dim_text(tr("The game saves in the game (menu \xE2\x86\x92 Save). \"Back up\" also makes a copy of the "
                    "save."));
        if (menu_button(tr("Settings")))
            show(Screen::Settings);
        if (menu_button(tr("Quit game")))
            action = Action::QuitGame;
        if (hints)
            hint_line({{"A", tr("Select")}, {"B", tr("Resume")}});
        break;
    }
    case Screen::Text: {
        panel_header(prompt_.header.empty() ? (prompt_.message ? tr("Note") : tr("Enter text")) : prompt_.header.c_str());
        if (!prompt_.sub.empty())
            ImGui::TextWrapped("%s", prompt_.sub.c_str());
        if (prompt_.message) {
            if (menu_button("OK"))
                action = Action::TextOk;
            break;
        }
        if (text_focus_) {
            ImGui::SetKeyboardFocusHere();  // the field takes the keys; on phones the keyboard opens
            text_focus_ = false;
        }
        ImGui::SetNextItemWidth(-1);
        const ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackResize |
                                          (prompt_.max_length != 0 ? ImGuiInputTextFlags_CallbackEdit : 0) |
                                          (prompt_.password ? ImGuiInputTextFlags_Password : 0);
        // The field edits text_ in place, growing it as needed (no fixed buffer
        // that would cut a long text inside a UTF-8 sequence).
        struct Field {
            std::string* text;
            uint32_t max_length;  // UTF-16 units, 0: none
        } field{&text_, prompt_.max_length};
        const auto callback = [](ImGuiInputTextCallbackData* d) {
            auto* f = static_cast<Field*>(d->UserData);
            if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
                f->text->resize(static_cast<size_t>(d->BufTextLen));
                d->Buf = f->text->data();
            } else if (d->EventFlag == ImGuiInputTextFlags_CallbackEdit) {
                // Cut in the field itself: the active field keeps its own copy,
                // cutting text_ alone left it showing more than the count said.
                const size_t keep = utf16_prefix(d->Buf, static_cast<size_t>(d->BufTextLen), f->max_length);
                if (keep < static_cast<size_t>(d->BufTextLen))
                    d->DeleteChars(static_cast<int>(keep), d->BufTextLen - static_cast<int>(keep));
            }
            return 0;
        };
        ImGui::PushID(prompt_id_);
        const bool enter = ImGui::InputTextWithHint("##text", prompt_.guide.c_str(), text_.data(), text_.capacity() + 1,
                                                    flags, callback, &field);
        ImGui::PopID();
        if (prompt_.max_length != 0) {
            // UTF-16 units as the game counts them; whole characters (also the initial text).
            text_.resize(utf16_prefix(text_.data(), text_.size(), prompt_.max_length));
            size_t units = 0;
            for (const char c : text_) {
                const auto b = static_cast<uint8_t>(c);
                if ((b & 0xC0) != 0x80)
                    units += b >= 0xF0 ? 2 : 1;  // per lead byte; 4 bytes are a surrogate pair
            }
            dim_text(std::to_string(units) + " / " + std::to_string(prompt_.max_length));
        }
        note_text(prompt_.note);
        if (enter || menu_button("OK"))
            action = Action::TextOk;
        if (menu_button(tr("Cancel")))
            action = Action::TextCancel;
        break;
    }
    case Screen::Loading: {
        panel_header(tr("Preparing graphics"));
        dim_text(tr("The first start builds the game's shaders once, so the game does not stutter later. Later "
                    "starts are quicker."));
        const float fraction = progress_total_ == 0 ? 0.0f : float(progress_done_) / float(progress_total_);
        const std::string count = std::to_string(progress_done_) + " / " + std::to_string(progress_total_);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.96f, 0.47f, 0.12f, 1.0f));
        ImGui::ProgressBar(fraction, ImVec2(-1, font * 1.2f), count.c_str());
        ImGui::PopStyleColor();
        ImGui::Spacing();
        if (menu_button(tr("Start now")))
            action = Action::Play;
        dim_text(tr("The rest is built while playing; the game may stutter."));
        if (hints)
            hint_line({{"A", tr("Select")}});
        break;
    }
    case Screen::None:
    case Screen::HudBounds:  // draw_hud_bounds()
        break;
    }
    if (!status_.empty()) {
        ImGui::Spacing();
        note_text(status_);
    }
    scroll_panel(false);
    // A scroll bar only where the panel is cut off (seen one frame later).
    overflow_ = ImGui::GetScrollMaxY() > 1.0f;  // auto-resize leaves a fraction of a point
    ImGui::Dummy(ImVec2(0, 2 * u));
    panel_frame();
    ImGui::End();
    g_focus_first = false;
    // Kept until the row still has it a frame later (ImGui's own first
    // focus of an appearing window comes at the end of its frame).
    if (g_focus_text_reached && text_focus_seen_)
        text_focus_row_ = nullptr;
    text_focus_seen_ = g_focus_text_reached;
    g_focus_text = nullptr;
    if (action == Action::Resume)
        show(Screen::None);
    return action;
}

void AppMenu::convert(ImDrawData* data) {
    // Textures first: the font atlas is the only one. ImGui asks for it to be
    // created or updated; this backend always hands over the whole atlas.
    if (data->Textures != nullptr)
        for (ImTextureData* tex : *data->Textures) {
            if (tex->Status == ImTextureStatus_WantCreate || tex->Status == ImTextureStatus_WantUpdates) {
                auto copy = std::make_shared<UiTexture>();
                copy->width = uint32_t(tex->Width);
                copy->height = uint32_t(tex->Height);
                copy->version = texture_ ? texture_->version + 1 : 1;
                copy->rgba.resize(size_t(tex->Width) * size_t(tex->Height) * 4);
                if (tex->Format == ImTextureFormat_RGBA32) {
                    std::memcpy(copy->rgba.data(), tex->GetPixels(), copy->rgba.size());
                } else {  // Alpha8
                    const auto* a = static_cast<const uint8_t*>(tex->GetPixels());
                    for (size_t i = 0; i < size_t(tex->Width) * size_t(tex->Height); ++i) {
                        copy->rgba[i * 4 + 0] = copy->rgba[i * 4 + 1] = copy->rgba[i * 4 + 2] = 255;
                        copy->rgba[i * 4 + 3] = a[i];
                    }
                }
                texture_ = std::move(copy);
                tex->SetTexID(ImTextureID(1));
                tex->SetStatus(ImTextureStatus_OK);
            } else if (tex->Status == ImTextureStatus_WantDestroy && tex->UnusedFrames > 0) {
                tex->SetTexID(ImTextureID_Invalid);
                tex->SetStatus(ImTextureStatus_Destroyed);
            }
        }

    auto out = std::make_shared<UiDrawData>();
    out->width = data->DisplaySize.x;
    out->height = data->DisplaySize.y;
    out->texture = texture_;
    out->images.assign(images_.begin(), images_.end());
    out->vertices.reserve(size_t(data->TotalVtxCount));
    out->indices.reserve(size_t(data->TotalIdxCount));
    for (const ImDrawList* list : data->CmdLists) {
        const uint32_t vtx_base = uint32_t(out->vertices.size());
        const uint32_t idx_base = uint32_t(out->indices.size());
        for (const ImDrawVert& v : list->VtxBuffer)
            out->vertices.push_back({v.pos.x - data->DisplayPos.x, v.pos.y - data->DisplayPos.y, v.uv.x, v.uv.y, v.col});
        for (const ImDrawIdx i : list->IdxBuffer)
            out->indices.push_back(i);
        for (const ImDrawCmd& cmd : list->CmdBuffer) {
            if (cmd.UserCallback != nullptr || cmd.ElemCount == 0)
                continue;
            UiCommand c;
            c.clip[0] = cmd.ClipRect.x - data->DisplayPos.x;
            c.clip[1] = cmd.ClipRect.y - data->DisplayPos.y;
            c.clip[2] = cmd.ClipRect.z - data->DisplayPos.x;
            c.clip[3] = cmd.ClipRect.w - data->DisplayPos.y;
            c.first_index = idx_base + cmd.IdxOffset;
            c.index_count = cmd.ElemCount;
            c.base_vertex = vtx_base + cmd.VtxOffset;
            ImTextureID id = cmd.GetTexID();
            if (id & kMultiplyBit) {
                c.blend = 1;
                id &= ~kMultiplyBit;
            }
            if (id >= 2) {
                if (id - 2 >= images_.size())
                    continue;
                c.image = uint32_t(id - 1);
            }
            out->commands.push_back(c);
        }
    }
    draw_data_ = out->commands.empty() ? nullptr : std::move(out);
}

}  // namespace lgp::platform::ui
