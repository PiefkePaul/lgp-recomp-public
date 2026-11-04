// Mod "random-encounters" (P8, docs/analysis/54-zufallsbegegnungen.md): wild
// Pokémon attack the player while walking, as in the old games, in addition
// to (or instead of) touching the ones on the field.
//
// The attacker is a field Pokémon the game spawned itself. When a spawn is
// made (spawn_ctor) the mod decides whether it is one of the hidden ones
// (`hidden_percent`) and switches its contact off, so it cannot be touched by
// chance. On every frame's encounter check the mod rolls a die per step the
// player walked; on a hit it starts the encounter with a hidden one as the
// game does on a touch (encounter_setup, then encounter_touch with the
// object's zone and area kind), so the catch scene, or the battle with
// wild-battles, runs as for a touch. The hidden ones are invisible too when
// the file names the byte for it (hidden_byte_offset: the field Pokémon's
// update shows its models only while that byte is 0; set right after the
// constructor and again on every check), else only untouchable.
//
// Live (ModInfo::live): switched off, the check runs as before and the hidden
// ones get their contact back.
//
// Lines in random-encounters.cfg (our own names; the numbers are game data and
// stay in $LGP_PRIVATE_DIR):
//   hook encounter_check        the field update's encounter check, every frame
//                               (required): x1 = the check context; returns
//                               check_result_encounter when an encounter started
//   hook spawn_ctor             the field Pokémon's constructor, once per spawn:
//                               x0 = the new object (without the line nothing
//                               is hidden and the die picks any field Pokémon)
//   value check_result_encounter N
//   value objects_holder N      the field objects: holder = [[x1] + N],
//   value objects_manager N     manager = [holder + N], entries (8 bytes, 0 =
//   value objects_list N        empty) from [manager + list], count at
//   value objects_count N       [manager + count]
//   value type_interface N      type chain: call vtable[0] of obj + N, the
//   value field_pokemon_type_got N   result is a chain (word 0 = next); a field
//                               Pokémon has the node whose address is the word at N
//   value encounter_setup VA    x1 = &object, x8 = &handle (out)
//   value encounter_touch VA    x0 = from ref_inc, x1 = check context, x2 =
//                               &handle, x3 = [obj + zone_hash_offset] (8 bytes),
//   value zone_hash_offset N    w4 = [obj + area_kind_offset] (4 bytes)
//   value area_kind_offset N
//   value touched_byte_offset N byte the mod clears after the touch, as the game
//   value ref_interface N       the handle's counter: x0 = handle + N, call
//   value ref_inc_slot N        vtable(x0) + slot (inc returns x0 for the
//   value ref_dec_slot N        touch; dec twice afterwards)
//   value contact_component N   C = [obj + N]; [C + contact_count] (4 bytes) = 0
//   value contact_count N       switches the touch off, 1 back on
// Optional:
//   value hidden_byte_offset N  byte at obj + N: 1 = hidden (no models, no shadow)
//   hook model_visibility       the field object's model switch (x0 = object,
//                               x1 = covered, x2 = in sight), called by the
//                               game's visibility pass, which computes the
//                               hidden byte anew each pass: the mod covers the
//                               ones it hides here instead of writing the byte
//                               (needs hook spawn_ctor and a re-lift)
//   hook effect_visibility      the field Pokémon's effect update (x0 =
//                               object; every pass): after it the mod turns
//                               the emitters of the hidden ones off (with
//                               model_visibility):
//   value emitter_lookup VA     handle -> emitter (x0 -> x0, 0 = none)
//   value emitter_on_offset N   the emitter's "on" byte, 0 = off
//   expr emitter_1 0 EXPR       the handles on the update's registers, e.g.
//   expr emitter_2 0 EXPR       x0>0xb90 (size aura, shiny sparkle)
//   expr shadow_off 0 EXPR      the shadow's "off" byte (1 = off), set after
//                               the model switch and the effect update
//   value hidden_percent N      share of spawns that are hidden, 0–100 (100)
//   value step_rate N           chance per step, out of 256 (10: about 9 a minute
//                               of walking; Gen 1 had a rate per area); the
//                               menu's row "Häufigkeit" wins
//                               (set_random_encounter_rate)
//   zone_rate <name> <zone> <rate>   the chance per step out of 256 in this
//                               zone (the field Pokémon's zone at
//                               uncount_zone_offset), for the menu's
//                               "Original" (kRandomEncounterOriginal): Gelb's
//                               rate of the Gen 1 area <name> (encount_data.py
//                               rates); the zone of the field Pokémon nearest
//                               to the player counts, other zones step_rate
//   value player_type_got N     the player: the object in the list whose type
//   value player_active_offset N     chain has the node at the word at N and
//   value player_active_size N  whose field at obj + offset (1, 4 or 8 bytes,
//                               default 4) is 0, as the check finds it; without
//   value player_index N        these lines the entry at this index (0)
//   value player_position_offset N   three floats x, y, z at [player + N];
//                               without the line the mod cannot count steps and
//   value step_length_milli N   rolls frame_rate per frame instead; a step is
//                               N/1000 units in x/z (100000: Gen 1's step of 8
//                               frames at 30 fps, at the walking speed of about
//                               12.5 units per frame, PC Zufall-4b)
//   value warp_milli N          a move above N/1000 units between two checks is
//                               a warp or a load, not walked (a step)
//   value frame_rate N          chance per frame, out of 65536 (0 = never)
//   value seed N                the die's seed, fixed for repeatable runs;
//                               without it from the clock, new each start
//   value dry_run N             1: roll and log "would start", start nothing
//                               (counting encounters per minute, PC Zufall-4c)
//   value return_gap_ms N       after an encounter: the check pausing this long
//                               (host time, default 500) means back in the field
//   value transition_checks N   … or this many checks without such a pause (3600)
//   value candidate_min_checks N  a hidden spawn may attack once it was in the
//                               check's list for N checks (60; a fresh one may
//                               still be appearing, PC Zufall-4d); the ages
//                               start again when the field returns
//   value forget_on_return N    1 (default): when the field returns, forget
//                               which objects are hidden. The game rebuilds the
//                               field from its placement list after a battle, at
//                               the same addresses, without spawn_ctor (PC
//                               Zufall-4e), but keeps what the mod wrote: when
//                               the check lists a forgotten one again, a contact
//                               of 0 goes back to 1 and the hidden byte to 0
//                               (PC Zufall-4h). 0: keep them hidden.
//   value hide_contact N        0 (default with hidden_byte_offset): hidden only
//                               by the hidden byte, which keeps it from being
//                               touched too (PC Zufall-2), so no object without
//                               contact is left for a transition to wait on (PC
//                               Zufall-4i: with contact 0 a white screen after
//                               some battles, without none). 1: also contact 0
//                               (always without hidden_byte_offset). 2: contact
//                               0 only while the game's check runs: the mod
//                               sets it for the listed hidden ones right before
//                               the original and puts back what was there right
//                               after, so the check skips them (untouchable)
//                               but no transition, despawn or object outside
//                               the list ever finds a contact the mod left at 0
//                               (PC hide_contact 1 run 02.11.: white after a
//                               touch with 2 of 6 hidden ones not restored)
//   value hide_listed N         1: a field Pokémon in the check's list that no
//                               constructor call decided (built from the
//                               placement list after a battle or a room change,
//                               or there before switching on; PC Zufall-4j: all
//                               visible with hidden_percent 100) gets the same
//                               die on its first listing; only the hidden byte
//                               is set, so it may show for a frame. Default 1
//                               (PC Zufall-4o: after each battle the rebuilt
//                               field stayed hidden, 9 encounters without a
//                               hang); 0: only spawns. The partner Pokémon has
//                               its own type node and is never hidden.
//   hook spawner                the field Pokémon spawner (optional, with
//   value uncount_zone_offset N  the next line): while it runs, the hidden ones
//   value uncount_zone_size N    do not count against the area's limit, so they
//                               do not crowd out the visible ones (only while
//                               some are visible, hidden_percent < 100). The
//                               spawner counts the field Pokémon of an area by
//                               the zone at obj + N (size 4 or 8, default 8; PC
//                               Zufall-4p); the mod sets it to all ones for the
//                               call and back afterwards, for objects that still
//                               have the field Pokémon's vtable. The game's own
//                               limit of 20 at once stays.
//   value list_log N            diagnosis: every N checks a line with the
//                               list's size, its field Pokémon, the spawns
//                               since the last line and the other objects by
//                               the first node of their type chain (PC
//                               Zufall-4k: hide_listed found nothing new while
//                               Route 3 showed field Pokémon); with hook spawner
//                               a second line with its calls, the hidden ones
//                               kept, the live ones, the zones left out, those
//                               the game wrote meanwhile and the last zone read;
//                               also on the first check after switching on, then
//                               with the first 8 field Pokémon (address, hidden,
//                               hidden byte set) and how many hidden ones still
//                               had the byte before the check
//   value area_type A           with list_log: also each spawn area in the list
//   value area_zone_offset N      (type node A, anywhere in its chain) with its zone,
//   value area_limit_offset N     its limits (3 words: ground, water, sky), its
//   [value area_data_offset N]    data value and the field Pokémon in its zone
//                               (by uncount_zone_offset; PC Ghidra 4p)
//   hook rate_lookup ADDR       the zone's spawn rate (0..101 per frame, from
//                               encount_data; PC Ghidra 4t): divided by the
//                               visible share while some are hidden, so as
//                               many stay visible as without the mod (at 50 %
//                               twice the rate), at most 101
//   hook duration_lookup ADDR   the pause after each spawn (frames, from
//   value duration_offset N       encount_data, written to x0 + N; PC Ghidra
//                               4v): multiplied by the visible share while some
//                               are hidden (half at 50 %), at least 1; this
//                               sets the pace of spawns, not the rate
//   value point_area_type A     with list_log: also each spawn area's spawn
//   value points_offset N         points (list at +N, count at +C, a point is
//   value points_count_offset C   used when its byte at +U is not 0; stride S,
//   value point_used_offset U     0 = a list of pointers) and its zone at +Z;
//   [value points_stride S]       the spawner takes a free point and removes
//   [value point_area_zone_offset Z]  it, none free: no spawn (PC Ghidra)
//   value far_log_milli N       diagnosis (Pokémon far outside the grass, Paul
//   [value object_position_offset P]  2026-10-29): a field Pokémon first listed
//                               at one place (floats x, y, z at obj + P, default
//                               player_position_offset) that is shown more than
//                               N/1000 units in x/z from there gets one line,
//                               with whether the mod ever hid it and its zone
//                               (first listed: since the constructor or the
//                               last return to the field); with list_log
//                               the census line also gives the farthest shown
//                               and hidden one
//   value grass_range_milli N   attacks only in the grass (Paul 2026-10-29: steps
//                               on the path of Route 4 started encounters, PC
//                               Zufall-5b): steps count, and a hidden one may
//                               attack, only within N/1000 units in x/z of where
//                               a hidden one was first listed (its spawn point,
//                               in the grass); needs player_position_offset, the
//                               objects' position as for far_log
//
// Gelb mode (Paul 2026-11-01: Pokémon Yellow's random battles): with the
// menu's "Original" and these lines the die no longer needs a hidden field
// Pokémon. Steps count only while the player stands inside a spawn area (the
// area's own test, the one the spawner asks) whose zone has slot lines; a hit
// rolls a slot of that zone and starts the battle with its species and level
// through wild-battles (wild_battles_direct, the scripted battle path). The
// field Pokémon on screen are hidden as with hidden_percent 100.
//   value area_type A           the spawn areas in the check's list (as for
//   value area_zone_offset N      list_log; area_limit_offset is needed too)
//   value area_inside_slot N    the area's vtable slot (byte offset): x0 =
//                               area, w0 != 0 while the player is inside its
//                               trigger (PC Ghidra Zufall-9)
//   slot <name> <zone> <weight> <species> <level>[-<max>] [form]
//                               one Gen 1 slot of the zone (weights as Gelb's
//                               20/20/15/10/10/10/5/5/4/1 %; a level range is
//                               rolled evenly, encount_data.py slots writes
//                               them); a zone without slots counts no steps
//   [value player_grass_offset N]  steps count only while the byte at player +
//                               N is not 0 (the player in the grass: the game's
//                               grass rustle and footstep sound, PC Ghidra
//                               Zufall-10: 0xbd0); the area's trigger alone is
//                               larger than its grass
//   cave <name> <zone>          with player_grass_offset: in this zone every
//                               step inside the area counts (caves, no grass)
//   [value area_active_slot N]  the area's "active" test first (vtable byte
//                               offset, x0 = area, bit 0 of w0 set: active),
//                               as the spawner (Ghidra Zufall-10: 0x58)
//                               area_type is the type node itself (not a GOT
//                               entry), matched anywhere in the chain from the
//                               type interface (is-a); objects without a lifted
//                               function at a slot are skipped with a log line
//   value spawns_off 1          with hook spawner: the spawner does not run
//                               while Gelb mode is on (no new field Pokémon)
//
// The game's transition into an encounter waits for the field Pokémon whose
// contact count is 0 (PC Zufall-4c: a white screen for good when another hidden
// one was there). So right before the forced touch every field Pokémon gets its
// contact back, and none is hidden again until the field returns: the check
// still runs about 200 frames in the white screen, and only pauses in the
// battle or catch scene. Their hidden byte stays (no models), which the
// transition does not wait for. The constructor sets only the hidden byte: the
// contact count goes to 0 only for objects the check finds in its list, so no
// object outside the list (still loading, PC Zufall-4d) is left without contact.
#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "lgp/runtime/dispatch.hpp"
#include "lgp/runtime/guest_mem.hpp"
#include "lgp/runtime/log.hpp"
#include "mod_impl.hpp"

