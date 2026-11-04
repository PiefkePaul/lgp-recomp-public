// Counts how the game's layouts anchor what sits at the screen's edges, for
// the widescreen mod (Analyse 50): would a larger root pane alone move header
// and footer bars to the new edges?
//
//   lgp_ui_anchors FILE|DIR... [--list] [--bands]
//   lgp_ui_anchors DIR --shift DY [--units H] [--axis x|y] [--into PANES] [--skip LAYOUTS] [--keep [LAYOUT:]PANES] [--move [LAYOUT:]PANES] [--tree LAYOUTS] --out DIR
//   lgp_ui_anchors --self-test
//
// Every BFLYT in every SARC (also Yaz0, also nested) under the paths is read.
// In layouts whose root is 1280x720, each drawn pane (picture, text, window,
// part) lying wholly in a band at the top (bottom edge >= 180), bottom
// (top edge <= -180), left (right edge <= -400) or right (left edge >= 400)
// is counted, and whether it hangs at that edge: the pane on its way up that
// sits directly in the root pane has its anchor in the root at that side.
// Groups below keep their size when the root grows, so only that anchor moves
// anything. Such panes would follow a larger root by themselves; the others
// sit at a fixed distance from the middle. Panes under a hidden one are not
// drawn and not counted.
// With --bands it also counts what moving whole groups would take (Analysis 50,
// moving header and footer groups in the layouts themselves instead of zones):
// a band group is the highest pane whose drawn subtree lies wholly in the top
// or bottom band while its parent's does not. Moving it by the extra height
// takes along everything under it (texts too). Counted: band groups, the
// drawn band panes they cover, text panes among those, and animation curves
// (anim/<layout>_*.bflan in the same archive) that set a band group's own
// translation (those would need the same shift).
// With --shift it writes copies of the archives (SARC, and .gfpak with SARC
// files inside) with band groups moved DY
// outwards (see shift_sarc) under --out, at the same path; they hold game data
// and go to $LGP_PRIVATE_DIR (e.g. a data mod's romfs/ folder). --into names
// groups (pane names) the game places at run time: their children move instead;
// --axis x moves right and left groups DY outwards instead (a wider screen; bands from |x| 320),
// --skip names layouts (file names without extension) left as they are,
// --keep panes (PANE, or LAYOUT:PANE for one layout) that stay with all under them,
// --move panes that move as one group whatever their size.
// Only counts on stdout; --list adds one line per layout with its archive
// path and name (game file names: $LGP_PRIVATE_DIR). The game can also move
// panes at run time, which this does not see.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "lgp/platform/ui/layout.hpp"
#include "lgp/platform/ui/layout_shift.hpp"
#include "lgp/platform/ui/sarc.hpp"
#include "ui_fixtures.hpp"

namespace fs = std::filesystem;
using namespace lgp::platform::ui;

