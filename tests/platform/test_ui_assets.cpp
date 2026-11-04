// The game's interface files for the menus, on files built here
// (ui_fixtures.hpp): textures (platform/ui/bntx.hpp, an RGBA8 texture in
// block-linear layout with a channel selector), layouts (layout.hpp) and an
// archive of both (layout_skin.hpp).
//   lgp_test_ui_assets --write-arcs DIR   writes the test archives (for lgp_menu_preview --skin)
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "../runtime/check.hpp"
#include "lgp/platform/ui/bntx.hpp"
#include "lgp/platform/ui/layout_skin.hpp"
#include "../../platform/ui/src/layout_draw.hpp"
#include "ui_fixtures.hpp"

using namespace lgp::platform::ui;
using lgp::test::make_bflyt;
using lgp::test::make_bntx;
using lgp::test::make_sarc;

namespace {

void test_bntx_rgba() {
    const uint32_t w = 20, h = 11;
    std::vector<uint8_t> rgba(size_t(w) * h * 4);
    for (size_t i = 0; i < rgba.size(); ++i)
        rgba[i] = uint8_t(i * 13 + 7);
    std::string error;
    auto images = decode_bntx(make_bntx(w, h, rgba, 0x05040302), &error);  // identity selector
    CHECK_EQ(images.size(), 1u);
    if (images.empty())
        return;
    CHECK(images[0].name == "test");
    CHECK_EQ(images[0].width, w);
    CHECK(images[0].rgba == rgba);
    // A mask texture: white, alpha from red (selector 1, 1, 1, R).
    images = decode_bntx(make_bntx(w, h, rgba, 0x02010101), &error);
    CHECK_EQ(images.size(), 1u);
    if (!images.empty()) {
        CHECK_EQ(images[0].rgba[0], 255);
        CHECK_EQ(images[0].rgba[3], rgba[0]);
    }
    CHECK(decode_bntx(std::vector<uint8_t>(0x100, 0), &error).empty());
}

void test_layout() {
    for (const bool switch_order : {false, true}) {
        std::string error;
        const auto l = parse_bflyt(make_bflyt("tex_00", switch_order), &error);
        CHECK(l.has_value());
        if (!l)
            continue;
        CHECK(l->warnings.empty());
        CHECK_EQ(l->width, 1280.0f);
        CHECK(l->textures.size() == 1 && l->textures[0] == "tex_00");
        CHECK_EQ(l->materials.size(), 1u);
        if (l->materials.size() == 1) {
            const LayoutMaterial& m = l->materials[0];
            CHECK(m.name == "P_picture");
            CHECK(m.maps.size() == 1 && m.maps[0].texture == 0 && m.maps[0].wrap_s == 1);
            CHECK_EQ(m.black.r, 0x30);
            CHECK_EQ(m.black.b, 0x10);
            CHECK_EQ(m.white.a, 255);
        }
        CHECK_EQ(l->panes.size(), 4u);
        const int pic = l->find("P_picture"), group = l->find("N_group"), text = l->find("T_label");
        CHECK(pic == 1 && group == 2 && text == 3);
        if (pic != 1 || group != 2 || text != 3)
            continue;
        CHECK(l->panes[0].children == std::vector<int>({1, 2}));
        CHECK_EQ(l->panes[3].parent, 2);
        CHECK(l->panes[1].kind == PaneKind::Picture);
        CHECK_EQ(l->panes[1].material, 0);
        CHECK_EQ(l->panes[1].uvs.size(), 1u);
        CHECK_EQ(l->panes[1].translate[0], -600.0f);
        CHECK_EQ(l->panes[1].origin, 5);
        CHECK(l->panes[3].kind == PaneKind::Text);
        CHECK_EQ(l->panes[3].font_size[1], 32.0f);
        CHECK_EQ(l->panes[3].text_top.r, 0xFF);
        CHECK_EQ(l->panes[3].text_top.a, 0xFF);
    }
    std::string error;
    CHECK(!parse_bflyt(std::vector<uint8_t>(32, 0), &error).has_value());
}

void test_animation() {
    std::string error;
    const auto a = parse_bflan(lgp::test::make_bflan(10, {{"P_picture", "FLVI", 0, 0},
                                                          {"N_group", "FLPA", 6, 2.0f},
                                                          {"T_label", "FLVC", 16, 128}}),
                               &error);
    CHECK(a.has_value());
    if (!a)
        return;
    CHECK_EQ(a->frames, 10.0f);
    CHECK_EQ(a->groups.size(), 3u);
    if (a->groups.size() != 3)
        return;
    CHECK(a->groups[1].name == "N_group" && a->groups[1].curves.size() == 1);
    CHECK_EQ(a->groups[1].curves[0].kind, 2);
    CHECK_EQ(a->groups[1].curves[0].at(5), 2.0f);
    const auto l = parse_bflyt(make_bflyt("tex_00"), &error);
    CHECK(l.has_value());
    if (!l)
        return;
    const Layout end = animated(*l, *a);
    CHECK(!end.panes[1].visible());
    CHECK_EQ(end.panes[2].scale[0], 2.0f);
    CHECK_EQ(end.panes[3].alpha, 128);
    CHECK(l->panes[1].visible());  // the original stays
    // Hermite between two keys with slopes 0: smooth, halfway at the middle.
    LayoutCurve c;
    c.kind = 2;
    c.keys = {{0, 0, 0}, {10, 1, 0}};
    CHECK_EQ(c.at(5), 0.5f);
    CHECK(c.at(2) < 0.2f);
    CHECK(!parse_bflan(std::vector<uint8_t>(32, 0), &error).has_value());
}

// A row like the menu's: a frame across the middle, a label on the left, a
// value on the right (in a group), a centred group; 40 units wider.
void test_widened() {
    Layout l;
    const auto add = [&](const char* name, int parent, float tx, float w, uint8_t origin = 0) {
        LayoutPane p;
        p.name = name;
        p.parent = parent;
        p.translate[0] = tx;
        p.width = w;
        p.height = 10;
        p.origin = origin;
        l.panes.push_back(p);
        if (parent >= 0)
            l.panes[size_t(parent)].children.push_back(int(l.panes.size()) - 1);
    };
    add("root", -1, 0, 0);
    add("W_frame", 0, 0, 200);
    add("T_label", 1, 10, 60, 1 | (1 << 4));  // left edge 10 right of the frame's
    add("N_value", 0, 70, 0);
    add("T_value", 3, 0, 30);
    add("N_middle", 0, 0, 20);
    add("T_text_01", 0, 33, 280, 1);  // a value right of the middle
    add("L_arrow", 0, -5, 90);        // narrow, across the middle, overlapping the value
    const Layout w = widened(l, 40, 100);
    CHECK_EQ(w.panes[6].translate[0], 53.0f);
    CHECK_EQ(w.panes[7].translate[0], 15.0f);  // goes with the value
    CHECK_EQ(w.panes[1].width, 240.0f);
    CHECK_EQ(w.panes[1].translate[0], 0.0f);
    CHECK_EQ(w.panes[2].width, 60.0f);  // narrow: moves with the frame's left edge
    CHECK_EQ(w.panes[2].translate[0], 10.0f);
    CHECK_EQ(w.panes[3].translate[0], 90.0f);  // right side: 20 further out
    CHECK_EQ(w.panes[4].translate[0], 0.0f);  // moves with its group
    CHECK_EQ(w.panes[5].translate[0], 0.0f);
    CHECK_EQ(w.panes[5].width, 20.0f);
    // Magnified part panes keep the value read.
    LayoutPane part;
    CHECK_EQ(part.magnify[0], 1.0f);
}

std::vector<uint8_t> make_archive() {
    const uint32_t w = 32, h = 16;
    std::vector<uint8_t> rgba(size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t* p = &rgba[(size_t(y) * w + x) * 4];
            p[0] = uint8_t(x * 8), p[1] = uint8_t(y * 16), p[2] = 200, p[3] = 255;
        }
    const auto layout = make_bflyt("tex_00");
    const auto bntx = make_bntx(w, h, rgba, 0x05040302, "tex_00");
    return make_sarc({{"blyt/menu_00.bflyt", std::string(layout.begin(), layout.end())},
                      {"timg/__Combined.bntx", std::string(bntx.begin(), bntx.end())}});
}

void test_skin_archive() {
    std::string error;
    std::vector<std::string> warnings;
    const auto a = load_skin_archive("menu_00", make_archive(), &error, &warnings);
    CHECK(a.has_value());
    if (!a)
        return;
    CHECK(warnings.empty());
    CHECK(a->layout("menu_00") != nullptr);
    CHECK_EQ(a->image("tex_00"), 0);
    CHECK_EQ(a->image("tex_00.bflim"), 0);
    CHECK_EQ(a->image("other"), -1);
    LayoutSkin skin;
    skin.archives.push_back(*a);
    CHECK(skin.archive_with("menu_00") == &skin.archives[0]);
    CHECK(!load_skin_archive("x", std::vector<uint8_t>(64, 0), &error, &warnings).has_value());
    const auto list = load_skin_archive("setting_list_00", lgp::test::make_settings_list_archive(), &error, &warnings);
    CHECK(list.has_value() && list->animation("setting_button_00_select") != nullptr &&
          list->layout("optionbar_00") != nullptr && list->layout("optionbar_00")->archive == "setting_list_00");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--write-arcs") {
        const std::pair<const char*, std::vector<uint8_t>> arcs[] = {
            {"menu_00.arc", make_archive()},
            {"setting_list_00.arc", lgp::test::make_settings_list_archive()},
            {"setting_bg_00.arc", lgp::test::make_settings_bg_archive()}};
        for (const auto& [name, arc] : arcs)
            std::ofstream(std::string(argv[2]) + "/" + name, std::ios::binary)
                .write(reinterpret_cast<const char*>(arc.data()), std::streamsize(arc.size()));
        return 0;
    }
    RUN(test_bntx_rgba);
    RUN(test_layout);
    RUN(test_animation);
    RUN(test_widened);
    RUN(test_skin_archive);
    return finish();
}