namespace lgp::mods::detail {

namespace {

std::atomic<bool> g_on{false};

constexpr uint64_t kScratch = 0x100;
constexpr uint64_t kMaxObjects = 4096;
constexpr uint32_t kPendingFrames = 300;  // a hit waits this long for a hidden Pokémon
constexpr uint32_t kFileShare = 0xFFFFFFFFu;

struct Config {
    uint64_t result_encounter = 0;
    uint64_t holder = 0, manager = 0, list = 0, count = 0;
    uint64_t type_interface = 0, type_got = 0;
    uint64_t setup = 0, touch = 0, zone_hash = 0, area_kind = 0, touched_byte = 0;
    uint64_t ref_interface = 0, ref_inc = 0, ref_dec = 0;
    uint64_t contact_component = 0, contact_count = 0;
    uint32_t hidden_percent = 100;
    uint32_t step_rate = 10;
    uint64_t player_index = 0;
    bool player_by_type = false;
    uint64_t player_type_got = 0;
    bool player_active = false;
    uint64_t player_active_offset = 0;
    uint32_t player_active_size = 4;
    bool steps = false;
    uint64_t player_position = 0;
    double step_length = 100.0;
    double warp = 100.0;
    uint32_t frame_rate = 0;
    bool dry_run = false;
    uint32_t return_gap_ms = 500;
    uint32_t transition_checks = 3600;
    uint64_t candidate_min_checks = 60;
    bool forget_on_return = true;
    bool hide_contact = false;
    bool contact_in_check = false;  // hide_contact 2
    bool hide_listed = false;
    uint64_t list_log = 0;
    bool uncount = false;  // spawner is hooked
    uint64_t uncount_zone = 0;
    uint32_t uncount_zone_size = 8;
    uint64_t field_zone = 0;  // uncount_zone_offset, also without the spawner (list_log)
    bool has_field_zone = false;
    uint64_t area_type = 0;  // list_log: spawn areas by their type node
    uint64_t area_zone = 0, area_data = 0, area_limit = 0;
    bool has_area_data = false;
    uint64_t point_area_type = 0;  // list_log: spawn areas with their spawn points
    uint64_t points = 0, points_count = 0, point_used = 0, points_stride = 0, point_area_zone = 0;
    bool has_point_area_zone = false;
    bool rate = false;  // rate_lookup is hooked
    std::unordered_map<uint64_t, uint32_t> zone_rates;  // zone_rate lines: zone -> per 256
    bool duration = false;  // duration_lookup is hooked
    uint64_t duration_offset = 0;
    bool hide = false;  // spawn_ctor is hooked
    bool invisible = false;  // hidden_byte_offset is given
    bool model_hook = false;  // hook model_visibility: hidden by the game's model switch
    // hook effect_visibility: effects drawn apart from the model (aura, shiny
    // sparkle) and the shadow, switched off for the hidden ones
    bool effect_hook = false;
    uint64_t emitter_lookup = 0, emitter_on = 0;
    HookDump emitters[2];
    size_t emitter_count = 0;
    HookDump shadow_off;
    bool has_shadow = false;
    uint64_t hidden_byte = 0;
    double far = 0;  // far_log_milli / 1000; 0: no far lines
    double range = 0;  // grass_range_milli / 1000; 0: attacks wherever hidden ones are loaded
    uint64_t object_position = 0;
    // Gelb mode: the zone's slots, the area test, spawner off.
    struct Slot {
        uint32_t weight = 0, species = 0, level = 0, form = 0, max_level = 0;  // level..max_level
    };
    std::unordered_map<uint64_t, std::vector<Slot>> slots;
    uint64_t area_inside_slot = 0;
    uint64_t area_active_slot = 0;  // Gelb mode: the area's "active" test (bit 0), 0 = none
    uint64_t player_grass = 0;      // Gelb mode: the player's "in grass" byte (player_grass_offset)
    bool grass = false;
    std::unordered_set<uint64_t> anywhere_zones;  // Gelb mode: zones where every step counts (cave lines)
    bool direct = false;  // slots, area_type and area_inside_slot given
    bool spawns_off = false;
};
Config g_config;

// Hidden objects, by address; the constructor decides again for an address
// that is reused. Both hooks may run on different guest threads, so the set
// and the die are locked, but never across guest memory or guest calls: a
// guest fault unwinds with longjmp (call_guest).
std::mutex g_mutex;
// Hidden objects -> the check number when the check first found them (0: not yet).
std::unordered_map<uint64_t, uint64_t> g_hidden;
// Forgotten at the return (forget_on_return): the game reuses the objects but
// keeps what the mod wrote (PC Zufall-4h: a contact left at 0 hung the next
// touch). When the check lists one again, a 0 contact goes back to 1 and a set
// hidden byte to 0; nothing else is written.
std::unordered_set<uint64_t> g_forgotten;
// Field Pokémon the die let be (hide_listed): not decided again on the next check.
std::unordered_set<uint64_t> g_shown;
// far_log, grass_range: where each field Pokémon was first listed; a new
// object (the constructor) and the return to the field start again.
struct Origin {
    double x = 0, z = 0;
    bool ever_hidden = false;
    bool reported = false;
};
std::unordered_map<uint64_t, Origin> g_origin;
double g_far_shown = 0, g_far_hidden = 0;  // farthest since the last list_log line
std::mt19937 g_die;
// Only the encounter check touches these (the field update, one thread).
bool g_was_on = false;
bool g_have_position = false;
uint64_t g_player = 0;  // whose position g_x/g_z is
double g_x = 0, g_z = 0, g_walked = 0;
uint32_t g_pending = 0;  // frames a hit still waits for a hidden Pokémon
uint64_t g_found[kMaxObjects];
bool g_found_hidden[kMaxObjects];
bool g_found_ready[kMaxObjects];
bool g_found_forgotten[kMaxObjects];  // hidden and in the list for candidate_min_checks
bool g_found_new[kMaxObjects];        // not decided yet (hide_listed)
uint64_t g_held_contact[kMaxObjects];  // hide_contact 2: components without contact for the check
uint32_t g_held_count[kMaxObjects];    // and the count each had
uint64_t g_check_no = 1;          // checks while on, from 1
uint64_t g_candidates[kMaxObjects];
uint64_t g_steps = 0;  // steps counted since switched on (dry_run's log)
uint32_t g_would = 0;  // dry_run hits
// After a forced touch until the field returns: nothing hidden again, no die.
// The constructor reads it on its own thread.
std::atomic<bool> g_transition{false};
uint32_t g_transition_checks = 0;
std::chrono::steady_clock::time_point g_last_check;
bool g_check_said = false;  // a check ran since install (the pause line)
bool g_hide_only_said = false;  // the mode the on line named last
std::atomic<uint32_t> g_started{0};
std::atomic<uint64_t> g_vtable{0};  // the field Pokémon's, from the first spawn
std::atomic<uint32_t> g_spawns{0};  // constructor calls since the last list_log line
// list_log with hook spawner: what the spawner door did since the last line.
struct SpawnerStats {
    std::atomic<uint32_t> calls{0}, skipped{0}, kept{0}, live{0}, left_out{0}, written_by_game{0};
    std::atomic<uint64_t> last_zone{0};
} g_spawner_stats;
// list_log with hook rate_lookup: its calls and the rates in and out.
struct RateStats {
    std::atomic<uint32_t> calls{0}, raised{0}, last_in{0}, last_out{0}, max_in{0};
} g_rate_stats;
// list_log with hook duration_lookup: its calls and the pauses in and out.
struct DurationStats {
    std::atomic<uint32_t> calls{0}, cut{0}, last_in{0}, last_out{0};
} g_duration_stats;
// list_log with area_type: the spawn areas found in the list.
constexpr size_t kMaxAreas = 8;
uint64_t g_areas[kMaxAreas];
size_t g_area_count = 0, g_area_more = 0;
uint64_t g_point_areas[kMaxAreas];  // with point_area_type
size_t g_point_area_count = 0, g_point_area_more = 0;
// Gelb mode: the spawn areas of the last check.
constexpr size_t kMaxDirectAreas = 64;
uint64_t g_direct_areas[kMaxDirectAreas];
size_t g_direct_area_count = 0;
uint64_t g_direct_zone = 0;   // the zone the player stands in (0: none with slots)
bool g_direct_said = false;    // g_direct_zone was logged
int g_direct_grass = -1;       // the last "in grass" state logged (-1: none yet)
uint64_t g_bad_area_vtable = 0;  // the last vtable without a function at area_inside_slot, logged
std::atomic<uint32_t> g_direct_started{0};
std::atomic<uint32_t> g_spawner_skipped{0};
// list_log: the list's size and its other objects by their type chain's first node.
uint64_t g_list_count = 0;
struct TypeCount {
    uint64_t node;
    uint32_t n;
};
TypeCount g_types[16];
size_t g_type_kinds = 0;
uint32_t g_type_more = 0;  // objects of further kinds
// The menu's rate per step out of 256 (set_random_encounter_rate); 0: the
// mod file's step_rate. Kept across install() (the menu sets it before start).
std::atomic<uint32_t> g_rate{0};
uint32_t g_rate_said = 0;  // the rate the check logged last
// The menu's share of hidden field Pokémon in percent
// (set_random_encounter_hidden_percent); above 100: the mod file's.
std::atomic<uint32_t> g_hidden_share{kFileShare};
// The app's "Hide field Pokémon" in Let's Go (set_random_encounter_hide_only):
// the field Pokémon are hidden, but no die, no Gelb battles, and the hidden
// ones stay touchable (no hide_contact 2 window). Kept across install().
std::atomic<bool> g_hide_only{false};

bool gelb_active();

uint32_t hidden_percent() {
    if (gelb_active())
        return 100;  // no field Pokémon in sight, the die brings the battles
    const uint32_t p = g_hidden_share.load(std::memory_order_relaxed);
    return p <= 100 ? p : g_config.hidden_percent;
}

// "Original": the zone_rate of the zone the player walks in, set by the check.
std::atomic<uint32_t> g_rate_here{0};
uint64_t g_zone_said = ~uint64_t{0};  // the zone the check logged last

uint32_t step_rate() {
    const uint32_t r = g_rate.load(std::memory_order_relaxed);
    if (r == kRandomEncounterOriginal) {
        const uint32_t here = g_rate_here.load(std::memory_order_relaxed);
        return here != 0 ? here : g_config.step_rate;
    }
    return r != 0 ? r : g_config.step_rate;
}

// Gelb mode is on: the mod on, the menu's "Original", the file has its lines
// and wild-battles can start such a battle.
bool gelb_active() {
    return g_config.direct && g_on.load(std::memory_order_relaxed) && !g_hide_only.load(std::memory_order_relaxed) &&
           g_rate.load(std::memory_order_relaxed) == kRandomEncounterOriginal && wild_battles_direct_ready();
}

bool chance(uint32_t numerator, uint32_t denominator) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return numerator != 0 && std::uniform_int_distribution<uint32_t>(0, denominator - 1)(g_die) < numerator;
}

size_t pick(size_t n) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return std::uniform_int_distribution<size_t>(0, n - 1)(g_die);
}