namespace {

enum Side { kTop, kBottom, kLeft, kRight, kSides };
const char* const kSideNames[kSides] = {"top", "bottom", "left", "right"};

struct Counts {
    size_t layouts = 0, screen_layouts = 0, other_layouts = 0, failed = 0;
    size_t panes = 0, drawn = 0, parent_anchored = 0;
    size_t edge[kSides] = {}, hanging[kSides] = {};
    // --bands (top and bottom only).
    size_t groups[2] = {}, covered[2] = {}, texts[2] = {}, texts_band[2] = {}, layouts_with_groups = 0;
    size_t anim_curves = 0, anim_files = 0;
    void add(const Counts& o) {
        layouts += o.layouts, screen_layouts += o.screen_layouts, other_layouts += o.other_layouts;
        failed += o.failed, panes += o.panes, drawn += o.drawn, parent_anchored += o.parent_anchored;
        for (int s = 0; s < kSides; ++s)
            edge[s] += o.edge[s], hanging[s] += o.hanging[s];
        for (int s = 0; s < 2; ++s)
            groups[s] += o.groups[s], covered[s] += o.covered[s], texts[s] += o.texts[s],
                texts_band[s] += o.texts_band[s];
        layouts_with_groups += o.layouts_with_groups, anim_curves += o.anim_curves, anim_files += o.anim_files;
    }
};

// Does pane i hang at side s of the root: is the pane above it (or itself)
// that sits directly in the root anchored at that side?
bool hangs(const Layout& l, size_t i, Side s) {
    const bool vertical = s == kTop || s == kBottom;
    const int want = s == kTop || s == kLeft ? 1 : 2;
    int k = int(i);
    while (k > 0 && l.panes[size_t(k)].parent > 0 && l.panes[size_t(k)].parent < k)
        k = l.panes[size_t(k)].parent;
    if (k <= 0 || l.panes[size_t(k)].parent != 0)
        return false;
    const uint8_t o = l.panes[size_t(k)].origin;
    return (vertical ? (o >> 6) & 3 : (o >> 4) & 3) == want;
}

Counts count(const Layout& l) {
    Counts c;
    c.layouts = 1;
    if (std::abs(l.width - 1280) > 0.5f || std::abs(l.height - 720) > 0.5f) {
        c.other_layouts = 1;
        return c;
    }
    c.screen_layouts = 1;
    const std::vector<LayoutRect> at = layout_place(l);
    std::vector<bool> shown(l.panes.size(), true);  // the pane and all above it visible
    for (size_t i = 0; i < l.panes.size(); ++i) {
        const int up = l.panes[i].parent;
        shown[i] = l.panes[i].visible() && (up < 0 || size_t(up) >= i || shown[size_t(up)]);
    }
    for (size_t i = 1; i < l.panes.size(); ++i) {
        const LayoutPane& p = l.panes[i];
        ++c.panes;
        if ((p.origin >> 4) != 0)
            ++c.parent_anchored;
        if (!layout_drawn(p) || !shown[i])
            continue;
        ++c.drawn;
        const LayoutRect r{at[i].cx, at[i].cy, std::abs(at[i].w), std::abs(at[i].h)};
        const bool side[kSides] = {r.cy - r.h / 2 >= 180, r.cy + r.h / 2 <= -180, r.cx + r.w / 2 <= -400,
                                   r.cx - r.w / 2 >= 400};
        for (int s = 0; s < kSides; ++s)
            if (side[s]) {
                ++c.edge[s];
                if (hangs(l, i, Side(s)))
                    ++c.hanging[s];
            }
    }
    return c;
}

float g_units = 0;  // --units H (0: band groups)
int g_axis = 1;     // --axis x: right and left groups for a wider screen

// --bands: the band groups (or units) of a 1280x720 layout, counted into c.
std::vector<int> band_groups(const Layout& l, Counts* c, bool all_panes = false) {
    BandStats st;
    std::vector<int> band = layout_bands(l, g_units, all_panes, &st, nullptr, g_axis);
    for (int s = 0; s < 2; ++s)
        c->groups[s] += st.groups[s], c->covered[s] += st.covered[s], c->texts[s] += st.texts[s],
            c->texts_band[s] += st.texts_band[s];
    if (st.groups[0] + st.groups[1] > 0)
        c->layouts_with_groups = 1;
    return band;
}

bool g_list = false;

bool g_bands = false;

// Curves in the archive's animations of this layout that set a band group's translation.
void count_anim(const Sarc& sarc, const std::string& layout, const Layout& l, const std::vector<int>& band,
                Counts* c) {
    for (const auto& e : sarc.entries()) {
        const auto f = sarc.file(e);
        if (f.size() < 4 || std::memcmp(f.data(), "FLAN", 4) != 0)
            continue;
        const std::string a = layout_stem(e.name);
        if (a.size() <= layout.size() || a.compare(0, layout.size(), layout) != 0 || a[layout.size()] != '_')
            continue;
        std::string why;
        const auto anim = parse_bflan(f, &why);
        if (!anim)
            continue;
        bool any = false;
        for (const LayoutAnimGroup& g : anim->groups) {
            if (g.material)
                continue;
            const int i = l.find(g.name);
            if (i <= 0 || band[size_t(i)] < 0)
                continue;
            for (const LayoutCurve& cv : g.curves)
                if (cv.tag == "FLPA" && cv.target <= 1)
                    ++c->anim_curves, any = true;
        }
        if (any)
            ++c->anim_files;
    }
}

// --shift DY --out DIR: moves every band group (hidden ones too) DY layout
// units outwards, top groups up and bottom groups down, and the translation
// keys of the curves that animate them, in place: the archive keeps its size
// and layout, only those floats change. DY is in screen units; it is divided
// through the parents' scale and rotation (band_shifts).
float g_shift = 0;
fs::path g_out;
std::vector<std::string> g_into, g_skip, g_keep, g_move;

// Names from a comma-separated option.
std::vector<std::string> split_list(const char* text) {
    std::vector<std::string> out;
    std::string cur;
    for (const char* p = text;; ++p) {
        if (*p == ',' || *p == '\0') {
            if (!cur.empty())
                out.push_back(cur);
            cur.clear();
            if (*p == '\0')
                break;
        } else {
            cur.push_back(*p);
        }
    }
    return out;
}

ShiftOptions options() {
    ShiftOptions o;
    o.shift = g_shift;
    o.units = g_units;
    o.axis = g_axis;
    o.into = g_into;
    o.skip = g_skip;
    o.keep = g_keep;
    o.move = g_move;
    return o;
}

// --list with --shift: what moved in each screen layout (pane names: game data, stdout stays private).
std::vector<std::string> g_tree;

// --tree: one line per pane, indented by depth.
void print_tree(const std::string& where, const std::string& name, const Layout& l,
                const std::vector<std::array<float, 2>>& shift) {
    static const char* const kKinds[] = {"null", "pic", "text", "win", "bound", "part"};
    const std::vector<LayoutRect> at = layout_place(l);
    std::printf("tree %s!%s\n", where.c_str(), name.c_str());
    for (size_t i = 0; i < l.panes.size(); ++i) {
        const LayoutPane& p = l.panes[i];
        int depth = 0;
        for (int k = p.parent; k >= 0 && size_t(k) < i && depth < 64; k = l.panes[size_t(k)].parent)
            ++depth;
        std::printf("  %*s%s %s%s box %.0f,%.0f %.0fx%.0f", depth * 2, "", kKinds[int(p.kind)], p.name.c_str(),
                    p.visible() ? "" : " (hidden)", double(at[i].cx), double(at[i].cy), double(std::abs(at[i].w)),
                    double(std::abs(at[i].h)));
        if (p.scale[0] != 1 || p.scale[1] != 1)
            std::printf(" scale %g,%g", double(p.scale[0]), double(p.scale[1]));
        if (p.rotate[0] != 0 || p.rotate[1] != 0 || p.rotate[2] != 0)
            std::printf(" turn %g,%g,%g", double(p.rotate[0]), double(p.rotate[1]), double(p.rotate[2]));
        if (i < shift.size() && (shift[i][0] != 0 || shift[i][1] != 0))
            std::printf(" MOVED %+g,%+g", double(shift[i][0]), double(shift[i][1]));
        std::printf("\n");
    }
}

ShiftListener lister(const std::string& where) {
    if (!g_list && g_tree.empty())
        return {};
    return [where](const std::string& name, const Layout& l, const std::vector<std::array<float, 2>>& shift) {
        if (!g_tree.empty() && (g_tree[0] == "*" || std::find(g_tree.begin(), g_tree.end(), layout_stem(name)) !=
                                                       g_tree.end()))
            print_tree(where, name, l, shift);
        if (!g_list)
            return;
        size_t groups = 0;
        for (const auto& d : shift)
            groups += d[0] != 0 || d[1] != 0;
        std::printf("%s!%s\tgroups %zu", where.c_str(), name.c_str(), groups);
        for (size_t i = 0; i < shift.size(); ++i)
            if (shift[i][0] != 0 || shift[i][1] != 0)
                std::printf("\t%s %+g,%+g", l.panes[i].name.c_str(), double(shift[i][0]), double(shift[i][1]));
        std::printf("\n");
    };
}

// Writes the shifted copy of each changed archive under g_out at its path below root.
void shift_tree(const fs::path& root, ShiftStats* sc) {
    std::error_code ec;
    std::vector<fs::path> files;
    if (fs::is_directory(root, ec)) {
        for (const auto& e : fs::recursive_directory_iterator(root, ec))
            if (e.is_regular_file(ec))
                files.push_back(e.path());
    } else {
        files.push_back(root);
    }
    for (const fs::path& path : files) {
        std::ifstream in(path, std::ios::binary);
        char head[8] = {};
        if (!in.read(head, 8) || (std::memcmp(head, "SARC", 4) != 0 && std::memcmp(head, "GFLXPACK", 8) != 0))
            continue;
        in.seekg(0);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const bool pack = std::memcmp(head, "GFLXPACK", 8) == 0;
        const ShiftListener list = lister(path.generic_string());
        if (pack ? !shift_gfpak(bytes, options(), sc, list) : !shift_sarc(bytes, options(), sc, list))
            continue;
        const fs::path rel = fs::is_directory(root, ec) ? fs::relative(path, root, ec) : path.filename();
        const fs::path dst = g_out / rel;
        fs::create_directories(dst.parent_path(), ec);
        std::ofstream out(dst, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    }
}

void scan(const std::string& where, std::vector<uint8_t> bytes, Counts* total) {
    std::string error;
    auto sarc = Sarc::parse(std::move(bytes), &error);
    if (!sarc)
        return;
    for (const auto& e : sarc->entries()) {
        const auto f = sarc->file(e);
        if (f.size() >= 4 && (std::memcmp(f.data(), "SARC", 4) == 0 || std::memcmp(f.data(), "Yaz0", 4) == 0)) {
            scan(where + "!" + e.name, std::vector<uint8_t>(f.begin(), f.end()), total);
            continue;
        }
        if (f.size() < 4 || std::memcmp(f.data(), "FLYT", 4) != 0)
            continue;
        std::string why;
        const auto l = parse_bflyt(f, &why);
        Counts c;
        if (!l) {
            c.failed = 1;
        } else {
            c = count(*l);
            if (g_bands) {
                const std::vector<int> band = band_groups(*l, &c);
                if (c.layouts_with_groups)
                    count_anim(*sarc, layout_stem(e.name), *l, band, &c);
            }
        }
        total->add(c);
        if (g_list && l && c.screen_layouts) {
            std::printf("%s!%s\tdrawn %zu", where.c_str(), e.name.c_str(), c.drawn);
            for (int s = 0; s < kSides; ++s)
                std::printf("\t%s %zu/%zu", kSideNames[s], c.hanging[s], c.edge[s]);
            std::printf("\n");
        }
    }
}

void scan_file(const fs::path& path, Counts* total) {
    std::ifstream in(path, std::ios::binary);
    char head[4] = {};
    if (!in.read(head, 4))
        return;
    if (std::memcmp(head, "SARC", 4) != 0 && std::memcmp(head, "Yaz0", 4) != 0)
        return;
    in.seekg(0);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    scan(path.generic_string(), std::move(bytes), total);
}

void report(const Counts& c) {
    std::printf("layouts %zu (1280x720 %zu, other sizes %zu, unreadable %zu)\n", c.layouts, c.screen_layouts,
                c.other_layouts, c.failed);
    std::printf("panes %zu, drawn %zu, with an anchor in the parent %zu\n", c.panes, c.drawn, c.parent_anchored);
    for (int s = 0; s < kSides; ++s)
        std::printf("%s: %zu panes, %zu hang at that edge\n", kSideNames[s], c.edge[s], c.hanging[s]);
    if (!g_bands)
        return;
    std::printf("bands: %zu layouts with band groups\n", c.layouts_with_groups);
    for (int s = 0; s < 2; ++s)
        std::printf("bands %s: %zu groups cover %zu of %zu drawn panes, texts %zu of %zu\n", kSideNames[s],
                    c.groups[s], c.covered[s], c.edge[s], c.texts[s], c.texts_band[s]);
    std::printf("bands: %zu animation curves in %zu files set a band group's translation\n", c.anim_curves,
                c.anim_files);
}

int self_test() {
    int failures = 0;
    const auto check = [&](bool ok, const char* what) {
        if (!ok) {
            std::printf("FAIL: %s\n", what);
            ++failures;
        }
    };
    lgp::test::LayoutBuilder b(1280, 720, false);
    b.materials({{"M", -1}});
    b.null("RootPane", 0, 0, 1280, 720);
    b.begin_children();
    // A footer group hung at the root's bottom (anchor bits 6-7 = 2), its bar and text inside.
    b.null("N_foot", 0, 40, 1280, 80, uint8_t(2 << 6));
    b.begin_children();
    b.picture("P_bar", 0, 0, 0, 1280, 80);
    b.text("T_hint", 0, 560, 0, 160, 40, 24, 0);
    b.end_children();
    // A header picture placed from the middle: same place, not hanging.
    b.picture("P_head", 0, 0, 320, 1280, 80);
    // A label on the right, hung at the root's right edge (bits 4-5 = 2).
    b.picture("P_side", 0, -100, 0, 160, 100, 0, 0xFFFFFFFF);
    b.end_children();
    std::vector<uint8_t> file = b.finish();
    // P_side: set its parent anchor by hand (picture() takes only the own origin).
    std::string error;
    auto l = parse_bflyt(file, &error);
    check(l.has_value(), "layout parses");
    if (!l)
        return 1;
    l->panes[size_t(l->find("P_side"))].origin = uint8_t(2 << 4);
    const Counts c = count(*l);
    check(c.screen_layouts == 1 && c.drawn == 4, "four drawn panes in a 1280x720 layout");
    check(c.edge[kBottom] == 2 && c.hanging[kBottom] == 2, "footer bar and text hang at the bottom");
    check(c.edge[kTop] == 1 && c.hanging[kTop] == 0, "header from the middle does not hang");
    check(c.edge[kRight] == 2 && c.hanging[kRight] == 1, "side label hangs right, footer text does not");
    const std::vector<LayoutRect> at = layout_place(*l);
    const LayoutRect& foot = at[size_t(l->find("N_foot"))];
    check(std::abs(foot.cy - (-320)) < 1e-3f, "footer centre 40 above the bottom edge");
    lgp::test::LayoutBuilder layout_part(400, 200, false);
    layout_part.null("RootPane", 0, 0, 400, 200);
    auto part = parse_bflyt(layout_part.finish(), &error);
    check(part && count(*part).other_layouts == 1, "a part's own layout is not counted as a screen");
    Counts scanned;
    scan("test.arc", lgp::test::make_sarc({{"blyt/a.bflyt", std::string(file.begin(), file.end())}}), &scanned);
    check(scanned.layouts == 1 && scanned.edge[kBottom] == 2, "a layout inside a SARC is read");
    // Mirrored (scale x -1), origin left at x 500: it covers 340..500, not the right band.
    {
        lgp::test::LayoutBuilder m(1280, 720, false);
        m.materials({{"M", -1}});
        m.null("RootPane", 0, 0, 1280, 720);
        m.begin_children();
        m.picture("P_flip", 0, 500, 0, 160, 100, 1);
        m.end_children();
        auto mirrored = parse_bflyt(m.finish(), &error);
        check(mirrored.has_value(), "mirrored layout parses");
        if (mirrored) {
            mirrored->panes[size_t(mirrored->find("P_flip"))].scale[0] = -1;
            const LayoutRect r = layout_place(*mirrored)[size_t(mirrored->find("P_flip"))];
            check(std::abs(r.cx - 420) < 1e-3f, "mirrored pane extends to the left of its origin");
            check(count(*mirrored).edge[kRight] == 0, "mirrored pane is not in the right band");
        }
    }
    // A picture hung at the bottom of a centred, screen-sized group: the group keeps
    // its size when the root grows, so it does not hang. And one under a hidden group.
    lgp::test::LayoutBuilder g(1280, 720, false);
    g.materials({{"M", -1}});
    g.null("RootPane", 0, 0, 1280, 720);
    g.begin_children();
    g.null("N_box", 0, 0, 1280, 720);
    g.begin_children();
    g.picture("P_low", 0, 0, 40, 1280, 80);
    g.end_children();
    g.null("N_hidden", 0, -320, 1280, 80, uint8_t(2 << 6));
    g.begin_children();
    g.picture("P_gone", 0, 0, 0, 1280, 80);
    g.end_children();
    g.end_children();
    auto grouped = parse_bflyt(g.finish(), &error);
    check(grouped.has_value(), "grouped layout parses");
    if (grouped) {
        grouped->panes[size_t(grouped->find("P_low"))].origin = uint8_t(2 << 6);
        grouped->panes[size_t(grouped->find("N_hidden"))].flags = 0;
        const Counts gc = count(*grouped);
        check(gc.drawn == 1, "a pane under a hidden group is not drawn");
        check(gc.edge[kBottom] == 1 && gc.hanging[kBottom] == 0, "hung in a fixed-size group: does not follow the root");
    }
    // --bands: the footer group of the first layout is one band group with bar and text;
    // the header picture is its own group.
    {
        Counts bc;
        const std::vector<int> band = band_groups(*l, &bc);
        check(bc.groups[1] == 1 && band[size_t(l->find("N_foot"))] == 1, "footer group is the bottom band group");
        check(bc.covered[1] == 2 && bc.texts[1] == 1 && bc.texts_band[1] == 1, "it covers bar and text");
        check(bc.groups[0] == 1 && band[size_t(l->find("P_head"))] == 0, "header picture is a top band group");
        check(band[size_t(l->find("P_bar"))] < 0, "the bar moves with its group, not on its own");
    }
    std::printf(failures ? "ui_anchors self-test: %d failures\n" : "ui_anchors self-test: ok\n", failures);
    return failures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<fs::path> paths;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--self-test") == 0)
            return self_test();
        if (std::strcmp(argv[i], "--list") == 0)
            g_list = true;
        else if (std::strcmp(argv[i], "--bands") == 0)
            g_bands = true;
        else if (std::strcmp(argv[i], "--shift") == 0 && i + 1 < argc)
            g_shift = std::strtof(argv[++i], nullptr);
        else if (std::strcmp(argv[i], "--into") == 0 && i + 1 < argc)
            g_into = split_list(argv[++i]);
        else if (std::strcmp(argv[i], "--skip") == 0 && i + 1 < argc)
            g_skip = split_list(argv[++i]);
        else if (std::strcmp(argv[i], "--keep") == 0 && i + 1 < argc)
            g_keep = split_list(argv[++i]);
        else if (std::strcmp(argv[i], "--move") == 0 && i + 1 < argc)
            g_move = split_list(argv[++i]);
        else if (std::strcmp(argv[i], "--tree") == 0 && i + 1 < argc)
            g_tree = split_list(argv[++i]);
        else if (std::strcmp(argv[i], "--units") == 0 && i + 1 < argc)
            g_units = std::strtof(argv[++i], nullptr);
        else if (std::strcmp(argv[i], "--axis") == 0 && i + 1 < argc && (std::strcmp(argv[i + 1], "x") == 0 ||
                                                                        std::strcmp(argv[i + 1], "y") == 0))
            g_axis = std::strcmp(argv[++i], "y") == 0;
        else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc)
            g_out = argv[++i];
        else
            paths.emplace_back(argv[i]);
    }
    if (paths.empty()) {
        std::fprintf(stderr, "usage: lgp_ui_anchors FILE|DIR... [--list] [--bands] [--shift DY [--units H] [--axis x|y] [--into PANES] [--skip LAYOUTS] [--keep [LAYOUT:]PANES] [--move [LAYOUT:]PANES] [--tree LAYOUTS] --out DIR] | --self-test\n");
        return 2;
    }
    if (g_shift != 0 || !g_out.empty()) {
        if (g_shift == 0 || g_out.empty()) {
            std::fprintf(stderr, "--shift needs --out and a shift other than 0\n");
            return 2;
        }
        ShiftStats sc;
        for (const fs::path& p : paths)
            shift_tree(p, &sc);
        std::printf("shift %g: %zu archives written, %zu layouts, %zu band groups (%zu under turned parents, %zu "
                    "under parents scaled to 0 taken as 1, %zu left), %zu curves with %zu keys, %zu part overrides, "
                    "%zu .gfpak archives (files in place %zu, appended %zu)\n",
                    double(g_shift), sc.archives, sc.layouts, sc.groups, sc.turned, sc.flat, sc.skipped, sc.curves,
                    sc.keys, sc.overrides, sc.gfpaks, sc.in_place, sc.appended);
        return 0;
    }
    Counts total;
    for (const fs::path& p : paths) {
        std::error_code ec;
        if (fs::is_directory(p, ec)) {
            for (const auto& e : fs::recursive_directory_iterator(p, ec))
                if (e.is_regular_file(ec))
                    scan_file(e.path(), &total);
        } else {
            scan_file(p, &total);
        }
    }
    report(total);
    return 0;
}
