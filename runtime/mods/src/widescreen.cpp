// Mod "widescreen" (docs/analysis/50-breitbild-ohne-strecken.md): the game
// renders its handheld picture in the screen's aspect instead of 16:9, so a
// wider or taller screen shows more of the world rather than bars or a
// stretched picture.
//
// The game builds its picture sizes at start-up from move-wide immediates
// (MOVZ+MOVK), so the mod replaces the value those instructions produce:
// patch sites, lifted from the `patch` lines of widescreen.cfg.
//
// Lines in widescreen.cfg (our own names; the addresses are game data):
//   patch <name> <va> LO [HI]   the MOVK (or MOVZ/MOV) at va yields LO | HI << 32;
//                               LO, HI: hw, hh (handheld width/height, set by
//                               the mod), dw, dh (docked, kept), a number, or
//                               a whole-number expression such as hh/2
//   patch <name> <va> size W H [C&MASK]
//                               observing patch: before the instruction at va
//                               runs, a width/height of the 16:9 family in W/H
//                               becomes that size for the new picture
//                               (widescreen_scale), e.g. render targets a
//                               creator makes from its own description. W, H, C:
//                               wN, xN or [xN+OFF] (u32 in guest memory, also
//                               written; [xN+OFF]f a float, as in viewport
//                               rectangles); with C&MASK only if C & MASK != 0.
//                               A description handed in again after the rule
//                               wrote it stays as it is. Appendix `skip WxH,…`:
//                               these sizes stay (an offscreen picture the
//                               game composites with fixed 16:9 geometry, such
//                               as the stat hexagon's 480x270). While docked
//                               (`docked wide`) the skip sizes times 1.5 stay
//                               instead, or the list after `docked` (`none`:
//                               nothing stays), since there the game's 1920x1080
//                               is the scene target, not a handheld offscreen one.
//                               `skip WxH@N` stays only while the operand of
//                               the appendix `format F` (before `skip`) holds
//                               N (the description's own format code), e.g. a
//                               video's planes against a colour target of the
//                               same size.
//   patch <name> <va> float EXPR
//                               the LDR St (literal, unsigned offset or LDUR) at
//                               va loads EXPR instead: A, A/B, A*B, A+B or A-B
//                               with A, B hw, hh, dw, dh, lx, ly (the `layouts`
//                               shift along x or y, 0 without it) or a number
//                               (e.g. hh/2 for a 2D drawing that divides by a
//                               fixed 360, 640+lx for code that places a pane
//                               at the old right edge itself); in TV
//                               mode the game's own value (the instruction
//                               runs as it is)
//   patch <name> <va> proj [xN+OFF]
//                               observing patch: the 4x4 float camera projection
//                               at the address (rows or columns) gets the new
//                               picture's aspect (widescreen_projection: Hor+
//                               wider, Vert+ narrower); one with another aspect
//                               (already changed, other cameras) stays.
//                               `crop WxH,…` (proj line): while an element with
//                               one of these rectangles passes a rect line
//                               (within the gate time), a taller picture
//                               keeps the game's vertical view and loses the
//                               sides (Hor-) instead of Vert+ (a 3D fade
//                               sized to the 16:9 view, the title);
//                               `crop … hold MS` keeps it open that long;
//                               back to Vert+ it eases over `ease MS`
//                               (default 1000, 0: at once); `crop … except
//                               WxH,…` shuts it at once, without hold or
//                               easing, while one of these elements is drawn
//                               (the next screen with the same camera).
//                               `zone Y [BOTTOM] [side X] [spread] [inside|follow] [when WxH,…]` (proj
//                               line, repeatable): a 2D matrix that holds
//                               its element's place (text) at least Y layout
//                               units from the middle moves to the new top or
//                               bottom edge (widescreen_clip_zone; BOTTOM, if
//                               given, is the threshold below the middle
//                               instead of -Y, e.g. -1000: only upwards); one whose
//                               gate was shut when it changed follows when the
//                               gate opens within 2 s (while still unchanged);
//                               `inside`: only onto an element a `zone` (or its
//                               `spread`) of a
//                               rect line moved the same way and still draws
//                               (text follows its box; it also waits for it);
//                               `follow`: like `inside`, but text on such an
//                               element moves exactly as far as the element
//                               did (spread boxes keep their size, so text
//                               spread on its own leaves them), whatever Y
//                               and side; text where a moved element is now
//                               but was not (built from it) stays;
//                               `cull [below Y] [above Y] [x MIN MAX] [y MIN MAX]
//                               [taller|wider] [when WxH,…]` (proj line,
//                               repeatable): a 2D matrix (text) that lies at
//                               or beyond Y, inside the x/y ranges (new
//                               picture units, as `log N` prints them), in
//                               a taller/wider picture only if named, is not
//                               drawn while one of the `when` elements is
//                               (list rows beyond their frame); the game's
//                               matrix comes back once the gate shuts.
//                               `log N` logs where the matrices lie.
//   patch <name> <va> rect [xN+OFF]   (or appended to a proj line)
//                               observing patch: a 2D element rectangle of
//                               floats w, h, x, y at the address (centred
//                               layout units) that spans the whole 16:9 width
//                               (height) reaches the new picture's edges
//                               (widescreen_rect): backgrounds, dimming, bars.
//   patch <name> <va> rect [xN+OFF] model [xN+OFF]
//                               the same with the element's 3x4 model matrix
//                               (rows, translation in the 4th column): an
//                               element that covers the whole 16:9 width
//                               (height) on screen - also turned or scaled -
//                               gets its model row widened instead
//                               (widescreen_element), e.g. turned fades;
//                               `… model [..] tex [xN+OFF]` also widens the
//                               texture rows there as the `tex` line does;
//                               `only WxH,…` limits that to elements with
//                               these rectangles (patterns, not framed art);
//                               `edge WxH,…` lets elements with these
//                               rectangles that touch one 16:9 edge reach the
//                               new edge on that side, texels unstretched
//                               (widescreen_edge: header bars), `edge all`
//                               every such element (to look; reaching up to
//                               32 units beyond the edge counts); `stretch
//                               WxH,…` the same with the texels stretched
//                               along (soft ends the old edge cut); `widen
//                               WxH,…` scales elements with these rectangles
//                               like the picture, centred (art beside widened
//                               frames), `xwiden WxH,… [when WxH,…]` only
//                               along x on wider pictures (companions of a
//                               full-width frame), only while one of the
//                               `when` elements is drawn if named; `span WxH,…` stretches them across
//                               the whole new width (tinted bands whose soft
//                               ends then lie at the edges); `zone TOP BOTTOM` moves elements
//                               wholly above/below these layout y to the new
//                               top/bottom edge on taller pictures
//                               (widescreen_zone: header and footer groups),
//                               `… side X` only elements wholly at |x| >= X
//                               (corner groups), `… with WxH,…` also moves
//                               elements with these rectangles that straddle
//                               the line (to the side of their middle),
//                               `… spread` moves what lies
//                               between apart like the picture (list rows),
//                               `… when WxH,…` only while an element with
//                               one of these rectangles is drawn; a line may
//                               have several zones, the first that acts wins
//                               (a zone without `when` follows all gates);
//                               `log N` logs the
//                               first N distinct elements with their screen
//                               extent (to find rectangles for the lists).
//                               `… movie WxH,…`: elements of these rectangles
//                               stay as the game made them while a video plays
//                               (a frame decoded in the last 250 ms) and has
//                               not faded to white (3 frames white all over),
//                               e.g. the
//                               backdrop behind a video the game draws
//                               unstretched in the middle of a wider picture.
//                               `… fill WxH,…`: below 100 % HUD bounds,
//                               elements of these rectangles whose outer side
//                               lies on the HUD bounds line (moved there by a
//                               zone or the layout shift) reach on out to the
//                               picture's edge (white footer bars, text bands).
//                               `… cull WxH,…`: elements of these rectangles
//                               lying entirely outside the old 1280x720 view
//                               are not drawn (rows a list draws beyond its
//                               frame, which the screen edge hid before);
//                               `below Y` / `above Y` move the limits (a row
//                               whose top edge is below Y goes, e.g. one a
//                               footer covered in 16:9), `when WxH,…` only
//                               while one of these elements is drawn (the
//                               list's frame); several `cull` groups.
//   patch <name> <va> vfov
//                               replacing patch at STR St, [Xn, #imm] or STUR
//                               St of a camera's vertical field of view
//                               (radians): on a taller picture it stores the
//                               Vert+ angle instead (the register stays), so
//                               whatever reads the angle (culling, loading)
//                               sees the picture's view; docked the game's.
//   patch <name> <va> before wN|xN VALUE [taller|wider]
//                               observing patch: on a picture other than 16:9
//                               (only taller or only wider ones) the register
//                               gets VALUE before the instruction at va runs,
//                               which still runs and must not read it; e.g. a
//                               draw cull's "outside" bit set to 0 after the
//                               game stored its own result for other readers.
//   patch <name> <va> const VALUE [taller|wider]
//                               replacing patch at AND/ORR/EOR or ADD/SUB
//                               (immediate, no flags): on a picture other than
//                               16:9 (only taller or only wider ones) its
//                               register gets VALUE instead, e.g. a
//                               draw cull's "outside" bit 0 so terrain beyond
//                               the old view is drawn (the game's logic keeps
//                               its own cull); docked the instruction's result.
//   patch <name> <va> cullfov
//                               replacing patch at LDP St1, St2, [Xn, #imm]
//                               that loads a culling frustum's fovy and aspect
//                               from a camera: a 16:9 pair reaches the code as
//                               the picture's aspect and, on a taller picture,
//                               the Vert+ angle (registers only, memory stays);
//                               docked the game's.
//   file <name> <offset> <path> scale x|y [was F]
//                               a float32 in a RomFS file times the picture's
//                               extra size along x (wider) or y (taller), as
//                               fsp-srv reads it (not docked at start): a
//                               particle emitter sized to the 16:9 view, such
//                               as the title's white fade. `was F`: only if
//                               the file holds F there.
//   repeat <name> <width> <height> u|v|uv
//                               file textures of that size sample with Repeat
//                               instead of a clamp on those axes (render hint
//                               to the graphics HLE), for patterns `tex` widens.
//   patch <name> <va> tex [xN+OFF] rect [xN+OFF] model [xN+OFF]
//                               observing patch: where an element's texture
//                               matrix rows u, v (2x4 floats) are written, an
//                               unturned element that spans exactly the new
//                               width (height) gets its u (v) range widened
//                               around the middle (widescreen_texture): a
//                               repeated pattern repeats more instead of
//                               stretching. Only for textures that repeat.
//   movie <name> <width> <height> fit|fill
//                               decoded video frames of that size (movie::
//                               HLE) for a game that stretches its video quad
//                               over a taller picture (iPad; wider pictures
//                               stay as they are): the frame's content is
//                               scaled within the frame so that, stretched,
//                               it keeps its aspect: `fit` with black bars,
//                               `fill` cropped (widescreen_movie_frame). In TV
//                               mode without `docked wide` and while the mod
//                               is off frames stay; with it docked they are
//                               fitted to the docked picture.
// At least one line must use hw or hh.
//
// State: the sizes follow the screen; whether the camera follows them
// (field of view) is checked on the PC (analysis 50).
#include "lgp/mods/widescreen.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "lgp/platform/ui/layout_shift.hpp"
#include "lgp/runtime/guest_mem.hpp"
#include "lgp/runtime/log.hpp"
#include "lgp/runtime/memory.hpp"
#include "lgp/runtime/patch.hpp"
#include "lgp/runtime/render_hints.hpp"
#include "mod_impl.hpp"