float load_float(uint64_t va) {
    const uint32_t bits = rt::load<uint32_t>(va);
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

// A diagnostic read (list_log): 0 where the guest has no readable memory, so
// wrong offsets in a mod file give odd numbers instead of a host crash.
template <typename T>
T peek(uint64_t va) {
    return rt::access_allowed(va, sizeof(T), rt::kPageRead) ? rt::load<T>(va) : T{0};
}

// The field Pokémon of the check's object list into g_found (their number in
// *n) and the player's object. False if the guest halted.
bool find_field_pokemon(lgp::CpuState* ctx, uint64_t scratch, uint64_t check_x1, size_t* n, uint64_t* player) {
    const Config& c = g_config;
    *n = 0;
    *player = 0;
    const uint64_t c0 = rt::load<uint64_t>(check_x1);
    const uint64_t holder = c0 ? rt::load<uint64_t>(c0 + c.holder) : 0;
    const uint64_t mgr = holder ? rt::load<uint64_t>(holder + c.manager) : 0;
    const bool census = c.list_log != 0 && g_check_no % c.list_log == 0;
    if (census) {
        g_list_count = 0;
        g_type_kinds = 0;
        g_type_more = 0;
        g_area_count = 0;
        g_area_more = 0;
        g_point_area_count = 0;
        g_point_area_more = 0;
    }
    g_direct_area_count = 0;  // no manager (a scene change): no areas of an earlier field
    if (mgr == 0)
        return true;
    const uint64_t node = rt::load<uint64_t>(c.type_got);  // the type nodes' addresses
    const uint64_t player_node = c.player_by_type ? rt::load<uint64_t>(c.player_type_got) : 0;
    // area_type and point_area_type are type nodes, matched anywhere in an
    // object's chain (is-a, as the spawner's check). They are not read as GOT
    // entries any more: a node's first word is its parent, so reading it
    // matched the parent, the base of all field objects (PC Zufall-10, Ghidra).
    const uint64_t begin = rt::load<uint64_t>(mgr + c.list);
    const uint64_t count = rt::load<uint64_t>(mgr + c.count);
    if (census)
        g_list_count = count;
    for (uint64_t i = 0; i < count && i < kMaxObjects; ++i) {
        const uint64_t obj = rt::load<uint64_t>(begin + 8 * i);
        if (!c.player_by_type && i == c.player_index)
            *player = obj;
        if (obj == 0)
            continue;
        ctx->sp = scratch;
        ctx->x[0] = obj + c.type_interface;
        if (!call_guest(ctx, rt::load<uint64_t>(rt::load<uint64_t>(obj + c.type_interface))))
            return false;
        bool field = false;
        for (uint64_t t = ctx->x[0], k = 0; t != 0 && k < 64; t = rt::load<uint64_t>(t), ++k) {
            if (t == node) {
                field = true;
                g_found[(*n)++] = obj;
                break;
            }
            if (t == player_node && player_node != 0 && *player == 0) {
                const uint64_t at = obj + c.player_active_offset;
                const uint64_t active = !c.player_active ? 0
                                       : c.player_active_size == 1 ? rt::load<uint8_t>(at)
                                       : c.player_active_size == 8 ? rt::load<uint64_t>(at)
                                                                   : rt::load<uint32_t>(at);
                if (active == 0)
                    *player = obj;
                break;
            }
        }
        if (census && !field && c.point_area_type != 0) {
            for (uint64_t t = ctx->x[0], k = 0; t != 0 && k < 64; t = rt::load<uint64_t>(t), ++k) {
                if (t == c.point_area_type) {
                    if (g_point_area_count < kMaxAreas)
                        g_point_areas[g_point_area_count++] = obj;
                    else
                        ++g_point_area_more;
                    break;
                }
            }
        }
        if ((census || c.direct) && !field && c.area_type != 0) {
            for (uint64_t t = ctx->x[0], k = 0; t != 0 && k < 64; t = rt::load<uint64_t>(t), ++k) {
                if (t == c.area_type) {
                    if (c.direct && g_direct_area_count < kMaxDirectAreas)
                        g_direct_areas[g_direct_area_count++] = obj;
                    if (!census)
                        break;
                    if (g_area_count < kMaxAreas)
                        g_areas[g_area_count++] = obj;
                    else
                        ++g_area_more;
                    break;
                }
            }
        }
        if (census && !field) {
            const uint64_t first = ctx->x[0];
            size_t k = 0;
            while (k < g_type_kinds && g_types[k].node != first)
                ++k;
            if (k < g_type_kinds)
                ++g_types[k].n;
            else if (g_type_kinds < 16)
                g_types[g_type_kinds++] = {first, 1};
            else
                ++g_type_more;
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (size_t i = 0; i < *n; ++i)
        {
            const auto it = g_hidden.find(g_found[i]);
            g_found_hidden[i] = it != g_hidden.end();
            g_found_forgotten[i] = !g_forgotten.empty() && g_forgotten.erase(g_found[i]) != 0;
            if (g_found_hidden[i] && it->second == 0)
                it->second = g_check_no;
            g_found_ready[i] = g_found_hidden[i] && g_check_no - it->second >= g_config.candidate_min_checks;
            g_found_new[i] = g_config.hide_listed && !g_found_hidden[i] &&
                             (g_found_forgotten[i] || g_shown.count(g_found[i]) == 0);
        }
    }
    return true;
}

bool ref_call(lgp::CpuState* ctx, uint64_t scratch, uint64_t handle, uint64_t slot) {
    ctx->sp = scratch;
    ctx->x[0] = handle + g_config.ref_interface;
    return call_guest(ctx, rt::load<uint64_t>(rt::load<uint64_t>(ctx->x[0]) + slot));
}

// What the check does on a touch of obj. False if the guest halted; *started
// whether there was an encounter to start.
bool force_contact(lgp::CpuState* ctx, uint64_t scratch, uint64_t check_x1, uint64_t obj, bool* started) {
    const Config& c = g_config;
    *started = false;
    const uint64_t slot = scratch + 0x40, out = scratch + 0x48;
    rt::store<uint64_t>(slot, obj);
    rt::store<uint64_t>(out, 0);
    ctx->sp = scratch;
    ctx->x[1] = slot;
    ctx->x[8] = out;
    if (!call_guest(ctx, c.setup))
        return false;
    const uint64_t handle = rt::load<uint64_t>(out);
    if (handle == 0)
        return true;
    if (!ref_call(ctx, scratch, handle, c.ref_inc))
        return false;
    const uint64_t counter = ctx->x[0];
    ctx->sp = scratch;
    ctx->x[0] = counter;
    ctx->x[1] = check_x1;
    ctx->x[2] = out;
    ctx->x[3] = rt::load<uint64_t>(obj + c.zone_hash);
    ctx->x[4] = rt::load<uint32_t>(obj + c.area_kind);
    ctx->x[24] = obj;  // the check holds the object there, and wild-battles' file reads it (PC Zufall-4)
    if (!call_guest(ctx, c.touch))
        return false;
    if (!ref_call(ctx, scratch, handle, c.ref_dec) || !ref_call(ctx, scratch, handle, c.ref_dec))
        return false;
    rt::store<uint8_t>(obj + c.touched_byte, 0);
    *started = true;
    return true;
}

void set_contact(uint64_t obj, bool on) {
    const uint64_t comp = rt::load<uint64_t>(obj + g_config.contact_component);
    if (comp != 0)
        rt::store<uint32_t>(comp + g_config.contact_count, on ? 1u : 0u);
}

// A hidden one: no touch by chance, and invisible if the file says how.
// `contact`: false leaves the contact count as it is (in a transition).
void set_hidden(uint64_t obj, bool hidden, bool contact = true) {
    if (contact && (g_config.hide_contact || !hidden))
        set_contact(obj, !hidden);
    if (g_config.invisible && !g_config.model_hook)
        rt::store<uint8_t>(obj + g_config.hidden_byte, hidden ? 1u : 0u);
}

// What the mod left in a forgotten object, undone (only values it writes).
void undo_forgotten(uint64_t obj) {
    const uint64_t comp = rt::load<uint64_t>(obj + g_config.contact_component);
    if (comp != 0 && rt::load<uint32_t>(comp + g_config.contact_count) == 0)
        rt::store<uint32_t>(comp + g_config.contact_count, 1u);
    if (g_config.invisible && !g_config.model_hook && rt::load<uint8_t>(obj + g_config.hidden_byte) == 1)
        rt::store<uint8_t>(obj + g_config.hidden_byte, 0u);
}

// The game's model switch (hook model_visibility, vt+0xc8 of a field object,
// Ghidra versteckt-292.md): its visibility pass computes the hidden byte anew
// on every pass and then calls this with x1 = "covered" and x2 = "in sight",
// so a byte the mod writes lasts until the next pass (PC tog2: switched on,
// both still visible). Here a field Pokémon the mod holds as hidden is
// covered on every pass, outside a transition (the encounter's scene), and
// the game's own result comes back as soon as the mod lets it go.
// What the model and effect hooks did since the last list_log line.
struct EffectStats {
    std::atomic<uint32_t> covered{0}, passes{0}, emitters{0}, emitters_off{0}, shadows{0};
} g_effect_stats;

// A field Pokémon the mod holds as hidden, outside a transition. In Gelb mode
// also through the transition into Gelb's battle (Paul, iPhone 02.11.: the
// hidden ones showed for about a second as the battle began): no field
// Pokémon takes part in that battle, and covering the model touches neither
// the contact count nor the hidden byte that the transition waits on. A touch
// in Let's Go still shows them, the touched one belongs to its scene.
bool held_hidden(uint64_t obj) {
    if (!g_on.load(std::memory_order_relaxed) ||
        (g_transition.load(std::memory_order_relaxed) && !gelb_active()))
        return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_hidden.count(obj) != 0;
}

// The shadow's "off" byte (expr shadow_off), set to 1; the game sets it anew
// on its own pass, so letting go needs nothing.
void shadow_off(const lgp::CpuState& entry) {
    uint64_t at = 0;
    if (g_config.has_shadow && eval_hook_expr(g_config.shadow_off, entry, 0, &at) &&
        rt::access_allowed(at, 1, rt::kPageWrite)) {
        rt::store<uint8_t>(at, 1u);
        g_effect_stats.shadows.fetch_add(1, std::memory_order_relaxed);
    }
}

void on_model_visibility(lgp::CpuState* ctx, lgp::LiftedFn original, void*) {
    const lgp::CpuState entry = *ctx;
    const bool held = held_hidden(entry.x[0]);
    if (held) {
        ctx->x[1] = 1;
        g_effect_stats.covered.fetch_add(1, std::memory_order_relaxed);
    }
    original(ctx);
    if (held && ctx->halt == kHaltNone)
        shadow_off(entry);
}

// The field Pokémon's effect update (hook effect_visibility, Ghidra
// versteckt-292.md "effect_visibility follow-up"): it writes each emitter's
// "on" byte from in sight and not hidden on every call, so the size aura and
// the shiny sparkle of a hidden one, drawn apart from its model, stayed (PC
// d84b111: an orange swirl in the grass). After it the mod turns them off for
// the hidden ones; the emitters come from the game's lookup each time (handle
// expr -> emitter, 0 = none), nothing is kept. The hidden byte is never
// forced: the game despawns on it.
void on_effect_visibility(lgp::CpuState* ctx, lgp::LiftedFn original, void*) {
    const lgp::CpuState entry = *ctx;
    original(ctx);
    if (ctx->halt != kHaltNone || !held_hidden(entry.x[0]))
        return;
    const Config& c = g_config;
    g_effect_stats.passes.fetch_add(1, std::memory_order_relaxed);
    const lgp::CpuState after = *ctx;
    const uint64_t scratch = (after.sp - kScratch) & ~uint64_t{0xF};
    for (size_t k = 0; k < c.emitter_count; ++k) {
        uint64_t handle = 0;
        if (!eval_hook_expr(c.emitters[k], entry, 0, &handle) || handle == 0)
            continue;
        ctx->sp = scratch;
        ctx->x[0] = handle;
        if (!call_guest(ctx, c.emitter_lookup))
            return;  // halted: the report keeps its state
        const uint64_t emitter = ctx->x[0];
        if (emitter == 0)
            continue;
        g_effect_stats.emitters.fetch_add(1, std::memory_order_relaxed);
        if (rt::access_allowed(emitter + c.emitter_on, 1, rt::kPageWrite)) {
            if (rt::load<uint8_t>(emitter + c.emitter_on) != 0)
                g_effect_stats.emitters_off.fetch_add(1, std::memory_order_relaxed);
            rt::store<uint8_t>(emitter + c.emitter_on, 0u);
        }
    }
    *ctx = after;
    shadow_off(entry);
}

// far_log: how far each listed field Pokémon is from where it was first
// listed, and one line for a shown one beyond the limit. Reads only.
void track_positions(size_t n) {
    const Config& c = g_config;
    if (c.far == 0 && c.range == 0)
        return;
    struct Far {
        uint64_t obj;
        double d, x0, z0, x, z;
        bool ever_hidden;
    };
    Far far[8];
    size_t far_n = 0;
    for (size_t i = 0; i < n; ++i) {
        const uint64_t obj = g_found[i];
        const uint64_t at = obj + c.object_position;
        if (!rt::access_allowed(at, 12, rt::kPageRead))
            continue;
        const double x = load_float(at);
        const double z = load_float(at + 8);
        if (!std::isfinite(x) || !std::isfinite(z))
            continue;
        const bool shown = c.invisible ? peek<uint8_t>(obj + c.hidden_byte) == 0 : !g_found_hidden[i];
        double d = 0;
        bool report = false;
        Origin o;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            const auto [it, added] = g_origin.try_emplace(obj);
            if (added) {
                it->second.x = x;
                it->second.z = z;
            }
            it->second.ever_hidden = it->second.ever_hidden || g_found_hidden[i] || !shown;
            d = std::hypot(x - it->second.x, z - it->second.z);
            if (c.far != 0 && shown && d > c.far && !it->second.reported) {
                it->second.reported = true;
                report = true;
            }
            o = it->second;
        }
        double& farthest = shown ? g_far_shown : g_far_hidden;
        farthest = std::max(farthest, d);
        if (report && far_n < 8)
            far[far_n++] = {obj, d, o.x, o.z, x, z, o.ever_hidden};
    }
    for (size_t k = 0; k < far_n; ++k) {
        const Far& f = far[k];
        char zone[40] = "";
        if (c.has_field_zone)
            std::snprintf(zone, sizeof(zone), ", zone 0x%" PRIx64, peek<uint64_t>(f.obj + c.field_zone));
        rt::log(rt::LogLevel::Info,
                "random-encounters: far: field Pokémon 0x%" PRIx64 " shown %.0f units from where it was first "
                "listed (%.0f, %.0f -> %.0f, %.0f), %s%s",
                f.obj, f.d, f.x0, f.z0, f.x, f.z, f.ever_hidden ? "hidden by the mod before" : "never hidden", zone);
    }
}

// grass_range: whether obj was first listed within the range of x/z.
bool near_origin(uint64_t obj, double x, double z) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_origin.find(obj);
    return it != g_origin.end() && std::hypot(x - it->second.x, z - it->second.z) <= g_config.range;
}

