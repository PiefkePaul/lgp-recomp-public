// Draws a game layout (lgp/platform/ui/layout.hpp) into an ImGui draw list:
// pictures, windows and parts with the game's textures, text panes with the
// menu's own strings. Coordinates: the layout's root centre at `center`,
// `scale` screen points per layout unit, layout y up.
#pragma once

#include <functional>
#include <string>
#include <string_view>

#include "imgui.h"
#include "lgp/platform/ui/layout.hpp"

namespace lgp::platform::ui {

struct LayoutDraw {
    ImDrawList* list = nullptr;
    // The texture of a material (its first map) and the colour to multiply
    // it with; false: draw the material's colour without a texture.
    std::function<bool(const Layout&, const LayoutMaterial&, ImTextureID* id, ImVec4* tint)> texture;
    // A picture pane's texture instead of its material's (e.g. a button
    // icon); false: the material's.
    std::function<bool(const Layout&, const LayoutPane&, ImTextureID* id, ImVec4* tint)> picture;
    // The text of a text pane (nullptr: leave it out; the layouts' own
    // strings are placeholders).
    std::function<const std::string*(const Layout&, const LayoutPane&)> text;
    // The share of a text pane's width its text may take before it is made
    // smaller (down to two thirds); 0 or no callback: it runs over.
    std::function<float(const Layout&, const LayoutPane&)> text_fit;
    // Panes to leave out with their children (state the layout's
    // animations would set).
    std::function<bool(const Layout&, const LayoutPane&)> hidden;
    // The layout of a part pane.
    std::function<const Layout*(std::string_view part)> part;
    ImFont* font = nullptr;
    float text_scale = 1.0f;  // the texts' size against the layout's
};

// scale_y > 0: that many screen points per layout unit vertically (a band
// stretched to a height), scale horizontally.
void draw_layout(const LayoutDraw& d, const Layout& l, ImVec2 center, float scale, float alpha = 1.0f,
                 float scale_y = 0.0f);

// The layout `dx` units wider (layout units of the root): panes at least
// `min_width` wide that cross the middle grow by dx, all others move dx/2
// outwards with their side. For window and fill panes, which stretch
// without distortion (the menu's rows and list frame).
Layout widened(const Layout& l, float dx, float min_width);

// The screen rectangle (bounds) of pane `index`; false if there is none.
bool layout_pane_rect(const Layout& l, int index, ImVec2 center, float scale, ImVec2* min, ImVec2* max);

}  // namespace lgp::platform::ui
