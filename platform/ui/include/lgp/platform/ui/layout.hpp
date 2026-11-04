// Layouts of the game's interface (BFLYT, "FLYT" in a SARC's blyt/): the
// pane tree with its materials and texture names, read at run time from the
// user's RomFS so the app's menus can be drawn with the game's own parts
// (docs/analysis/27-p7-app-oberflaeche.md). Nothing of a layout goes into
// the repo; the tests build their own.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lgp::platform::ui {

struct LayoutColor {
    uint8_t r = 255, g = 255, b = 255, a = 255;
};

struct LayoutTexMap {
    int texture = -1;  // index into Layout::textures
    uint8_t wrap_s = 0, wrap_t = 0;  // 0 clamp, 1 repeat, 2 mirror
};

struct LayoutTexSrt {
    float tx = 0, ty = 0, rotate = 0, sx = 1, sy = 1;
};

struct LayoutMaterial {
    std::string name;
    // The texture's colour maps from black to white (lerp per channel).
    LayoutColor black{0, 0, 0, 0}, white;
    uint32_t flags = 0;
    std::vector<LayoutTexMap> maps;
    std::vector<LayoutTexSrt> srts;
    // Blend mode where the material has its own (flags bit 10): operation
    // (1 add, 2 subtract, 3 reverse subtract, 4 min, 5 max) and factors
    // (0 zero, 1 one, 2 dst colour, 3 1-dst colour, 4 src alpha, 5 1-src
    // alpha, 6 dst alpha, 7 1-dst alpha, 8 src colour, 9 1-src colour);
    // op -1: the usual alpha blending.
    int blend_op = -1, blend_src = 4, blend_dst = 5;
};

enum class PaneKind { Null, Picture, Text, Window, Bounding, Part };

// The four corners in the order the format keeps them: top left, top
// right, bottom left, bottom right.
using LayoutQuadUv = std::array<std::array<float, 2>, 4>;

struct LayoutWindowFrame {
    int material = -1;
    uint8_t flip = 0;  // 0 none, 1 horizontal, 2 vertical, 3 rotate 90, 4 180, 5 270
};

// What a part pane changes of one pane of its part (prt1 property): the
// values whose bit is in `set`.
struct LayoutPartOverride {
    enum : uint32_t {
        kVisible = 1, kTranslate = 2, kScale = 4, kSize = 8, kAlpha = 16, kMaterial = 32,
        kPane = 64,  // the whole pane (placement, rotation, origin) from the property's pane block
    };
    std::string name;
    uint32_t set = 0;
    uint8_t flags = 1, origin = 0, alpha = 255;
    float translate[3] = {0, 0, 0};
    float rotate[3] = {0, 0, 0};
    float scale[2] = {1, 1};
    float width = 0, height = 0;
    int material = -1;  // in the layout that holds the part pane
    // Usage flags as read (pane, basic info, material), for lgp_ui_dump.
    uint8_t usage[3] = {0, 0, 0};
};

struct LayoutPane {
    PaneKind kind = PaneKind::Null;
    std::string name;
    uint8_t flags = 1;   // bit 0 visible, bit 1 children take on its alpha
    uint8_t origin = 0;  // bits 0-1 x (0 centre, 1 left, 2 right), 2-3 y (0 centre, 1 top, 2 bottom),
                         // 4-5 / 6-7 the same for the anchor in the parent
    uint8_t alpha = 255;
    float translate[3] = {0, 0, 0};
    float rotate[3] = {0, 0, 0};  // degrees
    float scale[2] = {1, 1};
    float width = 0, height = 0;
    int parent = -1;
    std::vector<int> children;

    // Picture, and a window's content.
    LayoutColor colors[4];
    int material = -1;
    std::vector<LayoutQuadUv> uvs;

    // Window: content inflation and frame sizes (left, right, top, bottom).
    float inflation[4] = {0, 0, 0, 0};
    float frame_size[4] = {0, 0, 0, 0};
    // Bits: 0 one material for all frames, 1 the content's vertex colours on
    // the frames too, 2-3 kind (0 around, 1 horizontal, 2 horizontal without
    // content), 4 content not drawn.
    uint8_t window_flags = 0;
    std::vector<LayoutWindowFrame> frames;

    // Text: the font (Layout::fonts), size, colours, alignment.
    int font = -1;
    float font_size[2] = {0, 0};
    LayoutColor text_top, text_bottom;
    uint8_t text_align = 0;  // bits 0-1 x, 2-3 y of the text in the pane (as origin)
    uint8_t line_align = 0;  // 0 unset, 1 left, 2 centre, 3 right

    // Part: the layout it shows (blyt/<part>.bflyt in the same archive).
    std::string part;
    float magnify[2] = {1, 1};  // multiplies the part's translations and sizes
    std::vector<LayoutPartOverride> overrides;

    bool visible() const { return (flags & 1) != 0; }
};

struct Layout {
    float width = 0, height = 0;
    std::vector<std::string> textures;
    std::vector<std::string> fonts;
    std::vector<LayoutMaterial> materials;
    std::vector<LayoutPane> panes;  // panes[0] is the root
    std::string archive;  // the skin archive it came from (its textures), set by load_skin_archive
    // What the reader could not take apart (one line each, no data).
    std::vector<std::string> warnings;

    int find(std::string_view pane) const;
};

// The layout in a BFLYT, or nullopt and *error.
std::optional<Layout> parse_bflyt(std::span<const uint8_t> file, std::string* error);

// An animation of a layout (BFLAN, "FLAN" in a SARC's anim/): curves per
// pane or material. The menus use the states an animation ends in (a row's
// "select", "unselect").
struct LayoutCurve {
    std::string tag;   // "FLPA" pane SRT, "FLVI" visibility, "FLVC" vertex colours/alpha, "FLMC" material colours
    uint8_t target = 0;  // which value of the tag (FLPA: 0-2 translate, 3-5 rotate, 6-7 scale, 8-9 size)
    uint8_t kind = 0;    // 0 constant, 1 step, 2 hermite
    struct Key {
        float frame = 0, value = 0, slope = 0;
    };
    std::vector<Key> keys;
    float at(float frame) const;
};

struct LayoutAnimGroup {
    std::string name;  // pane or material
    bool material = false;
    std::vector<LayoutCurve> curves;
};

struct LayoutAnimation {
    float frames = 0;
    bool loop = false;
    std::vector<LayoutAnimGroup> groups;
};

std::optional<LayoutAnimation> parse_bflan(std::span<const uint8_t> file, std::string* error);

// The layout as it stands at `frame` of `anim` (frame < 0: its end).
Layout animated(const Layout& layout, const LayoutAnimation& anim, float frame = -1);

}  // namespace lgp::platform::ui