// "Original": the zone of the listed field Pokémon nearest to the player and
// its zone_rate (0: none known here, step_rate counts).
void update_rate_here(size_t n, uint64_t player) {
    const Config& c = g_config;
    if (c.zone_rates.empty() || g_rate.load(std::memory_order_relaxed) != kRandomEncounterOriginal)
        return;
    const double x = player != 0 ? load_float(player + c.player_position) : NAN;
    const double z = player != 0 ? load_float(player + c.player_position + 8) : NAN;
    uint64_t zone = 0;
    bool found = false;
    double best = INFINITY;
    for (size_t i = 0; std::isfinite(x) && std::isfinite(z) && i < n; ++i) {
        const uint64_t at = g_found[i] + c.object_position;
        if (!rt::access_allowed(at, 12, rt::kPageRead))
            continue;
        const double d = std::hypot(load_float(at) - x, load_float(at + 8) - z);
        if (std::isfinite(d) && d < best) {
            best = d;
            zone = c.uncount_zone_size == 4 ? peek<uint32_t>(g_found[i] + c.field_zone)
                                             : peek<uint64_t>(g_found[i] + c.field_zone);
            found = true;
        }
    }
    if (!found)
        return;  // nobody listed: the last zone's rate stays
    const auto it = c.zone_rates.find(zone);
    const uint32_t rate = it != c.zone_rates.end() ? it->second : 0;
    g_rate_here.store(rate, std::memory_order_relaxed);
    if (zone != g_zone_said) {
        g_zone_said = zone;
        if (rate != 0)
            rt::log(rt::LogLevel::Info, "random-encounters: original rate %u/256 per step in zone 0x%" PRIx64, rate,
                    zone);
        else
            rt::log(rt::LogLevel::Info,
                    "random-encounters: zone 0x%" PRIx64 " has no zone_rate, %u/256 per step (step_rate)", zone,
                    c.step_rate);
    }
}

// Gelb mode: the zone of the first spawn area of the list whose test says the
// player is inside and that has slots, 0 if none. False if the guest halted.
bool zone_here(lgp::CpuState* ctx, uint64_t scratch, uint64_t* zone) {
    const Config& c = g_config;
    *zone = 0;
    for (size_t a = 0; a < g_direct_area_count; ++a) {
        const uint64_t area = g_direct_areas[a];
        // As the spawner (Ghidra, Zufall-10): an area counts only while its
        // "active" test gives bit 0, then the player test. An object whose
        // vtable has no lifted function at a slot is no spawn area (a wrong
        // area_type): skipped, logged once per vtable. This guards only the
        // slot itself; a lifted function of another class still runs, so
        // area_type must be right.
        const uint64_t vtable = rt::load<uint64_t>(area);
        bool skip = false;
        for (const uint64_t slot : {c.area_active_slot, c.area_inside_slot}) {
            if (slot == 0)
                continue;
            const uint64_t fn = rt::load<uint64_t>(vtable + slot);
            if (rt::lookup_function(fn) == nullptr) {
                if (vtable != g_bad_area_vtable) {
                    g_bad_area_vtable = vtable;
                    rt::log(rt::LogLevel::Info,
                            "random-encounters: Gelb: object 0x%" PRIx64 " (vtable 0x%" PRIx64
                            ") has no function at slot 0x%" PRIx64 " (0x%" PRIx64 "), not a spawn area",
                            area, vtable, slot, fn);
                }
                skip = true;
                break;
            }
            ctx->sp = scratch;
            ctx->x[0] = area;
            if (!call_guest(ctx, fn))
                return false;
            if ((ctx->x[0] & 1) == 0) {
                skip = true;
                break;
            }
        }
        if (skip)
            continue;
        const uint64_t z = rt::load<uint64_t>(area + c.area_zone);
        if (c.slots.count(z) != 0) {
            *zone = z;
            return true;
        }
    }
    return true;
}

// A slot of the zone by its weight.
const Config::Slot* pick_slot(uint64_t zone) {
    const auto it = g_config.slots.find(zone);
    if (it == g_config.slots.end() || it->second.empty())
        return nullptr;
    uint32_t total = 0;
    for (const Config::Slot& s : it->second)
        total += s.weight;
    if (total == 0)
        return nullptr;
    uint32_t r;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        r = std::uniform_int_distribution<uint32_t>(0, total - 1)(g_die);
    }
    for (const Config::Slot& s : it->second) {
        if (r < s.weight)
            return &s;
        r -= s.weight;
    }
    return &it->second.back();
}

// Whether the die says "encounter" this frame (per step walked, or per frame
// without the player's position). `near`: false (grass_range, no hidden one's
// spawn point near) lets the steps there not count.
bool roll(uint64_t player, bool near = true) {
    const Config& c = g_config;
    if (!c.steps)
        return chance(c.frame_rate, 65536);
    if (player == 0 || player != g_player) {  // counts from the second reading of this player
        g_player = player;
        g_have_position = false;
        if (player == 0)
            return false;
    }
    const double x = load_float(player + c.player_position);
    const double z = load_float(player + c.player_position + 8);
    if (!std::isfinite(x) || !std::isfinite(z)) {
        g_have_position = false;
        return false;
    }
    bool hit = false;
    if (g_have_position) {
        const double d = std::hypot(x - g_x, z - g_z);
        if (d <= c.warp && near)  // more is a warp (door, scene, load): no steps
            g_walked += d;
        while (g_walked >= c.step_length) {
            g_walked -= c.step_length;
            ++g_steps;
            hit = chance(step_rate(), 256) || hit;
        }
    }
    g_x = x;
    g_z = z;
    g_have_position = true;
    return hit;
}

