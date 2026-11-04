// Picture size of the widescreen mod (docs/analysis/50-breitbild-ohne-strecken.md):
// the game's own 16:9 picture widened to the screen instead of bars or
// stretching. Wider screens keep the height and get more width (Hor+),
// narrower ones (iPad) keep the width and get more height (Vert+), both
// within the window texture the game presents from.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <span>
#include <string>
#include <vector>

namespace lgp::mods {

struct PictureSize {
    uint32_t width = 0;
    uint32_t height = 0;
};

// Size for a screen of width/height `aspect`: base (the game's 1280x720)
// grown in one direction, a multiple of 16, at most `limit` (1920x1080); if
// the limit cuts the grown side, the other side shrinks instead (32:9 ->
// 1920x540). Within 1 % of base's aspect (or aspect <= 0): base unchanged.
PictureSize widescreen_size(double aspect, PictureSize base = {1280, 720}, PictureSize limit = {1920, 1080});

// A size of the game's 16:9 family (render targets made from the picture
// size) for the picture `to` instead of `from`: a step of from's halving chain
// (n -> (n + 1) / 2, 1280x720 ... 5x3) becomes the same step of to's chain;
// another size within 11 % of from's aspect (480x270, 16x10) is scaled and
// rounded (at least 1). Other sizes (256x256, 128x64) and sizes larger than
// `from` (the 1920x1080 window texture) stay, and so do sizes that already
// look scaled: steps of the new chain and sizes nearer to's aspect than from's
// (with 16:10, 1280x800 and 480x300 are within 11 % of 16:9).
PictureSize widescreen_scale(PictureSize size, PictureSize from, PictureSize to);

// A camera projection m[16] (rows or columns: m00 = m[0], m11 = m[5]) made for
// `from` widened to `to`: wider keeps the vertical field of view (m11) and
// shows more at the sides (Hor+), narrower keeps the horizontal one (m00) and
// shows more above and below (Vert+). Only a projection with from's aspect
// (|m11| / |m00| within 1 %) changes, so running it on one again is harmless.
// A 2D orthographic projection (the menus) works the same way: its x (y) row
// scales as a whole, so the 2D picture keeps its proportions, centred.
// True if it changed m.
// keep_height: a taller picture keeps the game's vertical view and loses the
// sides instead (Hor-), e.g. for a 3D backdrop sized to the 16:9 view.
bool widescreen_projection(float* m, PictureSize from, PictureSize to, bool keep_height = false);

// A 2D element rectangle r = {w, h, x, y} in the layout's units around the
// centre (1280x720: x -640 is the left edge), drawn with a projection that
// widescreen_projection changed: one exactly as wide (high) as `from` - a
// background, dimming or a bar across the screen - spans the new width
// (height) and so reaches the edges; others stay centred. True if it changed.
bool widescreen_rect(float* r, PictureSize from, PictureSize to);

// The same for an element placed by a model matrix (rows 0 and 1 of a 3x4:
// model[0..3] gives screen x, model[4..7] screen y; translation in [3], [7]).
// If the rectangle r = {w, h, x, y} covers the whole 16:9 width (height) on
// screen - whatever its size, scale or a 90 degree turn - but not the new
// one, that model row scales by the new size, so it reaches the edges.
// Running it again changes nothing. True if it changed the model.
bool widescreen_element(const float* r, float* model, PictureSize from, PictureSize to);

// For an element the mod widened (not turned, spanning exactly the new width
// or height on screen through `model`): its texture matrix rows tex[0..3] (u
// = tex[0] * x + tex[1] * y + tex[3] for quad corners x, y in 0..1) and
// tex[4..7] (v) cover a range wider by the same factor, around the middle, so
// a repeating texture repeats more instead of stretching. Not idempotent: the
// caller keeps track of rows it wrote. True if it changed tex.
bool widescreen_texture(const float* r, const float* model, float* tex, PictureSize from, PictureSize to);

// An unturned element that touches one 16:9 edge (left or right, top or
// bottom) but not the opposite one - a header bar, a side panel - reaches the
// new picture's edge on that side; its other side stays. With tex (rows as
// in widescreen_texture, may be null) its texels keep their size: the new
// part samples beyond the old texture range, so a clamped texture repeats
// its edge column there (a flat bar end) instead of stretching a rounded one.
// Assumes quad corners 0..1 run along the rectangle from (x, y) to
// (x + w, y - h). Running it again on the result changes nothing. True if it
// changed the model.
bool widescreen_edge(const float* r, float* model, float* tex, PictureSize from, PictureSize to);

// The same reach for elements that may be turned (`stretch`): an element
// touching one 16:9 edge grows along its own axis that runs most nearly
// across that edge, away from its far side, until it reaches as far beyond
// the old edge as the new edge lies; its texels stretch along (a soft end
// the old edge cut now meets the new one). For an unturned element this is
// widescreen_edge without tex. A turned element counts as touching while its
// outermost corner lies within 32 units plus its own tilt beyond the edge.
// Not idempotent. True if it changed the model.
// With only_x (`xstretch`) only along x: a taller picture leaves the element as it is.
bool widescreen_stretch(const float* r, float* model, PictureSize from, PictureSize to, bool only_x = false);

// An element scaled like the picture where it grew (both model rows' entries
// times the new / old size, so around the centre): framed art that sits with
// widened elements (`widen`). Not idempotent. True if it changed the model.
// With only_x (`xwiden`) only along x: a taller picture leaves the element as it is.
bool widescreen_widen(float* model, PictureSize from, PictureSize to, bool only_x = false);

// Wider pictures: an unturned element stretched across the whole new width
// (its ends at the edges), e.g. tinted bands with soft ends (`span`); running
// it again changes nothing. Taller pictures: its y row scaled like
// widescreen_widen (not idempotent there). True if it changed the model.
bool widescreen_span(const float* r, float* model, PictureSize from, PictureSize to);

// Taller pictures (Vert+): an element that lies wholly at or above layout y
// `top` (y up, 360 is the old top edge) moves up by half the extra height,
// one wholly at or below `bottom` down, so header and footer groups - bars
// with their text and buttons - sit at the new edges unstretched. Only what
// showed on the old picture (also partly, e.g. a turned plate cut by the
// edge); with side > 0 only elements wholly at |x| >= side (corner groups).
// Others stay. Not idempotent. True if it moved the element.
// With member, a listed element of a group that straddles the zone line
// (`zone … with WxH,…`) goes with the group on the side of its middle.
bool widescreen_zone(const float* r, float* model, float top, float bottom, PictureSize from, PictureSize to,
                     float side = 0, bool member = false);
// The same along x for wider pictures (Hor+, `xzone RIGHT LEFT`): wholly at
// or right of `right` moves right by half the extra width, wholly at or left
// of `left` moves left; side > 0: only elements wholly at |y| >= side.
bool widescreen_xzone(const float* r, float* model, float right, float left, PictureSize from, PictureSize to,
                      float side = 0, bool member = false);

// Taller pictures: an element narrower than the old picture that lies wholly
// between `bottom` and `top` (and at |x| >= side) moves its middle away from
// the picture's middle by the new / old height, its size unchanged: rows of a
// list spread over a box that grew with the picture (`zone … spread`). Not
// idempotent. True if it moved the element.
bool widescreen_spread(const float* r, float* model, float top, float bottom, PictureSize from, PictureSize to,
                       float side = 0);
// The same along x for wider pictures (elements lower than the old picture).
bool widescreen_xspread(const float* r, float* model, float right, float left, PictureSize from, PictureSize to,
                        float side = 0);

// The same for a 2D matrix that already holds the element's place (a
// projection times view times model, as text is drawn): after
// widescreen_projection, a clip-space y translation at least `y` layout units
// of the new picture from the middle moves outwards by half the extra
// height (whichever way y points). Rows (translation m[3], m[7]) or columns
// (m[12], m[13]); a matrix without translation stays. With side > 0 only one
// at |x| >= side layout units; with spread, one nearer the middle than `y`
// moves away from it by the new / old height (widescreen_spread for text).
// Not idempotent.
bool widescreen_clip_zone(float* m, float y, PictureSize from, PictureSize to, float side = 0, bool spread = false);
// The same along x for wider pictures (`xzone X` on a proj line): an x
// translation at least `x` from the middle moves outwards by half the extra
// width; side > 0: only at |y| >= side.
bool widescreen_clip_xzone(float* m, float x, PictureSize from, PictureSize to, float side = 0, bool spread = false);

// A zone as touches see it: layout units of the old picture, y up. With x
// it is an `xzone` (top/bottom are right/left, side is |y|).
struct TouchZone {
    float top = 0, bottom = 0, side = 0;
    bool spread = false;
    bool x = false;
};

// Handheld picture of the running widescreen mod, {0, 0} while it is off
// (set by its start, cleared by mods::stop()) or resting in the TV mode.
// With `docked wide 1` it does not rest: the handheld picture also while
// docked (layout units; the aspect is the same).
PictureSize widescreen_active();
// The cfg line `docked wide 1` of the running mod: it also runs in the TV
// mode, with the docked picture (the handheld picture times 1.5, 1280x960 ->
// 1920x1440, 1568x720 -> 2352x1080): `size` rules scale the game's 1920x1080
// family to it, dw/dh are its size, `data` keeps our values. {0, 0} without.
bool widescreen_docked_wide();
PictureSize widescreen_docked_picture();

// A decoded video frame (NV12, stride = width, UV after width * height bytes)
// that the game stretches over the whole picture, if that is taller than the
// frame (LGP draws it into the middle of a wider one): if the running mod has a
// `movie` line for this frame size, its content is scaled within the frame
// (bilinear, in place) so that it keeps its aspect once stretched: `fit`
// leaves black bars, `fill` crops. False (frame unchanged) while the mod is
// off or resting in TV mode, without a line, for a picture not taller than the frame or if bytes
// is too small.
bool widescreen_movie_frame(uint8_t* nv12, size_t bytes, uint32_t width, uint32_t height);

// The console's operation mode (boot, ProcessControl::set_docked). Without
// `docked wide 1` the mod widens only the handheld picture; docked (1920x1080
// for a 16:9 TV) its patches leave everything as the game made it, so the
// picture stays 16:9 instead of a wide view squeezed into 16:9 targets. A
// switch while the game runs is not followed (layouts are changed as the game
// reads them, cameras keep the format they were built with): the app applies
// the TV mode from the next start while the mod runs.
void set_widescreen_docked(bool docked);

// A touch at fractions u, v (0..1) of the widened picture `picture` as
// fractions of `base`: the game's 2D layout stays `base` in the middle of the
// picture (widescreen_projection), so touches move by half the extra size and
// those in the new side (top/bottom) strips clamp to its edge. Assumes the
// game reads touches in base's pixels. A picture of base's size (or {0, 0})
// leaves u, v unchanged. While the running mod's zones are in force (their
// `when` elements drawn) and picture is its picture, a touch on a moved group
// or a spread row finds the place the game drew it at first.
void widescreen_touch(float* u, float* v, PictureSize picture, PictureSize base = {1280, 720});
// With the cfg line `touch render 1` and the running mod's picture: the game
// reads touch positions in its render size, so the 1280x720 touch (after
// widescreen_touch) scales by picture / base per axis; {1, 1} otherwise. The
// boot program hands it to hid (hle::set_touch_scale).
std::pair<float, float> widescreen_touch_scale(PictureSize picture, PictureSize base = {1280, 720});
// The same with the given zones in force (first that matches wins).
void widescreen_touch(float* u, float* v, PictureSize picture, PictureSize base, const TouchZone* zones,
                      size_t count);

// The `layouts` line (taller pictures): the game's layout files are changed as
// it reads them, so header and footer units (a box with its texts) sit at the
// new top and bottom edges (lgp/platform/ui/layout_shift.hpp). Hooked into
// fsp-srv by the boot program (hle::FsConfig::romfs_changes); asked once the
// RomFS is first opened, after the mods started.
// The `file … scale` lines go the same way: a float32 in a named file times
// the picture's extra size along their axis.
// begin: false while the mod is off, has neither a `layouts` line for this
// picture nor a `file` line, or the console is docked; LGP_WIDESCREEN_LAYOUTS=0
// leaves only the layouts out; else *at_mount gets the line's `pack` archives
// (.gfpak, they may grow).
bool widescreen_layouts_begin(std::vector<std::string>* at_mount);
// A file to change: one a `file` line names, or a SARC (by its first bytes)
// under one of the `layouts` line's `in` folders.
bool widescreen_layouts_wants(const std::string& path, std::span<const uint8_t> head);
// Scales a `file` line's float, or moves the units of a SARC (same size) or of
// a .gfpak's SARC files; true if anything changed.
bool widescreen_layouts_change(const std::string& path, std::vector<uint8_t>& bytes);
// RomFsRewriter's epoch: the generation of a file of a `hud` layout (by its
// name in the path), 0 for every other file.
uint64_t widescreen_layouts_epoch(const std::string& path);
// The battle's HUD bounds while the game runs (StartOptions::hud_sides: top,
// bottom, left, right percent, -1 = hud_bounds): the `hud` layouts' files are
// changed anew on their next read (the next battle; the command boxes also
// after the bag). False without a `layouts … hud` line on this picture.
bool set_widescreen_hud_sides(const std::array<int, 4>& sides);
// Layout units the running mod moves its layouts by (0: none).
float widescreen_layout_shift();

namespace detail {
// widescreen_active() for the mod's start ({0, 0}: mods::stop()).
void set_widescreen_active(PictureSize size);
// mods::stop(), while the run's guest memory still lives: the game's values back
// at the `data` rules, then no rules and no memory (a later docked switch
// touches nothing).
void stop_widescreen_data();
}  // namespace detail

}  // namespace lgp::mods
