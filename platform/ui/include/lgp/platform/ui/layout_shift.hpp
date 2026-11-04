// Moving header and footer parts of the game's 1280x720 layouts to the edges
// of a taller picture (widescreen mod, docs/analysis/50-breitbild-ohne-strecken.md):
// instead of moving drawn elements at run time (zones), the layout files
// themselves are changed as the game reads them. A unit (a box with its texts,
// an icon with its label) whose middle lies in the top or bottom band moves
// outwards by the extra height; texts move with their boxes without any timing.
//
// Only the translation floats of panes and of the animation curves that move
// them change, so a SARC keeps its size. SARC files inside a .gfpak are
// compressed again and may grow the archive. Nothing of the game is part of
// the repository; this works on the user's files at run time (and in the
// local tool lgp_ui_anchors).
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "lgp/platform/ui/layout.hpp"

namespace lgp::platform::ui {

// Where a pane sits on the 1280x720 screen: the bounding box of its turned
// and scaled rectangle, y up, centre of the screen at 0.
struct LayoutRect {
    float cx = 0, cy = 0, w = 0, h = 0;
};
// Content boxes of part layouts by name (blyt/<name>.bflyt): what a part
// pane really draws, in the part layout's own units around its centre.
using PartBoxes = std::map<std::string, LayoutRect>;
// With `parts`, a part pane whose layout is listed gets its content box
// (times its magnify) instead of its own size, which can be far larger
// (dummy panes outside the part's canvas count into it).
std::vector<LayoutRect> layout_place(const Layout& l, const PartBoxes* parts = nullptr);
// The box around a layout's drawn panes that reach into its canvas (panes
// wholly outside it, such as animation targets, are left out); w = h = 0 if none.
LayoutRect layout_content(const Layout& l);
// Picture, text, window or part: panes that draw something.
bool layout_drawn(const LayoutPane& p);

// Band groups or units of a 1280x720 layout: result[i] 0 top, 1 bottom, -1 none.
//  units == 0: band groups, the highest panes whose drawn subtree lies wholly
//    in the top (from y 180) or bottom (to y -180) band.
//  units  > 0: units, panes less than `units` high (drawn subtree) whose
//    parents are all at least that high, with their middle in a band; a unit
//    lying on a drawn pane at least `units` and less than 640 high (a window
//    behind texts, not its parent or child) stays with that pane. Numbered
//    sibling units (P_badge_00, P_badge_01, ...) of which some would move and
//    some stay all stay, so grids, lists and charts keep their shape.
// all_panes: hidden panes count as shown (the game may show them later).
struct BandStats {
    size_t groups[2] = {};      // per band (top, bottom)
    size_t covered[2] = {};     // drawn shown panes under a group or unit
    size_t texts[2] = {};       // text panes among them
    size_t texts_band[2] = {};  // text panes wholly in the band
};
// axis 0: the same along x for a wider screen (0 right, 1 left; bands from |x| 320,
// edge bands span the screen's height and end at its side).
std::vector<int> layout_bands(const Layout& l, float units, bool all_panes, BandStats* stats,
                              const PartBoxes* parts = nullptr, int axis = 1);

struct ShiftOptions {
    float shift = 0;   // layout units outwards (half the extra height, or width with axis 0)
    int axis = 1;      // 1: top and bottom groups move in y; 0: right and left groups in x
    float units = 0;   // layout_bands(): 0 band groups, else units below this height (width with axis 0)
    std::vector<std::string> into;  // groups the game places itself: their children move instead
    std::vector<std::string> skip;  // layouts (file names without folder and extension) left as they are
    // Panes that stay with everything under them: PANE in every layout, or
    // LAYOUT:PANE in one (shift_sarc picks those for each layout).
    std::vector<std::string> keep;
    // Panes that move as one group whatever their size (PANE or LAYOUT:PANE),
    // to the side their drawn subtree's middle lies on; all under them move with them.
    std::vector<std::string> move;
    // Layouts (file names without folder and extension) that move by their own
    // amount per side instead of `shift` (the battle's HUD bounds): hud_shift[0]
    // for the top (axis 0: right) groups, [1] for the bottom (left) ones.
    std::vector<std::string> hud;
    std::array<float, 2> hud_shift = {0, 0};
    // Per side for this layout (shift_sarc sets it from hud), < 0: shift.
    std::array<float, 2> band_shift = {-1, -1};
};

struct ShiftStats {
    size_t archives = 0, layouts = 0, groups = 0, turned = 0, flat = 0, skipped = 0, curves = 0, keys = 0,
           overrides = 0, gfpaks = 0, in_place = 0, appended = 0;
};

// The local translation change (x, y) of each pane of a 1280x720 layout:
// the screen shift through the inverse of the parents' scale and turn.
std::vector<std::array<float, 2>> layout_shifts(const Layout& l, const ShiftOptions& o, ShiftStats* stats,
                                                const PartBoxes* parts = nullptr);

// Called for each screen layout of an archive with its shifts (for listings).
using ShiftListener = std::function<void(const std::string& name, const Layout& l,
                                         const std::vector<std::array<float, 2>>& shift)>;

// Shifts the screen layouts of one SARC in place (same size), with the
// overrides in part panes of the moved panes and the curves of animations
// (anim/<layout>_*.bflan) that move them. A layout that another layout of the
// archive shows as a part and that would move nothing on its canvas is left
// as it is: its part panes count by its content box (so far off dummy panes
// do not make them look tall) and can move as units. A part layout that moves
// something on its canvas keeps its own units. True if anything changed.
bool shift_sarc(std::vector<uint8_t>& bytes, const ShiftOptions& o, ShiftStats* stats,
                const ShiftListener& listener = {});
// The same for every SARC file in a GFLXPACK archive (.gfpak). LZ4 files are
// compressed again; one that fits its old place stays there, else it goes to
// the end of the archive and its entry points there.
bool shift_gfpak(std::vector<uint8_t>& bytes, const ShiftOptions& o, ShiftStats* stats,
                 const ShiftListener& listener = {});

// LZ4 block format (public): decode to exactly `size` bytes; compress (greedy).
std::optional<std::vector<uint8_t>> lz4_block_decode(std::span<const uint8_t> in, size_t size);
std::vector<uint8_t> lz4_block_compress(std::span<const uint8_t> in);

// The file name without folder and extension.
std::string layout_stem(const std::string& name);

}  // namespace lgp::platform::ui