void on_encounter_check(lgp::CpuState* ctx, lgp::LiftedFn original, void*) {
    const Config& c = g_config;
    const bool on = g_on.load(std::memory_order_relaxed);
    bool restore = false;  // switched off with hidden ones not listed then
    if (!on && !g_was_on) {
        std::lock_guard<std::mutex> lock(g_mutex);
        restore = !g_forgotten.empty();
    }
    // The check runs only in the field: a pause of a second or more is a
    // battle, a scene or a screen waiting for input. Said on or off, so a log
    // tells whether the field came back (PC togh2: no `on (` line after
    // switching on again following a catch while off).
    const auto now = std::chrono::steady_clock::now();
    const auto gap = now - g_last_check;
    g_last_check = now;
    if (gap >= std::chrono::seconds(1) && g_check_said)
        rt::log(rt::LogLevel::Info, "random-encounters: the check runs again after %lld ms (mod %s)",
                static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(gap).count()),
                on ? "on" : "off");
    g_check_said = true;
    if (!on && !g_was_on && !restore) {
        original(ctx);
        return;
    }
    if (on && g_transition.load(std::memory_order_relaxed)) {
        if (gap < std::chrono::milliseconds(c.return_gap_ms) && ++g_transition_checks < c.transition_checks) {
            original(ctx);  // still in the transition: leave every contact as it is
            return;
        }
        g_transition.store(false, std::memory_order_relaxed);
        // The game rebuilds the field after a battle (PC Zufall-4e): objects at
        // known addresses may be new, so their age starts again.
        size_t known = 0;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (c.forget_on_return) {
                for (const auto& [obj, first] : g_hidden)
                    g_forgotten.insert(obj);
                g_hidden.clear();
                g_shown.clear();  // rebuilt too: decided again (hide_listed)
            }
            for (auto& [obj, first] : g_hidden)
                first = 0;
            known = g_hidden.size();
            // Rebuilt at the same addresses without the constructor: their
            // first listing from now is their place (far_log, grass_range).
            g_origin.clear();
        }
        rt::log(rt::LogLevel::Info,
                "random-encounters: back in the field after %u checks, hiding again (%zu hidden known%s)",
                g_transition_checks, known, c.forget_on_return ? ", forgot the others" : ", ages reset");
        g_have_position = false;  // the player may stand elsewhere now
        g_pending = 0;
    }
    // On a halt the guest is gone; its state stays for the report.
    const lgp::CpuState entry = *ctx;
    const uint64_t scratch = (entry.sp - kScratch) & ~uint64_t{0xF};
    size_t n = 0;
    uint64_t player = 0;
    if (!find_field_pokemon(ctx, scratch, entry.x[1], &n, &player))
        return;
    if (!on && !g_was_on) {  // off, with hidden ones still to restore once listed
        for (size_t i = 0; i < n; ++i)
            if (g_found_forgotten[i])
                undo_forgotten(g_found[i]);
        track_positions(n);
        *ctx = entry;
        original(ctx);
        return;
    }
    if (!on) {  // switched off: the hidden ones can be touched again
        size_t shown = 0;
        for (size_t i = 0; i < n; ++i)
            if (g_found_hidden[i] || g_found_forgotten[i]) {
                set_hidden(g_found[i], false);
                ++shown;
            }
        size_t later = 0;
        {
            // Hidden ones not in the list now (a spawn not listed yet, or
            // forgotten after a rebuild) are restored when listed later.
            std::lock_guard<std::mutex> lock(g_mutex);
            for (size_t i = 0; i < n; ++i)
                if (g_found_hidden[i])
                    g_hidden.erase(g_found[i]);
            for (const auto& [obj, first] : g_hidden)
                g_forgotten.insert(obj);
            g_hidden.clear();
            g_shown.clear();
            later = g_forgotten.size();
        }
        rt::log(rt::LogLevel::Info,
                "random-encounters: switched off, %zu hidden Pokémon shown and touchable again (%zu once listed)",
                shown, later);
        g_was_on = false;
        g_pending = 0;
        g_transition.store(false, std::memory_order_relaxed);
        *ctx = entry;
        original(ctx);
        return;
    }
    const uint32_t rate = step_rate();
    const bool just_on = !g_was_on;
    const bool hide_only = g_hide_only.load(std::memory_order_relaxed);
    if (!g_was_on || hide_only != g_hide_only_said) {
        if (hide_only)
            rt::log(rt::LogLevel::Info,
                    "random-encounters: on, hiding only (%zu field Pokémon here, new spawns %u%% hidden, no die, "
                    "hidden ones touchable)", n, c.hide ? hidden_percent() : 0u);
        else
            rt::log(rt::LogLevel::Info,
                    "random-encounters: on (%zu field Pokémon here, new spawns %u%% hidden, %u/256 per step%s)", n,
                    c.hide ? hidden_percent() : 0u, rate, c.dry_run ? ", dry run" : "");
        g_hide_only_said = hide_only;
        g_pending = 0;
        g_steps = 0;
    }
    else if (rate != g_rate_said)
        rt::log(rt::LogLevel::Info, "random-encounters: now %u/256 per step", rate);
    g_was_on = true;
    g_rate_said = rate;
    ++g_check_no;
    // Without spawn_ctor every field Pokémon may be picked; with it only the
    // hidden ones, which stay hidden (the game may reset the fields).
    size_t candidates = 0, listed_new = 0, listed_hidden = 0, shown_now = 0, byte_found = 0;
    // Nothing hidden from now on (the menu's "An"): the ones hidden before are
    // shown and touchable again once listed, as when switched off.
    const bool none_hidden = c.hide && hidden_percent() == 0;
    for (size_t i = 0; i < n; ++i) {
        if (g_found_forgotten[i])
            undo_forgotten(g_found[i]);
        if (none_hidden && g_found_hidden[i]) {
            set_hidden(g_found[i], false);
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_hidden.erase(g_found[i]);
                g_shown.insert(g_found[i]);
            }
            g_found_hidden[i] = g_found_ready[i] = false;
            ++shown_now;
        }
        if (c.hide && g_found_new[i]) {  // hide_listed: the constructor's die, now
            const bool hide = chance(hidden_percent(), 100);
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                if (hide)
                    g_hidden[g_found[i]] = g_check_no;
                else
                    g_shown.insert(g_found[i]);
            }
            g_found_hidden[i] = hide;
            g_found_ready[i] = hide && c.candidate_min_checks == 0;
            ++listed_new;
            listed_hidden += hide;
        }
        if (c.hide && !g_found_hidden[i])
            continue;
        // Whether the byte the mod wrote on the last check is still there
        // (list_log): the game may clear it, or ignore it once models are shown.
        if (c.hide && c.invisible && peek<uint8_t>(g_found[i] + c.hidden_byte) == 1)
            ++byte_found;
        if (c.hide)
            set_hidden(g_found[i], true);
        if (!c.hide || g_found_ready[i])
            g_candidates[candidates++] = g_found[i];
    }
    if (shown_now != 0)
        rt::log(rt::LogLevel::Info, "random-encounters: none hidden now, %zu hidden Pokémon shown again", shown_now);
    track_positions(n);
    if (c.list_log != 0 && (just_on || (g_check_no - 1) % c.list_log == 0)) {  // the census of find_field_pokemon
        size_t hidden = 0;
        for (size_t i = 0; i < n; ++i)
            hidden += g_found_hidden[i];
        char others[16 * 32] = "";
        size_t at = 0;
        for (size_t k = 0; k < g_type_kinds && at < sizeof(others); ++k)
            at += static_cast<size_t>(std::snprintf(others + at, sizeof(others) - at, " 0x%" PRIx64 ":%u",
                                                    g_types[k].node, g_types[k].n));
        rt::log(rt::LogLevel::Info,
                "random-encounters: list %" PRIu64 " entries, %zu field Pokémon (%zu hidden), %u spawns since the "
                "last line, others by type:%s%s", g_list_count, n, hidden,
                g_spawns.exchange(0, std::memory_order_relaxed), others, g_type_more ? " and more" : "");
        if (c.model_hook) {
            EffectStats& e = g_effect_stats;
            const auto take = [](std::atomic<uint32_t>& a) { return a.exchange(0, std::memory_order_relaxed); };
            const uint32_t covered = take(e.covered), passes = take(e.passes), emitters = take(e.emitters);
            const uint32_t off = take(e.emitters_off), shadows = take(e.shadows);
            rt::log(rt::LogLevel::Info,
                    "random-encounters: since the last line %u models covered, %u effect passes on hidden ones, %u "
                    "emitters found (%u were on, turned off), %u shadows set",
                    covered, passes, emitters, off, shadows);
        }
        if (c.invisible && !c.model_hook && hidden != 0)  // with the hook the byte is the game's alone
            rt::log(rt::LogLevel::Info,
                    "random-encounters: hidden byte found still set on %zu of the %zu hidden ones before this check",
                    byte_found, hidden);
        {  // which objects: a spawn or a rebuild shows as a new address
            char objs[8 * 40] = "";
            size_t w = 0;
            for (size_t i = 0; i < n && i < 8 && w < sizeof(objs); ++i)
                w += static_cast<size_t>(std::snprintf(
                    objs + w, sizeof(objs) - w, " 0x%" PRIx64 "%s%s", g_found[i], g_found_hidden[i] ? " hidden" : "",
                    c.invisible && peek<uint8_t>(g_found[i] + c.hidden_byte) == 1 ? " byte" : ""));  // the game's value with the hook
            if (n != 0)
                rt::log(rt::LogLevel::Info, "random-encounters: field Pokémon listed:%s%s", objs, n > 8 ? " …" : "");
        }
        if (c.far != 0) {
            rt::log(rt::LogLevel::Info,
                    "random-encounters: farthest from where first listed: shown %.0f, hidden %.0f units",
                    g_far_shown, g_far_hidden);
            g_far_shown = g_far_hidden = 0;
        }
        for (size_t a = 0; a < g_area_count; ++a) {  // each spawn area with its field Pokémon
            const uint64_t area = g_areas[a];
            const uint64_t zone = peek<uint64_t>(area + c.area_zone);
            size_t here = 0, here_hidden = 0;
            for (size_t i = 0; c.has_field_zone && i < n; ++i) {
                if (peek<uint64_t>(g_found[i] + c.field_zone) == zone) {
                    ++here;
                    here_hidden += g_found_hidden[i];
                }
            }
            char data[32] = "";
            if (c.has_area_data)
                std::snprintf(data, sizeof(data), " data %u", peek<uint32_t>(area + c.area_data));
            rt::log(rt::LogLevel::Info,
                    "random-encounters: area %zu zone 0x%" PRIx64 "%s limits %u/%u/%u, field Pokémon here %zu (%zu "
                    "hidden)%s",
                    a, zone, data, peek<uint32_t>(area + c.area_limit), peek<uint32_t>(area + c.area_limit + 4),
                    peek<uint32_t>(area + c.area_limit + 8), here, here_hidden,
                    c.has_field_zone ? "" : " (no uncount_zone_offset: not counted)");
        }
        for (size_t a = 0; a < g_point_area_count; ++a) {  // each spawn area's free spawn points
            const uint64_t area = g_point_areas[a];
            const uint64_t list = peek<uint64_t>(area + c.points);
            const uint64_t count = peek<uint64_t>(area + c.points_count);
            uint64_t used = 0;
            for (uint64_t i = 0; list != 0 && i < count && i < 256; ++i) {
                const uint64_t point = c.points_stride == 0 ? peek<uint64_t>(list + 8 * i)
                                                            : list + c.points_stride * i;
                used += point != 0 && peek<uint8_t>(point + c.point_used) != 0;
            }
            char zone[40] = "";
            if (c.has_point_area_zone)
                std::snprintf(zone, sizeof(zone), " zone 0x%" PRIx64, peek<uint64_t>(area + c.point_area_zone));
            if (count != 0 && !rt::access_allowed(list, 8, rt::kPageRead))
                rt::log(rt::LogLevel::Info,
                        "random-encounters: spawn area %zu%s points %" PRIu64 " (list 0x%" PRIx64 " unreadable)", a,
                        zone, count, list);
            else
                rt::log(rt::LogLevel::Info,
                        "random-encounters: spawn area %zu%s points %" PRIu64 " (%" PRIu64 " used, %" PRIu64 " free)",
                        a, zone, count, used, std::min(count, uint64_t{256}) - used);
        }
        if (c.point_area_type != 0)
            rt::log(rt::LogLevel::Info, "random-encounters: %zu spawn areas with points in the list%s",
                    g_point_area_count + g_point_area_more, g_point_area_more ? " (only the first 8 above)" : "");
        if (c.area_type != 0)
            rt::log(rt::LogLevel::Info, "random-encounters: %zu spawn areas in the list%s", g_area_count + g_area_more,
                    g_area_more ? " (only the first 8 above)" : "");
        if (c.duration) {
            DurationStats& st = g_duration_stats;
            const auto take = [](std::atomic<uint32_t>& a) { return a.exchange(0, std::memory_order_relaxed); };
            const uint32_t calls = take(st.calls), cut = take(st.cut);
            rt::log(rt::LogLevel::Info, "random-encounters: duration_lookup %u calls, %u cut, last %u -> %u", calls,
                    cut, st.last_in.load(std::memory_order_relaxed), st.last_out.load(std::memory_order_relaxed));
        }
        if (c.rate) {
            RateStats& st = g_rate_stats;
            const auto take = [](std::atomic<uint32_t>& a) { return a.exchange(0, std::memory_order_relaxed); };
            const uint32_t calls = take(st.calls), raised = take(st.raised), max_in = take(st.max_in);
            rt::log(rt::LogLevel::Info,
                    "random-encounters: rate_lookup %u calls, %u raised, highest rate %u, last %u -> %u", calls, raised,
                    max_in, st.last_in.load(std::memory_order_relaxed), st.last_out.load(std::memory_order_relaxed));
        }
        if (c.uncount) {
            SpawnerStats& st = g_spawner_stats;
            const auto take = [](std::atomic<uint32_t>& a) { return a.exchange(0, std::memory_order_relaxed); };
            const uint32_t calls = take(st.calls), skipped = take(st.skipped), kept = take(st.kept);
            const uint32_t live = take(st.live), left_out = take(st.left_out), by_game = take(st.written_by_game);
            rt::log(rt::LogLevel::Info,
                    "random-encounters: spawner %u calls (%u without uncount), hidden kept %u, live %u, zones left "
                    "out %u, written by the game %u, last zone 0x%" PRIx64,
                    calls, skipped, kept, live, left_out, by_game,
                    st.last_zone.load(std::memory_order_relaxed));
        }
    }
    if (listed_new != 0)
        rt::log(rt::LogLevel::Info, "random-encounters: %zu of %zu newly listed field Pokémon hidden", listed_hidden,
                listed_new);
    if (gelb_active()) {
        uint64_t zone = 0;
        if (!zone_here(ctx, scratch, &zone))
            return;
        if (zone != g_direct_zone || !g_direct_said) {
            g_direct_zone = zone;
            g_direct_said = true;
            if (zone != 0) {
                const auto r = c.zone_rates.find(zone);
                rt::log(rt::LogLevel::Info, "random-encounters: Gelb: in zone 0x%" PRIx64 ", %u/256 per step", zone,
                        r != c.zone_rates.end() ? r->second : c.step_rate);
            } else {
                rt::log(rt::LogLevel::Info, "random-encounters: Gelb: in no spawn area with slots, no steps count");
            }
        }
        if (zone != 0) {
            const auto r = c.zone_rates.find(zone);
            g_rate_here.store(r != c.zone_rates.end() ? r->second : 0, std::memory_order_relaxed);
        }
        // Only on the tiles where Gelb's battles happen (Paul 2026-11-01): the
        // area's trigger is larger than its grass (PC Zufall-10: steps on the
        // path and by the Pokémon Center counted). In grass zones a step counts
        // while the player's own "in grass" byte is set (the game sets it for
        // the grass rustle and its footstep sound, Ghidra); cave lines name
        // zones where every step inside the area counts.
        bool ground = zone != 0;
        if (ground && c.grass && c.anywhere_zones.count(zone) == 0) {
            const int in_grass = player != 0 && rt::load<uint8_t>(player + c.player_grass) != 0 ? 1 : 0;
            if (in_grass != g_direct_grass) {
                g_direct_grass = in_grass;
                rt::log(rt::LogLevel::Info, in_grass ? "random-encounters: Gelb: in the grass, steps count"
                                                     : "random-encounters: Gelb: off the grass, no steps count");
            }
            ground = in_grass != 0;
        }
        if (roll(player, ground)) {
            const Config::Slot* slot = pick_slot(zone);
            // The level is rolled evenly in the slot's range, for the dry run too.
            uint32_t level = slot != nullptr ? slot->level : 0;
            if (slot != nullptr && slot->max_level > slot->level) {
                std::lock_guard<std::mutex> lock(g_mutex);
                level = std::uniform_int_distribution<uint32_t>(slot->level, slot->max_level)(g_die);
            }
            if (slot != nullptr && c.dry_run) {
                rt::log(rt::LogLevel::Info,
                        "random-encounters: dry run: Gelb would start encounter %u (%" PRIu64
                        " steps since on): species %u level %u form %u",
                        ++g_would, g_steps, slot->species, level, slot->form);
            } else if (slot != nullptr) {
                *ctx = entry;
                bool requested = false;
                if (!wild_battles_direct(ctx, slot->species, slot->form, level, &requested))
                    return;
                if (requested) {
                    const uint32_t k = g_direct_started.fetch_add(1, std::memory_order_relaxed) + 1;
                    rt::log(rt::LogLevel::Info,
                            "random-encounters: Gelb encounter %u in zone 0x%" PRIx64 ": species %u level %u form %u",
                            k, zone, slot->species, level, slot->form);
                    g_transition.store(true, std::memory_order_relaxed);
                    g_transition_checks = 0;
                } else {
                    rt::log(rt::LogLevel::Info, "random-encounters: Gelb: wild-battles started no battle");
                }
            }
        }
    } else {
    bool near = true;
    if (c.range != 0) {  // only in the grass: near where a hidden one was first listed
        const double x = player != 0 ? load_float(player + c.player_position) : NAN;
        const double z = player != 0 ? load_float(player + c.player_position + 8) : NAN;
        const bool have = std::isfinite(x) && std::isfinite(z);
        near = false;
        for (size_t i = 0; have && i < n && !near; ++i)
            near = g_found_hidden[i] && near_origin(g_found[i], x, z);
        size_t kept = 0;
        for (size_t k = 0; near && k < candidates; ++k)
            if (near_origin(g_candidates[k], x, z))
                g_candidates[kept++] = g_candidates[k];
        candidates = kept;
    }
    update_rate_here(n, player);
    if (roll(player, near) && !hide_only)
        g_pending = kPendingFrames;
    if (g_pending != 0 && candidates != 0 && c.dry_run) {
        g_pending = 0;
        rt::log(rt::LogLevel::Info, "random-encounters: dry run: would start encounter %u (%" PRIu64
                " steps since on, %zu candidates)", ++g_would, g_steps, candidates);
    } else if (g_pending != 0 && candidates != 0) {
        g_pending = 0;
        const uint64_t obj = g_candidates[pick(candidates)];
        // The transition waits for every field Pokémon without contact, also
        // one the mod forgot whose contact stayed 0 (PC Zufall-4h).
        size_t without = 0;
        for (size_t i = 0; i < n; ++i) {
            const uint64_t comp = rt::load<uint64_t>(g_found[i] + c.contact_component);
            without += comp != 0 && rt::load<uint32_t>(comp + c.contact_count) == 0;
            set_contact(g_found[i], true);
        }
        size_t known = 0;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            known = g_hidden.size();
        }
        g_transition.store(true, std::memory_order_relaxed);
        g_transition_checks = 0;
        bool started = false;
        if (!force_contact(ctx, scratch, entry.x[1], obj, &started))
            return;
        if (!started)
            g_transition.store(false, std::memory_order_relaxed);  // hidden again from the next check
        if (started) {
            const uint32_t k = g_started.fetch_add(1, std::memory_order_relaxed) + 1;
            rt::log(rt::LogLevel::Info,
                    "random-encounters: encounter %u (object 0x%" PRIx64 ", %zu candidates, %zu of %zu listed had no "
                    "contact, %zu hidden known)", k, obj, candidates, without, n, known);
            *ctx = entry;
            ctx->x[0] = c.result_encounter;
            return;
        }
    } else if (g_pending != 0) {
        --g_pending;
    }
    }  // not Gelb mode
    // hide_contact 2: the listed hidden ones are without contact only for the
    // game's check, which then skips them; afterwards each count the mod
    // changed gets its value back, unless the game wrote one meanwhile.
    size_t held = 0;
    for (size_t i = 0; c.contact_in_check && c.hide && !hide_only && i < n; ++i) {
        if (!g_found_hidden[i])
            continue;
        const uint64_t comp = rt::load<uint64_t>(g_found[i] + c.contact_component);
        const uint32_t was = comp != 0 ? rt::load<uint32_t>(comp + c.contact_count) : 0;
        if (was == 0)
            continue;
        g_held_contact[held] = comp;
        g_held_count[held++] = was;
        rt::store<uint32_t>(comp + c.contact_count, 0u);
    }
    *ctx = entry;
    original(ctx);
    for (size_t k = 0; k < held; ++k)
        if (rt::load<uint32_t>(g_held_contact[k] + c.contact_count) == 0)
            rt::store<uint32_t>(g_held_contact[k] + c.contact_count, g_held_count[k]);
    if (ctx->halt != kHaltNone || ctx->x[0] != c.result_encounter)
        return;
    // The game's own touch (a visible one): its transition waits for the
    // hidden ones as for a forced touch (PC Zufall-4g: white screen when the
    // mod hid new spawns while it ran), so the same rules hold until the field
    // returns.
    size_t back = 0;
    for (size_t i = 0; i < n; ++i) {
        const uint64_t comp = rt::load<uint64_t>(g_found[i] + c.contact_component);
        if (comp != 0 && rt::load<uint32_t>(comp + c.contact_count) == 0) {
            set_contact(g_found[i], true);
            ++back;
        }
    }
    g_transition.store(true, std::memory_order_relaxed);
    g_transition_checks = 0;
    g_pending = 0;
    rt::log(rt::LogLevel::Info, "random-encounters: the game's own encounter, contact back for %zu", back);
}