namespace lgp::mods {

namespace {
uint32_t round16(double v) { return std::max<uint32_t>(16, static_cast<uint32_t>(std::lround(v / 16.0)) * 16); }

std::atomic<uint64_t> g_active{0};  // width << 32 | height of the running mod's picture
std::atomic<bool> g_docked{false};  // set_widescreen_docked()
// `docked wide 1`: the mod also runs in the TV mode, with the docked picture
// (the handheld picture times 1.5, render-target pixels of the 1920x1080
// family) instead of resting at 16:9.
std::atomic<bool> g_docked_wide{false};
std::atomic<uint64_t> g_docked_picture{0};  // width << 32 | height
// The mod rests: docked without `docked wide 1`.
bool resting() { return g_docked.load(std::memory_order_relaxed) && !g_docked_wide.load(std::memory_order_relaxed); }
// `docked wide 2`: the docked picture no larger in area than 1920x1080, since
// the game sizes its pools for the TV mode's targets (PC 01.11.: at 1920x1440
// its allocator wrote past the end of the guest heap).
std::atomic<bool> g_docked_cap{false};
// The docked picture of a handheld picture: 1280x960 -> 1920x1440, 1568x720 ->
// 2352x1080; capped (`docked wide 2`) 1648x1232 and 2096x960, multiples of 16
// in the handheld aspect.
PictureSize docked_of(PictureSize handheld) {
    const PictureSize full{(handheld.width * 3 + 1) / 2, (handheld.height * 3 + 1) / 2};
    constexpr uint64_t kArea = 1920ull * 1080ull;
    if (!g_docked_cap.load(std::memory_order_relaxed) || uint64_t(full.width) * full.height <= kArea ||
        handheld.width == 0 || handheld.height == 0)
        return full;
    const double aspect = double(handheld.width) / handheld.height;
    for (uint32_t h = (full.height / 16) * 16; h >= 16; h -= 16) {
        const uint32_t w = round16(h * aspect);
        if (uint64_t(w) * h <= kArea) return {w, h};
    }
    return full;
}

// The `movie` lines: set by install, read by the movie:: HLE's decoder thread.
struct MovieRule {
    uint32_t width = 0, height = 0;
    bool fill = false;
};
std::mutex g_movie_mutex;
std::vector<MovieRule> g_movie_rules;
// `touch render 1`: the game reads touches in its render size (the wider or
// taller picture), not 1280x720 (the pet scene, the only touch scene of the
// handheld game; menus ignore touch); 2: the place on the whole picture as it
// is, for code patched to compute with the real render size.
std::atomic<int> g_touch_render{0};  // `touch render 0|1|2`
// HUD bounds (StartOptions::hud_bounds / 100): the share of the extra size by
// which zones, spreads and the layout shift move groups towards the edge.
std::atomic<float> g_hud{1.0f};
float hud() { return g_hud.load(std::memory_order_relaxed); }
std::atomic<bool> g_movie_logged{false};
std::atomic<int64_t> g_movie_frame_ns{0};  // host time of the last decoded video frame
// The video has faded to white: kMovieWhiteFrames frames in a row whose every
// sample (16 px grid) is at least kMovieWhiteY; 0 otherwise. Holds the
// frames' mean luma for the 4:3 bars.
constexpr int kMovieWhiteFrames = 3;
constexpr uint8_t kMovieWhiteY = 200;  // of 16..235
std::atomic<int> g_movie_white_run{0};
std::atomic<uint32_t> g_movie_white_y{0};
bool movie_white() { return g_movie_white_run.load(std::memory_order_relaxed) >= kMovieWhiteFrames; }

// The `layouts` line: set by install before guest code runs, read by fsp-srv.
struct Layouts {
    bool on = false;
    platform::ui::ShiftOptions options;
    std::vector<std::string> in;     // folders ("/bin/app/"), empty: all
    std::vector<std::string> packs;  // .gfpak archives changed at mount
    std::mutex mutex;                // stats, changes
    platform::ui::ShiftStats stats;
    size_t files = 0;
    size_t examined = 0;    // files handed to change()
    int64_t spent_ns = 0;   // time in change() (host)
    // `hud L,…`: half the extra size along the axis and the hud_bounds the
    // sides without a value of their own take, for set_widescreen_hud_sides;
    // options.hud_shift for the patches (edge_band) without the mutex; the
    // generation of the `hud` layouts' files (RomFsRewriter's epoch), one up
    // per change of the bounds.
    float half_extra = 0;
    int hud_bounds = 100;
    std::atomic<float> hud_now[2] = {0.0f, 0.0f};
    std::atomic<uint64_t> hud_epoch{1};
    bool run = false;  // widescreen_layouts_begin: the layouts change in this RomFS (not docked, not switched off)
};

// `file <name> <OFF> <PATH> scale x|y [was F]`: a float32 in a RomFS file times
// the picture's extra size along that axis, as fsp-srv reads it (the title's
// white fade, a particle emitter sized to the 16:9 view). Set by install.
struct FileScale {
    std::string name, path;  // path from the RomFS root, with a leading '/'
    uint64_t offset = 0;
    float factor = 1;
    bool has_was = false;
    float was = 0;  // `was F`: the file's value there, otherwise it stays (another game version)
};
std::mutex g_file_scale_mutex;
std::vector<FileScale> g_file_scales;
bool file_scale_wanted(const std::string& path) {
    std::lock_guard lock(g_file_scale_mutex);
    return std::any_of(g_file_scales.begin(), g_file_scales.end(), [&](const FileScale& f) { return f.path == path; });
}
Layouts& layouts() {
    static Layouts l;
    return l;
}

// `when WxH,…`: a gate is open while an element with its rectangle passed
// within kZoneGateNs (host time), so a zone acts only on its own screens (Pokédex and
// X menu by their footer, the battle by its boxes). Filled by install
// before guest code runs, read by patches and touches.
// Diagnosis: LGP_WIDESCREEN_GATE_MS=N sets that time in host milliseconds.
constexpr int64_t kZoneGateNs = 200'000'000;
int64_t zone_gate_ns() {
    static const int64_t ns = [] {
        const char* env = std::getenv("LGP_WIDESCREEN_GATE_MS");
        if (env == nullptr || *env == '\0') return kZoneGateNs;
        char* end = nullptr;
        const long long ms = std::strtoll(env, &end, 10);
        if (end == env || *end != '\0' || ms < 0) {
            rt::log(rt::LogLevel::Warn, "LGP_WIDESCREEN_GATE_MS: expected milliseconds, ignored");
            return kZoneGateNs;
        }
        rt::log(rt::LogLevel::Info, "widescreen: zone gates stay open %lld ms", ms);
        return int64_t(ms) * 1'000'000;
    }();
    return ns;
}
struct Gate {
    float w = 0, h = 0;
    std::atomic<int64_t> seen_ns{INT64_MIN / 2};
};
std::deque<Gate>& gates() {
    static std::deque<Gate> g;
    return g;
}
// `crop WxH,…` on proj lines: gates of their own, so they do not open the
// zones that follow all gates.
std::deque<Gate>& crop_gates() {
    static std::deque<Gate> g;
    return g;
}
int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
// Diagnosis: LGP_WIDESCREEN_LOG_AFTER_MS=N: `log N` lines only from N host
// milliseconds after the first one asked (title and field fill them otherwise).
bool log_open() {
    static const int64_t from = [] {
        const char* env = std::getenv("LGP_WIDESCREEN_LOG_AFTER_MS");
        if (env == nullptr || *env == '\0') return INT64_MIN;
        char* end = nullptr;
        const long long ms = std::strtoll(env, &end, 10);
        if (end == env || *end != '\0' || ms < 0) {
            rt::log(rt::LogLevel::Warn, "LGP_WIDESCREEN_LOG_AFTER_MS: expected milliseconds, ignored");
            return INT64_MIN;
        }
        rt::log(rt::LogLevel::Info, "widescreen: log lines from %lld ms on", ms);
        return now_ns() + int64_t(ms) * 1'000'000;
    }();
    return now_ns() >= from;
}
// Open if one of the listed gates is; a zone without `when` follows all gates
// (open while none is configured or any is open).
bool gate_open(const std::vector<size_t>& when) {
    const int64_t now = now_ns();
    const int64_t window = zone_gate_ns();
    const auto open = [&](const Gate& g) { return now - g.seen_ns.load(std::memory_order_relaxed) < window; };
    if (!when.empty())
        return std::any_of(when.begin(), when.end(), [&](size_t i) { return open(gates()[i]); });
    return gates().empty() || std::any_of(gates().begin(), gates().end(), open);
}
// The `rect` lines' zones, for touches.
struct GatedTouchZone {
    TouchZone zone;
    std::vector<size_t> when;
};
std::vector<GatedTouchZone>& touch_zones() {
    static std::vector<GatedTouchZone> z;
    return z;
}
}  // namespace

namespace {
// `patch <name> <va> data EXPR`: a float in guest memory (rodata too) the game
// copies from, e.g. the 2D layout bounds every interface camera starts with;
// written at start, the game's value back when docked.
struct DataRule {
    uint64_t va = 0;
    uint32_t game = 0, ours = 0;  // float bits
};
std::mutex g_data_mutex;
std::vector<DataRule> g_data_rules;
rt::GuestMemory* g_data_memory = nullptr;
}  // namespace

static void write_data_rules(bool ours) {
    std::lock_guard lock(g_data_mutex);
    if (g_data_memory == nullptr) return;
    for (const DataRule& d : g_data_rules) {
        std::string error;
        if (!rt::poke_guest(*g_data_memory, d.va, ours ? d.ours : d.game, 4, &error))
            rt::log(rt::LogLevel::Warn, "widescreen: data 0x%llx: %s", static_cast<unsigned long long>(d.va),
                    error.c_str());
    }
}

void set_widescreen_docked(bool docked) {
    g_docked.store(docked, std::memory_order_relaxed);
    write_data_rules(!resting());
}

bool widescreen_docked_wide() { return g_docked_wide.load(std::memory_order_relaxed); }

PictureSize widescreen_docked_picture() {
    if (!g_docked_wide.load(std::memory_order_relaxed)) return {};
    const uint64_t a = g_docked_picture.load(std::memory_order_relaxed);
    return {static_cast<uint32_t>(a >> 32), static_cast<uint32_t>(a)};
}

PictureSize widescreen_active() {
    if (resting()) return {};
    const uint64_t a = g_active.load(std::memory_order_relaxed);
    return {static_cast<uint32_t>(a >> 32), static_cast<uint32_t>(a)};
}

void widescreen_touch(float* u, float* v, PictureSize picture, PictureSize base, const TouchZone* zones,
                      size_t count) {
    if (picture.width == 0 || picture.height == 0 || base.width == 0 || base.height == 0) return;
    if (picture.width == base.width && picture.height == base.height) return;
    // Layout units of the picture around its middle, y up.
    const float pw = float(picture.width), ph = float(picture.height), bw = float(base.width), bh = float(base.height);
    const float x0 = *u * pw - pw / 2, y0 = ph / 2 - *v * ph;
    float y = y0;
    if (ph > bh) {
        const float shift = (ph - bh) / 2 * hud(), s = 1 + (ph / bh - 1) * hud();
        for (size_t i = 0; i < count; ++i) {
            const TouchZone& z = zones[i];
            if (z.x || (z.side > 0 && std::abs(x0) < z.side)) continue;
            if (y0 >= z.top + shift) {
                y = y0 - shift;
                break;
            }
            if (y0 <= z.bottom - shift) {
                y = y0 + shift;
                break;
            }
            if (z.spread && y0 > z.bottom * s && y0 < z.top * s) {
                y = y0 / s;
                break;
            }
        }
    }
    float x = x0;
    if (pw > bw) {  // `xzone`: the same along x
        const float shift = (pw - bw) / 2 * hud(), s = 1 + (pw / bw - 1) * hud();
        for (size_t i = 0; i < count; ++i) {
            const TouchZone& z = zones[i];
            if (!z.x || (z.side > 0 && std::abs(y0) < z.side)) continue;
            if (x0 >= z.top + shift) {
                x = x0 - shift;
                break;
            }
            if (x0 <= z.bottom - shift) {
                x = x0 + shift;
                break;
            }
            if (z.spread && x0 > z.bottom * s && x0 < z.top * s) {
                x = x0 / s;
                break;
            }
        }
    }
    if (picture.width != base.width) *u = std::clamp((x + bw / 2) / bw, 0.0f, 1.0f);
    if (picture.height != base.height) *v = std::clamp((bh / 2 - y) / bh, 0.0f, 1.0f);
}

// Diagnosis LGP_WIDESCREEN_TOUCH=wide|scaled (wider pictures): x is not mapped
// back onto the 1280 surface in the middle. wide: picture pixels as game x (up
// to the picture's width, so also beyond 1280; hid clamps at 1279 unless
// LGP_HID_TOUCH_MAX_X allows more); scaled: the whole picture width onto 0..1280.
int touch_x_mode() {
    static const int mode = [] {
        const char* env = std::getenv("LGP_WIDESCREEN_TOUCH");
        if (env == nullptr || *env == '\0') return 0;
        const std::string_view s(env);
        const int m = s == "wide" ? 1 : s == "scaled" ? 2 : 0;
        if (m == 0)
            rt::log(rt::LogLevel::Warn, "LGP_WIDESCREEN_TOUCH: expected wide or scaled, ignored");
        else
            rt::log(rt::LogLevel::Info, "widescreen: touch x %s (diagnosis)", env);
        return m;
    }();
    return mode;
}

void widescreen_touch(float* u, float* v, PictureSize picture, PictureSize base) {
    const float u0 = *u;
    std::vector<TouchZone> open;
    const PictureSize active = widescreen_active();
    // `touch render 1`: the game maps the whole render size onto its 1280x720
    // layout (hid scales the centred 1280x720 position up), so the zones' moves
    // back would count twice; a moved element is not reachable this way.
    const bool render = widescreen_touch_scale(picture, base) != std::pair<float, float>{1.0f, 1.0f};
    // `touch render 2`: the game reads the whole picture (code patched to the
    // render size), so the place on the picture goes through unchanged.
    if (render && g_touch_render.load(std::memory_order_relaxed) == 2) return;
    if (!render && active.width == picture.width && active.height == picture.height && active.width != 0) {
        // Moved layouts: units with their middle in the top or bottom band (axis
        // x: the right or left one) went to the edge (an approximation: a unit
        // reaching into the middle band is found only where it lies beyond the band).
        if (layouts().on) {
            if (layouts().options.axis != 0)
                open.push_back({180, -180, 0, false, false});
            else
                open.push_back({320, -320, 0, false, true});
        }
        for (const GatedTouchZone& z : touch_zones())
            if (gate_open(z.when)) open.push_back(z.zone);
    }
    widescreen_touch(u, v, picture, base, open.data(), open.size());
    if (const int mode = touch_x_mode(); mode != 0 && picture.width > base.width && base.width != 0)
        *u = mode == 1 ? std::max(0.0f, u0) * float(picture.width) / float(base.width) : std::clamp(u0, 0.0f, 1.0f);
}

std::pair<float, float> widescreen_touch_scale(PictureSize picture, PictureSize base) {
    const PictureSize active = widescreen_active();
    if (g_touch_render.load(std::memory_order_relaxed) == 0 || g_active.load(std::memory_order_relaxed) == 0 ||
        resting() || base.width == 0 || base.height == 0 ||
        active.width != picture.width || active.height != picture.height || picture.width == 0)
        return {1.0f, 1.0f};
    return {float(picture.width) / float(base.width), float(picture.height) / float(base.height)};
}

float widescreen_layout_shift() {
    Layouts& l = layouts();
    return l.on && g_active.load(std::memory_order_relaxed) != 0 && !resting()
               ? l.options.shift
               : 0;
}

uint64_t widescreen_layouts_epoch(const std::string& path) {
    Layouts& l = layouts();
    if (!l.on || l.options.hud.empty()) return 0;
    // The archives carry their layout's name (battle_top_00 in battle_top_00_ger.arc).
    const std::string_view name = std::string_view(path).substr(path.rfind('/') + 1);
    for (const std::string& h : l.options.hud)
        if (name.find(h) != std::string_view::npos) return l.hud_epoch.load(std::memory_order_relaxed);
    return 0;
}

bool set_widescreen_hud_sides(const std::array<int, 4>& sides) {
    Layouts& l = layouts();
    std::lock_guard lock(l.mutex);
    if (!l.on || l.options.hud.empty()) return false;
    const auto share = [&](int side) {  // 0 top, 1 bottom, 2 left, 3 right
        const int p = sides[size_t(side)] >= 0 ? sides[size_t(side)] : l.hud_bounds;
        return float(std::clamp(p, 0, 100)) / 100.0f;
    };
    const bool y = l.options.axis != 0;
    const std::array<float, 2> now = {l.half_extra * share(y ? 0 : 3), l.half_extra * share(y ? 1 : 2)};
    if (now == l.options.hud_shift) return true;
    l.options.hud_shift = now;
    l.hud_now[0] = now[0];
    l.hud_now[1] = now[1];
    l.hud_epoch.fetch_add(1, std::memory_order_relaxed);
    rt::log(rt::LogLevel::Info, "widescreen: HUD bounds of %zu layouts now %.0f %s, %.0f %s (from their next read)",
            l.options.hud.size(), double(now[0]), y ? "top" : "right", double(now[1]), y ? "bottom" : "left");
    return true;
}

bool widescreen_layouts_begin(std::vector<std::string>* at_mount) {
    Layouts& l = layouts();
    l.run = false;
    bool files = false;
    {
        std::lock_guard lock(g_file_scale_mutex);
        files = !g_file_scales.empty();
    }
    // The settings of an earlier start stay until the next install; only a
    // running mod (mods::stop() clears g_active) changes layouts.
    if ((!l.on && !files) || g_active.load(std::memory_order_relaxed) == 0) return false;
    if (resting()) {
        rt::log(rt::LogLevel::Info, "widescreen: docked at start, layouts and files stay as they are");
        return false;
    }
    if (!l.on) return true;
    if (const char* env = std::getenv("LGP_WIDESCREEN_LAYOUTS"); env != nullptr && std::string_view(env) == "0") {
        rt::log(rt::LogLevel::Info, "widescreen: LGP_WIDESCREEN_LAYOUTS=0, layouts stay as they are");
        return files;
    }
    // Diagnosis: more panes that stay (PANE or LAYOUT:PANE, comma-separated).
    if (const char* env = std::getenv("LGP_WIDESCREEN_LAYOUTS_KEEP"); env != nullptr && *env != '\0') {
        std::string_view v(env);
        while (!v.empty()) {
            const size_t comma = v.find(',');
            if (comma != 0) l.options.keep.emplace_back(v.substr(0, comma));
            if (comma == std::string_view::npos) break;
            v.remove_prefix(comma + 1);
        }
        rt::log(rt::LogLevel::Info, "widescreen: LGP_WIDESCREEN_LAYOUTS_KEEP, %zu panes stay",
                l.options.keep.size());
    }
    *at_mount = l.packs;
    l.run = true;
    return true;
}

bool widescreen_layouts_wants(const std::string& path, std::span<const uint8_t> head) {
    if (file_scale_wanted(path)) return true;
    Layouts& l = layouts();
    if (!l.on || !l.run || head.size() < 4 || std::memcmp(head.data(), "SARC", 4) != 0) return false;
    if (l.in.empty()) return true;
    return std::any_of(l.in.begin(), l.in.end(), [&](const std::string& dir) { return path.rfind(dir, 0) == 0; });
}

bool widescreen_layouts_change(const std::string& path, std::vector<uint8_t>& bytes) {
    if (file_scale_wanted(path)) {
        std::lock_guard lock(g_file_scale_mutex);
        bool changed = false;
        for (const FileScale& f : g_file_scales) {
            if (f.path != path) continue;
            float v = 0;
            if (f.offset + 4 > bytes.size()) {
                rt::log(rt::LogLevel::Warn, "widescreen: file %s: %s has %zu bytes, offset 0x%" PRIx64 " lies beyond",
                        f.name.c_str(), path.c_str(), bytes.size(), f.offset);
                continue;
            }
            std::memcpy(&v, bytes.data() + f.offset, 4);  // little-endian float32, as the host
            if (f.has_was && v != f.was) {
                rt::log(rt::LogLevel::Warn, "widescreen: file %s: %g at 0x%" PRIx64 " instead of %g, stays",
                        f.name.c_str(), double(v), f.offset, double(f.was));
                continue;
            }
            const float now = v * f.factor;
            std::memcpy(bytes.data() + f.offset, &now, 4);
            rt::log(rt::LogLevel::Info, "widescreen: file %s: %g -> %g at 0x%" PRIx64 " (x%.4f)", f.name.c_str(),
                    double(v), double(now), f.offset, double(f.factor));
            changed = true;
        }
        return changed;
    }
    Layouts& l = layouts();
    if (!l.on || !l.run) return false;
    std::lock_guard lock(l.mutex);
    const bool pack = bytes.size() >= 8 && std::memcmp(bytes.data(), "GFLXPACK", 8) == 0;
    const platform::ui::ShiftStats before = l.stats;
    // FNV-1a of the file before and after: lets two hosts compare what they read and wrote (diagnosis).
    const auto fnv = [](const std::vector<uint8_t>& b) {
        uint64_t h = 0xcbf29ce484222325ull;
        for (const uint8_t c : b) h = (h ^ c) * 0x100000001b3ull;
        return h;
    };
    const uint64_t in_hash = fnv(bytes);
    const int64_t start = now_ns();
    const bool changed = pack ? platform::ui::shift_gfpak(bytes, l.options, &l.stats)
                              : platform::ui::shift_sarc(bytes, l.options, &l.stats);
    const int64_t took = now_ns() - start;
    ++l.examined;
    l.spent_ns += took;
    if (pack)
        rt::log(rt::LogLevel::Info, "widescreen: %s (%zu bytes) took %.1f ms", path.c_str(), bytes.size(),
                double(took) / 1e6);
    if (!changed) return false;
    ++l.files;
    // One line per changed file (game file names: the log stays private), with the totals so far.
    rt::log(rt::LogLevel::Info,
            "widescreen: layouts of %s moved %.0f (%zu units); %zu files so far: %zu layouts, %zu units (%zu turned), "
            "%zu curves with %zu keys, %zu part overrides, %zu .gfpak (%zu in place, %zu appended); "
            "%zu files read for it in %.0f ms; fnv %016" PRIx64 " -> %016" PRIx64,
            path.c_str(), double(l.options.shift), l.stats.groups - before.groups, l.files, l.stats.layouts,
            l.stats.groups, l.stats.turned, l.stats.curves, l.stats.keys, l.stats.overrides, l.stats.gfpaks,
            l.stats.in_place, l.stats.appended, l.examined, double(l.spent_ns) / 1e6, in_hash, fnv(bytes));
    return true;
}

namespace {
// One plane (channels 1: Y, 2: interleaved UV) of w x h samples: dst from src
// scaled by sx, sy around the middle (bilinear), `bar` (one value per
// channel) outside.
void movie_plane(const uint8_t* src, uint8_t* dst, uint32_t w, uint32_t h, int channels, double sx, double sy,
                 const uint8_t* bar) {
    struct Tap {
        int32_t i = -1;  // first sample, -1: outside
        uint32_t f = 0;  // weight of the second, of 256
    };
    auto taps = [](uint32_t n, double s) {
        std::vector<Tap> t(n);
        for (uint32_t d = 0; d < n; ++d) {
            const double u = (double(d) + 0.5 - n / 2.0) / s + n / 2.0;  // source position
            if (u < 0 || u > double(n)) continue;
            const double p = std::clamp(u - 0.5, 0.0, double(n - 1));
            const auto i = static_cast<uint32_t>(p);
            t[d].i = int32_t(std::min(i, n - 1));
            t[d].f = i + 1 < n ? static_cast<uint32_t>(std::lround((p - i) * 256)) : 0;
        }
        return t;
    };
    const std::vector<Tap> tx = taps(w, sx), ty = taps(h, sy);
    const size_t row = size_t(w) * channels;
    for (uint32_t y = 0; y < h; ++y) {
        uint8_t* out = dst + y * row;
        const Tap ry = ty[y];
        if (ry.i < 0) {
            for (uint32_t x = 0; x < w; ++x)
                for (int c = 0; c < channels; ++c) out[x * channels + c] = bar[c];
            continue;
        }
        const uint8_t* r0 = src + size_t(ry.i) * row;
        const uint8_t* r1 = ry.f != 0 ? r0 + row : r0;
        for (uint32_t x = 0; x < w; ++x) {
            const Tap rx = tx[x];
            for (int c = 0; c < channels; ++c) {
                if (rx.i < 0) {
                    out[x * channels + c] = bar[c];
                    continue;
                }
                const size_t a = size_t(rx.i) * channels + c, b = rx.f != 0 ? a + channels : a;
                const uint32_t top = r0[a] * (256 - rx.f) + r0[b] * rx.f;
                const uint32_t bottom = r1[a] * (256 - rx.f) + r1[b] * rx.f;
                out[x * channels + c] = uint8_t((top * (256 - ry.f) + bottom * ry.f + (1u << 15)) >> 16);
            }
        }
    }
}
}  // namespace

bool widescreen_movie_frame(uint8_t* nv12, size_t bytes, uint32_t width, uint32_t height) {
    // rect `movie WxH,…`: a video plays; has it faded to white? Only a frame
    // that is white all over counts (light walls at the edge must not).
    if (nv12 != nullptr && width >= 2 && height >= 2 && bytes >= size_t(width) * height) {
        uint64_t sum = 0, n = 0;
        uint8_t low = 255;
        for (uint32_t y = 0; y < height; y += 16)
            for (uint32_t x = 0; x < width; x += 16, ++n) {
                const uint8_t s = nv12[size_t(y) * width + x];
                low = std::min(low, s);
                sum += s;
            }
        if (low >= kMovieWhiteY) {
            g_movie_white_y.store(uint32_t(sum / n), std::memory_order_relaxed);
            g_movie_white_run.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_movie_white_run.store(0, std::memory_order_relaxed);
        }
    }
    g_movie_frame_ns.store(now_ns(), std::memory_order_relaxed);
    // In the TV mode (`docked wide`) the video is stretched over the docked picture.
    PictureSize picture = widescreen_active();
    if (picture.width != 0 && g_docked.load(std::memory_order_relaxed)) picture = widescreen_docked_picture();
    if (picture.width == 0 || picture.height == 0 || width < 2 || height < 2) return false;
    bool fill = false, listed = false;
    {
        std::lock_guard lock(g_movie_mutex);
        for (const MovieRule& r : g_movie_rules)
            if (r.width == width && r.height == height) {
                fill = r.fill;
                listed = true;
            }
    }
    const uint32_t cw = (width + 1) / 2, ch = (height + 1) / 2;
    const size_t luma = size_t(width) * height, need = luma + size_t(cw) * ch * 2;
    if (!listed || nv12 == nullptr || bytes < need) return false;
    // The frame (aspect width/height) is stretched over the picture: its
    // content, scaled within the frame by sx, sy, covers sx * picture width by
    // sy * picture height and keeps its aspect where sx / sy = frame / screen.
    const double frame = double(width) / height, screen = double(picture.width) / picture.height;
    // Only a taller picture: in a wider one (iPhone) LGP draws its video into
    // the middle 1280x720 unstretched (PC, analysis 50).
    const double ratio = frame / screen;  // > 1: the screen is taller than the frame
    if (ratio < 1.01) return false;
    double sx = 1, sy = 1;
    if (fill)
        sx = ratio;  // cropped at the sides
    else
        sy = 1 / ratio;  // bars above and below
    // Bars are black, but white once the video has faded to white (its mean
    // luma), so the end of a video has no black bars around a white picture.
    uint8_t bar_y = 16;
    const uint8_t bar_uv[2] = {128, 128};
    if (movie_white()) bar_y = uint8_t(g_movie_white_y.load(std::memory_order_relaxed));
    const std::vector<uint8_t> src(nv12, nv12 + need);
    movie_plane(src.data(), nv12, width, height, 1, sx, sy, &bar_y);
    movie_plane(src.data() + luma, nv12 + luma, cw, ch, 2, sx, sy, bar_uv);
    if (!g_movie_logged.exchange(true))
        rt::log(rt::LogLevel::Info, "widescreen: movie %ux%u %s in the %ux%u picture (content %.3f x %.3f)", width,
                height, fill ? "fills" : "fits", picture.width, picture.height, sx, sy);
    return true;
}

namespace detail {
void set_widescreen_active(PictureSize size) {
    g_active.store(uint64_t(size.width) << 32 | size.height, std::memory_order_relaxed);
}

void stop_widescreen_data() {
    g_docked_wide = false;
    write_data_rules(false);
    std::lock_guard lock(g_data_mutex);
    g_data_rules.clear();
    g_data_memory = nullptr;
}
}  // namespace detail

PictureSize widescreen_size(double aspect, PictureSize base, PictureSize limit) {
    const double base_aspect = double(base.width) / double(base.height);
    if (!(aspect > 0) || std::abs(aspect / base_aspect - 1.0) < 0.01) return base;
    PictureSize out = base;
    if (aspect > base_aspect) {
        out.width = round16(base.height * aspect);
        if (out.width > limit.width) {
            out.width = limit.width;
            out.height = round16(limit.width / aspect);
        }
    } else {
        out.height = round16(base.width / aspect);
        if (out.height > limit.height) {
            out.height = limit.height;
            out.width = round16(limit.height * aspect);
        }
    }
    return out;
}

PictureSize widescreen_scale(PictureSize size, PictureSize from, PictureSize to) {
    if ((from.width == to.width && from.height == to.height) || size.width == 0 || size.height == 0 ||
        from.width == 0 || from.height == 0)
        return size;
    PictureSize a = from, b = to;
    for (int step = 0; step < 16; ++step) {
        if (a.width == size.width && a.height == size.height) return b;
        if (a.width == 1 && a.height == 1) break;
        a = {(a.width + 1) / 2, (a.height + 1) / 2};
        b = {(b.width + 1) / 2, (b.height + 1) / 2};
    }
    // A step of the new chain is already scaled (1280x800 at 16:10 is near 16:9).
    for (b = to; b.width > 1 || b.height > 1; b = {(b.width + 1) / 2, (b.height + 1) / 2})
        if (b.width == size.width && b.height == size.height) return size;
    // Larger than the picture (the 1920x1080 window texture): not made from it.
    if (size.width > from.width || size.height > from.height) return size;
    const double aspect = double(size.width) / size.height, from_aspect = double(from.width) / from.height,
                 to_aspect = double(to.width) / to.height;
    const double off_from = std::abs(aspect / from_aspect - 1.0), off_to = std::abs(aspect / to_aspect - 1.0);
    // Nearer the new aspect than the old one: already scaled (480x300 at 16:10).
    if (off_from >= 0.11 || off_to < off_from) return size;
    return {std::max<uint32_t>(1, uint32_t(std::lround(double(size.width) * to.width / from.width))),
            std::max<uint32_t>(1, uint32_t(std::lround(double(size.height) * to.height / from.height)))};
}

bool widescreen_projection(float* m, PictureSize from, PictureSize to, bool keep_height) {
    if (from.width == 0 || from.height == 0 || to.width == 0 || to.height == 0) return false;
    const double from_aspect = double(from.width) / from.height, to_aspect = double(to.width) / to.height;
    const double m00 = std::abs(double(m[0])), m11 = std::abs(double(m[5]));  // y is flipped in 2D (top-left origin)
    if (!(m00 > 0 && m11 > 0) || std::abs(to_aspect / from_aspect - 1.0) < 1e-6) return false;
    if (std::abs(m11 / m00 / from_aspect - 1.0) >= 0.01) return false;
    // The whole x (y) row scales, so clip x (y) shrinks around the centre: with a
    // 2D orthographic projection from the corner (translation -1), the picture
    // stays centred. The row is m[0..3] with rows, m[0,4,8,12] with columns. In a
    // plain projection the other layout's entries in the union are 0, so both go;
    // a rotated 2D matrix (m[1], m[4] set) needs its layout, read from where its
    // translation sits, or it is left as it is.
    static constexpr int kRowX[] = {0, 1, 2, 3, 4, 8, 12}, kRowY[] = {1, 4, 5, 6, 7, 9, 13};
    static constexpr int kRowsX[] = {0, 1, 2, 3}, kRowsY[] = {4, 5, 6, 7};
    static constexpr int kColumnsX[] = {0, 4, 8, 12}, kColumnsY[] = {1, 5, 9, 13};
    const bool by_rows = m[12] == 0 && m[13] == 0 && (m[3] != 0 || m[7] != 0),
               by_columns = m[3] == 0 && m[7] == 0 && (m[12] != 0 || m[13] != 0);
    const bool rotated = m[1] != 0 || m[4] != 0;
    if (rotated && by_rows == by_columns) return false;
    // keep_height: the x row as for a wider picture, so a taller one keeps the
    // game's vertical view and loses the sides (Hor-).
    const bool wider = to_aspect > from_aspect || keep_height;
    const double k = wider ? m11 / (to_aspect * m00) : to_aspect * m00 / m11;
    const auto scale = [&](const auto& row) {
        for (int i : row) m[i] = float(m[i] * k);
    };
    if (!rotated)
        wider ? scale(kRowX) : scale(kRowY);
    else if (by_rows)
        wider ? scale(kRowsX) : scale(kRowsY);
    else
        wider ? scale(kColumnsX) : scale(kColumnsY);
    return true;
}

bool widescreen_rect(float* r, PictureSize from, PictureSize to) {
    bool changed = false;
    if (to.width != from.width && from.width != 0 && std::abs(r[0] - float(from.width)) < 0.5f) {
        const float s = float(to.width) / float(from.width);
        r[0] *= s;
        r[2] *= s;
        changed = true;
    }
    if (to.height != from.height && from.height != 0 && std::abs(r[1] - float(from.height)) < 0.5f) {
        const float s = float(to.height) / float(from.height);
        r[1] *= s;
        r[3] *= s;
        changed = true;
    }
    return changed;
}

bool widescreen_element(const float* r, float* model, PictureSize from, PictureSize to) {
    // Screen extent of the rectangle's corners through the model's 2x2 part and translation.
    float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
    for (float lx : {r[2], r[2] + r[0]})
        for (float ly : {r[3], r[3] - r[1]})
            for (int row = 0; row < 2; ++row) {
                const float* m = model + 4 * row;
                const float v = m[0] * lx + m[1] * ly + m[3];
                lo[row] = std::min(lo[row], v);
                hi[row] = std::max(hi[row], v);
            }
    const auto covers = [&](int row, float half) { return lo[row] <= -half + 1 && hi[row] >= half - 1; };
    bool changed = false;
    const uint32_t f[2] = {from.width, from.height}, t[2] = {to.width, to.height};
    for (int row = 0; row < 2; ++row) {
        if (t[row] == f[row] || f[row] == 0 || !covers(row, f[row] / 2.f) || covers(row, t[row] / 2.f)) continue;
        const float s = float(t[row]) / float(f[row]);
        for (int i = 0; i < 4; ++i) model[4 * row + i] *= s;
        changed = true;
    }
    return changed;
}

bool widescreen_texture(const float* r, const float* model, float* tex, PictureSize from, PictureSize to) {
    float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
    for (float lx : {r[2], r[2] + r[0]})
        for (float ly : {r[3], r[3] - r[1]})
            for (int row = 0; row < 2; ++row) {
                const float* m = model + 4 * row;
                const float v = m[0] * lx + m[1] * ly + m[3];
                lo[row] = std::min(lo[row], v);
                hi[row] = std::max(hi[row], v);
            }
    bool changed = false;
    const uint32_t f[2] = {from.width, from.height}, t[2] = {to.width, to.height};
    for (int row = 0; row < 2; ++row) {
        const float half = t[row] / 2.f;
        // Only an element that spans exactly the new size (one this mod widened)
        // and is not turned: screen x is texture u, screen y texture v.
        if (t[row] == f[row] || f[row] == 0 || std::abs(lo[row] + half) > 1 || std::abs(hi[row] - half) > 1 ||
            std::abs(model[4 * row + 1 - row]) > 1e-3f)
            continue;
        const float s = float(t[row]) / float(f[row]);
        float* q = tex + 4 * row;  // q[0] * ux + q[1] * uy + q[3], corners ux, uy in 0..1
        const float centre = q[0] * 0.5f + q[1] * 0.5f + q[3];
        q[0] *= s;
        q[1] *= s;
        q[3] = s * q[3] + (1 - s) * centre;
        changed = true;
    }
    return changed;
}

namespace {
// How widescreen_edge moves an element per axis: screen x' = a * x + b, quad
// corner c (0..1) samples the texture where a * c + k was.
constexpr float kEdgeOverhang = 32;  // layout units

// Whether `edge`/`stretch` take an element whose outer side lies `side` units
// from the middle along row (0 x, 1 y), and how far they pull that side
// (*grow): an element on the old edge or up to `over` beyond it goes the
// extra half size, keeping its overhang. Below 100 % HUD bounds the `layouts`
// line moved units by m first: an element of such a unit then lies on the
// old edge + m (up to `over` beyond) and goes to the new edge, keeping its
// overhang beyond the moved line; a move by no more than the overhang band
// counts as that band, not as overhang (else the side ended past the new edge).
// upper: the top (x: right) side. With `hud L,…` the line moves those layouts
// by their own amount per side, so both moves count.
bool edge_band(int row, float side, bool upper, float old_half, float new_half, float over, float* grow) {
    const Layouts& l = layouts();
    float moves[2] = {0, 0};
    if (l.on && (l.options.axis != 0 ? 1 : 0) == row) {
        moves[0] = l.options.shift;
        if (!l.options.hud.empty()) moves[1] = l.hud_now[upper ? 0 : 1].load(std::memory_order_relaxed);
    }
    float small = 0;  // a move within the overhang band
    for (const float moved : moves) {
        if (!(moved > 0)) continue;
        if (moved <= kEdgeOverhang + 1) small = std::max(small, moved);
        if (side >= old_half + moved - 1 && side <= old_half + moved + over) {
            *grow = new_half - old_half - moved;
            return *grow >= 0.5f;  // at the whole extra already at the new edge
        }
    }
    if (side < old_half - 1 || side > old_half + over) return false;
    *grow = small > 0 ? new_half + std::max(0.0f, side - old_half - small) - side : new_half - old_half;
    return true;
}

struct EdgeStep {
    bool axis[2] = {false, false};
    float a[2] = {1, 1}, b[2] = {0, 0}, k[2] = {0, 0};
};

bool edge_step(const float* r, const float* model, PictureSize from, PictureSize to, EdgeStep* e) {
    float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
    for (float lx : {r[2], r[2] + r[0]})
        for (float ly : {r[3], r[3] - r[1]})
            for (int row = 0; row < 2; ++row) {
                const float* m = model + 4 * row;
                const float v = m[0] * lx + m[1] * ly + m[3];
                lo[row] = std::min(lo[row], v);
                hi[row] = std::max(hi[row], v);
            }
    bool any = false;
    const uint32_t f[2] = {from.width, from.height}, t[2] = {to.width, to.height};
    for (int row = 0; row < 2; ++row) {
        const float* m = model + 4 * row;
        if (t[row] <= f[row] || std::abs(m[1 - row]) > 1e-3f || !(std::abs(m[row]) > 1e-6f)) continue;
        const float old_half = f[row] / 2.f, new_half = t[row] / 2.f;
        // At an edge: on it or reaching up to kEdgeOverhang beyond (the header bar starts at -645).
        float grow_lo = 0, grow_hi = 0;
        const bool at_lo = edge_band(row, -lo[row], false, old_half, new_half, kEdgeOverhang, &grow_lo);
        const bool at_hi = edge_band(row, hi[row], true, old_half, new_half, kEdgeOverhang, &grow_hi);
        if (at_lo == at_hi) continue;  // neither edge, or spanning (widescreen_element's case)
        const float fixed = at_lo ? hi[row] : lo[row], from_x = at_lo ? lo[row] : hi[row];
        const float grow = at_lo ? grow_lo : grow_hi;
        const float to_x = at_lo ? from_x - grow : from_x + grow;
        const float a = (fixed - to_x) / (fixed - from_x), b = fixed - a * fixed;
        // Quad corner c along this axis: local x = r[2] + c * r[0], local y = r[3] - c * r[1].
        const float x0 = row == 0 ? m[0] * r[2] + m[3] : m[1] * r[3] + m[3];
        const float dx = row == 0 ? m[0] * r[0] : -m[1] * r[1];
        if (dx == 0) continue;
        e->axis[row] = true;
        e->a[row] = a;
        e->b[row] = b;
        e->k[row] = ((a - 1) * x0 + b) / dx;
        any = true;
    }
    return any;
}

void edge_model(const EdgeStep& e, float* model) {
    for (int row = 0; row < 2; ++row) {
        if (!e.axis[row]) continue;
        float* m = model + 4 * row;
        for (int i = 0; i < 4; ++i) m[i] *= e.a[row];
        m[3] += e.b[row];
    }
}

void edge_texture(const EdgeStep& e, float* tex) {
    for (int row = 0; row < 2; ++row) {
        if (!e.axis[row]) continue;
        for (int q = 0; q < 2; ++q) {
            float* t = tex + 4 * q;
            t[3] += t[row] * e.k[row];
            t[row] *= e.a[row];
        }
    }
}
}  // namespace

bool widescreen_widen(float* model, PictureSize from, PictureSize to, bool only_x) {
    bool changed = false;
    const uint32_t f[2] = {from.width, from.height}, t[2] = {to.width, to.height};
    for (int row = 0; row < (only_x ? 1 : 2); ++row) {
        if (t[row] <= f[row] || f[row] == 0) continue;
        const float s = float(t[row]) / float(f[row]);
        for (int i = 0; i < 4; ++i) model[4 * row + i] *= s;
        changed = true;
    }
    return changed;
}

namespace {
// Screen extent of an element: lo/hi per row (0 x, 1 y).
void extent(const float* r, const float* model, float lo[2], float hi[2]) {
    lo[0] = lo[1] = 1e30f;
    hi[0] = hi[1] = -1e30f;
    for (float lx : {r[2], r[2] + r[0]})
        for (float ly : {r[3], r[3] - r[1]})
            for (int row = 0; row < 2; ++row) {
                const float* m = model + 4 * row;
                const float v = m[0] * lx + m[1] * ly + m[3];
                lo[row] = std::min(lo[row], v);
                hi[row] = std::max(hi[row], v);
            }
}
// Wholly at |coordinate| >= side across axis a (the other one); side 0: anywhere.
bool beside(const float lo[2], const float hi[2], float side, int a = 1) {
    const int o = 1 - a;
    return side <= 0 || lo[o] >= side - 0.5f || hi[o] <= -side + 0.5f;
}
uint32_t along(PictureSize p, int a) { return a == 0 ? p.width : p.height; }

// widescreen_zone/widescreen_xzone along axis a (0 x, 1 y): `top` and `bottom`
// are the far and near thresholds (right and left for x).
bool zone_axis(const float* r, float* model, float top, float bottom, PictureSize from, PictureSize to, float side,
               bool member, int a) {
    if (along(to, a) <= along(from, a)) return false;
    float lo[2], hi[2];
    extent(r, model, lo, hi);
    if (!beside(lo, hi, side, a)) return false;
    const float shift = (float(along(to, a)) - float(along(from, a))) / 2.f * hud(),
                old_half = float(along(from, a)) / 2.f;
    float& t = model[a == 0 ? 3 : 7];
    if (member) {
        if (hi[a] <= -old_half || lo[a] >= old_half) return false;
        t += lo[a] + hi[a] >= 0 ? shift : -shift;
        return true;
    }
    if (lo[a] >= top - 0.5f && lo[a] < old_half) {  // only what showed on the old picture
        t += shift;
        return true;
    }
    if (hi[a] <= bottom + 0.5f && hi[a] > -old_half) {
        t -= shift;
        return true;
    }
    return false;
}

bool spread_axis(const float* r, float* model, float top, float bottom, PictureSize from, PictureSize to, float side,
                 int a) {
    if (along(to, a) <= along(from, a) || along(from, a) == 0) return false;
    float lo[2], hi[2];
    extent(r, model, lo, hi);
    const int o = 1 - a;
    if (hi[o] - lo[o] >= float(along(from, o)) - 1 || !beside(lo, hi, side, a)) return false;
    if (!(lo[a] > bottom && hi[a] < top)) return false;
    const float d = (lo[a] + hi[a]) / 2 * (float(along(to, a)) / float(along(from, a)) - 1) * hud();
    if (std::abs(d) < 0.01f) return false;
    model[a == 0 ? 3 : 7] += d;
    return true;
}

// From `top` up (and `bottom` down) a 2D matrix moves to the new edge; between them `spread` scales its place.
bool clip_zone_axis(float* m, float top, float bottom, PictureSize from, PictureSize to, float side, bool spread,
                    int a) {
    if (along(to, a) <= along(from, a)) return false;
    const bool rows = m[3] != 0 || m[7] != 0, columns = m[12] != 0 || m[13] != 0;
    if (rows == columns) return false;  // no translation (or both: not a 2D matrix)
    float* tr = rows ? &m[3] : &m[12];  // x; y is 4 (rows) or 1 (columns) further
    const int step = rows ? 4 : 1;
    float& t = tr[a == 0 ? 0 : step];
    const float across = tr[a == 0 ? step : 0];
    const int o = 1 - a;
    if (side > 0 && std::abs(across * float(along(to, o)) / 2.f) < side) return false;
    const float half = float(along(to, a)) / 2.f, layout = t * half;
    if (std::abs(layout) > float(along(from, a)) / 2.f + 1) return false;
    if (layout >= top || layout <= bottom) {
        t += std::copysign((float(along(to, a)) - float(along(from, a))) / 2.f * hud() / half,
                           layout >= top ? 1.f : -1.f);
        return true;
    }
    if (!spread || std::abs(layout) < 0.01f) return false;
    t *= 1 + (float(along(to, a)) / float(along(from, a)) - 1) * hud();
    return true;
}
}  // namespace

bool widescreen_zone(const float* r, float* model, float top, float bottom, PictureSize from, PictureSize to,
                     float side, bool member) {
    return zone_axis(r, model, top, bottom, from, to, side, member, 1);
}
bool widescreen_xzone(const float* r, float* model, float right, float left, PictureSize from, PictureSize to,
                      float side, bool member) {
    return zone_axis(r, model, right, left, from, to, side, member, 0);
}

bool widescreen_spread(const float* r, float* model, float top, float bottom, PictureSize from, PictureSize to,
                       float side) {
    return spread_axis(r, model, top, bottom, from, to, side, 1);
}
bool widescreen_xspread(const float* r, float* model, float right, float left, PictureSize from, PictureSize to,
                        float side) {
    return spread_axis(r, model, right, left, from, to, side, 0);
}

bool widescreen_span(const float* r, float* model, PictureSize from, PictureSize to) {
    if (to.height > from.height && from.height != 0) {  // taller: like widen, the bands keep their share of the height
        const float s = float(to.height) / float(from.height);
        for (int i = 4; i < 8; ++i) model[i] *= s;
        return true;
    }
    if (to.width <= from.width || std::abs(model[1]) > 1e-3f || !(std::abs(model[0]) > 1e-6f)) return false;
    float lo = 1e30f, hi = -1e30f;
    for (float lx : {r[2], r[2] + r[0]}) {
        const float x = model[0] * lx + model[3];
        lo = std::min(lo, x);
        hi = std::max(hi, x);
    }
    const float half = float(to.width) / 2.f;
    if (!(hi - lo > 1) || (std::abs(lo + half) < 0.5f && std::abs(hi - half) < 0.5f)) return false;
    const float a = 2 * half / (hi - lo), b = -half - a * lo;
    for (int i = 0; i < 4; ++i) model[i] *= a;
    model[3] += b;
    return true;
}

bool widescreen_clip_zone(float* m, float y, PictureSize from, PictureSize to, float side, bool spread) {
    return clip_zone_axis(m, y, -y, from, to, side, spread, 1);
}
bool widescreen_clip_xzone(float* m, float x, PictureSize from, PictureSize to, float side, bool spread) {
    return clip_zone_axis(m, x, -x, from, to, side, spread, 0);
}

bool widescreen_edge(const float* r, float* model, float* tex, PictureSize from, PictureSize to) {
    EdgeStep e;
    if (!edge_step(r, model, from, to, &e)) return false;
    edge_model(e, model);
    if (tex != nullptr) edge_texture(e, tex);
    return true;
}

bool widescreen_stretch(const float* r, float* model, PictureSize from, PictureSize to, bool only_x) {
    const float lmin[2] = {r[2], r[3] - r[1]}, lmax[2] = {r[2] + r[0], r[3]}, len[2] = {r[0], r[1]};
    const uint32_t f[2] = {from.width, from.height}, t[2] = {to.width, to.height};
    bool changed = false;
    for (int row = 0; row < 2; ++row) {
        if (t[row] <= f[row] || (only_x && row == 1)) continue;
        float* m = model + 4 * row;
        const int c = std::abs(m[0]) >= std::abs(m[1]) ? 0 : 1;  // local axis most nearly across this edge
        const float d = m[c];
        if (!(std::abs(d) > 1e-6f) || !(len[c] > 0)) continue;
        float lo = 1e30f, hi = -1e30f;
        for (float lx : {lmin[0], lmax[0]})
            for (float ly : {lmin[1], lmax[1]}) {
                const float v = m[0] * lx + m[1] * ly + m[3];
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
        const float old_half = f[row] / 2.f, shift = (t[row] - f[row]) / 2.f;
        const float over = kEdgeOverhang + std::abs(m[1 - c]) * len[1 - c];
        float grow_lo = 0, grow_hi = 0;
        const bool at_lo = edge_band(row, -lo, false, old_half, old_half + shift, over, &grow_lo);
        const bool at_hi = edge_band(row, hi, true, old_half, old_half + shift, over, &grow_hi);
        if (at_lo == at_hi) continue;
        // The side that moves outwards is where d * l is largest (at_hi) or smallest (at_lo).
        const bool grow_max = (d > 0) == at_hi;
        const float anchor = grow_max ? lmin[c] : lmax[c];
        const float grow = at_hi ? grow_hi : grow_lo;
        const float s = (len[c] + grow / std::abs(d)) / len[c];
        for (int k = 0; k < 2; ++k) {
            float* mk = model + 4 * k;
            mk[3] += mk[c] * (1 - s) * anchor;
            mk[c] *= s;
        }
        changed = true;
    }
    return changed;
}

namespace detail {

namespace {

constexpr PictureSize kDocked{1920, 1080};
constexpr PictureSize kHandheld{1280, 720};

// While install evaluates a line's value for handheld: dw/dh are the game's.
thread_local bool t_game_docked = false;

// dw/dh: the game's docked render size, with `docked wide 1` the docked picture.
PictureSize docked_render(PictureSize handheld) {
    return g_docked_wide.load(std::memory_order_relaxed) && !t_game_docked ? docked_of(handheld) : kDocked;
}

// A `patch <name> <va> LO [HI]` line naming dw or dh with `docked wide`: the
// docked picture only while docked, else the game's 1920x1080, so handheld
// runs stay as without the line (PC 01.11.: the boot builds the docked size
// in either mode, and a larger one there changed loading and timing).
struct ModeImmediate {
    unsigned rd = 0;
    bool sf = false;
    uint64_t game = 0, ours = 0;
    std::string name;
    mutable std::atomic<bool> logged{false};
};
std::vector<std::unique_ptr<ModeImmediate>>& mode_immediates() {
    static std::vector<std::unique_ptr<ModeImmediate>> v;
    return v;
}
void mode_immediate_patch(CpuState* ctx, void* user) {
    const ModeImmediate& m = *static_cast<const ModeImmediate*>(user);
    const bool docked = g_docked.load(std::memory_order_relaxed);
    const uint64_t v = docked ? m.ours : m.game;
    if (docked && !m.logged.exchange(true, std::memory_order_relaxed))
        rt::log(rt::LogLevel::Info, "widescreen: %s yields 0x%llx while docked", m.name.c_str(),
                static_cast<unsigned long long>(v));
    if (m.rd != 31) ctx->x[m.rd] = m.sf ? v : v & 0xFFFF'FFFFu;
}

// `size` patch: where width and height are, and an optional condition.
struct Operand {
    int reg = 0;
    bool wide = false;    // x register (else w: writes zero-extend)
    bool memory = false;  // u32 at [x<reg> + offset]
    bool is_float = false;  // [..]f: a float there (viewport rectangles)
    uint64_t offset = 0;
};
struct SizeRule {
    std::string name;
    bool projection = false;  // `proj` line: w is the matrix address
    bool proj_keep = false;   // `proj … keep`: the projection stays (wide already, `data` bounds); zones/cull only
    bool proj_matrix = true;  // false: a `rect` line alone
    bool has_rect = false;    // `rect [xN+OFF]`: 2D element rectangle w, h, x, y (floats)
    Operand rect;
    bool has_model = false;   // `rect … model [xN+OFF]`: the element's 3x4 model matrix (rows)
    Operand model;
    bool has_tex = false;     // `tex [xN+OFF] rect … model …`: texture matrix rows u, v (2x4 floats)
    Operand tex;
    std::vector<std::pair<float, float>> tex_only;  // `only WxH,…`: element rectangles whose texture widens
    mutable std::atomic<uint32_t> skipped{0};      // log lines for listed elements that did not qualify
    bool edge_all = false;                         // `edge all`: every element at one 16:9 edge
    std::vector<std::pair<float, float>> edge_only;  // `edge WxH,…`: element rectangles that reach the edge
    std::vector<std::pair<float, float>> stretch_only;  // `stretch WxH,…`: like edge, texels stretched along
    std::vector<std::pair<float, float>> xstretch_only;  // `xstretch WxH,…`: the same along x only (wider pictures)
    std::vector<std::pair<float, float>> span_only;   // `span WxH,…`: across the whole new width
    std::vector<size_t> span_except;                  // `span … except WxH,…`: taller: not while these are drawn
    std::vector<std::pair<float, float>> widen_only;  // `widen WxH,…`: scaled like the picture, centred
    std::vector<std::pair<float, float>> xwiden_only;  // `xwiden WxH,…`: the same along x only (wider pictures)
    std::vector<size_t> xwiden_when;                   // `xwiden … when WxH,…`: only while these are drawn
    std::vector<std::pair<float, float>> movie_keep;  // `movie WxH,…`: not widened while a video plays
    std::vector<std::pair<float, float>> fill_only;   // `fill WxH,…`: at the HUD bounds line, out to the edge
    // `cull WxH,… [below Y] [above Y] [when WxH,…]` (repeatable): not drawn when
    // entirely above or below the old view (or the given limits), only while
    // one of the `when` elements is drawn if any are named.
    struct Cull {
        std::vector<std::pair<float, float>> sizes;
        float below = -360, above = 360;
        std::vector<size_t> when;  // crop_gates()
    };
    std::vector<Cull> culls;
    mutable std::atomic<uint32_t> cull_lines{0};
    // `zone TOP BOTTOM [side X] [spread] [when WxH,…]` (proj line: `zone Y …`, bottom = -Y), in order;
    // `xzone RIGHT LEFT …` (proj line: `xzone X …`) the same along x for wider pictures (side: |y|).
    struct Zone {
        int axis = 1;  // 1: zone (y, taller pictures), 0: xzone (x, wider pictures)
        float top = 0, bottom = 0, side = 0;
        bool spread = false;
        bool inside = false;  // proj line: only text on an element a `zone` moved that way (still drawn)
        bool follow = false;  // proj line: text on a moved element moves as far as it did (implies inside)
        std::vector<size_t> when;  // gates
        std::vector<std::pair<float, float>> with;  // `with WxH,…`: members whatever their extent
    };
    std::vector<Zone> zones;
    std::vector<size_t> crop_when;  // proj line `crop WxH,…`: Hor- while one of these elements is drawn (crop gates)
    int64_t crop_hold_ns = 0;       // `crop … hold MS`: open that long after one passed (at least the gate time)
    int64_t crop_ease_ns = 1'000'000'000;  // `crop … ease MS`: back from Hor- over that long (the switch to Hor- is at once)
    std::vector<size_t> crop_except;  // `crop … except WxH,…`: shut at once while one of these is drawn (crop gates)
    mutable std::atomic<uint32_t> crop_lines{0};
    struct TextCull {  // proj line `cull …`, new picture units
        bool beyond = false;  // `below`/`above` given: y at or beyond one of them
        float below = -1e30f, above = 1e30f;
        float x0 = -1e30f, x1 = 1e30f, y0 = -1e30f, y1 = 1e30f;  // `x MIN MAX`, `y MIN MAX`: inside
        int shape = 0;  // 1: `taller` pictures only, 2: `wider` only
        std::vector<size_t> when;  // crop_gates()
    };
    std::vector<TextCull> text_culls;
    mutable std::atomic<uint32_t> text_cull_lines{0};
    uint32_t log_elements = 0;                     // `log N`: first N distinct elements with their screen extent
    mutable std::atomic<uint32_t> logged{0};
    mutable std::atomic<uint32_t> edge_lines{0};
    mutable std::atomic<uint32_t> lines{0};  // log lines so far
    mutable std::atomic<uint32_t> movie_lines{0};  // `movie WxH,…` passes
    Operand w, h;
    bool has_cond = false;
    Operand cond;
    uint64_t cond_mask = 0;
    // `size … skip WxH[@F],…`: these sizes stay; with @F only while the rule's
    // `format` operand holds F (the description's own format code, e.g. a video's luma).
    struct Skip {
        float w = 0, h = 0;
        int64_t format = -1;  // -1: any format
    };
    bool has_format = false;
    Operand format;
    std::vector<Skip> skip_sizes;
    // While docked (`docked wide`): `skip … docked WxH,…|none`, by default the
    // skip sizes times 1.5 (the same offscreen picture in the 1920x1080 family).
    std::vector<Skip> docked_skip_sizes;
    PictureSize to;
};
std::vector<std::unique_ptr<SizeRule>>& size_rules() {
    static std::vector<std::unique_ptr<SizeRule>> v;
    return v;
}
constexpr uint32_t kSizeLogLines = 16;  // per rule
// Fields the rule wrote: (address of the width, width, height). A caller
// that hands the same description in again (the PC saw 111 passes for 35
// textures) must not get it scaled twice.
std::mutex g_written_mutex;
std::set<std::tuple<uint64_t, uint32_t, uint32_t>> g_written;

bool parse_u64(std::string_view word, uint64_t* out) {
    const std::string s(word);
    char* end = nullptr;
    *out = std::strtoull(s.c_str(), &end, 0);
    return !s.empty() && s[0] != '-' && end == s.c_str() + s.size();
}
bool parse_reg(std::string_view word, int* reg, bool* wide) {
    if (word.size() < 2 || (word[0] != 'w' && word[0] != 'x')) return false;
    int n = 0;
    for (char ch : word.substr(1)) {
        if (ch < '0' || ch > '9') return false;
        n = n * 10 + (ch - '0');
    }
    if (n > 30) return false;
    *reg = n;
    *wide = word[0] == 'x';
    return true;
}
// wN, xN or [xN+OFF] (also [xN])
bool parse_operand(std::string_view word, Operand* o) {
    if (word.size() > 3 && word.front() == '[' && word.substr(word.size() - 2) == "]f") {
        o->is_float = true;
        word.remove_suffix(1);
    }
    if (word.size() > 2 && word.front() == '[' && word.back() == ']') {
        word = word.substr(1, word.size() - 2);
        const size_t plus = word.find('+');
        o->memory = true;
        if (plus != std::string_view::npos && !parse_u64(word.substr(plus + 1), &o->offset)) return false;
        return parse_reg(word.substr(0, plus), &o->reg, &o->wide) && o->wide;  // an address is an x register
    }
    return parse_reg(word, &o->reg, &o->wide);
}

// "1280x360,1280x606" -> {(1280, 360), (1280, 606)}
bool parse_sizes(std::string_view list, std::vector<std::pair<float, float>>* out) {
    while (!list.empty()) {
        const size_t comma = list.find(',');
        const std::string_view one = list.substr(0, comma);
        const size_t x = one.find('x');
        uint64_t w = 0, h = 0;
        if (x == std::string_view::npos || !parse_u64(one.substr(0, x), &w) || !parse_u64(one.substr(x + 1), &h) ||
            w == 0 || h == 0)
            return false;
        out->push_back({float(w), float(h)});
        list = comma == std::string_view::npos ? std::string_view() : list.substr(comma + 1);
    }
    return !out->empty();
}

// `WxH[@N],…` for the skip lists of `size` rules.
bool parse_skips(std::string_view list, std::vector<SizeRule::Skip>* out) {
    while (!list.empty()) {
        const size_t comma = list.find(',');
        std::string_view one = list.substr(0, comma);
        SizeRule::Skip k;
        if (const size_t at = one.find('@'); at != std::string_view::npos) {
            uint64_t f = 0;
            if (!parse_u64(one.substr(at + 1), &f) || f > 0xFFFF'FFFFu) return false;
            k.format = int64_t(f);
            one = one.substr(0, at);
        }
        std::vector<std::pair<float, float>> size;
        if (!parse_sizes(one, &size) || size.size() != 1) return false;
        k.w = size[0].first;
        k.h = size[0].second;
        out->push_back(k);
        list = comma == std::string_view::npos ? std::string_view() : list.substr(comma + 1);
    }
    return !out->empty();
}

bool read_u32(uint64_t va, uint32_t* out) {
    if (!rt::access_allowed(va, 4, rt::kPageRead)) return false;
    *out = rt::load<uint32_t>(va);
    return true;
}
bool get(const CpuState* ctx, const Operand& o, uint64_t* out) {
    if (!o.memory) {
        *out = ctx->x[o.reg];
        return true;
    }
    uint32_t v = 0;
    if (!read_u32(ctx->x[o.reg] + o.offset, &v)) return false;
    if (o.is_float) {
        float f = 0;
        std::memcpy(&f, &v, 4);
        if (!(f >= 0 && f < 65536) || f != std::floor(f)) return false;  // sizes are whole pixels
        v = uint32_t(f);
    }
    *out = v;
    return true;
}
void set(CpuState* ctx, const Operand& o, uint32_t v) {
    if (!o.memory) {
        ctx->x[o.reg] = o.wide ? (ctx->x[o.reg] & ~uint64_t(0xFFFF'FFFF)) | v : v;
        return;
    }
    const uint64_t va = ctx->x[o.reg] + o.offset;
    if (!rt::access_allowed(va, 4, rt::kPageWrite)) return;
    if (o.is_float) {
        const float f = float(v);
        std::memcpy(&v, &f, 4);
    }
    std::memcpy(rt::translate<true>(va, 4), &v, 4);
    rt::note_write<false>(va, 4);
}

// `proj`: the 4x4 float projection at [xN+OFF] (both floats checked and
// written in guest memory).
void texture_rows(CpuState* ctx, const SizeRule& r);

// Texture rows the `tex` rule wrote, by address: a buffer handed in again
// unchanged must not repeat at a wider step.
std::mutex g_tex_mutex;
std::map<uint64_t, std::array<float, 8>> g_tex_written;

bool listed(const std::vector<std::pair<float, float>>& sizes, const float* v) {
    return std::any_of(sizes.begin(), sizes.end(), [&](const std::pair<float, float>& wh) {
        return std::abs(v[0] - wh.first) < 0.5f && std::abs(v[1] - wh.second) < 0.5f;
    });
}

// `zone`, `edge`, `widen`: what the rule did to an element's model, by model
// address. The game keeps the model while it writes the texture rows anew
// each frame, so later passes recognise the model they wrote and move the
// texture again (edge) instead of moving the element twice.
struct MoveMemo {
    std::array<float, 8> model;
    EdgeStep step;  // texture rows (edge only)
};
std::mutex g_move_mutex;
std::map<uint64_t, MoveMemo> g_moved;

// Records an element for the gates whose rectangle it has.
void gate_opened();
void see_gates(const float* v) {
    const int64_t now = now_ns(), window = zone_gate_ns();
    bool opened = false;
    for (Gate& g : gates())
        if (std::abs(v[0] - g.w) < 0.5f && std::abs(v[1] - g.h) < 0.5f)
            opened = now - g.seen_ns.exchange(now, std::memory_order_relaxed) >= window || opened;
    static std::atomic<uint32_t> crop_logs{0};
    for (Gate& g : crop_gates())
        if (std::abs(v[0] - g.w) < 0.5f && std::abs(v[1] - g.h) < 0.5f &&
            now - g.seen_ns.exchange(now, std::memory_order_relaxed) >= window &&
            crop_logs.fetch_add(1, std::memory_order_relaxed) < 16)
            rt::log(rt::LogLevel::Info, "widescreen: crop element %gx%g drawn (gate opens)", double(g.w), double(g.h));
    if (opened) gate_opened();
}
size_t gate_index(float w, float h, std::deque<Gate>& list = gates()) {
    for (size_t i = 0; i < list.size(); ++i)
        if (list[i].w == w && list[i].h == h) return i;
    list.emplace_back();
    list.back().w = w;
    list.back().h = h;
    return list.size() - 1;
}
// `crop`: per camera matrix the game's own one and the mode it was written in,
// so a matrix the game keeps (and does not build again) switches when the gate does.
struct CropMemo {
    std::array<float, 16> game, written;
    float t = 0;          // 1: Hor- (crop), 0: as without crop; eases back to 0
    int64_t at_ns = 0;    // when t was set
};
std::mutex g_crop_mutex;
std::map<uint64_t, CropMemo> g_crop;
bool crop_open(const std::vector<size_t>& when, int64_t hold_ns) {
    const int64_t now = now_ns(), window = std::max(zone_gate_ns(), hold_ns);
    return std::any_of(when.begin(), when.end(), [&](size_t i) {
        return now - crop_gates()[i].seen_ns.load(std::memory_order_relaxed) < window;
    });
}

// `log N`: elements as they pass, once per rectangle and screen extent.
std::mutex g_seen_mutex;
std::set<std::array<int, 6>> g_seen;

void log_element(const SizeRule& r, const float* v, const float* m) {
    if (!log_open()) return;
    float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
    for (float lx : {v[2], v[2] + v[0]})
        for (float ly : {v[3], v[3] - v[1]})
            for (int row = 0; row < 2; ++row) {
                const float s = m[4 * row] * lx + m[4 * row + 1] * ly + m[4 * row + 3];
                lo[row] = std::min(lo[row], s);
                hi[row] = std::max(hi[row], s);
            }
    const std::array<int, 6> key = {int(std::lround(v[0])), int(std::lround(v[1])), int(std::lround(lo[0])),
                                    int(std::lround(hi[0])), int(std::lround(lo[1])), int(std::lround(hi[1]))};
    {
        std::lock_guard lock(g_seen_mutex);
        if (!g_seen.insert(key).second) return;
    }
    if (r.logged.fetch_add(1, std::memory_order_relaxed) >= r.log_elements) return;
    rt::log(rt::LogLevel::Info, "widescreen: %s element %gx%g on screen x %g..%g y %g..%g model (%g %g | %g %g)",
            r.name.c_str(), double(v[0]), double(v[1]), double(lo[0]), double(hi[0]), double(lo[1]),
            double(hi[1]), double(m[0]), double(m[1]), double(m[4]), double(m[5]));
}

// Elements a `zone` moved, by model address: their old extent (layout units),
// which way they went and when they last passed. `zone … inside` on a proj
// line moves text only onto one of these, so text follows its own box.
struct MovedBox {
    float lo[2], hi[2];
    float new_lo[2], new_hi[2];  // where it is now
    int axis = 1;  // which way it went: along axis (0 x, 1 y) by d
    float d = 0;
    std::atomic<int64_t> seen_ns{0};
};
std::mutex g_box_mutex;
std::map<uint64_t, std::unique_ptr<MovedBox>> g_boxes;

void element_extent(const float* v, const float* m, float* lo, float* hi) {
    lo[0] = lo[1] = 1e30f;
    hi[0] = hi[1] = -1e30f;
    for (float lx : {v[2], v[2] + v[0]})
        for (float ly : {v[3], v[3] - v[1]})
            for (int row = 0; row < 2; ++row) {
                const float s = m[4 * row] * lx + m[4 * row + 1] * ly + m[4 * row + 3];
                lo[row] = std::min(lo[row], s);
                hi[row] = std::max(hi[row], s);
            }
}

// `fill WxH,…`: below 100 % HUD bounds header and footer groups (zones and
// the layout shift) stop short of the picture's edge. An element of these
// rectangles whose outer side lies on that line (a footer's white bar, a text
// band) goes on out to the edge; its inner side stays with the group (texels
// stretched along y, or x on wider pictures).
bool fill_to_edge(const float* v, float* m, PictureSize to) {
    for (int a : {1, 0}) {
        if (along(to, a) <= along(kHandheld, a)) continue;
        const float old_half = float(along(kHandheld, a)) / 2.f, half = float(along(to, a)) / 2.f,
                    line = old_half + (half - old_half) * hud();
        if (line >= half - 0.5f) continue;
        float lo[2], hi[2];
        element_extent(v, m, lo, hi);
        const bool up = std::abs(hi[a] - line) < 2, down = std::abs(lo[a] + line) < 2;
        if (up == down) continue;  // not on the line, or across the whole height
        const float inner = up ? lo[a] : hi[a], outer = up ? hi[a] : lo[a], target = up ? half : -half;
        if (std::abs(outer - inner) < 0.5f) continue;
        const float k = (target - inner) / (outer - inner);
        for (int i = 0; i < 3; ++i) m[4 * a + i] *= k;
        m[4 * a + 3] = inner + k * (m[4 * a + 3] - inner);
        return true;
    }
    return false;
}

// True if a moved element still drawn holds layout point (x, y) and went along axis the way d points.
bool on_moved_box(float x, float y, int axis, float d) {
    const int64_t now = now_ns(), window = zone_gate_ns();
    std::lock_guard lock(g_box_mutex);
    for (const auto& [mva, b] : g_boxes)
        if (now - b->seen_ns.load(std::memory_order_relaxed) < window && b->axis == axis && (b->d > 0) == (d > 0) &&
            x >= b->lo[0] - 1 && x <= b->hi[0] + 1 && y >= b->lo[1] - 1 && y <= b->hi[1] + 1)
            return true;
    return false;
}

// `zone … follow`: the smallest moved element still drawn along axis whose old
// extent holds layout point (x, y): 1 and *d how far it went. 2 if the point
// lies only where such an element is now (the game built the text from the
// moved element: it went along already), 0 if neither.
int moved_box_under(float x, float y, int axis, float* d) {
    const int64_t now = now_ns(), window = zone_gate_ns();
    const auto holds = [&](const float* lo, const float* hi) {
        return x >= lo[0] - 1 && x <= hi[0] + 1 && y >= lo[1] - 1 && y <= hi[1] + 1;
    };
    std::lock_guard lock(g_box_mutex);
    float best = 1e30f;
    bool moved_along = false;
    for (const auto& [mva, b] : g_boxes) {
        if (now - b->seen_ns.load(std::memory_order_relaxed) >= window || b->axis != axis) continue;
        if (holds(b->lo, b->hi)) {
            const float area = (b->hi[0] - b->lo[0]) * (b->hi[1] - b->lo[1]);
            if (area < best) {
                best = area;
                *d = b->d;
            }
        } else if (holds(b->new_lo, b->new_hi)) {
            moved_along = true;
        }
    }
    return best < 1e30f ? 1 : moved_along ? 2 : 0;
}

// A moved element passes: refreshes it; true if it is new or back on screen.
bool box_seen(uint64_t mva, const float* lo, const float* hi, int axis, float d, const float* new_lo = nullptr,
              const float* new_hi = nullptr) {
    const int64_t now = now_ns();
    std::lock_guard lock(g_box_mutex);
    auto& b = g_boxes[mva];
    if (b == nullptr) b = std::make_unique<MovedBox>();
    const bool back = now - b->seen_ns.exchange(now, std::memory_order_relaxed) >= zone_gate_ns();
    if (lo != nullptr) {
        std::copy(lo, lo + 2, b->lo);
        std::copy(hi, hi + 2, b->hi);
        std::copy(new_lo, new_lo + 2, b->new_lo);
        std::copy(new_hi, new_hi + 2, b->new_hi);
        b->axis = axis;
        b->d = d;
    }
    return back;
}
bool box_known(uint64_t mva) {
    std::lock_guard lock(g_box_mutex);
    return g_boxes.count(mva) != 0;
}

// `zone`, `edge`, `widen` on one element; true if one of them owns it (now
// or from an earlier pass), so its texture rows are done here.
bool move_patch(CpuState* ctx, const SizeRule& r, const float* v, float* m, uint64_t mva) {
    const bool xwiden = listed(r.xwiden_only, v) && (r.xwiden_when.empty() || crop_open(r.xwiden_when, 0)),
               widen = xwiden || listed(r.widen_only, v), edge = r.edge_all || listed(r.edge_only, v),
               span = listed(r.span_only, v) && (r.span_except.empty() || r.to.height <= kHandheld.height ||
                                                 !crop_open(r.span_except, 0)),
               xstretch = listed(r.xstretch_only, v),
               stretch = xstretch || listed(r.stretch_only, v), fill = listed(r.fill_only, v);
    see_gates(v);
    bool zone = false;
    for (const SizeRule::Zone& z : r.zones) zone = zone || gate_open(z.when);
    if (!zone && !edge && !widen && !span && !stretch && !fill) return false;
    EdgeStep step;
    bool fresh = false, kept = false;
    const char* what = "";
    float old_lo[2], old_hi[2];
    element_extent(v, m, old_lo, old_hi);
    const float old_t[2] = {m[3], m[7]};
    int moved_axis = 1;
    {
        std::lock_guard lock(g_move_mutex);
        std::array<float, 8> now;
        std::memcpy(now.data(), m, 32);
        if (auto it = g_moved.find(mva); it != g_moved.end() && it->second.model == now) {
            step = it->second.step;
            kept = true;
        } else {
            for (const SizeRule::Zone& z : r.zones) {
                if (!gate_open(z.when)) continue;
                const bool x = z.axis == 0;
                if (x ? widescreen_xzone(v, m, z.top, z.bottom, kHandheld, r.to, z.side, listed(z.with, v))
                      : widescreen_zone(v, m, z.top, z.bottom, kHandheld, r.to, z.side, listed(z.with, v))) {
                    what = x ? "xzone" : "zone";
                    fresh = true;
                    moved_axis = z.axis;
                    break;
                }
                // Listed elements have their own rule; the rest of the middle spreads.
                if (z.spread && !edge && !stretch && !widen && !span &&
                    (x ? widescreen_xspread(v, m, z.top, z.bottom, kHandheld, r.to, z.side)
                       : widescreen_spread(v, m, z.top, z.bottom, kHandheld, r.to, z.side))) {
                    fresh = true;
                    what = x ? "xspread" : "spread";
                    moved_axis = z.axis;
                    break;
                }
            }
            if (edge && edge_step(v, m, kHandheld, r.to, &step)) {
                edge_model(step, m);
                fresh = true;
                what = "edge";
            } else if (stretch && widescreen_stretch(v, m, kHandheld, r.to, xstretch && !listed(r.stretch_only, v))) {
                // The texture rows stay: a soft end the screen edge used to cut now reaches the new edge.
                fresh = true;
                what = "stretch";
            } else if (widen && widescreen_widen(m, kHandheld, r.to, xwiden && !listed(r.widen_only, v))) {
                fresh = true;
                what = "widen";
            } else if (span && widescreen_span(v, m, kHandheld, r.to)) {
                fresh = true;
                what = "span";
            }
            if (fill && fill_to_edge(v, m, r.to)) {  // after a zone: the group's new line
                what = fresh ? (moved_axis == 0 ? "xzone fill" : "zone fill") : "fill";
                fresh = true;
            }
            if (!fresh) return false;
            std::memcpy(now.data(), m, 32);
            g_moved[mva] = {now, step};
        }
    }
    // Text on a box that a zone moved follows it (`zone … inside`), also text that waited for it.
    bool back = false;
    if (fresh && (std::strstr(what, "zone") != nullptr || std::strstr(what, "spread") != nullptr)) {
        const int t = moved_axis == 0 ? 3 : 7;
        float new_lo[2], new_hi[2];
        element_extent(v, m, new_lo, new_hi);
        back = box_seen(mva, old_lo, old_hi, moved_axis, m[t] - old_t[moved_axis], new_lo, new_hi);
    } else if (kept && box_known(mva)) {
        back = box_seen(mva, nullptr, nullptr, 1, 0);
    }
    if (back) gate_opened();
    if (fresh) {
        std::memcpy(rt::translate<true>(mva, 32), m, 32);
        rt::note_write<false>(mva, 32);
        if (r.edge_lines.fetch_add(1, std::memory_order_relaxed) < kSizeLogLines)
            rt::log(rt::LogLevel::Info, "widescreen: %s %s %gx%g model rows (%g %g %g | %g %g %g)", r.name.c_str(),
                    what, double(v[0]), double(v[1]), double(m[0]), double(m[1]), double(m[3]), double(m[4]),
                    double(m[5]), double(m[7]));
    }
    if (!r.has_tex || !(step.axis[0] || step.axis[1])) return true;
    const uint64_t tva = ctx->x[r.tex.reg] + r.tex.offset;
    if (!rt::access_allowed(tva, 32, rt::kPageRead | rt::kPageWrite)) return true;
    std::array<float, 8> q;
    std::memcpy(q.data(), rt::translate<false>(tva, 32), 32);
    {
        std::lock_guard lock(g_tex_mutex);
        if (auto it = g_tex_written.find(tva); it != g_tex_written.end() && it->second == q) return true;
    }
    edge_texture(step, q.data());
    std::memcpy(rt::translate<true>(tva, 32), q.data(), 32);
    rt::note_write<false>(tva, 32);
    std::lock_guard lock(g_tex_mutex);
    g_tex_written[tva] = q;
    return true;
}

void element_patch(CpuState* ctx, const SizeRule& r) {
    const uint64_t va = ctx->x[r.rect.reg] + r.rect.offset, mva = ctx->x[r.model.reg] + r.model.offset;
    if (!rt::access_allowed(va, 16, rt::kPageRead) || !rt::access_allowed(mva, 32, rt::kPageRead | rt::kPageWrite))
        return;
    float v[4], m[8];
    std::memcpy(v, rt::translate<false>(va, 16), 16);
    std::memcpy(m, rt::translate<false>(mva, 32), 32);
    if (r.log_elements > 0) log_element(r, v, m);
    // `movie WxH,…`: a backdrop behind a video the game draws unstretched in
    // the middle stays 16:9, so the side strips stay black (iPhone intro).
    // Once the video has faded to white it widens as usual, so no black
    // frame stays around the white picture.
    if (!r.movie_keep.empty() && listed(r.movie_keep, v) && !movie_white() &&
        now_ns() - g_movie_frame_ns.load(std::memory_order_relaxed) < 250'000'000) {
        if (r.movie_lines.fetch_add(1, std::memory_order_relaxed) == 0)
            rt::log(rt::LogLevel::Info, "widescreen: %s element %gx%g stays while a video plays", r.name.c_str(),
                    double(v[0]), double(v[1]));
        return;
    }
    // `cull WxH,…`: entirely outside the old 1280x720 view, where the screen
    // edge hid it, the element shrinks to nothing (its model rows zero).
    if (!r.culls.empty()) see_gates(v);  // a `when` element of this pass opens its gate first
    for (const SizeRule::Cull& c : r.culls) {
        if (!listed(c.sizes, v) || (!c.when.empty() && !crop_open(c.when, 0))) continue;
        float lo[2], hi[2];
        element_extent(v, m, lo, hi);
        if (hi[1] <= c.below || lo[1] >= c.above) {
            if (r.cull_lines.fetch_add(1, std::memory_order_relaxed) < kSizeLogLines)
                rt::log(rt::LogLevel::Info, "widescreen: %s element %gx%g at y %g..%g outside the old view, not drawn",
                        r.name.c_str(), double(v[0]), double(v[1]), double(lo[1]), double(hi[1]));
            std::memset(m, 0, sizeof(m));
            std::memcpy(rt::translate<true>(mva, 32), m, 32);
            rt::note_write<false>(mva, 32);
            return;
        }
    }
    if (move_patch(ctx, r, v, m, mva)) return;
    if (widescreen_element(v, m, kHandheld, r.to)) {
        std::memcpy(rt::translate<true>(mva, 32), m, 32);
        rt::note_write<false>(mva, 32);
        if (r.lines.fetch_add(1, std::memory_order_relaxed) < kSizeLogLines)
            rt::log(rt::LogLevel::Info, "widescreen: %s element %gx%g model rows (%g %g | %g %g) widened",
                    r.name.c_str(), double(v[0]), double(v[1]), double(m[0]), double(m[1]), double(m[4]),
                    double(m[5]));
    }
    // `tex`: the texture rows follow the (widened) model.
    if (r.has_tex) texture_rows(ctx, r);
}

void rect_patch(CpuState* ctx, const SizeRule& r) {
    if (r.has_model) return element_patch(ctx, r);
    const uint64_t va = ctx->x[r.rect.reg] + r.rect.offset;
    if (!rt::access_allowed(va, 16, rt::kPageRead | rt::kPageWrite)) return;
    float v[4];
    std::memcpy(v, rt::translate<false>(va, 16), 16);
    const float before[2] = {v[0], v[1]};
    if (!widescreen_rect(v, kHandheld, r.to)) return;
    std::memcpy(rt::translate<true>(va, 16), v, 16);
    rt::note_write<false>(va, 16);
    if (r.lines.fetch_add(1, std::memory_order_relaxed) < kSizeLogLines)
        rt::log(rt::LogLevel::Info, "widescreen: %s rect %gx%g -> %gx%g", r.name.c_str(), double(before[0]),
                double(before[1]), double(v[0]), double(v[1]));
}

void texture_rows(CpuState* ctx, const SizeRule& r);

void texture_patch(CpuState* ctx, void* user) {
    if (resting()) return;
    texture_rows(ctx, *static_cast<const SizeRule*>(user));
}

void texture_rows(CpuState* ctx, const SizeRule& r) {
    const uint64_t tva = ctx->x[r.tex.reg] + r.tex.offset, rva = ctx->x[r.rect.reg] + r.rect.offset,
                   mva = ctx->x[r.model.reg] + r.model.offset;
    if (!rt::access_allowed(tva, 32, rt::kPageRead | rt::kPageWrite) || !rt::access_allowed(rva, 16, rt::kPageRead) ||
        !rt::access_allowed(mva, 32, rt::kPageRead))
        return;
    std::array<float, 8> q;
    float v[4], m[8];
    std::memcpy(q.data(), rt::translate<false>(tva, 32), 32);
    std::memcpy(v, rt::translate<false>(rva, 16), 16);
    std::memcpy(m, rt::translate<false>(mva, 32), 32);
    if (!r.tex_only.empty() && !listed(r.tex_only, v)) return;
    {
        std::lock_guard lock(g_tex_mutex);
        if (auto it = g_tex_written.find(tva); it != g_tex_written.end() && it->second == q) return;
    }
    const std::array<float, 8> before = q;
    if (!widescreen_texture(v, m, q.data(), kHandheld, r.to)) {
        // A listed element that does not span the new picture: say where it lies (once per rule, a few times).
        if (!r.tex_only.empty() && r.skipped.fetch_add(1, std::memory_order_relaxed) < 8)
            rt::log(rt::LogLevel::Info,
                    "widescreen: %s texture not widened for %gx%g: model rows (%g %g %g | %g %g %g)", r.name.c_str(),
                    double(v[0]), double(v[1]), double(m[0]), double(m[1]), double(m[3]), double(m[4]),
                    double(m[5]), double(m[7]));
        return;
    }
    std::memcpy(rt::translate<true>(tva, 32), q.data(), 32);
    rt::note_write<false>(tva, 32);
    {
        std::lock_guard lock(g_tex_mutex);
        g_tex_written[tva] = q;
    }
    if (r.lines.fetch_add(1, std::memory_order_relaxed) < kSizeLogLines)
        rt::log(rt::LogLevel::Info, "widescreen: %s texture u %g %g | v %g %g -> u %g %g | v %g %g", r.name.c_str(),
                double(before[0]), double(before[3]), double(before[5]), double(before[7]), double(q[0]),
                double(q[3]), double(q[5]), double(q[7]));
}

// Matrices this mod changed (`proj` lines with zones): what it wrote and whether a
// zone took it. A gate is often shut when the game builds a screen's text
// matrices (its footer or box comes a few frames later). Within kLateZoneNs a
// still unchanged matrix gets another chance: when a gate opens (the game may
// build it once and never pass again) and on a later pass. Host time: on an
// iPad the box came 2.5-2.8 s after the text while a menu loaded, so 2 s were
// too short there (all texts stayed); 8 s cover that with room.
struct ProjMemo {
    std::array<float, 16> matrix;
    bool zoned = false;
    const SizeRule* rule = nullptr;
    int64_t made_ns = 0;
};
std::mutex g_proj_mutex;
std::map<uint64_t, ProjMemo> g_proj;
// Diagnosis: LGP_WIDESCREEN_LATE_MS=N sets that time in host milliseconds.
constexpr int64_t kLateZoneNs = 8'000'000'000;
int64_t late_zone_ns() {
    static const int64_t ns = [] {
        const char* env = std::getenv("LGP_WIDESCREEN_LATE_MS");
        if (env == nullptr || *env == '\0') return kLateZoneNs;
        char* end = nullptr;
        const long long ms = std::strtoll(env, &end, 10);
        if (end == env || *end != '\0' || ms < 0) {
            rt::log(rt::LogLevel::Warn, "LGP_WIDESCREEN_LATE_MS: expected milliseconds, ignored");
            return kLateZoneNs;
        }
        rt::log(rt::LogLevel::Info, "widescreen: text waits for its zone %lld ms", ms);
        return int64_t(ms) * 1'000'000;
    }();
    return ns;
}
std::atomic<uint32_t> g_late_lines{0}, g_expired_lines{0}, g_changed_lines{0};

// The first zone (1-based) that takes the matrix, 0 if none.
int clip_zones(const SizeRule& r, float* m) {
    const bool rows = m[3] != 0 || m[7] != 0;
    const float x = (rows ? m[3] : m[12]) * float(r.to.width) / 2.f, y = (rows ? m[7] : m[13]) * float(r.to.height) / 2.f;
    for (size_t i = 0; i < r.zones.size(); ++i) {
        const SizeRule::Zone& z = r.zones[i];
        if (!gate_open(z.when)) continue;
        if (z.follow) {
            if (rows == (m[12] != 0 || m[13] != 0)) continue;  // not a 2D matrix with a place
            float d = 0;
            const int under = moved_box_under(x, y, z.axis, &d);
            if (under == 2) return 0;  // went along with its element already
            if (under == 0) continue;
            float* tr = rows ? &m[3] : &m[12];
            tr[z.axis == 0 ? 0 : (rows ? 4 : 1)] += d / (float(z.axis == 0 ? r.to.width : r.to.height) / 2.f);
            return int(i) + 1;
        }
        if (z.inside && !on_moved_box(x, y, z.axis, z.axis == 0 ? x : y)) continue;
        if (clip_zone_axis(m, z.top, z.bottom, kHandheld, r.to, z.side, z.spread, z.axis)) return int(i) + 1;
    }
    return 0;
}

// With g_proj_mutex held: zone a remembered matrix now if a zone takes it.
void late_zone_locked(uint64_t va, ProjMemo& memo, float* m, const char* when) {
    const SizeRule& r = *memo.rule;
    const int zone = clip_zones(r, m);
    if (zone == 0) return;
    std::memcpy(rt::translate<true>(va, 64), m, 64);
    rt::note_write<false>(va, 64);
    std::memcpy(memo.matrix.data(), m, 64);
    memo.zoned = true;
    // A few always (diagnosis on devices without `log N`), then as `log N` allows.
    if (g_late_lines.fetch_add(1, std::memory_order_relaxed) < kSizeLogLines ||
        (log_open() && r.logged.fetch_add(1, std::memory_order_relaxed) < r.log_elements)) {
        const bool rows = m[3] != 0 || m[7] != 0;
        rt::log(rt::LogLevel::Info,
                "widescreen: %s matrix at x %d y %d (new picture units), moved to the edge later (zone %d, %s)",
                r.name.c_str(), int(std::lround((rows ? m[3] : m[12]) * r.to.width / 2.f)),
                int(std::lround((rows ? m[7] : m[13]) * r.to.height / 2.f)), zone, when);
    }
}

void late_zone(uint64_t va, float* m) {
    std::lock_guard lock(g_proj_mutex);
    const auto it = g_proj.find(va);
    if (it == g_proj.end() || it->second.zoned || now_ns() - it->second.made_ns >= late_zone_ns() ||
        std::memcmp(it->second.matrix.data(), m, 64) != 0)
        return;
    late_zone_locked(va, it->second, m, "pass");
}

void gate_opened() {
    const int64_t now = now_ns();
    std::lock_guard lock(g_proj_mutex);
    for (auto it = g_proj.begin(); it != g_proj.end();) {
        ProjMemo& memo = it->second;
        if (now - memo.made_ns >= late_zone_ns()) {
            if (!memo.zoned && g_expired_lines.fetch_add(1, std::memory_order_relaxed) < kSizeLogLines)
                rt::log(rt::LogLevel::Info, "widescreen: %s matrix waited %lld ms for its zone, gave up",
                        memo.rule->name.c_str(), static_cast<long long>((now - memo.made_ns) / 1'000'000));
            it = g_proj.erase(it);
            continue;
        }
        if (memo.zoned) {
            ++it;
            continue;
        }
        const uint64_t va = it->first;
        if (rt::access_allowed(va, 64, rt::kPageRead | rt::kPageWrite)) {
            float m[16];
            std::memcpy(m, rt::translate<false>(va, 64), 64);
            if (std::memcmp(memo.matrix.data(), m, 64) == 0)
                late_zone_locked(va, memo, m, "gate");
            else if (g_changed_lines.fetch_add(1, std::memory_order_relaxed) < kSizeLogLines)
                // Diagnosis on devices: the game rewrote the matrix without passing the patch again.
                rt::log(rt::LogLevel::Info, "widescreen: %s matrix changed %lld ms before its gate opened, left as is",
                        memo.rule->name.c_str(), static_cast<long long>((now - memo.made_ns) / 1'000'000));
        }
        ++it;
    }
}

// Proj line `cull`: matrices this zeroed, with the game's (changed) matrix to put back.
std::mutex g_text_cull_mutex;
std::unordered_map<uint64_t, std::array<float, 16>> g_text_cull;

bool all_zero(const float* m) {
    return std::all_of(m, m + 16, [](float f) { return f == 0; });
}

// Zero a 2D matrix that lies beyond a `cull` limit while its gate is open, put a
// zeroed one back once none applies. True when it wrote the matrix (or left a
// zeroed one zero), so the caller stops.
bool text_cull(const SizeRule& r, uint64_t va, float* m) {
    std::lock_guard lock(g_text_cull_mutex);
    auto it = g_text_cull.find(va);
    const bool zero = all_zero(m);
    if (zero && it == g_text_cull.end()) return false;
    if (!zero && it != g_text_cull.end()) {  // the game wrote a new matrix
        g_text_cull.erase(it);
        it = g_text_cull.end();
    }
    const float* place = zero ? it->second.data() : m;
    const bool rows = place[3] != 0 || place[7] != 0, columns = place[12] != 0 || place[13] != 0;
    bool cut = false;
    if (rows != columns) {
        const float y = (rows ? place[7] : place[13]) * float(r.to.height) / 2.f;
        const float x = (rows ? place[3] : place[12]) * float(r.to.width) / 2.f;
        const bool taller = uint64_t(r.to.width) * kHandheld.height < uint64_t(kHandheld.width) * r.to.height;
        for (const SizeRule::TextCull& c : r.text_culls) {
            if ((c.shape == 1 && !taller) || (c.shape == 2 && taller)) continue;
            if (x < c.x0 || x > c.x1 || y < c.y0 || y > c.y1) continue;
            if (c.beyond && y < c.above && y > c.below) continue;
            if (!c.when.empty() && !crop_open(c.when, 0)) continue;
            cut = true;
            break;
        }
        if (cut && !zero && r.text_cull_lines.fetch_add(1, std::memory_order_relaxed) < kSizeLogLines)
            rt::log(rt::LogLevel::Info, "widescreen: %s matrix at x %d y %d (new picture units) not drawn (cull)",
                    r.name.c_str(), int(std::lround((rows ? place[3] : place[12]) * float(r.to.width) / 2.f)),
                    int(std::lround(y)));
    }
    if (cut == zero) return zero;
    if (cut) {
        std::array<float, 16> game;
        std::memcpy(game.data(), m, 64);
        g_text_cull[va] = game;
        std::memset(m, 0, 64);
    } else {
        std::memcpy(m, it->second.data(), 64);
        g_text_cull.erase(it);
    }
    std::memcpy(rt::translate<true>(va, 64), m, 64);
    rt::note_write<false>(va, 64);
    return true;
}

// `proj … keep`: true when the matrix at va is the one this rule wrote last
// (seen already), false for one the game wrote since (zones and cull apply).
std::mutex g_keep_mutex;
std::unordered_map<uint64_t, std::array<float, 16>> g_keep;  // kept apart from g_proj, whose entries expire
bool keep_seen(uint64_t va, const float* m) {
    std::lock_guard lock(g_keep_mutex);
    const auto it = g_keep.find(va);
    return it != g_keep.end() && std::memcmp(it->second.data(), m, 64) == 0;
}

// A 2D projection made for `picture` (the `data` bounds) back to the game's
// 16:9: x by picture/1280, y by picture/720, rows or columns as they hold the
// translation. False if its aspect is not the picture's (16:9 already).
bool unwiden_projection(float* m, PictureSize picture) {
    if (picture.width == 0 || picture.height == 0) return false;
    const double aspect = double(picture.width) / picture.height;
    const double m00 = std::abs(double(m[0])), m11 = std::abs(double(m[5]));
    if (!(m00 > 0 && m11 > 0) || std::abs(m11 / m00 / aspect - 1.0) >= 0.01 ||
        std::abs(aspect / (double(kHandheld.width) / kHandheld.height) - 1.0) < 1e-6)
        return false;
    const bool by_columns = m[3] == 0 && m[7] == 0 && (m[12] != 0 || m[13] != 0);
    const bool rotated = m[1] != 0 || m[4] != 0;
    const float sx = float(picture.width) / float(kHandheld.width), sy = float(picture.height) / float(kHandheld.height);
    static constexpr int kRowX[] = {0, 1, 2, 3, 4, 8, 12}, kRowY[] = {1, 4, 5, 6, 7, 9, 13};
    static constexpr int kRowsX[] = {0, 1, 2, 3}, kRowsY[] = {4, 5, 6, 7};
    static constexpr int kColumnsX[] = {0, 4, 8, 12}, kColumnsY[] = {1, 5, 9, 13};
    const auto scale = [&](const auto& row, float k) {
        for (int i : row) m[i] *= k;
    };
    if (!rotated) {
        if (sx != 1) scale(kRowX, sx);
        if (sy != 1) scale(kRowY, sy);
    } else if (!by_columns) {
        if (sx != 1) scale(kRowsX, sx);
        if (sy != 1) scale(kRowsY, sy);
    } else {
        if (sx != 1) scale(kColumnsX, sx);
        if (sy != 1) scale(kColumnsY, sy);
    }
    return true;
}

void projection_patch(CpuState* ctx, void* user) {
    const SizeRule& r = *static_cast<const SizeRule*>(user);
    if (resting()) {
        // `keep`: a camera built handheld keeps the wide `data` bounds; in the
        // TV mode it goes back to 16:9 (the game copies the bounds only once).
        if (!r.proj_keep) return;
        const uint64_t va = ctx->x[r.w.reg] + r.w.offset;
        if (!rt::access_allowed(va, 64, rt::kPageRead | rt::kPageWrite)) return;
        float m[16];
        std::memcpy(m, rt::translate<false>(va, 64), 64);
        if (!unwiden_projection(m, r.to)) return;
        std::memcpy(rt::translate<true>(va, 64), m, 64);
        rt::note_write<false>(va, 64);
        return;
    }
    if (r.has_rect) rect_patch(ctx, r);
    if (!r.proj_matrix) return;
    const uint64_t va = ctx->x[r.w.reg] + r.w.offset;
    if (!rt::access_allowed(va, 64, rt::kPageRead | rt::kPageWrite)) return;
    float m[16];
    std::memcpy(m, rt::translate<false>(va, 64), 64);
    if (!r.text_culls.empty() && all_zero(m)) {  // one `cull` zeroed: back once the gate shuts
        text_cull(r, va, m);
        if (r.proj_keep) {  // the matrix put back is ours: not zoned again
            std::lock_guard lock(g_keep_mutex);
            std::memcpy(g_keep[va].data(), m, 64);
        }
        return;
    }
    const float before[2] = {m[0], m[5]};
    // `except`: the next screen with the same camera is up, so no hold and no easing back.
    const bool cut = !r.crop_except.empty() && crop_open(r.crop_except, 0);
    const bool crop = !r.crop_when.empty() && !cut && crop_open(r.crop_when, r.crop_hold_ns) &&
                      uint64_t(r.to.width) * kHandheld.height < uint64_t(kHandheld.width) * r.to.height;
    if (crop && r.crop_lines.fetch_add(1, std::memory_order_relaxed) == 0)
        rt::log(rt::LogLevel::Info, "widescreen: %s keeps the game's height while a crop element is drawn",
                r.name.c_str());
    if (!r.crop_when.empty()) {
        std::array<float, 16> now;
        std::memcpy(now.data(), m, 64);
        const int64_t at = now_ns();
        std::lock_guard lock(g_crop_mutex);
        const auto it = g_crop.find(va);
        const bool ours = it != g_crop.end() && it->second.written == now;
        if (ours) std::memcpy(m, it->second.game.data(), 64);  // a matrix the game keeps: again from its own
        // Hor- at once when the gate opens; back to the plain rule over crop_ease_ns,
        // so the scene does not jump when the element goes (both keep the aspect).
        float t = crop ? 1.0f : 0.0f;
        if (!crop && !cut && ours && it->second.t > 0 && r.crop_ease_ns > 0)
            t = std::max(0.0f, it->second.t - float(at - it->second.at_ns) / float(r.crop_ease_ns));
        if (ours && t == it->second.t && (t == 0 || t == 1)) return;
        std::array<float, 16> game, plain, cropped;
        std::memcpy(game.data(), m, 64);
        plain = cropped = game;
        if (!widescreen_projection(plain.data(), kHandheld, r.to, false) ||
            !widescreen_projection(cropped.data(), kHandheld, r.to, t > 0))
            return;
        for (int i = 0; i < 16; ++i) m[i] = plain[i] + t * (cropped[i] - plain[i]);
        std::array<float, 16> written;
        std::memcpy(written.data(), m, 64);
        g_crop[va] = {game, written, t, at};
    } else if (r.proj_keep ? !widescreen_projection(m, kHandheld, r.to) && keep_seen(va, m)
                           : !widescreen_projection(m, kHandheld, r.to)) {
        // keep: a 16:9 matrix (a camera built while docked) still widens as before
        if (!r.text_culls.empty() && text_cull(r, va, m)) return;  // changed before, its gate opened since
        if (!r.zones.empty()) late_zone(va, m);
        return;
    }
    // `zone Y`: with the projection changed just now (so once), one far from the
    // middle moves to the new edge. Text comes this way, not through `rect`.
    const int zone = clip_zones(r, m);  // 1-based, 0: none
    const bool zoned = zone != 0;
    if (!r.text_culls.empty() && text_cull(r, va, m)) return;
    std::memcpy(rt::translate<true>(va, 64), m, 64);  // the whole row changed (2D: translation too)
    rt::note_write<false>(va, 64);
    if (r.proj_keep) {
        std::lock_guard lock(g_keep_mutex);
        std::memcpy(g_keep[va].data(), m, 64);
    }
    if (!r.zones.empty()) {
        std::lock_guard lock(g_proj_mutex);
        std::array<float, 16> now;
        std::memcpy(now.data(), m, 64);
        g_proj[va] = {now, zoned, &r, now_ns()};
    }
    if (r.lines.fetch_add(1, std::memory_order_relaxed) < kSizeLogLines)
        rt::log(rt::LogLevel::Info, "widescreen: %s m00 %.4f m11 %.4f -> m00 %.4f m11 %.4f", r.name.c_str(),
                double(before[0]), double(before[1]), double(m[0]), double(m[5]));
    if (r.log_elements > 0 && log_open()) {
        // Clip-space translation (rows m[3], m[7]; columns m[12], m[13]) as layout units of the new picture.
        const float tx = m[3] != 0 || m[7] != 0 ? m[3] : m[12], ty = m[3] != 0 || m[7] != 0 ? m[7] : m[13];
        const std::array<int, 6> key = {-1, -1, int(std::lround(tx * r.to.width / 2.f)),
                                        int(std::lround(ty * r.to.height / 2.f)), zone, 0};
        bool fresh = false;
        {
            std::lock_guard lock(g_seen_mutex);
            fresh = g_seen.insert(key).second;
        }
        if (fresh && r.logged.fetch_add(1, std::memory_order_relaxed) < r.log_elements)
            rt::log(rt::LogLevel::Info, "widescreen: %s matrix at x %d y %d (new picture units)%s", r.name.c_str(),
                    key[2], key[3], zoned ? (", moved to the edge (zone " + std::to_string(zone) + ")").c_str() : "");
    }
}

// `float` patch: an LDR St at the site yields a value of the picture size.
struct FloatRule {
    std::string name;
    unsigned rt = 0;       // St
    bool literal = false;  // LDR St, label
    unsigned rn = 0;       // base register (31: sp)
    int64_t offset = 0;    // from the base, or from va for a literal
    uint64_t va = 0;
    float value = 0;
    // `docked EXPR` (with `docked wide 1`): the value while docked, e.g. a
    // pixel size the game takes from its render size (dw instead of hw).
    float docked_value = 0;
    bool has_docked = false;
    mutable std::atomic<bool> logged{false}, logged_docked{false};
};
std::vector<std::unique_ptr<FloatRule>>& float_rules() {
    static std::vector<std::unique_ptr<FloatRule>> v;
    return v;
}
// LDR St, label / LDR St, [Xn, #imm] / LDUR St, [Xn, #simm]
bool decode_float_load(uint32_t insn, FloatRule* r) {
    r->rt = insn & 31u;
    if ((insn & 0xFF00'0000u) == 0x1C00'0000u) {
        r->literal = true;
        r->offset = int64_t(int32_t(insn << 8) >> 13) * 4;  // imm19, sign-extended
        return true;
    }
    r->rn = (insn >> 5) & 31u;
    if ((insn & 0xFFC0'0000u) == 0xBD40'0000u) {
        r->offset = int64_t((insn >> 10) & 0xFFFu) * 4;
        return true;
    }
    if ((insn & 0xFFE0'0C00u) == 0xBC40'0000u) {
        r->offset = int64_t(int32_t(insn << 11) >> 23);  // imm9
        return true;
    }
    return false;
}
void float_patch(CpuState* ctx, void* user) {
    const FloatRule& r = *static_cast<const FloatRule*>(user);
    float v = r.value;
    if (resting()) {  // the game's load
        const uint64_t base = r.literal ? r.va : r.rn == 31 ? ctx->sp : ctx->x[r.rn];
        uint32_t bits = 0;
        if (!read_u32(base + uint64_t(r.offset), &bits)) {
            ctx->halt = kHaltMemFault;
            ctx->halt_info = base + uint64_t(r.offset);
            return;
        }
        std::memcpy(&v, &bits, 4);
    } else if (r.has_docked && g_docked.load(std::memory_order_relaxed)) {
        v = r.docked_value;
        if (!r.logged_docked.exchange(true, std::memory_order_relaxed))
            rt::log(rt::LogLevel::Info, "widescreen: %s loads %g while docked", r.name.c_str(), double(v));
    } else if (!r.logged.exchange(true, std::memory_order_relaxed)) {
        rt::log(rt::LogLevel::Info, "widescreen: %s loads %g", r.name.c_str(), double(v));
    }
    uint32_t bits = 0;
    std::memcpy(&bits, &v, 4);
    ctx->vreg[r.rt].lo = bits;  // a scalar load clears the rest of the register
    ctx->vreg[r.rt].hi = 0;
}

// `vfov`: STR St, [Xn, #imm] / STUR St, [Xn, #simm] of a camera's vertical
// field of view (radians); the patch stores the Vert+ angle for a taller
// picture instead, so what reads it (culling, loading) sees the picture's view.
struct VfovRule {
    std::string name;
    unsigned rt = 0, rn = 0;  // St, base register (31: sp)
    int64_t offset = 0;
    float factor = 1;  // tan(f/2) times this: (16/9) / the picture's aspect
    mutable std::atomic<bool> logged{false};
};
std::vector<std::unique_ptr<VfovRule>>& vfov_rules() {
    static std::vector<std::unique_ptr<VfovRule>> v;
    return v;
}
bool decode_float_store(uint32_t insn, VfovRule* r) {
    r->rt = insn & 31u;
    r->rn = (insn >> 5) & 31u;
    if ((insn & 0xFFC0'0000u) == 0xBD00'0000u) {
        r->offset = int64_t((insn >> 10) & 0xFFFu) * 4;
        return true;
    }
    if ((insn & 0xFFE0'0C00u) == 0xBC00'0000u) {
        r->offset = int64_t(int32_t(insn << 11) >> 23);  // imm9
        return true;
    }
    return false;
}
std::mutex g_vfov_mutex;
std::map<uint64_t, float> g_vfov_written;  // field address -> the angle this patch stored there
void vfov_patch(CpuState* ctx, void* user) {
    const VfovRule& r = *static_cast<const VfovRule*>(user);
    const uint64_t va = (r.rn == 31 ? ctx->sp : ctx->x[r.rn]) + uint64_t(r.offset);
    if (!rt::access_allowed(va, 4, rt::kPageRead | rt::kPageWrite)) {
        ctx->halt = kHaltMemFault;
        ctx->halt_info = va;
        return;
    }
    uint32_t bits = uint32_t(ctx->vreg[r.rt].lo);
    float f = 0;
    std::memcpy(&f, &bits, 4);
    // Docked: the game's angle. An angle this patch stored there before, read
    // back and stored again by the game, is ours already: it stays, no growth.
    if (!resting() && f > 0 && f < 3.1f) {
        std::lock_guard lock(g_vfov_mutex);
        const auto it = g_vfov_written.find(va);
        if (it == g_vfov_written.end() || it->second != f) {
            const float g = 2.0f * std::atan(std::tan(f / 2) * r.factor);
            if (!r.logged.exchange(true, std::memory_order_relaxed))
                rt::log(rt::LogLevel::Info, "widescreen: %s stores %g instead of %g", r.name.c_str(), double(g),
                        double(f));
            g_vfov_written[va] = g;
            std::memcpy(&bits, &g, 4);
        }
    }
    std::memcpy(rt::translate<true>(va, 4), &bits, 4);
    rt::note_write<false>(va, 4);
}

// `cullfov`: LDP St1, St2, [Xn, #imm] of a culling frustum's (fovy, aspect)
// pair from the camera. The patch loads both and hands on, in the registers
// only, the picture's aspect and, on a taller picture, the Vert+ angle; the
// camera's fields stay as they are (other readers keep the game's values).
struct CullFovRule {
    std::string name;
    unsigned rt = 0, rt2 = 0, rn = 0;  // fovy, aspect, base register (31: sp)
    int64_t offset = 0;
    float aspect = 16.0f / 9.0f;  // the picture's
    mutable std::atomic<bool> logged{false};
};
std::vector<std::unique_ptr<CullFovRule>>& cullfov_rules() {
    static std::vector<std::unique_ptr<CullFovRule>> v;
    return v;
}
bool decode_float_pair_load(uint32_t insn, CullFovRule* r) {
    if ((insn & 0xFFC0'0000u) != 0x2D40'0000u) return false;  // LDP St, St, [Xn, #imm] (signed offset)
    r->rt = insn & 31u;
    r->rn = (insn >> 5) & 31u;
    r->rt2 = (insn >> 10) & 31u;
    r->offset = int64_t(int32_t(insn << 10) >> 25) * 4;  // imm7
    return r->rt != r->rt2;
}
void set_float_reg(CpuState* ctx, unsigned reg, float v) {
    uint32_t bits = 0;
    std::memcpy(&bits, &v, 4);
    ctx->vreg[reg].lo = bits;  // a scalar load clears the rest of the register
    ctx->vreg[reg].hi = 0;
}
void cullfov_patch(CpuState* ctx, void* user) {
    const CullFovRule& r = *static_cast<const CullFovRule*>(user);
    const uint64_t va = (r.rn == 31 ? ctx->sp : ctx->x[r.rn]) + uint64_t(r.offset);
    if (!rt::access_allowed(va, 8, rt::kPageRead)) {
        ctx->halt = kHaltMemFault;
        ctx->halt_info = va;
        return;
    }
    float v[2];
    std::memcpy(v, rt::translate<false>(va, 8), 8);
    float fovy = v[0], aspect = v[1];
    // Docked: the game's values. Only a 16:9 frustum is the picture's view;
    // another aspect (an offscreen camera) stays.
    if (!resting() && std::fabs(aspect - 16.0f / 9.0f) < 1e-3f && fovy > 0 &&
        fovy < 3.1f) {
        const float g = r.aspect < aspect ? 2.0f * std::atan(std::tan(fovy / 2) * aspect / r.aspect) : fovy;
        if (!r.logged.exchange(true, std::memory_order_relaxed))
            rt::log(rt::LogLevel::Info, "widescreen: %s culls with fovy %g aspect %g instead of %g %g", r.name.c_str(),
                    double(g), double(r.aspect), double(fovy), double(aspect));
        fovy = g;
        aspect = r.aspect;
    }
    set_float_reg(ctx, r.rt, fovy);
    set_float_reg(ctx, r.rt2, aspect);
}

// `const`: a plain ALU instruction with an immediate (AND/ORR/EOR, ADD/SUB
// without flags) whose result the patch replaces by a constant on a picture
// other than 16:9, e.g. a draw cull's "outside" bit set to 0; docked the
// instruction's own result.
struct ConstRule {
    std::string name;
    unsigned rd = 0, rn = 0;
    bool sf = false;
    char op = 0;  // '&', '|', '^', '+', '-'
    uint64_t imm = 0;
    uint64_t value = 0;
    mutable std::atomic<bool> logged{false};
};
std::vector<std::unique_ptr<ConstRule>>& const_rules() {
    static std::vector<std::unique_ptr<ConstRule>> v;
    return v;
}
bool decode_alu_immediate(uint32_t insn, ConstRule* r) {
    r->rd = insn & 31u;
    r->rn = (insn >> 5) & 31u;
    r->sf = (insn >> 31) != 0;
    if (r->rd == 31) return false;  // SP (or ZR): not a value to replace
    if ((insn & 0x1F80'0000u) == 0x1200'0000u) {  // logical immediate
        const unsigned opc = (insn >> 29) & 3u;
        if (opc == 3) return false;  // ANDS sets flags
        // The mask, decoded as the same fields of ORR Rd, ZR, #imm.
        unsigned rd = 0;
        const uint32_t orr = (insn & 0x8000'0000u) | 0x3200'03E0u | (insn & 0x007F'FC00u);
        if (!rt::decode_move_immediate(orr, &rd, &r->imm)) return false;
        r->op = opc == 0 ? '&' : opc == 1 ? '|' : '^';
        return true;
    }
    if ((insn & 0x1F80'0000u) == 0x1100'0000u) {  // add/sub immediate
        if ((insn >> 29) & 1u) return false;  // ADDS/SUBS set flags
        r->imm = uint64_t((insn >> 10) & 0xFFFu) << ((insn >> 22) & 1u ? 12 : 0);
        r->op = (insn >> 30) & 1u ? '-' : '+';
        return true;
    }
    return false;
}
void const_patch(CpuState* ctx, void* user) {
    const ConstRule& r = *static_cast<const ConstRule*>(user);
    uint64_t v = r.value;
    if (resting()) {  // the instruction's own result
        const bool add = r.op == '+' || r.op == '-';
        const uint64_t a = r.rn == 31 ? (add ? ctx->sp : 0) : ctx->x[r.rn];
        v = r.op == '&' ? a & r.imm : r.op == '|' ? a | r.imm : r.op == '^' ? a ^ r.imm : r.op == '+' ? a + r.imm : a - r.imm;
    } else if (!r.logged.exchange(true, std::memory_order_relaxed)) {
        rt::log(rt::LogLevel::Info, "widescreen: %s sets %c%u to %" PRIu64, r.name.c_str(), r.sf ? 'x' : 'w', r.rd,
                r.value);
    }
    ctx->x[r.rd] = r.sf ? v : v & 0xFFFF'FFFFu;
}

// `before`: an observing patch that sets a register before the instruction at
// the site runs (which still runs), e.g. a draw cull's "outside" bit after the
// game stored its own result for the other readers. Not while docked.
struct BeforeRule {
    std::string name;
    int reg = 0;
    bool wide = false;
    uint64_t value = 0;
    mutable std::atomic<bool> logged{false};
};
std::vector<std::unique_ptr<BeforeRule>>& before_rules() {
    static std::vector<std::unique_ptr<BeforeRule>> v;
    return v;
}
void before_patch(CpuState* ctx, void* user) {
    if (resting()) return;
    const BeforeRule& r = *static_cast<const BeforeRule*>(user);
    if (!r.logged.exchange(true, std::memory_order_relaxed))
        rt::log(rt::LogLevel::Info, "widescreen: %s sets %c%d to %" PRIu64 " before the instruction", r.name.c_str(),
                r.wide ? 'x' : 'w', r.reg, r.value);
    ctx->x[r.reg] = r.value;
}

void size_patch(CpuState* ctx, void* user) {
    if (resting()) return;
    const SizeRule& r = *static_cast<const SizeRule*>(user);
    uint64_t v = 0;
    if (r.has_cond && (!get(ctx, r.cond, &v) || (v & r.cond_mask) == 0)) return;
    uint64_t w = 0, h = 0;
    if (!get(ctx, r.w, &w) || !get(ctx, r.h, &h)) return;
    const PictureSize in{uint32_t(w), uint32_t(h)};
    // Docked (`docked wide 1`): the game's targets are of the 1920x1080 family.
    const bool docked = g_docked.load(std::memory_order_relaxed);
    uint64_t format = 0;
    const bool format_known = r.has_format && get(ctx, r.format, &format);
    for (const SizeRule::Skip& k : docked ? r.docked_skip_sizes : r.skip_sizes)
        if (float(in.width) == k.w && float(in.height) == k.h &&
            (k.format < 0 || (format_known && format == uint64_t(k.format)))) {
            if (r.lines.fetch_add(1, std::memory_order_relaxed) < kSizeLogLines)
                rt::log(rt::LogLevel::Info, "widescreen: %s %ux%u stays (skip)", r.name.c_str(), in.width, in.height);
            return;
        }
    const uint64_t where = r.w.memory ? ctx->x[r.w.reg] + r.w.offset : 0;
    if (where != 0) {
        std::lock_guard lock(g_written_mutex);
        if (g_written.count({where, in.width, in.height}) != 0) return;
    }
    const PictureSize out = docked ? widescreen_scale(in, kDocked, docked_of(r.to)) : widescreen_scale(in, kHandheld, r.to);
    if (out.width == in.width && out.height == in.height) return;
    set(ctx, r.w, out.width);
    set(ctx, r.h, out.height);
    if (where != 0) {
        std::lock_guard lock(g_written_mutex);
        g_written.insert({where, out.width, out.height});
    }
    if (r.lines.fetch_add(1, std::memory_order_relaxed) < kSizeLogLines)
        rt::log(rt::LogLevel::Info, "widescreen: %s %ux%u -> %ux%u", r.name.c_str(), in.width, in.height, out.width,
                out.height);
}

std::vector<std::string_view> words(std::string_view s) {
    std::vector<std::string_view> out;
    while (true) {
        s.remove_prefix(std::min(s.find_first_not_of(" \t"), s.size()));
        if (s.empty()) return out;
        const size_t end = std::min(s.find_first_of(" \t"), s.size());
        out.push_back(s.substr(0, end));
        s.remove_prefix(end);
    }
}

// A size expression: A, A/B, A*B, A+B or A-B, each hw, hh, dw, dh, lx, ly or a
// number (hh/2, 2/hh, 640+lx). lx and ly: layout units the `layouts` line moves
// along x or y for this picture (0 without one; install reads that line first).
bool size_expr(std::string_view e, PictureSize handheld, double* out, bool* uses_handheld) {
    auto term = [&](std::string_view t, double* v) {
        if (t == "lx" || t == "ly") {
            const Layouts& l = layouts();
            *v = l.on && (l.options.axis != 0) == (t == "ly") ? l.options.shift : 0;
            return true;
        }
        if (t == "hw" || t == "hh") {
            *v = t == "hw" ? handheld.width : handheld.height;
            *uses_handheld = true;
            return true;
        }
        if (t == "dw" || t == "dh") {
            const PictureSize d = docked_render(handheld);
            *v = t == "dw" ? d.width : d.height;
            return true;
        }
        const std::string b(t);
        char* end = nullptr;
        *v = std::strtod(b.c_str(), &end);
        return !b.empty() && end == b.c_str() + b.size();
    };
    const size_t op = e.find_first_of("/*+-", 1);  // a sign in front is the number's
    if (op == std::string_view::npos)
        return term(e, out);
    double a = 0, b = 0;
    if (!term(e.substr(0, op), &a) || !term(e.substr(op + 1), &b) || (e[op] == '/' && b == 0))
        return false;
    *out = e[op] == '/' ? a / b : e[op] == '*' ? a * b : e[op] == '+' ? a + b : a - b;
    return true;
}

// One half of a patch value: hw/hh/dw/dh, a number, or an expression of them
// that gives a whole number (hh/2).
bool half(std::string_view word, PictureSize handheld, uint64_t* out, bool* uses_handheld) {
    if (word.find_first_of("/*+-", 1) != std::string_view::npos) {
        double v = 0;
        if (!size_expr(word, handheld, &v, uses_handheld) || !(v >= 0 && v <= 0xFFFF'FFFFu) || v != std::floor(v))
            return false;
        *out = uint64_t(v);
        return true;
    }
    if (word == "hw") { *out = handheld.width; *uses_handheld = true; return true; }
    if (word == "hh") { *out = handheld.height; *uses_handheld = true; return true; }
    if (word == "dw") { *out = docked_render(handheld).width; return true; }
    if (word == "dh") { *out = docked_render(handheld).height; return true; }
    const std::string s(word);
    char* end = nullptr;
    *out = std::strtoull(s.c_str(), &end, 0);
    return !s.empty() && s[0] != '-' && end == s.c_str() + s.size() && *out <= 0xFFFF'FFFFu;
}

// After `zone …` or `xzone …`: `side X`, `spread`, `with WxH,…`, `when WxH,…` for the last zone. Reads
// w[*i] (the key) and its value; false if the key is not one of these or
// its value is wrong, *known false if the key is something else.
bool zone_option(const std::vector<std::string_view>& w, size_t* i, SizeRule* r, bool* known) {
    *known = true;
    const std::string_view key = w[*i];
    if (r->zones.empty()) return *known = false;
    SizeRule::Zone& z = r->zones.back();
    if (key == "spread" && !z.spread) {
        z.spread = true;
        ++*i;
        return true;
    }
    if (key == "inside" && !z.inside) {
        z.inside = true;
        ++*i;
        return true;
    }
    if (key == "follow" && !z.follow) {
        z.follow = z.inside = true;
        ++*i;
        return true;
    }
    if (*i + 1 >= w.size()) return *known = false;
    const std::string value(w[*i + 1]);
    if (key == "side" && z.side == 0) {
        char* end = nullptr;
        z.side = std::strtof(value.c_str(), &end);
        *i += 2;
        return end == value.c_str() + value.size() && z.side > 0;
    }
    if (key == "with" && z.with.empty()) {
        *i += 2;
        return parse_sizes(value, &z.with);
    }

    if (key == "when" && z.when.empty()) {
        std::vector<std::pair<float, float>> sizes;
        if (!parse_sizes(value, &sizes)) return false;
        for (const auto& wh : sizes) z.when.push_back(gate_index(wh.first, wh.second));
        *i += 2;
        return true;
    }
    return *known = false;
}

bool install(const ModFile& file, const StartOptions& options, std::string* why) {
    if (options.memory == nullptr) {
        *why = "no guest memory to patch";
        return false;
    }
    if (!(options.display_aspect > 0)) {
        *why = "the screen's aspect is unknown (no window; LGP_WIDESCREEN_ASPECT sets it)";
        return false;
    }
    const PictureSize size = widescreen_size(options.display_aspect);
    g_hud = float(std::clamp(options.hud_bounds, 0, 100)) / 100.0f;
    // `docked wide 1` first: dw/dh in the other lines depend on it.
    bool docked_wide = false;
    for (const ModLine& l : file.lines) {
        if (l.directive != "docked") continue;
        if (l.name != "wide" || l.number > 2 || !words(l.rest).empty()) {
            *why = "docked " + l.name + ": expected docked wide 0|1|2";
            return false;
        }
        docked_wide = l.number != 0;
        g_docked_cap = l.number == 2;
    }
    if (!docked_wide) g_docked_cap = false;
    g_docked_wide = docked_wide;
    const PictureSize docked_picture = docked_of(size);
    g_docked_picture = uint64_t(docked_picture.width) << 32 | docked_picture.height;
    bool uses_handheld = false;
    size_t patches = 0;
    size_rules().clear();
    {
        std::lock_guard lock(g_vfov_mutex);
        g_vfov_written.clear();
    }
    {  // matrices an earlier run zeroed are not this run's
        std::lock_guard lock(g_text_cull_mutex);
        g_text_cull.clear();
    }
    write_data_rules(false);  // an earlier start's values back to the game's
    {
        std::lock_guard lock(g_data_mutex);
        g_data_rules.clear();
        g_data_memory = options.memory;
    }
    {
        std::lock_guard lock(g_written_mutex);
        g_written.clear();
    }
    {
        std::lock_guard lock(g_tex_mutex);
        g_tex_written.clear();
    }
    {
        std::lock_guard lock(g_move_mutex);
        g_moved.clear();
    }
    {
        std::lock_guard lock(g_keep_mutex);
        g_keep.clear();
    }
    {
        std::lock_guard lock(g_proj_mutex);
        g_proj.clear();
    }
    {
        std::lock_guard lock(g_box_mutex);
        g_boxes.clear();
    }
    g_late_lines = 0;
    g_changed_lines = 0;
    g_expired_lines = 0;
    gates().clear();
    crop_gates().clear();
    {
        std::lock_guard lock(g_crop_mutex);
        g_crop.clear();
    }
    touch_zones().clear();
    {
        std::lock_guard lock(g_seen_mutex);
        g_seen.clear();
    }
    {
        std::lock_guard lock(g_file_scale_mutex);
        g_file_scales.clear();
    }
    {
        Layouts& lay = layouts();
        std::lock_guard lock(lay.mutex);
        lay.on = false;
        lay.run = false;
        lay.options = {};
        lay.in.clear();
        lay.packs.clear();
        lay.stats = {};
        lay.files = 0;
        lay.examined = 0;
        lay.spent_ns = 0;
    }
    mode_immediates().clear();
    std::vector<rt::RepeatHint> repeats;
    std::vector<MovieRule> movies;
    int touch_render = 0;
    // `layouts` lines first: lx/ly in expressions need their shift.
    std::vector<const ModLine*> ordered;
    for (const ModLine& l : file.lines)
        if (l.directive == "layouts") ordered.push_back(&l);
    for (const ModLine& l : file.lines)
        if (l.directive != "layouts") ordered.push_back(&l);
    for (const ModLine* line : ordered) {
        const ModLine& l = *line;
        // layouts <name> <units> [axis x|y] [in DIR,…] [skip L,…] [into P,…] [keep P,…] [move P,…] [hud L,…] [pack PATH,…]
        if (l.directive == "layouts") {
            // Parsed apart and taken only if it applies to this picture: one line
            // per axis may stand in the cfg (y for taller, x for wider pictures).
            struct {
                platform::ui::ShiftOptions options;
                std::vector<std::string> in, packs;
            } lay;
            bool axis_set = false;
            const std::vector<std::string_view> w = words(l.rest);
            bool ok = l.number > 0 && l.number < 65536 && w.size() % 2 == 0;
            auto list = [](std::string_view v, std::vector<std::string>* out) {
                while (!v.empty()) {
                    const size_t comma = v.find(',');
                    if (comma != 0) out->emplace_back(v.substr(0, comma));
                    if (comma == std::string_view::npos) break;
                    v.remove_prefix(comma + 1);
                }
                return !out->empty();
            };
            for (size_t i = 0; ok && i + 1 < w.size(); i += 2) {
                const std::string_view key = w[i];
                if (key == "axis" && !axis_set && (w[i + 1] == "x" || w[i + 1] == "y")) {
                    lay.options.axis = w[i + 1] == "x" ? 0 : 1;
                    axis_set = true;
                } else if (key == "in" && lay.in.empty()) {
                    ok = list(w[i + 1], &lay.in);
                    for (std::string& d : lay.in)
                        if (d.back() != '/') d += '/';
                } else if (key == "skip" && lay.options.skip.empty()) {
                    ok = list(w[i + 1], &lay.options.skip);
                } else if (key == "into" && lay.options.into.empty()) {
                    ok = list(w[i + 1], &lay.options.into);
                } else if (key == "keep" && lay.options.keep.empty()) {
                    ok = list(w[i + 1], &lay.options.keep);
                } else if (key == "move" && lay.options.move.empty()) {
                    ok = list(w[i + 1], &lay.options.move);
                } else if (key == "hud" && lay.options.hud.empty()) {
                    ok = list(w[i + 1], &lay.options.hud);
                } else if (key == "pack" && lay.packs.empty()) {
                    ok = list(w[i + 1], &lay.packs);
                } else {
                    ok = false;
                }
            }
            if (!ok) {
                *why = "layouts " + l.name + ": expected layouts <name> <units> [axis x|y] [in DIR,…] [skip LAYOUT,…] "
                       "[into PANE,…] [keep [LAYOUT:]PANE,…] [move [LAYOUT:]PANE,…] [hud LAYOUT,…] [pack PATH,…]";
                return false;
            }
            lay.options.units = float(l.number);
            // Half the extra height in layout units (the 2D layout keeps the width, Vert+);
            // axis x: half the extra width (it keeps the height, Hor+).
            const float extra = lay.options.axis != 0
                                    ? 1280.0f * float(size.height) / float(size.width) - 720.0f
                                    : 720.0f * float(size.width) / float(size.height) - 1280.0f;
            const auto share = [&](int side) {  // 0 top, 1 bottom, 2 left, 3 right; -1 hud_bounds
                const int p = side >= 0 && options.hud_sides[size_t(side)] >= 0 ? options.hud_sides[size_t(side)]
                                                                                 : options.hud_bounds;
                return float(std::clamp(p, 0, 100)) / 100.0f;
            };
            if (lay.options.hud.empty()) {
                lay.options.shift = extra / 2 * share(-1);
            } else {
                // `hud L,…`: these layouts (the battle's) move by their bounds per
                // side, all the others to the edge.
                lay.options.shift = extra / 2;
                const bool y = lay.options.axis != 0;
                lay.options.hud_shift = {extra / 2 * share(y ? 0 : 3), extra / 2 * share(y ? 1 : 2)};
            }
            const float half_extra = extra / 2;
            if (lay.options.shift < 1) continue;  // not this picture's axis, or no room
            Layouts& on = layouts();
            if (on.on) {
                *why = "layouts " + l.name + ": a second `layouts` line for the same axis";
                return false;
            }
            on.options = std::move(lay.options);
            on.half_extra = half_extra;
            on.hud_bounds = options.hud_bounds;
            on.hud_now[0] = on.options.hud_shift[0];
            on.hud_now[1] = on.options.hud_shift[1];
            on.in = std::move(lay.in);
            on.packs = std::move(lay.packs);
            on.on = true;
            continue;
        }
        if (l.directive == "movie") {  // movie <name> <width> <height> fit|fill
            const std::vector<std::string_view> w = words(l.rest);
            uint64_t height = 0;
            const bool ok = w.size() == 2 && parse_u64(w[0], &height) && height >= 2 && height < 65536 &&
                            l.number >= 2 && l.number < 65536 && (w[1] == "fit" || w[1] == "fill");
            if (!ok) {
                *why = "movie " + l.name + ": expected movie <name> <width> <height> fit|fill";
                return false;
            }
            movies.push_back({uint32_t(l.number), uint32_t(height), w[1] == "fill"});
            continue;
        }
        if (l.directive == "file") {  // file <name> <OFF> <PATH> scale x|y [was F]
            const std::vector<std::string_view> w = words(l.rest);
            FileScale f;
            f.name = l.name;
            f.offset = l.number;
            bool ok = (w.size() == 3 || w.size() == 5) && w[1] == "scale" && (w[2] == "x" || w[2] == "y");
            if (ok) {
                std::string_view path = w[0];
                if (path.rfind("romfs:", 0) == 0) path.remove_prefix(6);
                f.path = path.empty() || path[0] != '/' ? "/" + std::string(path) : std::string(path);
                ok = f.path.size() > 1;
            }
            if (ok && w.size() == 5) {
                const std::string t(w[4]);
                char* end = nullptr;
                f.was = std::strtof(t.c_str(), &end);
                ok = w[3] == "was" && !t.empty() && end == t.c_str() + t.size();
                f.has_was = true;
            }
            if (!ok) {
                *why = "file " + l.name + ": expected file <name> <offset> <path> scale x|y [was F]";
                return false;
            }
            // The extra size along the axis in 16:9 layout units: a taller picture keeps
            // the width (more height), a wider one the height (more width).
            const double w16 = 720.0 * size.width / size.height, h16 = 1280.0 * size.height / size.width;
            f.factor = float(w[2] == "x" ? w16 / 1280.0 : h16 / 720.0);
            if (f.factor <= 1.0001f) continue;  // not this picture's axis
            std::lock_guard lock(g_file_scale_mutex);
            g_file_scales.push_back(std::move(f));
            continue;
        }
        if (l.directive == "docked") continue;  // read above
        if (l.directive == "touch") {  // touch render 0|1|2
            if (l.name != "render" || l.number > 2 || !words(l.rest).empty()) {
                *why = "touch " + l.name + ": expected touch render 0|1|2";
                return false;
            }
            touch_render = int(l.number);
            continue;
        }
        if (l.directive == "repeat") {  // repeat <name> <width> <height> u|v|uv
            const std::vector<std::string_view> w = words(l.rest);
            uint64_t height = 0;
            rt::RepeatHint hint;
            hint.width = uint32_t(l.number);
            const bool ok = w.size() == 2 && parse_u64(w[0], &height) && height > 0 && height < 65536 &&
                            l.number > 0 && l.number < 65536 &&
                            (w[1] == "u" || w[1] == "v" || w[1] == "uv");
            if (!ok) {
                *why = "repeat " + l.name + ": expected repeat <name> <width> <height> u|v|uv";
                return false;
            }
            hint.height = uint32_t(height);
            hint.u = w[1] != "v";
            hint.v = w[1] != "u";
            repeats.push_back(hint);
            continue;
        }
        if (l.directive != "patch") continue;
        if (const std::vector<std::string_view> w = words(l.rest); !w.empty() && w[0] == "size") {
            auto r = std::make_unique<SizeRule>();
            r->name = l.name;
            r->to = size;
            bool ok = w.size() >= 3 && parse_operand(w[1], &r->w) && parse_operand(w[2], &r->h);
            size_t next = 3;
            if (ok && w.size() > next && w[next] != "skip" && w[next] != "format") {
                const size_t amp = w[next].rfind('&');
                r->has_cond = true;
                ok = amp != std::string_view::npos && parse_operand(w[next].substr(0, amp), &r->cond) &&
                     parse_u64(w[next].substr(amp + 1), &r->cond_mask) && r->cond_mask != 0;
                ++next;
            }
            if (ok && w.size() > next && w[next] == "format") {
                r->has_format = w.size() > next + 1 && parse_operand(w[next + 1], &r->format);
                ok = r->has_format;
                next += 2;
            }
            if (ok && w.size() > next) {
                ok = w[next] == "skip" && w.size() >= next + 2 && parse_skips(w[next + 1], &r->skip_sizes);
                next += 2;
                for (const SizeRule::Skip& k : r->skip_sizes)
                    r->docked_skip_sizes.push_back({k.w * 1.5f, k.h * 1.5f, k.format});
                if (ok && w.size() > next) {
                    ok = w[next] == "docked" && w.size() == next + 2;
                    r->docked_skip_sizes.clear();
                    ok = ok && (w[next + 1] == "none" || parse_skips(w[next + 1], &r->docked_skip_sizes));
                    next += 2;
                }
            }
            ok = ok && w.size() == next;
            for (const SizeRule::Skip& k : r->skip_sizes) ok = ok && (k.format < 0 || r->has_format);
            for (const SizeRule::Skip& k : r->docked_skip_sizes) ok = ok && (k.format < 0 || r->has_format);
            if (!ok) {
                *why = "patch " + l.name +
                       ": expected size W H [C&MASK] [format F] [skip WxH[@N],… [docked WxH[@N],…|none]] (@N needs "
                       "format), each wN, xN, [xN+OFF] or [xN+OFF]f";
                return false;
            }
            rt::set_patch(l.number, &size_patch, r.get(), true);
            size_rules().push_back(std::move(r));
            uses_handheld = true;
            ++patches;
            continue;
        }
        if (const std::vector<std::string_view> w = words(l.rest); !w.empty() && w[0] == "data") {
            double v = 0;
            const uint8_t* at = options.memory->host_ptr(l.number, 4);
            if (w.size() != 2 || !size_expr(w[1], size, &v, &uses_handheld) || at == nullptr) {
                *why = "patch " + l.name + ": expected data EXPR (A, A/B, A*B, A+B or A-B; A, B: hw, hh, dw, dh, lx, ly or a number) at a mapped address";
                return false;
            }
            DataRule d;
            d.va = l.number;
            std::memcpy(&d.game, at, 4);
            const float f = float(v);
            std::memcpy(&d.ours, &f, 4);
            {
                std::lock_guard lock(g_data_mutex);
                g_data_rules.push_back(d);
            }
            float game = 0;
            std::memcpy(&game, &d.game, 4);
            if (resting())
                rt::log(rt::LogLevel::Info, "widescreen: %s 0x%llx keeps the game's %g while docked (%g handheld)",
                        l.name.c_str(), static_cast<unsigned long long>(l.number), double(game), double(f));
            else
                rt::log(rt::LogLevel::Info, "widescreen: %s 0x%llx holds %g", l.name.c_str(),
                        static_cast<unsigned long long>(l.number), double(f));
            ++patches;
            continue;
        }
        if (const std::vector<std::string_view> w = words(l.rest); !w.empty() && w[0] == "float") {
            auto r = std::make_unique<FloatRule>();
            r->name = l.name;
            r->va = l.number;
            bool ok = w.size() == 2 || (w.size() == 4 && w[2] == "docked");
            if (ok) {
                double v = 0;
                ok = size_expr(w[1], size, &v, &uses_handheld);
                r->value = float(v);
            }
            if (ok && w.size() == 4) {
                double v = 0;
                ok = size_expr(w[3], size, &v, &uses_handheld);
                r->docked_value = float(v);
                r->has_docked = true;
            }
            if (!ok) {
                *why = "patch " + l.name + ": expected float EXPR [docked EXPR] (A, A/B, A*B, A+B or A-B; A, B: hw, hh, dw, dh, lx, ly or a number)";
                return false;
            }
            const uint8_t* code = options.memory->host_ptr(l.number, 4);
            uint32_t insn = 0;
            if (code != nullptr) std::memcpy(&insn, code, 4);
            if (code == nullptr || !decode_float_load(insn, r.get())) {
                *why = "patch " + l.name + ": not LDR St (literal, unsigned offset or LDUR)";
                return false;
            }
            rt::set_patch(l.number, &float_patch, r.get());
            float_rules().push_back(std::move(r));
            ++patches;
            continue;
        }
        if (const std::vector<std::string_view> w = words(l.rest); !w.empty() && w[0] == "vfov") {
            auto r = std::make_unique<VfovRule>();
            r->name = l.name;
            const uint8_t* code = options.memory->host_ptr(l.number, 4);
            uint32_t insn = 0;
            if (code != nullptr) std::memcpy(&insn, code, 4);
            if (w.size() != 1 || code == nullptr || !decode_float_store(insn, r.get())) {
                *why = "patch " + l.name + ": expected vfov at STR St (unsigned offset) or STUR St";
                return false;
            }
            // Taller pictures only (Vert+); a wider one keeps the game's angle (Hor+).
            const double aspect = double(size.width) / double(size.height);
            if (aspect >= 16.0 / 9.0 - 1e-6) continue;
            r->factor = float((16.0 / 9.0) / aspect);
            rt::set_patch(l.number, &vfov_patch, r.get());
            vfov_rules().push_back(std::move(r));
            ++patches;
            continue;
        }
        if (const std::vector<std::string_view> w = words(l.rest); !w.empty() && w[0] == "before") {
            auto r = std::make_unique<BeforeRule>();
            r->name = l.name;
            const bool only_taller = w.size() == 4 && w[3] == "taller", only_wider = w.size() == 4 && w[3] == "wider";
            if ((w.size() != 3 && !only_taller && !only_wider) || !parse_reg(w[1], &r->reg, &r->wide) ||
                !parse_u64(w[2], &r->value) || (!r->wide && r->value > 0xFFFF'FFFFu)) {
                *why = "patch " + l.name + ": expected before wN|xN VALUE [taller|wider]";
                return false;
            }
            if (size.width * 9 == size.height * 16) continue;  // 16:9: the game's value
            const bool taller = size.width * 9 < size.height * 16;
            if ((only_taller && !taller) || (only_wider && taller)) continue;
            rt::set_patch(l.number, &before_patch, r.get(), /*observe=*/true);
            before_rules().push_back(std::move(r));
            ++patches;
            continue;
        }
        if (const std::vector<std::string_view> w = words(l.rest); !w.empty() && w[0] == "const") {
            auto r = std::make_unique<ConstRule>();
            r->name = l.name;
            const uint8_t* code = options.memory->host_ptr(l.number, 4);
            uint32_t insn = 0;
            if (code != nullptr) std::memcpy(&insn, code, 4);
            const bool only_taller = w.size() == 3 && w[2] == "taller", only_wider = w.size() == 3 && w[2] == "wider";
            if ((w.size() != 2 && !only_taller && !only_wider) || !parse_u64(w[1], &r->value) || code == nullptr ||
                !decode_alu_immediate(insn, r.get())) {
                *why = "patch " + l.name +
                       ": expected const VALUE [taller|wider] at AND/ORR/EOR or ADD/SUB with an immediate (no flags, "
                       "Rd not sp)";
                return false;
            }
            if (!r->sf && r->value > 0xFFFF'FFFFu) {
                *why = "patch " + l.name + ": const VALUE does not fit a w register";
                return false;
            }
            if (size.width * 9 == size.height * 16) continue;  // 16:9: the game's result
            const bool taller = size.width * 9 < size.height * 16;
            if ((only_taller && !taller) || (only_wider && taller)) continue;
            rt::set_patch(l.number, &const_patch, r.get());
            const_rules().push_back(std::move(r));
            ++patches;
            continue;
        }
        if (const std::vector<std::string_view> w = words(l.rest); !w.empty() && w[0] == "cullfov") {
            auto r = std::make_unique<CullFovRule>();
            r->name = l.name;
            const uint8_t* code = options.memory->host_ptr(l.number, 4);
            uint32_t insn = 0;
            if (code != nullptr) std::memcpy(&insn, code, 4);
            if (w.size() != 1 || code == nullptr || !decode_float_pair_load(insn, r.get())) {
                *why = "patch " + l.name + ": expected cullfov at LDP St, St, [Xn, #imm]";
                return false;
            }
            r->aspect = float(double(size.width) / double(size.height));
            if (std::fabs(r->aspect - 16.0f / 9.0f) < 1e-4f) continue;  // 16:9: nothing to do
            rt::set_patch(l.number, &cullfov_patch, r.get());
            cullfov_rules().push_back(std::move(r));
            ++patches;
            continue;
        }
        if (const std::vector<std::string_view> w = words(l.rest); !w.empty() && w[0] == "proj") {
            auto r = std::make_unique<SizeRule>();
            r->name = l.name;
            r->to = size;
            r->projection = true;
            // proj [..] [rect [..]] [zone Y [BOTTOM]|xzone X [LEFT] [side S] [spread] [inside|follow] [when WxH,…]]… [crop WxH,… [hold MS] [ease MS] [except WxH,…]] [log N]
            bool ok = w.size() >= 2 && parse_operand(w[1], &r->w) && r->w.memory;
            for (size_t i = 2; ok && i < w.size();) {
                if (w[i] == "keep" && !r->proj_keep) {
                    r->proj_keep = true;
                    ++i;
                    continue;
                }
                bool known = false;
                const bool done = zone_option(w, &i, r.get(), &known);
                if (known) {
                    ok = done;
                    continue;
                }
                if (i + 1 >= w.size()) {
                    ok = false;
                    break;
                }
                const std::string_view key = w[i], value = w[i + 1];
                uint64_t n = 0;
                if (key == "rect" && i == 2) {
                    r->has_rect = parse_operand(value, &r->rect) && r->rect.memory && !r->rect.is_float;
                    ok = r->has_rect;
                } else if (key == "zone" || key == "xzone") {
                    const std::string a(value);
                    char* end = nullptr;
                    SizeRule::Zone z;
                    z.axis = key == "zone" ? 1 : 0;
                    z.top = std::strtof(a.c_str(), &end);
                    z.bottom = -z.top;
                    ok = end == a.c_str() + a.size() && z.top > 0;
                    // `zone TOP BOTTOM`: other thresholds below the middle (BOTTOM < TOP).
                    if (ok && i + 2 < w.size()) {
                        const std::string b(w[i + 2]);
                        char* eb = nullptr;
                        const float bottom = std::strtof(b.c_str(), &eb);
                        if (!b.empty() && eb == b.c_str() + b.size()) {
                            z.bottom = bottom;
                            ok = bottom < z.top;
                            ++i;
                        }
                    }
                    r->zones.push_back(z);
                } else if (key == "crop" && r->crop_when.empty()) {
                    std::vector<std::pair<float, float>> sizes;
                    ok = parse_sizes(std::string(value), &sizes);
                    for (const auto& wh : sizes) r->crop_when.push_back(gate_index(wh.first, wh.second, crop_gates()));
                    for (bool hold = false, ease = false, except = false;
                         ok && i + 2 < w.size() &&
                         ((w[i + 2] == "hold" && !hold) || (w[i + 2] == "ease" && !ease) ||
                          (w[i + 2] == "except" && !except));) {
                        if (w[i + 2] == "except") {
                            std::vector<std::pair<float, float>> shut;
                            ok = i + 3 < w.size() && parse_sizes(w[i + 3], &shut);
                            for (const auto& wh : shut)
                                r->crop_except.push_back(gate_index(wh.first, wh.second, crop_gates()));
                            except = true;
                            i += 2;
                            continue;
                        }
                        const bool is_hold = w[i + 2] == "hold";
                        ok = i + 3 < w.size() && parse_u64(w[i + 3], &n) && n <= 60000;
                        (is_hold ? r->crop_hold_ns : r->crop_ease_ns) = int64_t(n) * 1'000'000;
                        (is_hold ? hold : ease) = true;
                        i += 2;
                    }
                } else if (key == "log" && r->log_elements == 0) {
                    ok = parse_u64(value, &n) && n > 0 && n <= 4096;
                    r->log_elements = uint32_t(n);
                } else if (key == "cull") {
                    // `cull [below Y] [above Y] [x MIN MAX] [y MIN MAX] [taller|wider] [when WxH,…]`,
                    // at least one limit; all given must hold
                    SizeRule::TextCull c;
                    bool limit = false;
                    const auto number = [](std::string_view word, float* f) {
                        const std::string t(word);
                        char* end = nullptr;
                        *f = std::strtof(t.c_str(), &end);
                        return !t.empty() && end == t.c_str() + t.size();
                    };
                    std::set<std::string_view> seen;
                    size_t j = i + 1;
                    while (ok && j < w.size()) {
                        const std::string_view k = w[j];
                        if (seen.count(k) != 0) break;
                        if (k == "taller" || k == "wider") {
                            if (c.shape != 0) break;
                            c.shape = k == "taller" ? 1 : 2;
                            j += 1;
                        } else if ((k == "below" || k == "above") && j + 1 < w.size()) {
                            ok = number(w[j + 1], k == "below" ? &c.below : &c.above);
                            c.beyond = limit = true;
                            j += 2;
                        } else if ((k == "x" || k == "y") && j + 2 < w.size()) {
                            ok = number(w[j + 1], k == "x" ? &c.x0 : &c.y0) &&
                                 number(w[j + 2], k == "x" ? &c.x1 : &c.y1);
                            limit = true;
                            j += 3;
                        } else if (k == "when" && j + 1 < w.size()) {
                            std::vector<std::pair<float, float>> sizes;
                            ok = parse_sizes(std::string(w[j + 1]), &sizes);
                            for (const auto& wh : sizes)
                                c.when.push_back(gate_index(wh.first, wh.second, crop_gates()));
                            j += 2;
                        } else {
                            break;
                        }
                        seen.insert(k);
                    }
                    ok = ok && limit;
                    i = j - 2;  // the loop steps past two words
                    r->text_culls.push_back(std::move(c));
                } else {
                    ok = false;
                }
                i += 2;
            }
            if (!ok) {
                *why = "patch " + l.name +
                       ": expected proj [xN+OFF] [rect [xN+OFF]] [zone Y [BOTTOM]|xzone X [LEFT] [side S] [spread] [inside|follow] [when WxH,…]]… [crop WxH,… [hold MS] [ease MS] [except WxH,…]] [cull [below Y] [above Y] [x MIN MAX] [y MIN MAX] [taller|wider] [when WxH,…]]… [keep] [log N]";
                return false;
            }
            rt::set_patch(l.number, &projection_patch, r.get(), true);
            size_rules().push_back(std::move(r));
            ++patches;
            continue;
        }
        if (const std::vector<std::string_view> w = words(l.rest); !w.empty() && w[0] == "tex") {
            auto r = std::make_unique<SizeRule>();
            r->name = l.name;
            r->to = size;
            r->has_tex = true;
            const auto mem = [](std::string_view word, Operand* o) {
                return parse_operand(word, o) && o->memory && !o->is_float;
            };
            const bool only_ok = w.size() == 6 || (w.size() == 8 && w[6] == "only" && parse_sizes(w[7], &r->tex_only));
            if ((w.size() != 6 && w.size() != 8) || !only_ok || w[2] != "rect" || w[4] != "model" ||
                !mem(w[1], &r->tex) || !mem(w[3], &r->rect) || !mem(w[5], &r->model)) {
                *why = "patch " + l.name + ": expected tex [xN+OFF] rect [xN+OFF] model [xN+OFF] [only WxH,…]";
                return false;
            }
            rt::set_patch(l.number, &texture_patch, r.get(), true);
            size_rules().push_back(std::move(r));
            ++patches;
            continue;
        }
        if (const std::vector<std::string_view> w = words(l.rest); !w.empty() && w[0] == "rect") {
            auto r = std::make_unique<SizeRule>();
            r->name = l.name;
            r->to = size;
            r->projection = true;
            r->proj_matrix = false;
            r->has_rect = true;
            const auto mem = [](std::string_view word, Operand* o) {
                return parse_operand(word, o) && o->memory && !o->is_float;
            };
            // rect [..] [model [..] [tex [..] [only WxH,…]] [edge WxH,…|all] [stretch WxH,…] [xstretch WxH,…] [widen WxH,…]
            //   [xwiden WxH,… [when WxH,…]] [span WxH,…] [zone TOP BOTTOM|xzone RIGHT LEFT [side S] [spread] [with WxH,…] [when WxH,…]]… [fill WxH,…] [log N]]
            bool ok = w.size() >= 2 && mem(w[1], &r->rect);
            for (size_t i = 2; ok && i < w.size(); i += 2) {
                if (bool known = false; r->has_model) {
                    const bool done = zone_option(w, &i, r.get(), &known);
                    if (known) {
                        ok = done;
                        i -= 2;  // the loop adds it back
                        continue;
                    }
                }
                if (i + 1 >= w.size()) {
                    ok = false;
                    break;
                }
                const std::string_view key = w[i], value = w[i + 1];
                uint64_t n = 0;
                if (key == "model" && i == 2) {
                    r->has_model = mem(value, &r->model);
                    ok = r->has_model;
                } else if (key == "tex" && r->has_model && !r->has_tex) {
                    r->has_tex = mem(value, &r->tex);
                    ok = r->has_tex;
                } else if (key == "only" && r->has_tex && r->tex_only.empty()) {
                    ok = parse_sizes(value, &r->tex_only);
                } else if (key == "edge" && r->has_model && !r->edge_all && r->edge_only.empty()) {
                    r->edge_all = value == "all";
                    ok = r->edge_all || parse_sizes(value, &r->edge_only);
                } else if (key == "stretch" && r->has_model && r->stretch_only.empty()) {
                    ok = parse_sizes(value, &r->stretch_only);
                } else if (key == "xstretch" && r->has_model && r->xstretch_only.empty()) {
                    ok = parse_sizes(value, &r->xstretch_only);
                } else if (key == "span" && r->has_model && r->span_only.empty()) {
                    ok = parse_sizes(value, &r->span_only);
                    // `span … except WxH,…`: on a taller picture not while one of these elements is drawn
                    // (the box's frame, whose bands of the Pokédex's sizes stay with it); wider ones still span.
                    if (ok && i + 3 < w.size() && w[i + 2] == "except") {
                        std::vector<std::pair<float, float>> sizes;
                        ok = parse_sizes(w[i + 3], &sizes);
                        for (const auto& wh : sizes)
                            r->span_except.push_back(gate_index(wh.first, wh.second, crop_gates()));
                        i += 2;
                    }
                } else if (key == "widen" && r->has_model && r->widen_only.empty()) {
                    ok = parse_sizes(value, &r->widen_only);
                } else if (key == "xwiden" && r->has_model && r->xwiden_only.empty()) {
                    ok = parse_sizes(value, &r->xwiden_only);
                    // `xwiden … when WxH,…`: only while one of these elements is drawn (the Pokédex's
                    // frame, whose row masks the box's bands of the same sizes must not follow).
                    if (ok && i + 3 < w.size() && w[i + 2] == "when") {
                        std::vector<std::pair<float, float>> sizes;
                        ok = parse_sizes(w[i + 3], &sizes);
                        for (const auto& wh : sizes)
                            r->xwiden_when.push_back(gate_index(wh.first, wh.second, crop_gates()));
                        i += 2;
                    }
                } else if (key == "fill" && r->has_model && r->fill_only.empty()) {
                    ok = parse_sizes(value, &r->fill_only);
                } else if (key == "movie" && r->has_model && r->movie_keep.empty()) {
                    ok = parse_sizes(value, &r->movie_keep);
                } else if (key == "cull" && r->has_model) {
                    SizeRule::Cull c;
                    ok = parse_sizes(value, &c.sizes);
                    // `below Y` / `above Y`: the element goes once its top edge is below Y
                    // (its bottom edge above Y), e.g. a list row a footer covered in 16:9;
                    // `when WxH,…`: only while one of these elements is drawn (the list's frame).
                    for (bool below = false, above = false, when = false;
                         ok && i + 3 < w.size() &&
                         ((w[i + 2] == "below" && !below) || (w[i + 2] == "above" && !above) ||
                          (w[i + 2] == "when" && !when));) {
                        const std::string_view k = w[i + 2];
                        if (k == "when") {
                            std::vector<std::pair<float, float>> sizes;
                            ok = parse_sizes(w[i + 3], &sizes);
                            for (const auto& wh : sizes) c.when.push_back(gate_index(wh.first, wh.second, crop_gates()));
                            when = true;
                        } else {
                            const std::string y(w[i + 3]);
                            char* end = nullptr;
                            const float f = std::strtof(y.c_str(), &end);
                            ok = !y.empty() && end == y.c_str() + y.size();
                            (k == "below" ? c.below : c.above) = f;
                            (k == "below" ? below : above) = true;
                        }
                        i += 2;
                    }
                    r->culls.push_back(std::move(c));
                } else if ((key == "zone" || key == "xzone") && r->has_model && i + 2 < w.size()) {
                    // zone TOP BOTTOM (xzone RIGHT LEFT) takes two values
                    const std::string a(value), b(w[i + 2]);
                    char* ea = nullptr;
                    char* eb = nullptr;
                    SizeRule::Zone z;
                    z.axis = key == "zone" ? 1 : 0;
                    z.top = std::strtof(a.c_str(), &ea);
                    z.bottom = std::strtof(b.c_str(), &eb);
                    ok = ea == a.c_str() + a.size() && eb == b.c_str() + b.size() && z.top > z.bottom;
                    r->zones.push_back(z);
                    ++i;
                } else if (key == "log" && r->has_model && r->log_elements == 0) {
                    ok = parse_u64(value, &n) && n > 0 && n <= 4096;
                    r->log_elements = uint32_t(n);
                } else {
                    ok = false;
                }
            }
            if (!ok) {
                *why = "patch " + l.name +
                       ": expected rect [xN+OFF] [model [xN+OFF] [tex [xN+OFF] [only WxH,…]] [edge WxH,…|all] "
                       "[stretch WxH,…] [xstretch WxH,…] [widen WxH,…] [xwiden WxH,… [when WxH,…]] [span WxH,… [except WxH,…]] [movie WxH,…] [cull WxH,… [below Y] [above Y] [when WxH,…]]… [zone TOP BOTTOM|xzone RIGHT LEFT [side S] [spread] [with WxH,…] "
                       "[when WxH,…]]… [fill WxH,…] "
                       "[log N]]";
                return false;
            }
            for (const SizeRule::Zone& z : r->zones)
                touch_zones().push_back({{z.top, z.bottom, z.side, z.spread, z.axis == 0}, z.when});
            rt::set_patch(l.number, &projection_patch, r.get(), true);
            size_rules().push_back(std::move(r));
            ++patches;
            continue;
        }
        std::string_view rest = l.rest;
        const size_t space = rest.find_first_of(" \t");
        const std::string_view lo = rest.substr(0, space);
        std::string_view hi;
        if (space != std::string_view::npos) {
            hi = rest.substr(space);
            hi.remove_prefix(std::min(hi.find_first_not_of(" \t"), hi.size()));
        }
        uint64_t lo_value = 0, hi_value = 0;
        if (!half(lo, size, &lo_value, &uses_handheld) ||
            (!hi.empty() && !half(hi, size, &hi_value, &uses_handheld))) {
            *why = "patch " + l.name + ": expected LO [HI] of hw, hh, dw, dh or a 32-bit number";
            return false;
        }
        const auto names_docked = [](std::string_view t) {
            return t.find("dw") != std::string_view::npos || t.find("dh") != std::string_view::npos;
        };
        if (docked_wide && (names_docked(lo) || names_docked(hi))) {
            uint64_t game_lo = 0, game_hi = 0;
            bool unused = false;
            t_game_docked = true;
            const bool ok = half(lo, size, &game_lo, &unused) && (hi.empty() || half(hi, size, &game_hi, &unused));
            t_game_docked = false;
            const uint8_t* code = options.memory->host_ptr(l.number, 4);
            uint32_t insn = 0;
            if (code != nullptr) std::memcpy(&insn, code, 4);
            const bool move_wide = (insn & 0x1F80'0000u) == 0x1280'0000u && ((insn >> 29) & 3u) != 1u;
            const bool mov_bitmask = (insn & 0x7F80'03E0u) == 0x3200'03E0u;
            if (!ok || code == nullptr || (!move_wide && !mov_bitmask)) {
                *why = "patch " + l.name + ": not MOVZ/MOVN/MOVK or MOV (bitmask immediate)";
                return false;
            }
            auto m = std::make_unique<ModeImmediate>();
            m->rd = insn & 31u;
            m->sf = (insn >> 31) != 0;
            m->game = game_lo | game_hi << 32;
            m->ours = lo_value | hi_value << 32;
            m->name = l.name;
            rt::set_patch(l.number, &mode_immediate_patch, m.get());
            mode_immediates().push_back(std::move(m));
            ++patches;
            continue;
        }
        std::string error;
        if (!rt::set_immediate_patch(*options.memory, l.number, lo_value | hi_value << 32, &error)) {
            *why = "patch " + l.name + ": " + error;
            return false;
        }
        ++patches;
    }
    if (!uses_handheld) {
        *why = "no `patch` line with hw or hh in " + file.path.filename().string();
        return false;
    }
    // Moved layouts replace the zone rules along their axis (`zone` for y,
    // `xzone` for x; they would move the moved units a second time).
    if (layouts().on) {
        const int axis = layouts().options.axis != 0 ? 1 : 0;
        size_t dropped = 0;
        for (const auto& r : size_rules()) {
            const size_t n = r->zones.size();
            std::erase_if(r->zones, [axis](const SizeRule::Zone& z) { return z.axis == axis; });
            dropped += n - r->zones.size();
        }
        std::erase_if(touch_zones(), [axis](const GatedTouchZone& z) { return z.zone.x == (axis == 0); });
        rt::log(rt::LogLevel::Info,
                "widescreen: layouts move %.0f units in %s (units below %.0f %s%s%s), %zu %s zones left out",
                double(layouts().options.shift), axis == 0 ? "x" : "y", double(layouts().options.units),
                axis == 0 ? "wide" : "high",
                layouts().packs.empty() ? "" : ", with .gfpak archives",
                layouts().options.skip.empty() ? "" : ", some layouts skipped", dropped, axis == 0 ? "x" : "y");
        if (const auto& o = layouts().options; !o.hud.empty()) {
            // The HUD bounds belong to the `hud` layouts alone; everything else
            // (zones, spreads, fill, touch) follows the whole picture.
            g_hud = 1.0f;
            rt::log(rt::LogLevel::Info, "widescreen: HUD bounds of %zu layouts: %.0f %s, %.0f %s", o.hud.size(),
                    double(o.hud_shift[0]), axis == 0 ? "right" : "top", double(o.hud_shift[1]),
                    axis == 0 ? "left" : "bottom");
        }
    }
    // Whether the game was lifted with these patch sites shows only when they
    // run: "patch: first pass through 0x…" in the log (lgp_lift --mods).
    rt::set_repeat_hints(std::move(repeats));
    {
        std::lock_guard lock(g_movie_mutex);
        g_movie_rules = std::move(movies);
    }
    g_movie_logged = false;
    g_touch_render = touch_render;
    write_data_rules(!resting());
    set_widescreen_active(size);
    rt::log(rt::LogLevel::Info, "widescreen: screen %.3f:1, handheld picture %ux%u (%zu patches)",
            options.display_aspect, size.width, size.height, patches);
    if (docked_wide)
        rt::log(rt::LogLevel::Info, "widescreen: also in the TV mode, docked picture %ux%u%s", docked_picture.width,
                docked_picture.height, g_docked.load(std::memory_order_relaxed) ? " (docked now)" : "");
    if (options.hud_bounds != 100)
        rt::log(rt::LogLevel::Info, "widescreen: HUD bounds %d %% (groups move that share of the way to the edge)",
                std::clamp(options.hud_bounds, 0, 100));
    return true;
}

}  // namespace

std::atomic<bool> g_on{false};  // ModImpl::on; not live: installed only when switched on

const ModImpl& widescreen_mod() {
    static const ModImpl m = {
        {"widescreen", "Widescreen",
         "More of the game world to fit the screen instead of borders. Applies from the next start.",
         true, false},
        &install,
        &g_on,
    };
    return m;
}

}  // namespace detail
}  // namespace lgp::mods
