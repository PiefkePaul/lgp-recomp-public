// Interface files of the game's kind built in the tests (SARC, BNTX,
// BFLYT): the game's own files stay with the user.
#pragma once

#include <cstdint>
#include <algorithm>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "lgp/gpu/nvn/texture_layout.hpp"
#include "lgp/platform/ui/sarc.hpp"

namespace lgp::test {

using lgp::platform::ui::sarc_name_hash;

inline void put16(std::vector<uint8_t>& v, size_t at, uint16_t x) { v[at] = uint8_t(x), v[at + 1] = uint8_t(x >> 8); }
inline void put32(std::vector<uint8_t>& v, size_t at, uint32_t x) {
    for (int i = 0; i < 4; ++i)
        v[at + size_t(i)] = uint8_t(x >> (8 * i));
}
inline void put64(std::vector<uint8_t>& v, size_t at, uint64_t x) {
    put32(v, at, uint32_t(x));
    put32(v, at + 4, uint32_t(x >> 32));
}
inline void putf(std::vector<uint8_t>& v, size_t at, float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    put32(v, at, x);
}

// A little-endian SARC as the Switch's tools write it.
inline std::vector<uint8_t> make_sarc(const std::vector<std::pair<std::string, std::string>>& files) {
    std::vector<uint8_t> names;
    std::vector<uint32_t> name_at;
    for (const auto& f : files) {
        name_at.push_back(uint32_t(names.size() / 4));
        names.insert(names.end(), f.first.begin(), f.first.end());
        do
            names.push_back(0);
        while (names.size() % 4 != 0);
    }
    const size_t sfat = 0x14, nodes = sfat + 0x0C, sfnt = nodes + files.size() * 16, name_start = sfnt + 8;
    const size_t data = (name_start + names.size() + 0x7F) & ~size_t(0x7F);
    std::vector<uint8_t> v(data);
    std::memcpy(&v[0], "SARC", 4);
    put16(v, 4, 0x14);
    v[6] = 0xFF, v[7] = 0xFE;
    put32(v, 0x0C, uint32_t(data));
    std::memcpy(&v[sfat], "SFAT", 4);
    put16(v, sfat + 4, 0x0C);
    put16(v, sfat + 6, uint16_t(files.size()));
    put32(v, sfat + 8, 0x65);
    std::memcpy(&v[sfnt], "SFNT", 4);
    put16(v, sfnt + 4, 8);
    std::memcpy(&v[name_start], names.data(), names.size());
    uint32_t offset = 0;
    for (size_t i = 0; i < files.size(); ++i) {
        const size_t n = nodes + i * 16;
        put32(v, n, sarc_name_hash(files[i].first));
        put32(v, n + 4, 0x01000000u | name_at[i]);
        put32(v, n + 8, offset);
        v.insert(v.end(), files[i].second.begin(), files[i].second.end());
        offset += uint32_t(files[i].second.size());
        put32(v, n + 12, offset);
    }
    put32(v, 8, uint32_t(v.size()));
    return v;
}

// One RGBA8 texture `name` in block-linear layout.
inline std::vector<uint8_t> make_bntx(uint32_t w, uint32_t h, const std::vector<uint8_t>& rgba, uint32_t selector,
                                       const std::string& texture = "test") {
    const auto layout = lgp::gpu::nvn::block_linear_layout(4, 1, 1, w, h, 1, 1, 1, 0);
    const auto& level = layout.levels[0];
    std::vector<uint8_t> swizzled(size_t(level.size), 0);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
            std::memcpy(&swizzled[lgp::gpu::nvn::block_linear_offset(x * 4, y, level.pitch, level.gob_height)],
                        &rgba[(size_t(y) * w + x) * 4], 4);
    const size_t table = 0x48, brti = 0x50, name = brti + 0xA0, mips = name + 0x10, data = 0x200;
    std::vector<uint8_t> v(data);
    std::memcpy(&v[0], "BNTX", 4);
    std::memcpy(&v[0x20], "NX  ", 4);
    put32(v, 0x24, 1);
    put64(v, 0x28, table);
    put64(v, table, brti);
    std::memcpy(&v[brti], "BRTI", 4);
    put16(v, brti + 0x12, 0);  // block linear
    put32(v, brti + 0x1C, 0x0B01);  // R8G8B8A8_UNORM
    put32(v, brti + 0x24, w);
    put32(v, brti + 0x28, h);
    put32(v, brti + 0x34, 0);  // block height 1 GOB
    put32(v, brti + 0x58, selector);
    put64(v, brti + 0x60, name);
    put64(v, brti + 0x70, mips);
    put16(v, name, uint16_t(texture.size()));
    std::memcpy(&v[name + 2], texture.data(), std::min<size_t>(texture.size(), 14));
    put64(v, mips, data);
    v.insert(v.end(), swizzled.begin(), swizzled.end());
    return v;
}


// Builds a BFLYT section by section (version 8.6, little endian).
class LayoutBuilder {
public:
    // `switch_order`: material header flags first (as some Switch layouts).
    explicit LayoutBuilder(float width = 1280, float height = 720, bool switch_order = false)
        : switch_order_(switch_order), v_(0x14) {
        std::memcpy(&v_[0], "FLYT", 4);
        put16(v_, 4, 0xFEFF);
        put16(v_, 6, 0x14);
        put32(v_, 8, 0x08060000);
        std::vector<uint8_t> b(0x14);
        putf(b, 4, width);
        putf(b, 8, height);
        section("lyt1", b);
    }
    // Textures, then materials (each: one texture or none, black, white),
    // before the panes.
    void textures(const std::vector<std::string>& names) {
        std::vector<uint8_t> b(4 + names.size() * 4);
        put16(b, 0, uint16_t(names.size()));
        for (size_t i = 0; i < names.size(); ++i) {
            put32(b, 4 + i * 4, uint32_t(b.size() - 4));
            b.insert(b.end(), names[i].begin(), names[i].end());
            b.push_back(0);
        }
        section("txl1", b);
    }
    struct Material {
        std::string name;
        int texture = -1;
        uint32_t black = 0, white = 0xFFFFFFFF;  // as R, G, B, A bytes from the low end
    };
    void materials(const std::vector<Material>& mats) {
        std::vector<uint8_t> b(4 + mats.size() * 4);  // count, offsets from the section start
        put16(b, 0, uint16_t(mats.size()));
        for (size_t i = 0; i < mats.size(); ++i) {
            put32(b, 4 + i * 4, uint32_t(b.size() + 8));
            const size_t m = b.size();
            const size_t black = switch_order_ ? 0x24 : 0x1C, white = switch_order_ ? 0x28 : 0x20,
                         flags = switch_order_ ? 0x1C : 0x24, maps = switch_order_ ? 0x2C : 0x28;
            b.resize(m + maps + (mats[i].texture >= 0 ? 4 : 0));
            std::memcpy(&b[m], mats[i].name.data(), std::min<size_t>(mats[i].name.size(), 0x1B));
            put32(b, m + black, mats[i].black);
            put32(b, m + white, mats[i].white);
            put32(b, m + flags, mats[i].texture >= 0 ? 1 : 0);
            if (mats[i].texture >= 0) {
                put16(b, m + maps, uint16_t(mats[i].texture));
                b[m + maps + 2] = 1;  // repeat
            }
        }
        section("mat1", b);
    }
    // origin: bits 0-1 x (0 centre, 1 left, 2 right), 2-3 y (1 top, 2 bottom).
    void null(const char* name, float x, float y, float w, float h, uint8_t origin = 0) {
        section("pan1", pane(name, origin, x, y, w, h));
    }
    void bounding(const char* name, float x, float y, float w, float h) {
        section("bnd1", pane(name, 0, x, y, w, h));
    }
    void picture(const char* name, int material, float x, float y, float w, float h, uint8_t origin = 0,
                 uint32_t color = 0xFFFFFFFF) {
        std::vector<uint8_t> b = pane(name, origin, x, y, w, h);
        std::vector<uint8_t> pic(0x14 + 32);
        for (int i = 0; i < 4; ++i)
            put32(pic, size_t(i) * 4, color);
        put16(pic, 0x10, uint16_t(material));
        pic[0x12] = 1;
        const float uv[8] = {0, 0, 1, 0, 0, 1, 1, 1};
        for (int i = 0; i < 8; ++i)
            putf(pic, 0x14 + size_t(i) * 4, uv[i]);
        b.insert(b.end(), pic.begin(), pic.end());
        section("pic1", b);
    }
    void text(const char* name, int material, float x, float y, float w, float h, float size, uint8_t align,
              uint32_t color = 0xFF000000) {
        std::vector<uint8_t> b = pane(name, 0, x, y, w, h);
        std::vector<uint8_t> txt(0x38);
        put16(txt, 4, uint16_t(material));
        txt[8] = align;
        put32(txt, 0x14, color);
        putf(txt, 0x1C, size);
        putf(txt, 0x20, size);
        b.insert(b.end(), txt.begin(), txt.end());
        section("txt1", b);
    }
    // A part pane showing the layout `part`; each override replaces the
    // placement of one of the part's panes by a pane block at (0, y).
    void part(const char* name, float x, float y, float w, float h, const char* part,
              const std::vector<std::pair<std::string, float>>& overrides = {}) {
        std::vector<uint8_t> b = pane(name, 0, x, y, w, h);
        const size_t ext = b.size();
        const size_t n = overrides.size();
        b.resize(ext + 12 + n * 0x28 + 0x44);
        put32(b, ext, uint32_t(n));
        putf(b, ext + 4, 1);
        putf(b, ext + 8, 1);
        std::memcpy(&b[ext + 12 + n * 0x28], part, std::min<size_t>(std::strlen(part), 0x43));
        for (size_t k = 0; k < n; ++k) {
            const size_t e = ext + 12 + k * 0x28;
            std::memcpy(&b[e], overrides[k].first.data(), std::min<size_t>(overrides[k].first.size(), 0x17));
            b[e + 0x18] = 1;  // the pane block is used
            const size_t block = b.size();
            put32(b, e + 0x1C, uint32_t(8 + block));  // from the section's start
            std::vector<uint8_t> body = pane(overrides[k].first.c_str(), 0, 0, overrides[k].second, 100, 100);
            b.resize(block + 8);
            std::memcpy(&b[block], "pan1", 4);
            put32(b, block + 4, uint32_t(8 + body.size()));
            b.insert(b.end(), body.begin(), body.end());
        }
        section("prt1", b);
    }
    void begin_children() { section("pas1", {}); }
    void end_children() { section("pae1", {}); }
    std::vector<uint8_t> finish() {
        put16(v_, 0x10, sections_);
        put32(v_, 0x0C, uint32_t(v_.size()));
        return v_;
    }

private:
    static std::vector<uint8_t> pane(const char* name, uint8_t origin, float x, float y, float w, float h) {
        std::vector<uint8_t> b(0x4C);
        b[0] = 1;  // visible
        b[1] = origin;
        b[2] = 255;
        std::memcpy(&b[4], name, std::min<size_t>(std::strlen(name), 0x17));
        putf(b, 0x24, x);
        putf(b, 0x28, y);
        putf(b, 0x3C, 1);
        putf(b, 0x40, 1);
        putf(b, 0x44, w);
        putf(b, 0x48, h);
        return b;
    }
    void section(const char* magic, std::vector<uint8_t> body) {
        while ((body.size() + 8) % 4 != 0)
            body.push_back(0);
        const size_t at = v_.size();
        v_.resize(at + 8);
        std::memcpy(&v_[at], magic, 4);
        put32(v_, at + 4, uint32_t(body.size() + 8));
        v_.insert(v_.end(), body.begin(), body.end());
        ++sections_;
    }
    bool switch_order_;
    std::vector<uint8_t> v_;
    uint16_t sections_ = 0;
};

// A small layout: root pane (1280x720) with a picture (material 0 with
// texture `texture`, black #302010 to white) and a text pane under a null pane.
inline std::vector<uint8_t> make_bflyt(const std::string& texture, bool switch_order = false) {
    LayoutBuilder b(1280, 720, switch_order);
    b.textures({texture});
    b.materials({{"P_picture", 0, 0x00102030}});
    b.null("RootPane", 0, 0, 1280, 720);
    b.begin_children();
    b.picture("P_picture", 0, -600, 300, 400, 200, 1 | 1 << 2);  // left/top at the root's centre
    b.null("N_group", 0, -100, 600, 200);
    b.begin_children();
    b.text("T_label", 0, 0, 0, 500, 60, 32, 1 | 1 << 2, 0xFF0000FF);
    b.end_children();
    b.end_children();
    return b.finish();
}

// A BFLAN with step curves: per pane, (tag, target, value) set from frame 0
// to `frames`.
struct AnimKey {
    std::string pane;
    std::string tag;  // "FLVI", "FLVC", "FLPA" (FLPA values hermite, the others step)
    uint8_t target;
    float value;
};
inline std::vector<uint8_t> make_bflan(uint16_t frames, const std::vector<AnimKey>& keys) {
    std::vector<uint8_t> v(0x14);
    std::memcpy(&v[0], "FLAN", 4);
    put16(v, 4, 0xFEFF);
    put16(v, 6, 0x14);
    put32(v, 8, 0x08060000);
    // pai1: header 0x14, group table, groups (one per key)
    std::vector<uint8_t> s(0x14 + keys.size() * 4);
    std::memcpy(&s[0], "pai1", 4);
    put16(s, 8, frames);
    put16(s, 14, uint16_t(keys.size()));
    put32(s, 16, 0x14);
    for (size_t i = 0; i < keys.size(); ++i) {
        const AnimKey& k = keys[i];
        const bool hermite = k.tag == "FLPA";
        put32(s, 0x14 + i * 4, uint32_t(s.size()));
        const size_t g = s.size();
        // group: name 0x1C, tags 1, type 0, pad, tag offset; tag: magic, 1
        // entry, offset; entry: index, target, kind, pad, keys, pad, key offset
        s.resize(g + 0x24 + 0x0C + 0x0C + (hermite ? 24 : 16));
        std::memcpy(&s[g], k.pane.data(), std::min<size_t>(k.pane.size(), 0x1B));
        s[g + 0x1C] = 1;
        put32(s, g + 0x20, 0x24);
        const size_t tag = g + 0x24;
        std::memcpy(&s[tag], k.tag.data(), 4);
        s[tag + 4] = 1;
        put32(s, tag + 8, 0x0C);
        const size_t e = tag + 0x0C;
        s[e + 1] = k.target;
        s[e + 2] = hermite ? 2 : 1;
        put16(s, e + 4, 2);
        put32(s, e + 8, 0x0C);
        const size_t key = e + 0x0C;
        for (int n = 0; n < 2; ++n) {
            const size_t kp = key + size_t(n) * (hermite ? 12 : 8);
            putf(s, kp, n == 0 ? 0.0f : float(frames));
            if (hermite)
                putf(s, kp + 4, k.value);
            else
                put16(s, kp + 4, uint16_t(k.value));
        }
    }
    put32(s, 4, uint32_t(s.size()));
    v.insert(v.end(), s.begin(), s.end());
    put16(v, 0x10, 1);
    put32(v, 0x0C, uint32_t(v.size()));
    return v;
}

// Stand-ins for the archives the menus look for, with their pane names
// (own shapes and colours): setting_list_00 with the row layout
// setting_button_00, setting_bg_00 with a striped background.
inline std::vector<uint8_t> make_settings_list_archive() {
    LayoutBuilder b(806, 58);
    b.materials({{"P_glare", -1, 0, 0xFF30D0FF}, {"P_fill", -1, 0, 0xFFF0F0F0}, {"T_text", -1, 0, 0xFF000000}});
    b.null("RootPane", 0, 0, 806, 58);
    b.begin_children();
    b.null("N_passiveActive_00", 0, 0, 806, 58);
    b.begin_children();
    b.null("N_select_00", 0, 0, 806, 58);
    b.begin_children();
    b.picture("W_glare_00", 0, 0, 0, 806, 58);
    b.end_children();
    b.picture("W_window_fill", 1, 0, 0, 792, 44);
    b.null("N_pattern_00", 0, 0, 792, 44);
    b.begin_children();
    b.text("T_text_00", 2, -232, 0, 280, 44, 26, 1, 0xFF303030);
    b.text("T_text_01", 2, 173, 0, 280, 44, 26, 0, 0xFF303030);
    b.end_children();
    b.null("N_pattern_01", 0, 0, 792, 44);
    b.begin_children();
    b.text("T_text_02", 2, 0, 0, 392, 44, 26, 0, 0xFF303030);
    b.end_children();
    b.end_children();
    b.end_children();
    const auto row = b.finish();
    // The hint bar: a strip at the bottom, sets of icon and text from the right.
    LayoutBuilder bar(1280, 720);
    bar.materials({{"P_bar", -1, 0, 0xFF404040}, {"P_option", -1, 0, 0xFFFFFFFF}, {"T_option", -1, 0, 0xFFFFFFFF}});
    bar.null("RootPane", 0, 0, 1280, 720);
    bar.begin_children();
    bar.picture("P_bar", 0, 0, -342, 1280, 36);
    for (int i = 0; i < 3; ++i) {
        const std::string set = "N_optionSet_0" + std::to_string(i);
        bar.null(set.c_str(), 520 - 250 * float(i), -342, 250, 36);
        bar.begin_children();
        bar.picture(("P_option_0" + std::to_string(i)).c_str(), 1, -100, 0, 28, 28);
        bar.text(("T_option_0" + std::to_string(i)).c_str(), 2, 30, 0, 180, 34, 22, 1, 0xFFFFFFFF);
        bar.end_children();
    }
    bar.end_children();
    const auto bar_layout = bar.finish();
    // The list screen: a header box with the title above the list's area.
    LayoutBuilder list(1280, 720);
    list.materials({{"P_base", -1, 0, 0xFF2050E0}, {"P_fill", -1, 0, 0xFFFAFAFA}, {"T_text", -1, 0, 0xFF000000}});
    list.null("RootPane", 0, 0, 1280, 720);
    list.begin_children();
    list.picture("P_base_00", 0, 0, 273, 820, 170);
    list.picture("P_fill_00", 1, 0, 273, 800, 150);
    list.text("T_text_00", 2, 0, 273, 720, 112, 30, 0, 0xFF303030);
    list.bounding("scroll_00", 0, -40, 842, 480);
    list.null("N_list", 0, -40, 842, 480);
    list.end_children();
    const auto list_layout = list.finish();
    // A round icon for A (B has none: its letter goes into the text).
    std::vector<uint8_t> icon(28 * 28 * 4);
    for (int y = 0; y < 28; ++y)
        for (int x = 0; x < 28; ++x) {
            uint8_t* p = &icon[size_t(y * 28 + x) * 4];
            const bool in = (x - 14) * (x - 14) + (y - 14) * (y - 14) < 13 * 13;
            p[0] = p[1] = p[2] = 240, p[3] = in ? 255 : 0;
        }
    const auto bntx = make_bntx(28, 28, icon, 0x05040302, "button_icon_A_00");
    // States: the selection rim hidden or shown, the row a little larger when selected.
    const auto select = make_bflan(8, {{"N_select_00", "FLVI", 0, 1}, {"N_passiveActive_00", "FLPA", 6, 1.03f}});
    const auto unselect = make_bflan(8, {{"N_select_00", "FLVI", 0, 0}});
    return make_sarc({{"blyt/setting_button_00.bflyt", std::string(row.begin(), row.end())},
                      {"anim/setting_button_00_select.bflan", std::string(select.begin(), select.end())},
                      {"anim/setting_button_00_unselect.bflan", std::string(unselect.begin(), unselect.end())},
                      {"blyt/optionbar_00.bflyt", std::string(bar_layout.begin(), bar_layout.end())},
                      {"blyt/setting_list_00.bflyt", std::string(list_layout.begin(), list_layout.end())},
                      {"timg/__Combined.bntx", std::string(bntx.begin(), bntx.end())}});
}

inline std::vector<uint8_t> make_settings_bg_archive() {
    const uint32_t w = 64, h = 64;
    std::vector<uint8_t> rgba(size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t* p = &rgba[(size_t(y) * w + x) * 4];
            const bool stripe = ((x + y) / 8) % 2 == 0;
            p[0] = stripe ? 120 : 90, p[1] = stripe ? 170 : 140, p[2] = 230, p[3] = 255;
        }
    LayoutBuilder b(1280, 720);
    b.textures({"bg_tile"});
    b.materials({{"P_bg", 0}});
    b.null("RootPane", 0, 0, 1280, 720);
    b.begin_children();
    b.picture("P_bg", 0, 0, 0, 1280, 720);
    b.end_children();
    const auto layout = b.finish();
    const auto bntx = make_bntx(w, h, rgba, 0x05040302, "bg_tile");
    return make_sarc({{"blyt/setting_bg_00.bflyt", std::string(layout.begin(), layout.end())},
                      {"timg/__Combined.bntx", std::string(bntx.begin(), bntx.end())}});
}

}  // namespace lgp::test