void on_spawn_ctor(lgp::CpuState* ctx, lgp::LiftedFn original, void*) {
    const uint64_t obj = ctx->x[0];
    original(ctx);
    if (ctx->halt != kHaltNone)
        return;
    g_spawns.fetch_add(1, std::memory_order_relaxed);
    uint64_t none = 0;
    g_vtable.compare_exchange_strong(none, rt::load<uint64_t>(obj), std::memory_order_relaxed);
    // Not in a transition: the game sets up the field anew then, and on the
    // return the mod forgets what it hid (forget_on_return).
    const bool hide = g_on.load(std::memory_order_relaxed) && !g_transition.load(std::memory_order_relaxed) &&
                      chance(hidden_percent(), 100);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (hide)
            g_hidden[obj] = 0;  // a reused address starts again
        else
            g_hidden.erase(obj);
        g_forgotten.erase(obj);  // a new object: nothing of the mod's left in it
        g_origin.erase(obj);
        // Let be by the die: not decided again by the check (hide_listed); in
        // a transition or switched off it is decided when listed after it.
        if (hide || !g_on.load(std::memory_order_relaxed) || g_transition.load(std::memory_order_relaxed))
            g_shown.erase(obj);
        else
            g_shown.insert(obj);
    }
    if (hide)  // before its first update: no models; the check takes its contact once it is listed
        set_hidden(obj, true, false);
}

// The spawner (uncount): the hidden ones' zone is all ones for the call, so the
// area's count leaves them out.
void on_spawner(lgp::CpuState* ctx, lgp::LiftedFn original, void*) {
    const Config& c = g_config;
    const uint64_t vtable = g_vtable.load(std::memory_order_relaxed);
    SpawnerStats& st = g_spawner_stats;
    st.calls.fetch_add(1, std::memory_order_relaxed);
    if (c.spawns_off && gelb_active()) {  // Gelb mode: no new field Pokémon
        if (g_spawner_skipped.fetch_add(1, std::memory_order_relaxed) == 0)
            rt::log(rt::LogLevel::Info, "random-encounters: Gelb: the spawner does not run");
        return;
    }
    if (!g_on.load(std::memory_order_relaxed) || hidden_percent() >= 100 || vtable == 0) {
        st.skipped.fetch_add(1, std::memory_order_relaxed);
        original(ctx);
        return;
    }
    // g_hidden keeps destroyed objects until a return or reuse, so all of it is
    // read and only the live ones (still the field Pokémon's vtable) are limited.
    constexpr size_t kMax = 64;
    thread_local std::vector<uint64_t> all;
    all.clear();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const auto& [obj, first] : g_hidden)
            all.push_back(obj);
    }
    st.kept.fetch_add(static_cast<uint32_t>(all.size()), std::memory_order_relaxed);
    uint64_t objs[kMax], zones[kMax];
    const uint64_t ones = c.uncount_zone_size == 4 ? 0xFFFFFFFFu : ~uint64_t{0};
    size_t m = 0;
    for (const uint64_t obj : all) {
        if (m == kMax)
            break;
        if (rt::load<uint64_t>(obj) != vtable)  // gone (a destructor sets another vtable)
            continue;
        st.live.fetch_add(1, std::memory_order_relaxed);
        const uint64_t at = obj + c.uncount_zone;
        zones[m] = c.uncount_zone_size == 4 ? rt::load<uint32_t>(at) : rt::load<uint64_t>(at);
        st.last_zone.store(zones[m], std::memory_order_relaxed);
        objs[m++] = obj;
        if (c.uncount_zone_size == 4)
            rt::store<uint32_t>(at, 0xFFFFFFFFu);
        else
            rt::store<uint64_t>(at, ones);
    }
    st.left_out.fetch_add(static_cast<uint32_t>(m), std::memory_order_relaxed);
    original(ctx);
    if (ctx->halt != kHaltNone)
        return;
    for (size_t i = 0; i < m; ++i) {  // back, unless the spawner wrote the field itself
        const uint64_t at = objs[i] + c.uncount_zone;
        const bool ours = c.uncount_zone_size == 4 ? rt::load<uint32_t>(at) == 0xFFFFFFFFu
                                                   : rt::load<uint64_t>(at) == ones;
        if (!ours)
            st.written_by_game.fetch_add(1, std::memory_order_relaxed);
        else if (c.uncount_zone_size == 4)
            rt::store<uint32_t>(at, static_cast<uint32_t>(zones[i]));
        else
            rt::store<uint64_t>(at, zones[i]);
    }
}

// The spawn rate of a zone (PC Ghidra 4t: the spawner draws 0..100 once a
// frame and opens a spawn point below the rate). Hiding a share of the spawns
// leaves fewer visible ones at the same rate; so the rate is divided by the
// visible share (twice at 50 %), at most 101 (a spawn every frame). Unchanged
// with the mod off, all hidden (nothing to make up) or none hidden.
uint32_t scaled_rate(uint32_t rate, uint32_t hidden) {
    if (hidden == 0 || hidden >= 100 || rate == 0)
        return rate;
    const uint64_t scaled = (uint64_t{rate} * 100 + (100 - hidden) - 1) / (100 - hidden);
    return static_cast<uint32_t>(std::min<uint64_t>(std::max<uint64_t>(scaled, rate), 101));
}

// The pause after each spawn (PC Ghidra 4v: duration_lookup sets the
// spawner's countdown at x0 + duration_offset from encount_data, about 5 s in
// Mt. Moon; no new spawn point before it runs out). This, not the rate, sets
// the pace; it is multiplied by the visible share (half at 50 %), at least 1.
uint32_t scaled_duration(uint32_t duration, uint32_t hidden) {
    if (hidden == 0 || hidden >= 100 || duration == 0)
        return duration;
    const uint64_t scaled = (uint64_t{duration} * (100 - hidden) + 99) / 100;
    return static_cast<uint32_t>(std::max<uint64_t>(scaled, 1));
}

void on_duration_lookup(lgp::CpuState* ctx, lgp::LiftedFn original, void*) {
    const uint64_t counter = ctx->x[0] + g_config.duration_offset;
    original(ctx);
    if (ctx->halt != kHaltNone || !g_on.load(std::memory_order_relaxed) ||
        !rt::access_allowed(counter, 4, rt::kPageRead | rt::kPageWrite))
        return;
    const uint32_t duration = rt::load<uint32_t>(counter);
    const uint32_t scaled = scaled_duration(duration, hidden_percent());
    DurationStats& st = g_duration_stats;
    st.calls.fetch_add(1, std::memory_order_relaxed);
    st.last_in.store(duration, std::memory_order_relaxed);
    st.last_out.store(scaled, std::memory_order_relaxed);
    if (scaled != duration) {
        st.cut.fetch_add(1, std::memory_order_relaxed);
        rt::store<uint32_t>(counter, scaled);
    }
}

void on_rate_lookup(lgp::CpuState* ctx, lgp::LiftedFn original, void*) {
    original(ctx);
    if (ctx->halt != kHaltNone || !g_on.load(std::memory_order_relaxed))
        return;
    const uint32_t rate = static_cast<uint32_t>(ctx->x[0]);
    const uint32_t scaled = scaled_rate(rate, hidden_percent());
    RateStats& st = g_rate_stats;
    st.calls.fetch_add(1, std::memory_order_relaxed);
    st.last_in.store(rate, std::memory_order_relaxed);
    st.last_out.store(scaled, std::memory_order_relaxed);
    uint32_t max = st.max_in.load(std::memory_order_relaxed);
    while (rate > max && !st.max_in.compare_exchange_weak(max, rate, std::memory_order_relaxed)) {
    }
    if (scaled != rate) {
        st.raised.fetch_add(1, std::memory_order_relaxed);
        ctx->x[0] = scaled;
    }
}

bool read_config(const ModFile& file, Config* c, std::string* why) {
    struct Required {
        const char* name;
        uint64_t* to;
    } const required[] = {
        {"check_result_encounter", &c->result_encounter},
        {"objects_holder", &c->holder},
        {"objects_manager", &c->manager},
        {"objects_list", &c->list},
        {"objects_count", &c->count},
        {"type_interface", &c->type_interface},
        {"field_pokemon_type_got", &c->type_got},
        {"encounter_setup", &c->setup},
        {"encounter_touch", &c->touch},
        {"zone_hash_offset", &c->zone_hash},
        {"area_kind_offset", &c->area_kind},
        {"touched_byte_offset", &c->touched_byte},
        {"ref_interface", &c->ref_interface},
        {"ref_inc_slot", &c->ref_inc},
        {"ref_dec_slot", &c->ref_dec},
        {"contact_component", &c->contact_component},
        {"contact_count", &c->contact_count},
    };
    *c = Config{};
    for (const Required& r : required) {
        const auto v = file.value(r.name);
        if (!v) {
            *why = std::string("no `value ") + r.name + "` in " + file.path.filename().string();
            return false;
        }
        *r.to = *v;
    }
    auto small = [&](const char* name, uint32_t max, uint32_t* to) {
        const auto v = file.value(name);
        if (!v)
            return true;
        if (*v > max) {
            *why = std::string("value ") + name + " above " + std::to_string(max);
            return false;
        }
        *to = static_cast<uint32_t>(*v);
        return true;
    };
    if (!small("hidden_percent", 100, &c->hidden_percent) || !small("step_rate", 256, &c->step_rate) ||
        !small("frame_rate", 65536, &c->frame_rate))
        return false;
    for (const ModLine& l : file.lines) {
        if (l.directive != "zone_rate")
            continue;
        char* end = nullptr;
        const unsigned long r = std::strtoul(l.rest.c_str(), &end, 0);
        if (l.rest.empty() || end == nullptr || *end != '\0' || r == 0 || r > 256) {
            *why = "line " + std::to_string(l.line) + ": zone_rate " + l.name + " needs a rate 1–256 after the zone";
            return false;
        }
        c->zone_rates[l.number] = static_cast<uint32_t>(r);
    }
    for (const ModLine& l : file.lines) {
        if (l.directive != "slot")
            continue;
        // WEIGHT SPECIES LEVEL[-MAX] [FORM]
        uint64_t v[5] = {0, 0, 0, 0, 0};  // weight, species, level, max, form
        bool ok = true;
        unsigned got = 0;
        const char* at = l.rest.c_str();
        while (ok && *at != '\0') {
            char* end = nullptr;
            const uint64_t n = std::strtoull(at, &end, 0);
            if (end == at || got == 4) {
                ok = false;
                break;
            }
            const unsigned slot = got == 3 ? 4 : got;
            v[slot] = n;
            at = end;
            if (got == 2) {
                v[3] = n;
                if (*at == '-') {
                    v[3] = std::strtoull(at + 1, &end, 0);
                    ok = end != at + 1;
                    at = end;
                }
            }
            ++got;
            if (*at != '\0' && *at != ' ' && *at != '\t')
                ok = false;
            while (*at == ' ' || *at == '\t')
                ++at;
        }
        if (!ok || got < 3 || v[0] == 0 || v[0] > 1000 || v[1] == 0 || v[1] > 0xFFFF || v[2] == 0 || v[3] < v[2] ||
            v[3] > 100 || v[4] > 0xFFFF) {
            *why = "line " + std::to_string(l.line) + ": slot " + l.name +
                   " needs WEIGHT (1–1000) SPECIES LEVEL[-MAX] (1–100) [FORM] after the zone";
            return false;
        }
        c->slots[l.number].push_back({static_cast<uint32_t>(v[0]), static_cast<uint32_t>(v[1]),
                                      static_cast<uint32_t>(v[2]), static_cast<uint32_t>(v[4]),
                                      static_cast<uint32_t>(v[3])});
    }
    c->player_index = file.value("player_index").value_or(0);
    if (const auto t = file.value("player_type_got")) {
        c->player_by_type = true;
        c->player_type_got = *t;
    }
    if (const auto a = file.value("player_active_offset")) {
        c->player_active = true;
        c->player_active_offset = *a;
    }
    if (!small("player_active_size", 8, &c->player_active_size))
        return false;
    if (c->player_active_size != 1 && c->player_active_size != 4 && c->player_active_size != 8) {
        *why = "value player_active_size must be 1, 4 or 8";
        return false;
    }
    if (const auto p = file.value("player_position_offset")) {
        c->steps = true;
        c->player_position = *p;
    }
    const uint64_t milli = file.value("step_length_milli").value_or(100000);
    if (milli == 0) {
        *why = "value step_length_milli must not be 0";
        return false;
    }
    c->step_length = static_cast<double>(milli) / 1000.0;
    c->warp = static_cast<double>(file.value("warp_milli").value_or(milli)) / 1000.0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        // Without a seed each start rolls differently (the clock); a fixed one repeats a run.
        const auto seed = file.value("seed");
        g_die.seed(seed ? static_cast<uint32_t>(*seed)
                        : static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^
                              static_cast<uint32_t>(std::chrono::system_clock::now().time_since_epoch().count()));
    }
    c->dry_run = file.value("dry_run").value_or(0) != 0;
    c->return_gap_ms = static_cast<uint32_t>(std::min<uint64_t>(file.value("return_gap_ms").value_or(500), 60000));
    c->transition_checks =
        static_cast<uint32_t>(std::min<uint64_t>(file.value("transition_checks").value_or(3600), 1u << 30));
    c->candidate_min_checks = file.value("candidate_min_checks").value_or(60);
    c->forget_on_return = file.value("forget_on_return").value_or(1) != 0;
    // Without the hidden byte the contact is the only way to keep one untouched.
    const uint64_t hide_contact = file.value("hide_contact").value_or(0);
    c->contact_in_check = hide_contact == 2;
    c->hide_contact = hide_contact == 1 || (!c->contact_in_check && !file.value("hidden_byte_offset"));
    c->hide_listed = file.value("hide_listed").value_or(1) != 0;
    c->list_log = file.value("list_log").value_or(0);
    if (const auto z = file.value("uncount_zone_offset")) {
        c->field_zone = *z;
        c->has_field_zone = true;
        // The width matters to zone_rate and the spawner alike, so read it with the offset.
        if (!small("uncount_zone_size", 8, &c->uncount_zone_size))
            return false;
        if (c->uncount_zone_size != 4 && c->uncount_zone_size != 8) {
            *why = "value uncount_zone_size must be 4 or 8";
            return false;
        }
    }
    if (const auto a = file.value("point_area_type")) {
        const auto list = file.value("points_offset");
        const auto count = file.value("points_count_offset");
        const auto used = file.value("point_used_offset");
        if (!list || !count || !used) {
            *why = "value point_area_type needs values points_offset, points_count_offset and point_used_offset";
            return false;
        }
        c->point_area_type = *a;
        c->points = *list;
        c->points_count = *count;
        c->point_used = *used;
        c->points_stride = file.value("points_stride").value_or(0);
        if (const auto z = file.value("point_area_zone_offset")) {
            c->point_area_zone = *z;
            c->has_point_area_zone = true;
        }
    }
    if (const auto a = file.value("area_type")) {
        const auto zone = file.value("area_zone_offset");
        const auto limit = file.value("area_limit_offset");
        if (!zone || !limit) {
            *why = "value area_type needs values area_zone_offset and area_limit_offset";
            return false;
        }
        c->area_type = *a;
        c->area_zone = *zone;
        c->area_limit = *limit;
        if (const auto d = file.value("area_data_offset")) {
            c->area_data = *d;
            c->has_area_data = true;
        }
    }
    if (file.hook("spawner")) {
        const auto z = file.value("uncount_zone_offset");
        if (!z) {
            *why = "hook spawner needs value uncount_zone_offset";
            return false;
        }
        c->uncount = true;
        c->uncount_zone = *z;
    }
    if (const auto inside = file.value("area_inside_slot")) {
        if (c->area_type == 0 || c->slots.empty() || !c->steps) {
            *why = "value area_inside_slot (Gelb mode) needs value area_type, slot lines and player_position_offset";
            return false;
        }
        c->area_inside_slot = *inside;
        c->direct = true;
        c->area_active_slot = file.value("area_active_slot").value_or(0);
        if (const auto g = file.value("player_grass_offset")) {
            c->player_grass = *g;
            c->grass = true;
        }
        for (const ModLine& l : file.lines)
            if (l.directive == "cave") {
                if (!c->grass) {
                    *why = "line " + std::to_string(l.line) + ": cave lines need value player_grass_offset";
                    return false;
                }
                c->anywhere_zones.insert(l.number);
            }
    } else if (!c->slots.empty()) {
        *why = "slot lines (Gelb mode) need value area_inside_slot";
        return false;
    } else if (file.value("player_grass_offset") || file.value("area_active_slot")) {
        *why = "values player_grass_offset and area_active_slot need Gelb mode (value area_inside_slot)";
        return false;
    }
    if (const auto v = file.value("spawns_off")) {
        c->spawns_off = *v != 0;
        if (c->spawns_off && (!c->direct || !c->uncount)) {
            *why = "value spawns_off needs Gelb mode (value area_inside_slot) and hook spawner";
            return false;
        }
    }
    c->hide = file.hook("spawn_ctor").has_value();
    c->model_hook = file.hook("model_visibility").has_value();
    if (c->model_hook && !c->hide) {
        *why = "hook model_visibility needs hook spawn_ctor";
        return false;
    }
    c->effect_hook = file.hook("effect_visibility").has_value();
    if (c->effect_hook) {
        const auto lookup = file.value("emitter_lookup");
        const auto on = file.value("emitter_on_offset");
        if (!c->model_hook || !lookup || *lookup == 0 || !on) {
            *why = "hook effect_visibility needs hook model_visibility and values emitter_lookup and emitter_on_offset";
            return false;
        }
        c->emitter_lookup = *lookup;
        c->emitter_on = *on;
    }
    for (const char* name : {"emitter_1", "emitter_2", "shadow_off"}) {
        const ModLine* l = nullptr;
        for (const ModLine& m : file.lines)
            if (m.directive == "expr" && m.name == name)
                l = &m;
        if (l == nullptr)
            continue;
        const bool shadow = std::string_view(name) == "shadow_off";
        HookDump* to = shadow ? &c->shadow_off : &c->emitters[c->emitter_count];
        if (!c->model_hook || (!shadow && !c->effect_hook) || !parse_hook_expr(l->rest, to) ||
            to->reg == HookDump::kRet) {
            *why = std::string("expr ") + name + ": expected `expr " + name + " 0 EXPR` on the object's registers (x0 " +
                   "= the field Pokémon), with hook model_visibility" + (shadow ? "" : " and hook effect_visibility");
            return false;
        }
        if (shadow)
            c->has_shadow = true;
        else
            ++c->emitter_count;
    }
    c->rate = file.hook("rate_lookup").has_value();
    if (file.hook("duration_lookup")) {
        const auto d = file.value("duration_offset");
        if (!d) {
            *why = "hook duration_lookup needs value duration_offset";
            return false;
        }
        c->duration = true;
        c->duration_offset = *d;
    }
    if (const auto b = file.value("hidden_byte_offset")) {
        c->invisible = true;
        c->hidden_byte = *b;
    }
    if (const auto f = file.value("far_log_milli")) {
        const auto p = file.value("object_position_offset");
        if (!p && !c->steps) {
            *why = "value far_log_milli needs value object_position_offset or player_position_offset";
            return false;
        }
        if (*f == 0) {
            *why = "value far_log_milli must not be 0";
            return false;
        }
        c->far = static_cast<double>(*f) / 1000.0;
        c->object_position = p ? *p : c->player_position;
    }
    if (const auto g = file.value("grass_range_milli")) {
        if (!c->steps) {
            *why = "value grass_range_milli needs value player_position_offset";
            return false;
        }
        if (*g == 0) {
            *why = "value grass_range_milli must not be 0";
            return false;
        }
        c->range = static_cast<double>(*g) / 1000.0;
        c->object_position = file.value("object_position_offset").value_or(c->player_position);
    }
    if (!c->zone_rates.empty()) {
        if (!c->steps || !c->has_field_zone) {
            *why = "zone_rate needs values player_position_offset and uncount_zone_offset";
            return false;
        }
        c->object_position = file.value("object_position_offset").value_or(c->player_position);
    }
    return true;
}

bool install(const ModFile& file, const StartOptions&, std::string* why) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_hidden.clear();
        g_forgotten.clear();
        g_shown.clear();
        g_origin.clear();
    }
    g_far_shown = g_far_hidden = 0;
    g_rate_here.store(0, std::memory_order_relaxed);
    g_zone_said = ~uint64_t{0};
    g_direct_area_count = 0;
    g_direct_zone = 0;
    g_direct_said = false;
    g_direct_grass = -1;
    g_bad_area_vtable = 0;
    for (std::atomic<uint32_t>* a : {&g_effect_stats.covered, &g_effect_stats.passes, &g_effect_stats.emitters,
                                     &g_effect_stats.emitters_off, &g_effect_stats.shadows})
        a->store(0, std::memory_order_relaxed);
    g_direct_started = 0;
    g_spawner_skipped = 0;
    g_was_on = false;
    g_have_position = false;
    g_player = 0;
    g_walked = 0;
    g_pending = 0;
    g_started = 0;
    g_spawns = 0;
    for (auto* a : {&g_spawner_stats.calls, &g_spawner_stats.skipped, &g_spawner_stats.kept, &g_spawner_stats.live,
                    &g_spawner_stats.left_out, &g_spawner_stats.written_by_game})
        a->store(0, std::memory_order_relaxed);
    g_spawner_stats.last_zone = 0;
    for (auto* a : {&g_duration_stats.calls, &g_duration_stats.cut, &g_duration_stats.last_in,
                    &g_duration_stats.last_out})
        a->store(0, std::memory_order_relaxed);
    for (auto* a : {&g_rate_stats.calls, &g_rate_stats.raised, &g_rate_stats.last_in, &g_rate_stats.last_out,
                    &g_rate_stats.max_in})
        a->store(0, std::memory_order_relaxed);
    g_vtable = 0;
    g_steps = 0;
    g_would = 0;
    g_check_no = 1;
    g_transition = false;
    g_transition_checks = 0;
    g_last_check = {};
    g_check_said = false;
    g_hide_only_said = false;
    if (!read_config(file, &g_config, why))
        return false;
    if (g_config.hide && !hook_role(file, "spawn_ctor", &on_spawn_ctor, nullptr, why))
        return false;
    if (g_config.uncount && !hook_role(file, "spawner", &on_spawner, nullptr, why))
        return false;
    if (g_config.model_hook && !hook_role(file, "model_visibility", &on_model_visibility, nullptr, why))
        return false;
    if (g_config.effect_hook && !hook_role(file, "effect_visibility", &on_effect_visibility, nullptr, why))
        return false;
    if (g_config.rate && !hook_role(file, "rate_lookup", &on_rate_lookup, nullptr, why))
        return false;
    if (g_config.duration && !hook_role(file, "duration_lookup", &on_duration_lookup, nullptr, why))
        return false;
    return hook_role(file, "encounter_check", &on_encounter_check, nullptr, why);
}

}  // namespace

const ModImpl& random_encounters_mod() {
    static const ModImpl m = {
        {"random-encounters", "Pokémon Yellow",
         "On: random battles while walking where wild Pokémon live, with the chance per step, species and "
         "levels of Pokémon Yellow; no wild Pokémon stand in the field. Off: the game as it is (needs the mod "
         "file).",
         true,
         true},
        &install,
        &g_on,
    };
    return m;
}

}  // namespace lgp::mods::detail

namespace lgp::mods {

void set_random_encounter_rate(uint32_t per256) {
    detail::g_rate.store(per256 == kRandomEncounterOriginal ? per256 : per256 > 256 ? 256 : per256,
                         std::memory_order_relaxed);
}

uint32_t random_encounter_rate() {
    const uint32_t r = detail::g_rate.load(std::memory_order_relaxed);
    return r == kRandomEncounterOriginal ? r : detail::step_rate();
}

void set_random_encounter_hidden_percent(uint32_t percent) {
    detail::g_hidden_share.store(percent > 100 ? detail::kFileShare : percent, std::memory_order_relaxed);
}

uint32_t random_encounter_hidden_percent() {
    return detail::hidden_percent();
}

void set_random_encounter_hide_only(bool on) {
    detail::g_hide_only.store(on, std::memory_order_relaxed);
}

bool random_encounter_hide_only() {
    return detail::g_hide_only.load(std::memory_order_relaxed);
}

}  // namespace lgp::mods
