// Mod "wild-battles" (P8, docs/analysis/51-p8-wilde-kaempfe.md): touching a
// wild Pokémon starts a classic battle instead of the catch scene, the catch
// follows after the battle (as for the scripted encounters: Snorlax, the
// legendaries …).
//
// The mod takes the game's own scripted path. At the touch it makes the
// battle event object as the CallWildBattle script handler does (creator
// function, fields: template hash, "requested", switches), drops the
// creator's reference and skips encounter_touch. The object's update then
// looks up the template record by its hash (static_battle_lookup); the mod
// puts species, level … of the touched Pokémon into it and back after the
// game copied it (record_fill). Battle, catch, experience and box run as for
// a scripted battle (such a battle gives experience only with expfix,
// below). When the event's update is done, the mod does what the waiting
// script would do next (fade the field in: end_K, end_call).
// Link battles (ldn) take another path and stay untouched; encounter_touch has
// one caller, the field update.
//
// Live (ModInfo::live): the menu switches it while the game runs; the switch
// is read at the touch, a battle already requested runs to its end.
//
// Lines in wild-battles.cfg (our own names; the numbers are game data and
// stay in $LGP_PRIVATE_DIR):
//   hook encounter_touch     runs when the player touches a wild Pokémon and
//                            the catch scene would start (required);
//                            x2 = the encounter object (or a reference to it)
//   value redirect N         0 (default) = log the touches only, 3 = battle
//                            (1 and 2 were probes, P8.2b–f, now refused)
//   hook event_create VA     the creator of the battle event as the handler
//                            calls it: x0 = start_x0, x1 = start_x1, x8 = out
//   addr start_x0 VA [STEPS] its x0 (a manager) and x1 (names from the probes
//   addr start_x1 VA [STEPS] with the start function)
//   addr event_from_create N [STEPS]  the event object from the word the
//                            creator leaves at x8 (a process handle): word +
//                            N, then the steps; e.g. `0 >0x60-0x50`; without
//                            the line the word itself
//   value event_K_offset N   K = 1…8: written into it at the touch (hash =
//   value event_K N          template_hash, "requested", the switches);
//   value event_K_size N     size default 4
//   value event_release_interface N  drop the creator's reference after the
//   value event_release_slot N       fields (as the handler; the process tree
//                                    keeps its own): x0 = handle + N, call
//                                    vtable(x0) + slot
//   value event_touch 1      run encounter_touch as well (diagnosis)
//   value touch_return N     x0 when encounter_touch is skipped (default:
//                            x0 as it came)
//   hook static_battle_lookup  returns the record for a hash in x0; the mod
//   value template_hash N      changes the template's record once per touch
//   value lookup_hash_reg N    register of the hash (default x0)
//   value lookup_hash_ptr 1    it holds a pointer to the hash
//   hook record_fill           copies the record (x0 = object, x1 = record);
//                              the template gets its bytes back after it
//   value record_K_offset N  K = 1…8: written into the template record:
//   value record_K N         a constant (record_K_size bytes, default 4)
//   from record_K SIZE EXPR  or SIZE bytes read at EXPR, which starts at an
//                            entry register of encounter_touch (x0…x30) or at
//                            `obj` (the encounter object): level …
//   call record_K VA EXPR    or x0 of the guest function at VA, called with
//                            x0 = EXPR (a getter: the species from the key)
//   value object_load 1      `obj` is the word at x2 of encounter_touch, not
//                            x2 itself
//   hook battle_event_update  the battle event's update; when it returns
//   value end_result N        N (default 1, "done") for the touch's object
//   addr end_base VA [STEPS]  (x0 = object or handle), the mod writes end_K
//   value end_K_offset N …    at end_base + offset and calls end_call with
//   value end_call VA         x0…x2 = addr end_x0…end_x2: what the waiting
//   addr end_x0 VA [STEPS]    script would do next (fade the field in)
//   patch battle_time VA      the MOVZ that gives event battles their time
//   value battle_time N       limit (300 s, "Kampfzeit"); its first pass
//                             after the touch (our battle's start) yields N
//                             (default 0 = no limit, no counter), the game's
//                             own event battles keep theirs (lgp_lift needs
//                             the patch site: --mods)
//   call team_fit VA ADDR [STEPS]  a getter of the team (x0 = ADDR with
//   value team_fit_x1 N       steps, e.g. the party; x1, x2 default 0; spaces
//   value team_fit_x2 N       between the steps allowed): x0 back
//   value direct_species K    K = 1…8: the record_K that random-encounters'
//   value direct_form K       Gelb mode fills with the species, form and
//   value direct_level K      level it rolled (wild_battles_direct, no field
//                             Pokémon to read them from); without
//                             direct_species no such battle. The other
//                             record_K and event_K must then be constants, or
//   value direct_record_K V   give this constant for a direct battle
//   value team_down N         == N (default 0)
//                             means no Pokémon can fight: a touch then gets
//                             the catch scene as before (no battle the team
//                             would lose at once)
//   hook lose_create VA       the defeat (P8.10): when our battle ended with
//   value defeat_K_offset N   the team down (team_fit), after our end make
//   value defeat_K N          the defeat event as the CallBattleLose handler
//   value lose_release 1      does: the creator with x0 = start_x0, x1 =
//                             start_x1, x8 = out; defeat_K fields (K = 1…8,
//                             size default 4) in the object as
//                             event_from_create says, with lose_release the
//                             creator's reference dropped as event_release
//                             says. On the PC the game then warps to the
//                             Pokémon Center and heals the team. Without the
//                             line the player stays where the battle was
//   addr partner_state VA [STEPS]  a byte that our battle needs as
//   value partner_set N       partner_set (default 1): set at the touch, the
//                             old value back when our battle is done (P8.19:
//                             before the story enables the partner Pokémon
//                             the byte is 0 and the battle's opening crashes
//                             on a model that is not there)
//   value expfix_reg N        battle experience (P8.19): the event battle's
//   value expfix_flags_offset N  start marks its setup "no experience";
//   value expfix_flags_clear N   at our pass of patch battle_time xN is the
//   value expfix_byte_offset N   battle, and at the next pass of
//   hook expfix_door VA       expfix_door (a function that runs while the
//                             battle view is built) the mod clears
//                             flags_clear in the u32 at battle +
//                             flags_offset and the byte at battle +
//                             byte_offset: the battle then gives
//                             experience, level-ups and new moves as a
//                             trainer battle does (needs patch battle_time)
//   hook recall_end_start VA  optional with expfix: the start of the recall at
//                             a K.O. (x0 = the handler's view …). The game
//                             skips it for a wild battle's opponent (the
//                             bit expfix clears marks the wild battle): it
//                             faints and is gone. In our battle the mod sets
//                             the cleared bits again for this call only, so
//                             the opponent goes as in a wild battle instead
//                             of being drawn into a ball (ee620, Fang-8)
//   hook end_view_wild VA     the same for the battle's end view (Fang-12:
//                             with the bit it fades to white instead of
//                             black and unloads the models, as in a wild
//                             battle) and the server's K.O. message
//   hook ko_message_wild VA   ("… was defeated" comes only without the bit:
//                             with this door it is left out as in the
//                             game's wild battles; without it ours keep
//                             it); both optional with expfix. The
//                             experience reads the bit elsewhere and keeps
//                             it cleared
//   value ko_seq_text VA      with hook recall_end_start: the opponent's K.O.
//   path ko_seq 0 TEXT        in our battle plays TEXT (a sequence in a data
//                             mod) in place of the recall ee620: the door
//                             leaves the bit cleared and writes TEXT over the
//                             recall's path text at VA (rodata) for the call
//                             only (the call copies and opens it, Fang-36);
//                             faint, sink or shrink, no ball, as in Yellow.
//                             TEXT at most as long as the original
//   from ko_side SIZE xN…     optional: only when the SIZE bytes at the
//   value ko_side N           entry expression are N (the opponent's slot;
//                             SIZE 0: the expression's value itself, e.g.
//                             `from ko_side 0 x1`); other calls set the bit
//                             as before
//   hook loss_amount VA       the money lost on a defeat (P8.21; x0 = the
//                             battle's main object, w0 = the amount; the
//                             client's defeat flow calls it for wild and
//                             trainer battles): 0 in our battles, no money
//                             lost and no message; the game's own battles
//                             (Snorlax too) as before (needs patch battle_time)
//   hook part_find_guard VA   the part lookup of a battle character model
//   value registry_find VA    (x0 = character) halts when the character's
//   value part_holder N       model resource is not in the battle's
//   value part_registry N     registry (the experience effect in our
//   value part_key N          battles). The mod asks registry_find first
//   value part_alt_switch N   (x0 = [[x0 + holder] + registry], x1 = &key,
//   value part_alt_key N      x8 = out; key = [x0 + key], or [x0 + alt_key]
//   value part_alt_count N    when the byte at x0 + alt_switch is set and
//   value part_release_interface N  [[x0 + alt_key] + alt_count] != 0),
//   value part_release_slot N  drops the reference it gets (x0 = out +
//                             interface, call vtable(x0) + slot) and
//                             without one returns 0 like a missing part
//                             (the caller checks); a small part of the
//                             effect is then not drawn. Only in our battle
//                             (from patch battle_time until its event is
//                             done); other battles keep the game's lookup
//   hook post_battle VA       evolution after our battles (P8.19d): the
//   value post_state_offset N  process after a battle (x0) compares the
//   value post_flags_offset N  levels in its first state (u32 at
//   value post_catch_state N   post_state_offset = 0) into one "levelled up"
//                             byte per member (6 at post_flags_offset); when
//                             the catch scene follows (it goes on with
//                             post_catch_state) it never evolves them. The
//   hook capture_process VA   mod ORs those bytes into the catch process's
//   value capture_state_offset N   own (6 at capture_flags_offset) when it
//   value capture_flags_offset N   enters capture_evolve_state (u32 at
//   value capture_counter_offset N capture_state_offset); when the player
//   value capture_evolve_state N   gave up (the state before jumps straight
//   value capture_skip_state N     to capture_skip_state) it runs
//                             capture_evolve_state first (member counter at
//                             capture_counter_offset from 0) and then goes
//                             on with capture_skip_state, past the state
//                             that boxes the Pokémon. Only after our
//                             battles with experience (expfix_door changed
//                             the battle); needs expfix
//   hook catch_exp_amount VA  the experience of the catch scene (returns it
//                             in w0): 0 in the catch scene after our battle
//                             with experience (until the next touch or
//                             battle), so it gives none twice; other
//                             catches keep theirs (needs hook post_battle)
//   value post_end_state N    evolution after a catch in the battle (P8.20):
//   value post_evolve_state N  the process evolves only for trainer battles
//                             (its post_evolve_state); ours go from state 0
//                             to post_catch_state (won) or post_end_state
//                             (caught with a ball, fled). When a member
//                             levelled up and our battle went on with
//                             post_end_state, the mod sets post_evolve_state
//                             (needs hook post_battle)
//   from post_result SIZE obj…   caught in the battle (P8.20, way A): the
//   value post_caught_result N   battle's result (SIZE bytes, obj = the
//   value capture_result_offset N  post-battle process) is post_caught_result
//   value capture_caught_state N   after a ball caught the Pokémon; the
//                             process then ends without catch scene, dex
//                             and box. The mod goes on with
//                             post_catch_state instead of post_end_state and
//                             after the first update of the catch process
//                             sets its result (u32 at capture_result_offset)
//                             to 0 = caught and its state to
//                             capture_caught_state, past the throw; the
//                             level-ups carry over as after a catch scene
//                             (needs post_end_state). This holds for every
//                             battle with that result, the game's own wild
//                             ones too (legendaries, Snorlax) and with the
//                             menu's switch off (battle-balls); the level-ups
//                             only for ours
//   value knockout_result N   won by K.O. with battle-balls started (P8,
//                             Paul 23.10.: the Pokémon is gone, as in
//                             Yellow): no catch scene; after the first update
//                             of the catch process the mod sets its result to
//                             N (the game's "given up") and its state to
//                             capture_skip_state, with a level-up first
//                             capture_evolve_state (needs from post_result);
//   value knockout_always 1   the same without battle-balls (diagnosis, tests)
//   value capture_entry_state N  both of the above not after the first update
//                             of the catch process but once its state is N
//                             (past its start state: fade-out and music push,
//                             Fang-10; needs from post_result)
//   call capture_entry_ready VA ADDR [STEPS]  and there only once this
//   value capture_entry_ready_x1 N  guest function (x0 = ADDR with steps,
//                             x1 default 0) returns bit 0 set: the fader
//                             is done (Fang-11; needs capture_entry_state);
//                             asked before the update, which goes on into
//                             the Go app once the fader is done
//   value post_as_game 1      diagnosis: the post-battle process treats our
//                             battles as the game's own (no level-ups carried,
//                             the catch keeps its experience), to check that
//                             path without a legendary in reach
//   hook ball_choice VA       Let's Go catch from the battle menu (Paul
//   value ball_choice_item_reg N  2026-11-02): with battle-balls started and
//   value ball_choice_kind VA  battle-throw not on (its menu row "Catch
//   value ball_choice_field N  style in battle" on "Let's Go"), a ball chosen
//   value ball_choice N       in our wild battle (the game's item data
//   value ball_choice_return N  getter ball_choice_kind, called with x0 = the
//   value ball_choice_always 1  item number, the low 16 bits of entry
//                             register ball_choice_item_reg, and x1 =
//                             ball_choice_field, returns ball_choice in w0:
//                             the item's kind) skips the game's item handler
//                             and returns ball_choice_return (default 1, the
//                             escape item's way): the battle ends as fled,
//                             the ball is not used up there, and the
//                             post-battle process goes on with
//                             post_catch_state instead of post_end_state,
//                             into the Let's Go catch scene, which keeps its
//                             experience. Other items, the game's own battles
//                             and the classic throw keep the handler. Needs
//                             post_end_state; ball_choice_always 1: also
//                             without battle-balls (diagnosis, tests)
//   (`value defeat`, a defeat switch in the battle event, is gone: its warp
//   broke the scene, P8.9. `hook catch_exp` is gone too: it added the catch
//   scene's experience to our battles, which give theirs with expfix now, P8.11.)
// STEPS as in LGP_HOOK_DUMP: `>OFF` loads the 64-bit word at value + OFF
// (a null word ends the chain with 0), `+OFF` adds, `-OFF` subtracts; e.g.
// `addr start_x1 0x7101234560 >0+0x18`, `from record_2 1 obj>0x58+0x10`.
// Lines of the probes (hook wild_battle_start, object_K, process_K …) are
// left alone.
#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstring>
#include <string>

#include "lgp/lifted/cpu_state.hpp"
#include "lgp/runtime/guest_mem.hpp"
#include "lgp/runtime/hooks.hpp"
#include "lgp/runtime/log.hpp"
#include "lgp/runtime/patch.hpp"
#include "mod_impl.hpp"

namespace lgp::mods::detail {

namespace {

constexpr unsigned kMaxSteps = 8;
constexpr unsigned kMaxWrites = 8;

std::atomic<uint32_t> g_battles_started{0};
std::atomic<uint32_t> g_battles_done{0};

// A guest address from the file: a number, then loads and offsets.
struct Expr {
    static constexpr int kConstant = -1;
    static constexpr int kObject = 31;
    int reg = kConstant;  // where it starts: the number, an entry register or the object
    uint64_t base = 0;
    struct Step {
        bool load = false;
        bool sub = false;  // `-OFF`
        uint64_t offset = 0;
    } steps[kMaxSteps];
    unsigned count = 0;
};

struct Write {
    uint64_t offset = 0;
    uint64_t value = 0;
    uint32_t size = 0;
    bool from = false;  // size bytes read at src instead of value
    bool call = false;  // or x0 of the guest function fn, called with x0 = src
    uint64_t fn = 0;
    Expr src;
};
constexpr uint32_t kMaxFrom = 0x20;

constexpr uint64_t kScratch = 0x100;

struct Config {
    uint32_t redirect = 0;
    Expr x0;  // of event_create
    Expr x1;
    unsigned lookup_hash_reg = 0;
    bool lookup_hash_ptr = false;  // the register holds a pointer to the hash
    uint64_t template_hash = 0;
    Write record_writes[kMaxWrites];
    unsigned record_count = 0;
    // wild_battles_direct: record indices (0-based) of species, form, level
    // (-1: none) and the constants of the other records for it.
    int direct_species = -1, direct_form = -1, direct_level = -1;
    uint64_t direct_value[kMaxWrites] = {};
    bool direct_ok = false;
    bool object_load = false;
    bool has_return = false;
    uint64_t touch_return = 0;
    uint64_t event_create = 0;
    Expr event_from_create;       // the event object from the word in x8 (default: the word)
    bool event_release = false;   // drop the creator's reference after the fields
    uint64_t event_release_interface = 0;
    uint64_t event_release_slot = 0;
    Write event_writes[kMaxWrites];
    unsigned event_count = 0;
    bool event_touch = false;
    // When the battle event's update (hook battle_event_update) returns
    // end_result for our object: writes and a call the waiting script would
    // have made (fade the field in …).
    bool end = false;
    uint64_t end_result = 1;
    Expr end_base;
    Write end_writes[kMaxWrites];
    unsigned end_count = 0;
    uint64_t end_call = 0;
    Expr end_x[3];
    unsigned end_args = 0;
    uint64_t battle_time_site = 0;  // patch battle_time (0: none)
    uint64_t battle_time = 0;
    uint64_t team_fit = 0;  // call team_fit (0: none)
    Expr team_x0;
    uint64_t team_x1 = 0;
    uint64_t team_x2 = 0;
    uint64_t team_down = 0;
    uint64_t lose_create = 0;  // hook lose_create (0: none)
    Write defeat_writes[kMaxWrites];
    unsigned defeat_count = 0;
    bool lose_release = false;
    bool partner = false;  // addr partner_state
    Expr partner_state;
    uint8_t partner_set = 1;
    uint64_t expfix_reg = 0;  // value expfix_reg (0: off)
    uint64_t expfix_flags_offset = 0;
    uint64_t expfix_flags_clear = 0;
    uint64_t expfix_byte_offset = 0;
    bool part_guard = false;  // hook part_find_guard
    bool loss_amount = false;  // hook loss_amount
    uint64_t registry_find = 0;
    uint64_t part_holder = 0;
    uint64_t part_registry = 0;
    uint64_t part_key = 0;
    uint64_t part_alt_switch = 0;
    uint64_t part_alt_key = 0;
    uint64_t part_alt_count = 0;
    uint64_t part_release_interface = 0;
    uint64_t part_release_slot = 0;
    // hook post_battle + capture_process (0: off)
    uint64_t post_state_offset = 0, post_flags_offset = 0, post_catch_state = 0;
    uint64_t capture_state_offset = 0, capture_flags_offset = 0, capture_counter_offset = 0;
    uint64_t capture_evolve_state = 0, capture_skip_state = 0;
    bool post_evolve = false;  // value post_end_state + post_evolve_state
    bool post_as_game = false;  // value post_as_game 1 (diagnosis)
    bool caught = false;       // from post_result
    Expr post_result;
    uint32_t post_result_size = 4;
    uint64_t post_caught_result = 0, capture_result_offset = 0, capture_caught_state = 0;
    uint64_t post_end_state = 0, post_evolve_state = 0;
    bool knockout = false, knockout_always = false;  // value knockout_result
    bool capture_entry = false;  // value capture_entry_state
    uint64_t capture_entry_state = 0;
    uint64_t capture_ready = 0;  // call capture_entry_ready (0: none)
    Expr capture_ready_x0;
    uint64_t capture_ready_x1 = 0;
    uint64_t knockout_result = 0;
    bool catch_exp_amount = false;  // hook catch_exp_amount
    uint64_t ko_seq_text = 0;  // value ko_seq_text + path ko_seq (0: off)
    std::string ko_seq;
    bool ko_side = false;  // from ko_side + value ko_side
    Expr ko_side_expr;
    uint32_t ko_side_size = 4;
    uint64_t ko_side_value = 0;
    bool ball_choice = false;  // hook ball_choice + from ball_choice + value ball_choice
    bool ball_choice_always = false;
    uint64_t ball_choice_item_reg = 0, ball_choice_kind = 0, ball_choice_field = 0;
    uint64_t ball_choice_value = 0, ball_choice_return = 1;
};

Config g_config;
std::atomic<uint32_t> g_encounters{0};
std::atomic<bool> g_on{false};  // ModImpl::on: the menu's switch

// The record fields of the last touch, put into the template
// record when the event object looks it up, and the template's own bytes,
// put back after the fill (the table may stay loaded for a real battle).
struct Pending {
    uint8_t bytes[kMaxWrites][kMaxFrom];
    uint64_t record = 0;  // template record changed, 0 = none
    uint8_t saved[kMaxWrites][kMaxFrom];
};
Pending g_pending;
// The battle event of the last touch (object and handle), until its update ends.
std::atomic<uint64_t> g_event{0};
std::atomic<uint64_t> g_handle{0};
std::atomic<bool> g_pending_touch{false};
// From the touch that requested our battle to its start (battle_time) or end.
std::atomic<bool> g_in_battle{false};
// addr partner_state: the byte changed for our battle (0 = none) and its old value.
std::atomic<uint64_t> g_partner{0};
uint8_t g_partner_old = 0;
// expfix: our battle from patch battle_time until expfix_door changes it.
std::atomic<uint64_t> g_exp_battle{0};
// Our battle runs: from its start (patch battle_time) until its event is done,
// the next touch or the start of the game's own event battle; part_find_guard
// and loss_amount act only then.
std::atomic<bool> g_our_battle{false};
// From expfix_door changing our battle until its post-battle process starts.
std::atomic<bool> g_exp_ours{false};
// The battle expfix changed and the bits it cleared, for recall_end_start
// (until the next touch or our event's end).
std::atomic<uint64_t> g_flags_battle{0};
std::atomic<uint32_t> g_flags_cleared{0};
// Evolution after our battles: the post-battle bytes (one per member), the
// catch scene after our battle until the next touch or battle, the catch
// process that took the bytes over and whether it runs the evolution in place
// of the skip.
constexpr unsigned kMembers = 6;
std::atomic<uint64_t> g_levelled{0};
std::atomic<bool> g_our_catch{false};
std::atomic<uint64_t> g_carry_process{0};
std::atomic<bool> g_carry_skipped{false};
// Caught in our battle: the next catch process starts past the throw.
std::atomic<bool> g_capture_caught{false};
// Won by K.O. with balls in the bag: the next catch process ends as given up.
std::atomic<bool> g_capture_knockout{false};
// hook ball_choice: a ball was chosen in our battle and the battle ends as
// fled; the post-battle process goes on into the Let's Go catch scene.
std::atomic<bool> g_ball_go{false};

uint64_t eval(const Expr& e, const lgp::CpuState* entry = nullptr, uint64_t object = 0) {
    uint64_t v = e.base;
    if (e.reg == Expr::kObject)
        v = object;
    else if (e.reg != Expr::kConstant)
        v = entry->x[e.reg];
    for (unsigned i = 0; i < e.count; ++i) {
        const Expr::Step& st = e.steps[i];
        if (st.load) {
            v = rt::load<uint64_t>(v + st.offset);
            if (v == 0)  // a null pointer stays null (no battle event: catch scene as before)
                return 0;
        } else {
            v = st.sub ? v - st.offset : v + st.offset;
        }
    }
    return v;
}

// The bytes of w (w.size of them) into out; false if a call halted the guest.
// Calls run on ctx with sp = scratch.
bool resolve(const Write& w, lgp::CpuState* ctx, const lgp::CpuState& entry, uint64_t object, uint64_t scratch,
             uint8_t* out) {
    uint64_t v = w.value;
    if (w.from) {
        const uint64_t src = eval(w.src, &entry, object);
        for (uint32_t i = 0; i < w.size; ++i)
            out[i] = rt::load<uint8_t>(src + i);
        return true;
    }
    if (w.call) {
        ctx->sp = scratch;
        ctx->x[0] = eval(w.src, &entry, object);
        if (!call_guest(ctx, w.fn))
            return false;
        v = ctx->x[0];
    }
    for (uint32_t i = 0; i < w.size; ++i)
        out[i] = static_cast<uint8_t>(v >> (8 * i));
    return true;
}

bool write_value(uint64_t va, const Write& w, lgp::CpuState* ctx, const lgp::CpuState& entry, uint64_t object,
                 uint64_t scratch) {
    uint8_t bytes[kMaxFrom];
    if (!resolve(w, ctx, entry, object, scratch, bytes))
        return false;
    for (uint32_t i = 0; i < w.size; ++i)
        rt::store<uint8_t>(va + i, bytes[i]);
    return true;
}

// call team_fit on ctx (sp = scratch): *down = no Pokémon can fight. False
// if the call halted the guest. Without the line the team always can.
bool team_down(lgp::CpuState* ctx, uint64_t scratch, bool* down) {
    const Config& c = g_config;
    *down = false;
    if (c.team_fit == 0)
        return true;
    ctx->sp = scratch;
    ctx->x[0] = eval(c.team_x0);
    ctx->x[1] = c.team_x1;
    ctx->x[2] = c.team_x2;
    if (!call_guest(ctx, c.team_fit))
        return false;
    *down = ctx->x[0] == c.team_down;
    return true;
}

// addr partner_state: the byte to partner_set for our battle (once: a battle
// that never reported done keeps the value from before it).
void set_partner(uint32_t n) {
    const Config& c = g_config;
    if (!c.partner || g_partner.load(std::memory_order_relaxed) != 0)
        return;
    const uint64_t va = eval(c.partner_state);
    if (va == 0) {
        rt::log(rt::LogLevel::Warn, "wild-battles: encounter %u: no partner state (null on the way)", n);
        return;
    }
    g_partner_old = rt::load<uint8_t>(va);
    if (g_partner_old == c.partner_set)
        return;
    rt::store<uint8_t>(va, c.partner_set);
    g_partner.store(va, std::memory_order_relaxed);
    rt::log(rt::LogLevel::Info, "wild-battles: encounter %u: partner state %u instead of %u for our battle", n,
            c.partner_set, g_partner_old);
}

void restore_partner() {
    const uint64_t va = g_partner.exchange(0, std::memory_order_relaxed);
    if (va == 0)
        return;
    rt::store<uint8_t>(va, g_partner_old);
    rt::log(rt::LogLevel::Info, "wild-battles: partner state %u again", g_partner_old);
}

// The battle event as the CallWildBattle handler makes it, with the record
// bytes in g_pending (filled by the caller). *requested: an event exists.
// False if a call halted the guest. Calls run on ctx with sp = scratch.
bool request_battle(lgp::CpuState* ctx, const lgp::CpuState& entry, uint64_t object, uint64_t scratch, uint32_t n,
                    bool* requested) {
    const Config& c = g_config;
    *requested = false;
    // As the CallWildBattle handler: x0 = the manager, x1, x8 = where the reference goes.
    const uint64_t out = scratch + 0x20;
    rt::store<uint64_t>(out, 0);
    ctx->sp = scratch;
    ctx->x[0] = eval(c.x0);
    ctx->x[1] = eval(c.x1);
    ctx->x[8] = out;
    if (!call_guest(ctx, c.event_create))
        return false;
    const uint64_t handle = rt::load<uint64_t>(out);
    uint64_t event = 0;
    if (handle != 0) {
        Expr e = c.event_from_create;
        e.base += handle;
        event = eval(e);
    }
    if (event == 0) {
        rt::log(rt::LogLevel::Warn, "wild-battles: encounter %u: no battle event object, none requested", n);
        return true;
    }
    g_handle.store(handle, std::memory_order_relaxed);
    for (unsigned k = 0; k < c.event_count; ++k)
        if (!write_value(event + c.event_writes[k].offset, c.event_writes[k], ctx, entry, object, scratch))
            return false;
    g_event.store(event, std::memory_order_relaxed);
    set_partner(n);
    if (c.event_release) {
        // The process tree keeps its own reference (as after the handler).
        ctx->sp = scratch;
        ctx->x[0] = handle + c.event_release_interface;
        const uint64_t fn = rt::load<uint64_t>(rt::load<uint64_t>(ctx->x[0]) + c.event_release_slot);
        if (!call_guest(ctx, fn))
            return false;
    }
    g_pending_touch.store(true, std::memory_order_release);
    g_in_battle.store(true, std::memory_order_release);
    g_battles_started.fetch_add(1, std::memory_order_relaxed);
    rt::log(rt::LogLevel::Info,
            "wild-battles: encounter %u: battle event 0x%" PRIx64 " requested (%u fields), record waits for 0x%" PRIx64, n,
            event, c.event_count, c.template_hash);
    *requested = true;
    return true;
}

void on_encounter_touch(lgp::CpuState* ctx, lgp::LiftedFn original, void*) {
    const uint32_t n = g_encounters.fetch_add(1, std::memory_order_relaxed) + 1;
    const Config& c = g_config;
    g_our_catch = false;  // a new catch scene or battle: its own experience
    g_our_battle = false;
    g_capture_caught = false;
    g_capture_knockout = false;
    g_ball_go = false;
    g_exp_battle = 0;
    g_flags_battle = 0;
    const bool on = g_on.load(std::memory_order_relaxed);
    rt::log(rt::LogLevel::Info, "wild-battles: encounter %u (x0=0x%" PRIx64 " x1=0x%" PRIx64 " x2=0x%" PRIx64 ")%s", n,
            ctx->x[0], ctx->x[1], ctx->x[2], !on ? ", switched off" : c.redirect == 0 ? ", log only" : "");
    if (!on || c.redirect == 0) {
        original(ctx);
        return;
    }
    // On a halt the guest is gone; its state stays for the report.
    const lgp::CpuState entry = *ctx;
    const uint64_t scratch = (entry.sp - kScratch) & ~uint64_t{0xF};
    const uint64_t object = c.object_load ? rt::load<uint64_t>(entry.x[2]) : entry.x[2];
    bool down = false;
    if (!team_down(ctx, scratch, &down))
        return;
    if (down) {
        rt::log(rt::LogLevel::Info, "wild-battles: encounter %u: no Pokémon can fight, catch scene as before", n);
        *ctx = entry;
        original(ctx);
        return;
    }
    for (unsigned k = 0; k < c.record_count; ++k)
        if (!resolve(c.record_writes[k], ctx, entry, object, scratch, g_pending.bytes[k]))
            return;
    bool requested = false;
    if (!request_battle(ctx, entry, object, scratch, n, &requested))
        return;
    *ctx = entry;
    if (!requested)
        original(ctx);
    else if (c.event_touch)
        original(ctx);
    else
        ctx->x[0] = c.has_return ? c.touch_return : entry.x[0];
}

// static_battle_lookup(…, hash) → record.
void on_lookup(lgp::CpuState* ctx, lgp::LiftedFn original, void*) {
    const Config& c = g_config;
    const uint64_t reg = ctx->x[c.lookup_hash_reg];
    const uint64_t hash = c.lookup_hash_ptr ? rt::load<uint64_t>(reg) : reg;
    original(ctx);
    const uint64_t record = ctx->x[0];
    if (hash != c.template_hash || record == 0 || !g_pending_touch.exchange(false, std::memory_order_acquire))
        return;
    for (unsigned k = 0; k < c.record_count; ++k) {
        const Write& w = c.record_writes[k];
        for (uint32_t i = 0; i < w.size; ++i) {
            g_pending.saved[k][i] = rt::load<uint8_t>(record + w.offset + i);
            rt::store<uint8_t>(record + w.offset + i, g_pending.bytes[k][i]);
        }
    }
    g_pending.record = record;
    rt::log(rt::LogLevel::Info, "wild-battles: template record 0x%" PRIx64 " for the touched Pokémon (%u fields)", record,
            c.record_count);
}

// After the fill copied the record, the template gets its bytes back.
void on_fill(lgp::CpuState* ctx, lgp::LiftedFn original, void*) {
    original(ctx);
    const uint64_t record = g_pending.record;
    if (record == 0 || ctx->halt != kHaltNone)
        return;
    const Config& c = g_config;
    for (unsigned k = 0; k < c.record_count; ++k)
        for (uint32_t i = 0; i < c.record_writes[k].size; ++i)
            rt::store<uint8_t>(record + c.record_writes[k].offset + i, g_pending.saved[k][i]);
    g_pending.record = 0;
}

// hook lose_create: the defeat event as the CallBattleLose handler makes it.
// False if a call halted the guest.
bool create_defeat(lgp::CpuState* ctx, const lgp::CpuState& done, uint64_t scratch) {
    const Config& c = g_config;
    const uint64_t out = scratch + 0x20;
    rt::store<uint64_t>(out, 0);
    ctx->sp = scratch;
    ctx->x[0] = eval(c.x0);
    ctx->x[1] = eval(c.x1);
    ctx->x[8] = out;
    if (!call_guest(ctx, c.lose_create))
        return false;
    const uint64_t handle = rt::load<uint64_t>(out);
    if (handle == 0) {
        rt::log(rt::LogLevel::Warn, "wild-battles: lose_create left no handle");
        return true;
    }
    // The object only for defeat_K fields (CallBattleLose writes none).
    uint64_t object = 0;
    if (c.defeat_count != 0) {
        Expr e = c.event_from_create;
        e.base += handle;
        object = eval(e);
        if (object == 0)
            rt::log(rt::LogLevel::Warn, "wild-battles: no defeat event object (handle 0x%" PRIx64 "), no fields",
                    handle);
        else
            for (unsigned k = 0; k < c.defeat_count; ++k)
                if (!write_value(object + c.defeat_writes[k].offset, c.defeat_writes[k], ctx, done, 0, scratch))
                    return false;
    }
    if (c.lose_release) {
        ctx->sp = scratch;
        ctx->x[0] = handle + c.event_release_interface;
        const uint64_t fn = rt::load<uint64_t>(rt::load<uint64_t>(ctx->x[0]) + c.event_release_slot);
        if (!call_guest(ctx, fn))
            return false;
    }
    rt::log(rt::LogLevel::Info, "wild-battles: defeat event (handle 0x%" PRIx64 ") requested (%u fields%s)", handle,
            c.defeat_count, c.lose_release ? ", released" : "");
    return true;
}

// hook expfix_door: our battle gives experience (see the header).
void on_expfix_door(lgp::CpuState* ctx, lgp::LiftedFn original, void*) {
    if (const uint64_t b = g_exp_battle.exchange(0, std::memory_order_relaxed); b != 0) {
        const Config& c = g_config;
        const uint32_t flags = rt::load<uint32_t>(b + c.expfix_flags_offset);
        const uint32_t to = flags & ~static_cast<uint32_t>(c.expfix_flags_clear);
        rt::store<uint32_t>(b + c.expfix_flags_offset, to);
        rt::store<uint8_t>(b + c.expfix_byte_offset, 0);
        g_flags_cleared = flags & static_cast<uint32_t>(c.expfix_flags_clear);
        g_flags_battle = b;
        g_exp_ours = true;
        rt::log(rt::LogLevel::Info, "wild-battles: battle experience on (flags 0x%x -> 0x%x)", flags, to);
    }
    original(ctx);
}

// A door that sees the wild bit: its log line once per encounter (the end
// view runs every frame), for at most 16 encounters each.
struct WildDoor {
    const char* role;
    const char* what;
    std::atomic<uint32_t> encounter{0};
    std::atomic<uint32_t> logged{0};
};
WildDoor g_wild_doors[] = {{"recall_end_start", "recall at the end"},
                           {"end_view_wild", "end view"},
                           {"ko_message_wild", "K.O. message"}};

// Host pointer to n guest bytes at va, read-only pages too (the path text is
// in rodata), or nullptr.
uint8_t* text_bytes(uint64_t va, uint64_t n) {
    if (n == 0 || !rt::access_allowed(va, static_cast<unsigned>(n), rt::kPageRead))
        return nullptr;
    return rt::g_view.base + (va - rt::g_view.guest_base);
}

// The recall's path text at va, up to its NUL ("" if unreadable or too long).
std::string read_text(uint64_t va) {
    std::string t;
    for (uint64_t at = va; t.size() < 0x100; ++at) {
        const uint8_t* p = text_bytes(at, 1);
        if (p == nullptr)
            return {};
        if (*p == 0)
            return t;
        t += static_cast<char>(*p);
    }
    return {};
}

// value ko_side: whether this recall_end_start call is the opponent's.
bool ko_side_matches(const lgp::CpuState& entry) {
    const Config& c = g_config;
    if (!c.ko_side)
        return true;
    const uint64_t at = eval(c.ko_side_expr, &entry);
    if (c.ko_side_size == 0)  // SIZE 0: the value itself, e.g. the slot in x1
        return at == c.ko_side_value;
    if (at == 0 || !rt::access_allowed(at, c.ko_side_size, rt::kPageRead))
        return false;
    uint64_t v = 0;
    for (uint32_t i = 0; i < c.ko_side_size; ++i)
        v |= uint64_t{rt::load<uint8_t>(at + i)} << (8 * i);
    return v == c.ko_side_value;
}

// path ko_seq: the opponent's K.O. plays our sequence, the bit stays cleared.
// False: the text could not be written, the caller goes on as without it.
bool play_ko_seq(lgp::CpuState* ctx, lgp::LiftedFn original) {
    const Config& c = g_config;
    const std::string old = read_text(c.ko_seq_text);
    if (old.empty() || c.ko_seq.size() > old.size()) {
        rt::log(rt::LogLevel::Warn, "wild-battles: %s at ko_seq_text, K.O. as without path ko_seq",
                old.empty() ? "no path text" : "path ko_seq is longer than the text");
        return false;
    }
    uint8_t* p = text_bytes(c.ko_seq_text, old.size() + 1);
    if (p == nullptr)
        return false;
    std::memset(p, 0, old.size() + 1);
    std::memcpy(p, c.ko_seq.data(), c.ko_seq.size());
    original(ctx);  // seq_path copies the text, seq_play opens the file within the call (Fang-36)
    std::memcpy(p, old.data(), old.size());
    p[old.size()] = 0;
    return true;
}

// hook recall_end_start, end_view_wild, ko_message_wild: in our battle as
// in a wild one, the bits expfix cleared set for the call (see the header).
void on_wild_bit_door(lgp::CpuState* ctx, lgp::LiftedFn original, void* user) {
    const uint64_t b = g_flags_battle.load(std::memory_order_relaxed);
    const uint32_t bits = g_flags_cleared.load(std::memory_order_relaxed);
    if (!g_our_battle.load(std::memory_order_relaxed) || b == 0 || bits == 0) {
        original(ctx);
        return;
    }
    WildDoor& door = *static_cast<WildDoor*>(user);
    if (&door == &g_wild_doors[0] && g_config.ko_seq_text != 0 && ko_side_matches(*ctx)) {
        const lgp::CpuState entry = *ctx;
        if (play_ko_seq(ctx, original)) {
            const uint32_t n = g_encounters.load(std::memory_order_relaxed);
            if (door.encounter.exchange(n, std::memory_order_relaxed) != n &&
                door.logged.fetch_add(1, std::memory_order_relaxed) < 16)
                rt::log(rt::LogLevel::Info, "wild-battles: K.O. plays %s (encounter %u)", g_config.ko_seq.c_str(), n);
            return;
        }
        *ctx = entry;
    }
    const uint64_t at = b + g_config.expfix_flags_offset;
    const uint32_t flags = rt::load<uint32_t>(at);
    rt::store<uint32_t>(at, flags | bits);
    original(ctx);
    rt::store<uint32_t>(at, flags);
    const uint32_t n = g_encounters.load(std::memory_order_relaxed);
    if (door.encounter.exchange(n, std::memory_order_relaxed) != n &&
        door.logged.fetch_add(1, std::memory_order_relaxed) < 16)
        rt::log(rt::LogLevel::Info, "wild-battles: %s as in a wild battle (flags 0x%x for the call, encounter %u)",
                door.what, flags | bits, n);
}

// hook post_battle: after our battle the level-ups for the catch scene (see the header).
void on_post_battle(lgp::CpuState* ctx, lgp::LiftedFn original, void*) {
    const Config& c = g_config;
    const uint64_t self = ctx->x[0];
    const uint32_t state = self != 0 ? rt::load<uint32_t>(self + c.post_state_offset) : ~0u;
    original(ctx);
    if (ctx->halt != kHaltNone || state != 0 || self == 0)
        return;
    g_levelled = 0;
    g_our_catch = false;
    g_carry_process = 0;
    g_carry_skipped = false;
    g_capture_caught = false;
    g_capture_knockout = false;
    const bool ball_go = g_ball_go.exchange(false);
    const bool ours = g_exp_ours.exchange(false) && !c.post_as_game;
    const uint32_t next = rt::load<uint32_t>(self + c.post_state_offset);
    // A ball chosen in our battle (hook ball_choice): it ended as fled; on
    // into the Let's Go catch scene as after a win, with its own experience.
    if (ball_go && ours) {
        if (next == c.post_end_state) {
            uint64_t bytes = 0;
            for (unsigned i = 0; i < kMembers; ++i)
                if (rt::load<uint8_t>(self + c.post_flags_offset + i) != 0)
                    bytes |= uint64_t{0xff} << (8 * i);
            rt::store<uint32_t>(self + c.post_state_offset, static_cast<uint32_t>(c.post_catch_state));
            g_levelled = bytes;
            rt::log(rt::LogLevel::Info,
                    "wild-battles: ball chosen in the battle, on to the Let's Go catch scene (level-ups 0x%012" PRIx64
                    ")",
                    bytes);
            return;
        }
        rt::log(rt::LogLevel::Warn, "wild-battles: ball chosen in the battle, but next state %u: left as it is", next);
    }
    // Caught with a ball (raw result 5, only with battle-balls): every battle,
    // the game's own wild ones too (legendaries, Snorlax), and also with the
    // menu's switch off. Without this the process ends and the Pokémon is lost.
    if (c.caught) {
        uint64_t result = ~uint64_t{0};
        if (const uint64_t at = eval(c.post_result, nullptr, self); at != 0) {
            result = 0;
            for (uint32_t i = 0; i < c.post_result_size; ++i)
                result |= uint64_t{rt::load<uint8_t>(at + i)} << (8 * i);
        }
        if (ours || result == c.post_caught_result)
            rt::log(rt::LogLevel::Info, "wild-battles: after %s battle: result %" PRId64 ", next state %u",
                    ours ? "our" : "the game's", static_cast<int64_t>(result), next);
        if (next == c.post_end_state && result == c.post_caught_result) {
            uint64_t bytes = 0;
            for (unsigned i = 0; ours && i < kMembers; ++i)
                if (rt::load<uint8_t>(self + c.post_flags_offset + i) != 0)
                    bytes |= uint64_t{0xff} << (8 * i);
            rt::store<uint32_t>(self + c.post_state_offset, static_cast<uint32_t>(c.post_catch_state));
            g_our_catch = ours;  // the game's own battles give no experience: the catch keeps its own
            g_levelled = bytes;
            g_capture_caught = true;
            rt::log(rt::LogLevel::Info,
                    "wild-battles: caught in the battle, catch process for dex and box (level-ups 0x%012" PRIx64 ")",
                    bytes);
            return;
        }
    }
    if (!ours)
        return;  // the game's own battle
    uint64_t bytes = 0;
    for (unsigned i = 0; i < kMembers; ++i)
        if (rt::load<uint8_t>(self + c.post_flags_offset + i) != 0)
            bytes |= uint64_t{0xff} << (8 * i);
    if (next != c.post_catch_state) {
        // no catch scene (caught in the battle, fled): the process ends
        // without evolving, as it evolves only after trainer battles
        if (c.post_evolve && bytes != 0 && next == c.post_end_state) {
            rt::store<uint32_t>(self + c.post_state_offset, static_cast<uint32_t>(c.post_evolve_state));
            rt::log(rt::LogLevel::Info, "wild-battles: levelled up in the battle: 0x%012" PRIx64 ", evolution now",
                    bytes);
        }
        return;
    }
    g_our_catch = true;
    g_levelled = bytes;
    if (bytes != 0)
        rt::log(rt::LogLevel::Info, "wild-battles: levelled up in the battle: 0x%012" PRIx64, bytes);
    if (c.knockout && (c.knockout_always || enabled("battle-balls"))) {
        g_capture_knockout = true;
        rt::log(rt::LogLevel::Info, "wild-battles: won by K.O. with balls in the bag, no catch scene");
    }
}

// Caught in the battle or won by K.O. (way A): the catch process past the
// throw or on as given up. False if neither is pending.
bool redirect_capture(uint64_t self) {
    const Config& c = g_config;
    if (g_capture_caught.exchange(false)) {
        rt::store<uint32_t>(self + c.capture_result_offset, 0);
        rt::store<uint32_t>(self + c.capture_state_offset, static_cast<uint32_t>(c.capture_caught_state));
        rt::log(rt::LogLevel::Info, "wild-battles: catch process: caught in the battle, past the throw");
        return true;
    }
    if (g_capture_knockout.exchange(false)) {
        rt::store<uint32_t>(self + c.capture_result_offset, static_cast<uint32_t>(c.knockout_result));
        if (g_levelled.load() != 0) {  // evolve first, then on as given up
            rt::store<uint32_t>(self + c.capture_state_offset, static_cast<uint32_t>(c.capture_evolve_state));
            rt::store<uint32_t>(self + c.capture_counter_offset, 0);
            g_carry_skipped = true;
        } else {
            rt::store<uint32_t>(self + c.capture_state_offset, static_cast<uint32_t>(c.capture_skip_state));
        }
        rt::log(rt::LogLevel::Info, "wild-battles: catch process: won by K.O., the Pokémon is gone");
        return true;
    }
    return false;
}

// hook capture_process: the catch scene after our battle evolves them (see the header).
void on_capture_process(lgp::CpuState* ctx, lgp::LiftedFn original, void*) {
    const Config& c = g_config;
    const uint64_t self = ctx->x[0];
    uint32_t state = self != 0 ? rt::load<uint32_t>(self + c.capture_state_offset) : ~0u;
    // capture_entry_ready: in capture_entry_state, before the original (which
    // goes on into the Go app once the fader is done), ask the fader itself
    if (self != 0 && c.capture_ready != 0 && state == c.capture_entry_state &&
        (g_capture_caught.load() || g_capture_knockout.load())) {
        const lgp::CpuState entry = *ctx;
        ctx->sp = (entry.sp - kScratch) & ~uint64_t{0xF};
        ctx->x[0] = eval(c.capture_ready_x0);
        ctx->x[1] = c.capture_ready_x1;
        if (!call_guest(ctx, c.capture_ready))
            return;  // the halt stays
        const bool ready = (ctx->x[0] & 1) != 0;
        *ctx = entry;
        if (ready && redirect_capture(self))
            state = rt::load<uint32_t>(self + c.capture_state_offset);
    }
    if (state == c.capture_evolve_state) {
        if (const uint64_t bytes = g_levelled.exchange(0); bytes != 0) {
            g_carry_process = self;
            for (unsigned i = 0; i < kMembers; ++i)
                if ((bytes >> (8 * i)) & 0xff)
                    rt::store<uint8_t>(self + c.capture_flags_offset + i, 1);
            rt::log(rt::LogLevel::Info, "wild-battles: catch process evolves the battle's level-ups");
        }
    }
    original(ctx);
    if (ctx->halt != kHaltNone || self == 0)
        return;
    const uint32_t now = rt::load<uint32_t>(self + c.capture_state_offset);
    // capture_entry_state: not after the first update but once the process
    // got there (its fade-out and music past the start state, Fang-10)
    if (c.capture_ready == 0 && (!c.capture_entry || now == c.capture_entry_state) && redirect_capture(self))
        return;
    if (state == c.capture_evolve_state - 1 && now == c.capture_skip_state && g_levelled.load() != 0) {
        // given up: evolve first, then where the game was going
        rt::store<uint32_t>(self + c.capture_state_offset, static_cast<uint32_t>(c.capture_evolve_state));
        rt::store<uint32_t>(self + c.capture_counter_offset, 0);
        g_carry_skipped = true;
        rt::log(rt::LogLevel::Info, "wild-battles: catch given up, evolution first");
    } else if (state == c.capture_evolve_state && now == c.capture_evolve_state + 1 && g_carry_skipped.load() &&
               g_carry_process.load() == self) {
        rt::store<uint32_t>(self + c.capture_state_offset, static_cast<uint32_t>(c.capture_skip_state));
        g_carry_skipped = false;
        g_carry_process = 0;
    }
}

// hook catch_exp_amount (zero_catch_exp): no catch experience after our battle's experience.
bool our_catch() { return g_our_catch.load(); }

// hook ball_choice: whether the item the handler is about to use is a ball,
// asked of the game's item data getter as the game asks it. False with the
// halt set if the call halted.
bool ball_chosen(lgp::CpuState* ctx) {
    const Config& c = g_config;
    const lgp::CpuState entry = *ctx;
    ctx->sp = (entry.sp - kScratch) & ~uint64_t{0xF};
    ctx->x[0] = entry.x[c.ball_choice_item_reg] & 0xffff;
    ctx->x[1] = c.ball_choice_field;
    if (!call_guest(ctx, c.ball_choice_kind))
        return false;  // the halt stays
    const bool ball = (ctx->x[0] & 0xffffffff) == c.ball_choice_value;
    *ctx = entry;
    return ball;
}

// hook ball_choice: a ball in our battle with the throw's animation off ends
// the battle as fled, for the Let's Go catch scene (see the header).
void on_ball_choice(lgp::CpuState* ctx, lgp::LiftedFn original, void*) {
    const Config& c = g_config;
    const bool wanted = c.ball_choice_always || (enabled("battle-balls") && !active("battle-throw"));
    if (!g_our_battle.load(std::memory_order_relaxed) || !wanted) {
        original(ctx);  // the game's own battles, the classic throw
        return;
    }
    if (!ball_chosen(ctx)) {
        if (ctx->halt == kHaltNone)
            original(ctx);  // other items
        return;
    }
    ctx->x[0] = c.ball_choice_return;
    g_ball_go = true;
    rt::log(rt::LogLevel::Info, "wild-battles: ball chosen in our battle, the battle ends for the Let's Go catch");
}

// hook loss_amount: a defeat in our wild battle costs no money.
void on_loss_amount(lgp::CpuState* ctx, lgp::LiftedFn original, void*) {
    if (!g_our_battle.load(std::memory_order_relaxed)) {
        original(ctx);  // the game's own battles: as before
        return;
    }
    ctx->x[0] = 0;
    rt::log(rt::LogLevel::Info, "wild-battles: defeat in our battle, no money lost");
}

// hook part_find_guard: without the character's model resource in the
// battle's registry the lookup returns 0 instead of halting.
void on_part_find(lgp::CpuState* ctx, lgp::LiftedFn original, void*) {
    if (!g_our_battle.load(std::memory_order_relaxed)) {
        original(ctx);  // the game's own battles and link battles: untouched
        return;
    }
    const Config& c = g_config;
    const lgp::CpuState entry = *ctx;
    const uint64_t obj = entry.x[0];
    const uint64_t scratch = (entry.sp - kScratch) & ~uint64_t{0xF};
    const uint64_t holder = obj != 0 ? rt::load<uint64_t>(obj + c.part_holder) : 0;
    const uint64_t registry = holder != 0 ? rt::load<uint64_t>(holder + c.part_registry) : 0;
    uint64_t key = obj != 0 ? rt::load<uint64_t>(obj + c.part_key) : 0;
    if (obj != 0 && rt::load<uint8_t>(obj + c.part_alt_switch) != 0) {
        const uint64_t alt = rt::load<uint64_t>(obj + c.part_alt_key);
        if (alt != 0 && rt::load<uint32_t>(alt + c.part_alt_count) != 0)
            key = alt;
    }
    uint64_t found = 0;
    if (registry != 0 && key != 0) {
        const uint64_t key_at = scratch + 0x40;
        const uint64_t out = scratch + 0x48;
        rt::store<uint64_t>(key_at, key);
        rt::store<uint64_t>(out, 0);
        ctx->sp = scratch;
        ctx->x[0] = registry;
        ctx->x[1] = key_at;
        ctx->x[8] = out;
        if (!call_guest(ctx, c.registry_find))
            return;
        found = rt::load<uint64_t>(out);
        if (found != 0) {
            ctx->sp = scratch;
            ctx->x[0] = found + c.part_release_interface;
            if (!call_guest(ctx, rt::load<uint64_t>(rt::load<uint64_t>(ctx->x[0]) + c.part_release_slot)))
                return;
        }
    }
    *ctx = entry;
    if (found == 0) {
        static std::atomic<uint32_t> logged{0};
        if (logged.fetch_add(1, std::memory_order_relaxed) < 8)
            rt::log(rt::LogLevel::Info,
                    "wild-battles: part lookup skipped (model resource not in the battle's registry)");
        ctx->x[0] = 0;
        return;
    }
    original(ctx);
}

// The battle event's update; when ours is done, what the script would do next.
void on_event_update(lgp::CpuState* ctx, lgp::LiftedFn original, void*) {
    const uint64_t self = ctx->x[0];
    original(ctx);
    const Config& c = g_config;
    if (ctx->halt != kHaltNone || ctx->x[0] != c.end_result || self == 0 ||
        (self != g_event.load(std::memory_order_relaxed) && self != g_handle.load(std::memory_order_relaxed)))
        return;
    g_event = 0;
    g_handle = 0;
    g_our_battle = false;
    g_exp_battle = 0;
    g_flags_battle = 0;
    restore_partner();
    const lgp::CpuState done = *ctx;
    const uint64_t scratch = (done.sp - kScratch) & ~uint64_t{0xF};
    bool lost = false;
    if (c.lose_create != 0 && !team_down(ctx, scratch, &lost))
        return;
    // As a script after any battle: the end (after a lost one, then
    // CallBattleLose).
    const uint64_t base = eval(c.end_base);
    for (unsigned k = 0; k < c.end_count; ++k)
        if (!write_value(base + c.end_writes[k].offset, c.end_writes[k], ctx, done, 0, scratch))
            return;
    if (c.end_call != 0) {
        ctx->sp = scratch;
        for (unsigned i = 0; i < c.end_args; ++i)
            ctx->x[i] = eval(c.end_x[i]);
        if (!call_guest(ctx, c.end_call))
            return;
    }
    if (lost) {
        if (!create_defeat(ctx, done, scratch))
            return;
        rt::log(rt::LogLevel::Info,
                "wild-battles: battle event 0x%" PRIx64 " done, team down: the game's defeat (after our end)", self);
        g_battles_done.fetch_add(1, std::memory_order_relaxed);
        g_in_battle.store(false, std::memory_order_release);
        *ctx = done;
        return;
    }
    rt::log(rt::LogLevel::Info, "wild-battles: battle event 0x%" PRIx64 " done (%u fields%s)", self, c.end_count,
            c.end_call != 0 ? ", call" : "");
    g_battles_done.fetch_add(1, std::memory_order_relaxed);
    g_in_battle.store(false, std::memory_order_release);
    const uint32_t halt = ctx->halt;
    *ctx = done;
    ctx->halt = halt;
}

const ModLine* find_line(const ModFile& file, std::string_view directive, std::string_view name) {
    for (const ModLine& l : file.lines)
        if (l.directive == directive && l.name == name)
            return &l;
    return nullptr;
}

bool parse_steps(std::string_view s, Expr* e) {
    while (!s.empty()) {
        if ((s[0] != '>' && s[0] != '+' && s[0] != '-') || e->count == kMaxSteps)
            return false;
        Expr::Step& step = e->steps[e->count++];
        step.load = s[0] == '>';
        step.sub = s[0] == '-';
        s.remove_prefix(1);
        const size_t end = std::min(s.find_first_of(">+-"), s.size());
        if (!parse_number(s.substr(0, end), &step.offset))
            return false;
        s.remove_prefix(end);
    }
    return true;
}

// `addr NAME VA [STEPS]`
bool parse_expr(const ModLine& l, Expr* e) {
    *e = Expr{};
    e->base = l.number;
    return parse_steps(l.rest, e);
}

// `call NAME VA ADDR [STEPS]`: x0 of the call, ADDR with steps (spaces allowed).
bool parse_call_x0(const ModLine& t, Expr* x0) {
    *x0 = Expr{};
    uint64_t base = 0;
    std::string_view rest = t.rest;
    const size_t end = std::min(rest.find_first_of(">+- \t"), rest.size());
    const bool number = end != 0 && parse_number(rest.substr(0, end), &base);
    rest.remove_prefix(end);
    std::string steps(rest);
    steps.erase(std::remove_if(steps.begin(), steps.end(), [](char ch) { return ch == ' ' || ch == '\t'; }),
                steps.end());
    if (!number || !parse_steps(steps, x0))
        return false;
    x0->base = base;
    return true;
}

// x0…x30 or obj, then steps (`from` lines).
bool parse_reg_expr(std::string_view s, Expr* e) {
    *e = Expr{};
    if (s.rfind("obj", 0) == 0) {
        e->reg = Expr::kObject;
        s.remove_prefix(3);
    } else if (!s.empty() && s[0] == 'x') {
        size_t n = 1;
        while (n < s.size() && s[n] >= '0' && s[n] <= '9')
            ++n;
        uint64_t reg = 0;
        if (n == 1 || n > 3 || !parse_number(s.substr(1, n - 1), &reg) || reg > 30)
            return false;
        e->reg = static_cast<int>(reg);
        s.remove_prefix(n);
    } else {
        return false;
    }
    return parse_steps(s, e);
}

bool read_writes(const ModFile& file, const char* prefix, uint32_t default_size, Write* out, unsigned* count,
                 std::string* why) {
    *count = 0;
    for (unsigned k = 1; k <= kMaxWrites; ++k) {
        const std::string name = std::string(prefix) + "_" + std::to_string(k);
        const auto offset = file.value(name + "_offset");
        const auto value = file.value(name);
        const ModLine* from = find_line(file, "from", name);
        const ModLine* call = find_line(file, "call", name);
        const int sources = int(value.has_value()) + int(from != nullptr) + int(call != nullptr);
        if (!offset && sources == 0)
            break;
        if (!offset || sources != 1) {
            *why = name + " needs `value " + name + "_offset` and one of `value " + name + "`, `from " + name +
                   "`, `call " + name + "`";
            return false;
        }
        Write& w = out[(*count)++];
        w.offset = *offset;
        if (call != nullptr) {
            w.call = true;
            w.fn = call->number;
            w.size = static_cast<uint32_t>(file.value(name + "_size").value_or(default_size));
            if (!parse_reg_expr(call->rest, &w.src) || (w.size != 1 && w.size != 2 && w.size != 4 && w.size != 8)) {
                *why = "call " + name + ": expected VA, then x0…x30 or obj with >OFF or +OFF steps (x0 of the call); " +
                       name + "_size 1, 2, 4 or 8";
                return false;
            }
            continue;
        }
        if (from != nullptr) {
            w.from = true;
            w.size = static_cast<uint32_t>(from->number);
            if (!parse_reg_expr(from->rest, &w.src) || w.size == 0 || w.size > kMaxFrom) {
                *why = "from " + name + ": expected SIZE (1…32) and x0…x30 or obj, then >OFF or +OFF steps";
                return false;
            }
            continue;
        }
        w.value = *value;
        w.size = static_cast<uint32_t>(file.value(name + "_size").value_or(default_size));
        if (w.size != 1 && w.size != 2 && w.size != 4 && w.size != 8) {
            *why = "value " + name + "_size must be 1, 2, 4 or 8";
            return false;
        }
    }
    return true;
}

bool read_config(const ModFile& file, Config* c, std::string* why) {
    *c = Config{};
    if (const auto v = file.value("redirect"))
        c->redirect = static_cast<uint32_t>(*v);
    if (c->redirect != 0 && c->redirect != 3) {
        *why = "value redirect must be 0 (log only) or 3 (battle); 1 and 2 were probes and are gone";
        return false;
    }
    if (c->redirect == 0)
        return true;
    const auto create = file.hook("event_create");
    const ModLine* x0 = find_line(file, "addr", "start_x0");
    const ModLine* x1 = find_line(file, "addr", "start_x1");
    if (!create || x0 == nullptr || x1 == nullptr) {
        *why = "redirect 3 needs `hook event_create`, `addr start_x0` and `addr start_x1` (its x0 and x1)";
        return false;
    }
    c->event_create = *create;
    if (!parse_expr(*x0, &c->x0) || !parse_expr(*x1, &c->x1)) {
        *why = "addr start_x0/start_x1: expected VA, then >OFF, +OFF or -OFF steps (at most 8)";
        return false;
    }
    if (const ModLine* f = find_line(file, "addr", "event_from_create");
        f != nullptr && !parse_expr(*f, &c->event_from_create)) {
        *why = "addr event_from_create: expected N, then >OFF, +OFF or -OFF steps (at most 8)";
        return false;
    }
    const auto hash = file.value("template_hash");
    if (!file.hook("static_battle_lookup") || !hash || !file.hook("record_fill")) {
        *why = "redirect 3 needs `hook static_battle_lookup`, `value template_hash` and `hook record_fill`";
        return false;
    }
    c->template_hash = *hash;
    c->lookup_hash_reg = static_cast<unsigned>(file.value("lookup_hash_reg").value_or(0));
    if (c->lookup_hash_reg > 7) {
        *why = "value lookup_hash_reg: x0…x7";
        return false;
    }
    if (const auto v = file.value("lookup_hash_ptr"))
        c->lookup_hash_ptr = *v != 0;
    if (!read_writes(file, "event", 4, c->event_writes, &c->event_count, why) ||
        !read_writes(file, "record", 4, c->record_writes, &c->record_count, why))
        return false;
    if (c->event_count == 0) {
        *why = "redirect 3 needs `value event_1_offset` … (the fields CallWildBattle sets)";
        return false;
    }
    if (const auto sp = file.value("direct_species")) {
        const auto index = [&](const char* name, int* to) {
            const auto v = file.value(name);
            if (!v)
                return true;
            if (*v < 1 || *v > c->record_count) {
                *why = std::string("value ") + name + ": the number K of a record_K line (1…" +
                       std::to_string(c->record_count) + ")";
                return false;
            }
            *to = static_cast<int>(*v) - 1;
            return true;
        };
        if (!index("direct_species", &c->direct_species) || !index("direct_form", &c->direct_form) ||
            !index("direct_level", &c->direct_level))
            return false;
        c->direct_ok = true;
        for (unsigned k = 0; k < c->record_count; ++k) {
            const int i = static_cast<int>(k);
            if (i == c->direct_species || i == c->direct_form || i == c->direct_level)
                continue;
            const Write& w = c->record_writes[k];
            if (const auto v = file.value("direct_record_" + std::to_string(k + 1)))
                c->direct_value[k] = *v;
            else if (w.from || w.call)
                c->direct_ok = false;  // read from the touched Pokémon: none in a direct battle
            else
                c->direct_value[k] = w.value;
        }
        for (unsigned k = 0; k < c->event_count; ++k)
            if (c->event_writes[k].from || c->event_writes[k].call)
                c->direct_ok = false;
        if (!c->direct_ok) {
            *why = "direct battles need every other record_K and event_K as a constant (value direct_record_K V)";
            return false;
        }
    }
    if (const auto v = file.value("event_touch"))
        c->event_touch = *v != 0;
    if (const auto v = file.value("object_load"))
        c->object_load = *v != 0;
    if (const auto v = file.value("touch_return")) {
        c->has_return = true;
        c->touch_return = *v;
    }
    const auto ri = file.value("event_release_interface");
    const auto rs = file.value("event_release_slot");
    if (ri.has_value() != rs.has_value()) {
        *why = "event release needs both `value event_release_interface` and `value event_release_slot`";
        return false;
    }
    if (ri) {
        c->event_release = true;
        c->event_release_interface = *ri;
        c->event_release_slot = *rs;
    }
    if (file.hook("battle_event_update")) {
        c->end = true;
        if (const auto v = file.value("end_result"))
            c->end_result = *v;
        if (const ModLine* b = find_line(file, "addr", "end_base"); b != nullptr && !parse_expr(*b, &c->end_base)) {
            *why = "addr end_base: expected VA, then >OFF, +OFF or -OFF steps (at most 8)";
            return false;
        }
        if (!read_writes(file, "end", 4, c->end_writes, &c->end_count, why))
            return false;
        if (c->end_count != 0 && find_line(file, "addr", "end_base") == nullptr) {
            *why = "end_K fields need `addr end_base`";
            return false;
        }
        if (const auto v = file.value("end_call")) {
            c->end_call = *v;
            for (unsigned i = 0; i < 3; ++i) {
                const ModLine* a = find_line(file, "addr", "end_x" + std::to_string(i));
                if (a == nullptr)
                    break;
                if (!parse_expr(*a, &c->end_x[i])) {
                    *why = "addr end_x" + std::to_string(i) + ": expected VA, then >OFF, +OFF or -OFF steps";
                    return false;
                }
                c->end_args = i + 1;
            }
        }
    }
    if (const auto site = file.patch("battle_time")) {
        c->battle_time_site = *site;
        c->battle_time = file.value("battle_time").value_or(0);
    }
    if (const ModLine* t = find_line(file, "call", "team_fit")) {
        c->team_fit = t->number;
        if (!parse_call_x0(*t, &c->team_x0)) {
            *why = "call team_fit: expected VA, then ADDR with >OFF, +OFF or -OFF steps (x0 of the call)";
            return false;
        }
        c->team_x1 = file.value("team_fit_x1").value_or(0);
        c->team_x2 = file.value("team_fit_x2").value_or(0);
        c->team_down = file.value("team_down").value_or(0);
    }
    if (file.value("defeat")) {
        *why = "value defeat is gone (the game's warp broke the scene, P8.9): use hook lose_create";
        return false;
    }
    if (file.hook("catch_exp")) {
        *why = "hook catch_exp is gone (our battles give no experience, the catch gives the only, P8.11)";
        return false;
    }
    if (const auto v = file.hook("lose_create"))
        c->lose_create = *v;
    if (!read_writes(file, "defeat", 4, c->defeat_writes, &c->defeat_count, why))
        return false;
    if (const auto v = file.value("lose_release"))
        c->lose_release = *v != 0;
    if (c->lose_release && !c->event_release) {
        *why = "value lose_release needs `value event_release_interface` and `value event_release_slot`";
        return false;
    }
    if (c->lose_create != 0 && !c->end) {
        *why = "hook lose_create needs `hook battle_event_update` (our battle's end)";
        return false;
    }
    if (const ModLine* a = find_line(file, "addr", "partner_state")) {
        if (!parse_expr(*a, &c->partner_state)) {
            *why = "addr partner_state: expected VA, then >OFF, +OFF or -OFF steps (at most 8)";
            return false;
        }
        if (!c->end) {
            *why = "addr partner_state needs `hook battle_event_update` (the old value back at our end)";
            return false;
        }
        c->partner = true;
        const uint64_t v = file.value("partner_set").value_or(1);
        if (v > 0xff) {
            *why = "value partner_set: a byte (0…255)";
            return false;
        }
        c->partner_set = static_cast<uint8_t>(v);
    }
    if (const auto r = file.value("expfix_reg")) {
        const auto off = file.value("expfix_flags_offset");
        const auto clear = file.value("expfix_flags_clear");
        const auto byte = file.value("expfix_byte_offset");
        if (*r == 0 || *r > 30 || !off || !clear || !byte || !file.hook("expfix_door")) {
            *why = "value expfix_reg (x1…x30) needs value expfix_flags_offset, expfix_flags_clear, "
                   "expfix_byte_offset and hook expfix_door";
            return false;
        }
        if (c->battle_time_site == 0) {
            *why = "value expfix_reg needs `patch battle_time` (where our battle is taken)";
            return false;
        }
        c->expfix_reg = *r;
        c->expfix_flags_offset = *off;
        c->expfix_flags_clear = *clear;
        c->expfix_byte_offset = *byte;
    }
    if (file.hook("part_find_guard")) {
        static const char* const names[] = {"registry_find",   "part_holder",          "part_registry",
                                            "part_key",        "part_alt_switch",      "part_alt_key",
                                            "part_alt_count",  "part_release_interface", "part_release_slot"};
        uint64_t* const fields[] = {&c->registry_find,  &c->part_holder,           &c->part_registry,
                                    &c->part_key,       &c->part_alt_switch,       &c->part_alt_key,
                                    &c->part_alt_count, &c->part_release_interface, &c->part_release_slot};
        for (unsigned i = 0; i < 9; ++i) {
            const auto v = file.value(names[i]);
            if (!v) {
                *why = std::string("hook part_find_guard needs value ") + names[i];
                return false;
            }
            *fields[i] = *v;
        }
        if (c->battle_time_site == 0) {
            *why = "hook part_find_guard needs `patch battle_time` (where our battle starts)";
            return false;
        }
        c->part_guard = true;
    }
    if (file.hook("loss_amount")) {
        if (c->battle_time_site == 0) {
            *why = "hook loss_amount needs `patch battle_time` (where our battle starts)";
            return false;
        }
        c->loss_amount = true;
    }
    if (file.hook("post_battle") || file.hook("capture_process")) {
        static const char* const names[] = {"post_state_offset",      "post_flags_offset",    "post_catch_state",
                                            "capture_state_offset",   "capture_flags_offset",
                                            "capture_counter_offset", "capture_evolve_state", "capture_skip_state"};
        uint64_t* const fields[] = {&c->post_state_offset,      &c->post_flags_offset,    &c->post_catch_state,
                                    &c->capture_state_offset,   &c->capture_flags_offset,
                                    &c->capture_counter_offset, &c->capture_evolve_state, &c->capture_skip_state};
        for (unsigned i = 0; i < 8; ++i) {
            const auto v = file.value(names[i]);
            if (!v || !file.hook("post_battle") || !file.hook("capture_process")) {
                *why = std::string("hook post_battle and hook capture_process need each other and value ") + names[i];
                return false;
            }
            *fields[i] = *v;
        }
        if (c->capture_evolve_state == 0 || c->capture_skip_state <= c->capture_evolve_state + 1) {
            *why = "value capture_skip_state must lie past capture_evolve_state + 1";
            return false;
        }
        if (c->expfix_reg == 0) {
            *why = "hook post_battle needs value expfix_reg (only our battles with experience)";
            return false;
        }
    }
    {
        const auto end = file.value("post_end_state");
        const auto evolve = file.value("post_evolve_state");
        if (end || evolve) {
            if (!end || !evolve || c->capture_evolve_state == 0) {
                *why = "value post_end_state and post_evolve_state need each other and hook post_battle";
                return false;
            }
            if (*end == *evolve || *end == c->post_catch_state || *evolve == 0) {
                *why = "value post_end_state, post_evolve_state: three different states, evolve not 0";
                return false;
            }
            c->post_evolve = true;
            c->post_end_state = *end;
            c->post_evolve_state = *evolve;
        }
    }
    c->post_as_game = file.value("post_as_game").value_or(0) != 0;
    if (const ModLine* f = find_line(file, "from", "post_result")) {
        const auto result = file.value("post_caught_result");
        const auto offset = file.value("capture_result_offset");
        const auto state = file.value("capture_caught_state");
        if (!parse_reg_expr(f->rest, &c->post_result) || c->post_result.reg != Expr::kObject ||
            (f->number != 1 && f->number != 2 && f->number != 4)) {
            *why = "from post_result: expected SIZE (1, 2 or 4) and obj, then >OFF or +OFF steps";
            return false;
        }
        if (!result || !offset || !state || !c->post_evolve) {
            *why = "from post_result needs value post_caught_result, capture_result_offset, capture_caught_state "
                   "and post_end_state";
            return false;
        }
        c->caught = true;
        c->post_result_size = static_cast<uint32_t>(f->number);
        c->post_caught_result = *result;
        c->capture_result_offset = *offset;
        c->capture_caught_state = *state;
    }
    if (const auto v = file.value("capture_entry_state")) {
        if (!c->caught) {
            *why = "value capture_entry_state needs from post_result";
            return false;
        }
        c->capture_entry = true;
        c->capture_entry_state = *v;
    }
    if (const ModLine* t = find_line(file, "call", "capture_entry_ready")) {
        if (!c->capture_entry) {
            *why = "call capture_entry_ready needs value capture_entry_state";
            return false;
        }
        c->capture_ready = t->number;
        if (!parse_call_x0(*t, &c->capture_ready_x0)) {
            *why = "call capture_entry_ready: expected VA, then ADDR with >OFF, +OFF or -OFF steps (x0 of the call)";
            return false;
        }
        c->capture_ready_x1 = file.value("capture_entry_ready_x1").value_or(0);
    }
    if (const auto v = file.value("knockout_result")) {
        if (!c->caught) {
            *why = "value knockout_result needs from post_result";
            return false;
        }
        c->knockout = true;
        c->knockout_result = *v;
        c->knockout_always = file.value("knockout_always").value_or(0) != 0;
    }
    if (file.hook("ball_choice")) {
        const auto reg = file.value("ball_choice_item_reg");
        const auto kind = file.value("ball_choice_kind");
        const auto field = file.value("ball_choice_field");
        const auto value = file.value("ball_choice");
        if (!reg || *reg > 30 || !kind || *kind == 0 || !field || !value) {
            *why = "hook ball_choice needs value ball_choice_item_reg (0..30), ball_choice_kind (the item data "
                   "getter), ball_choice_field and ball_choice";
            return false;
        }
        c->ball_choice_item_reg = *reg;
        c->ball_choice_kind = *kind;
        c->ball_choice_field = *field;
        if (!c->post_evolve) {
            *why = "hook ball_choice needs value post_end_state (the battle ends as fled)";
            return false;
        }
        c->ball_choice = true;
        c->ball_choice_value = *value;
        c->ball_choice_return = file.value("ball_choice_return").value_or(1);
        c->ball_choice_always = file.value("ball_choice_always").value_or(0) != 0;
    }
    if (file.hook("catch_exp_amount")) {
        if (c->capture_evolve_state == 0) {
            *why = "hook catch_exp_amount needs hook post_battle";
            return false;
        }
        c->catch_exp_amount = true;
    }
    if (const ModLine* l = find_line(file, "path", "ko_seq")) {
        const auto text = file.value("ko_seq_text");
        if (l->rest.empty() || l->rest.size() >= 0x100 || !text || *text == 0 || !file.hook("recall_end_start")) {
            *why = "path ko_seq: expected 0 and a TEXT under 256 characters, with value ko_seq_text and hook "
                   "recall_end_start";
            return false;
        }
        c->ko_seq = l->rest;
        c->ko_seq_text = *text;
        if (const ModLine* f = find_line(file, "from", "ko_side")) {
            const auto side = file.value("ko_side");
            if (!parse_reg_expr(f->rest, &c->ko_side_expr) || c->ko_side_expr.reg == Expr::kObject ||
                c->ko_side_expr.reg == Expr::kConstant || (f->number != 0 && f->number != 1 && f->number != 2 && f->number != 4) ||
                !side) {
                *why = "from ko_side: expected SIZE (0 = the value itself, 1, 2 or 4) and xN with steps, and value "
                       "ko_side N";
                return false;
            }
            c->ko_side = true;
            c->ko_side_size = static_cast<uint32_t>(f->number);
            c->ko_side_value = *side;
        }
    } else if (file.value("ko_seq_text")) {
        *why = "value ko_seq_text: also a `path ko_seq 0 TEXT` is needed in " + file.path.filename().string();
        return false;
    }
    if (c->lose_create != 0 && c->team_fit == 0) {
        *why = "hook lose_create needs `call team_fit`";
        return false;
    }
    return true;
}

// patch battle_time: the MOVZ (32 or 64 bit) that sets the time limit of an
// event battle. Ours get battle_time, the game's own the constant.
void on_battle_time(lgp::CpuState* ctx, void*) {
    const uint64_t va = ctx->pc - 4;
    const uint32_t insn = rt::load<uint32_t>(va);
    if ((insn & 0x7f800000u) != 0x52800000u) {
        rt::log(rt::LogLevel::Error, "wild-battles: patch battle_time 0x%" PRIx64 " is no MOVZ (0x%08x)", va, insn);
        ctx->halt = kHaltUndefined;
        ctx->halt_info = insn;
        return;
    }
    const unsigned rd = insn & 31;
    uint64_t value = uint64_t((insn >> 5) & 0xffff) << (16 * ((insn >> 21) & 3));
    // Once per requested battle: its start writes the limit once (P8.6), so a
    // battle of ours that never reports done cannot lift the limit of the
    // game's own event battles later.
    if (g_in_battle.exchange(false, std::memory_order_acq_rel)) {
        rt::log(rt::LogLevel::Info, "wild-battles: battle time %" PRIu64 " instead of %" PRIu64, g_config.battle_time,
                value);
        value = g_config.battle_time;
        g_our_battle = true;
        if (g_config.expfix_reg != 0)
            g_exp_battle.store(ctx->x[g_config.expfix_reg], std::memory_order_relaxed);
    } else {
        // The game's own event battle (Snorlax …) starts: ours is over, even
        // when its event never reported done (no battle_event_update).
        g_our_battle = false;
    }
    if (!(insn & 0x80000000u))
        value &= 0xffffffffu;
    if (rd != 31)
        ctx->x[rd] = value;
}

bool install(const ModFile& file, const StartOptions&, std::string* why) {
    g_encounters = 0;
    g_pending_touch = false;
    g_in_battle = false;
    g_pending.record = 0;
    g_partner = 0;
    g_exp_battle = 0;
    g_exp_ours = false;
    g_flags_battle = 0;
    g_flags_cleared = 0;
    g_our_battle = false;
    g_capture_caught = false;
    g_capture_knockout = false;
    g_ball_go = false;
    g_levelled = 0;
    g_our_catch = false;
    g_carry_process = 0;
    g_carry_skipped = false;
    if (!read_config(file, &g_config, why))
        return false;
    g_event = 0;
    g_handle = 0;
    if (g_config.expfix_reg != 0 && !hook_role(file, "expfix_door", &on_expfix_door, nullptr, why))
        return false;
    if (g_config.part_guard && !hook_role(file, "part_find_guard", &on_part_find, nullptr, why))
        return false;
    for (WildDoor& door : g_wild_doors) {
        door.encounter = 0;
        door.logged = 0;
        if (!file.hook(door.role))
            continue;
        if (g_config.expfix_reg == 0) {
            *why = std::string("hook ") + door.role + " needs expfix (value expfix_reg …)";
            return false;
        }
        if (!hook_role(file, door.role, &on_wild_bit_door, &door, why))
            return false;
    }
    if (g_config.loss_amount && !hook_role(file, "loss_amount", &on_loss_amount, nullptr, why))
        return false;
    if (g_config.capture_evolve_state != 0 &&
        (!hook_role(file, "post_battle", &on_post_battle, nullptr, why) ||
         !hook_role(file, "capture_process", &on_capture_process, nullptr, why)))
        return false;
    if (g_config.catch_exp_amount && !zero_catch_exp(file, &our_catch, why))
        return false;
    if (g_config.ball_choice && !hook_role(file, "ball_choice", &on_ball_choice, nullptr, why))
        return false;
    if (g_config.end && !hook_role(file, "battle_event_update", &on_event_update, nullptr, why))
        return false;
    if (g_config.redirect == 3 && (!hook_role(file, "static_battle_lookup", &on_lookup, nullptr, why) ||
                                   !hook_role(file, "record_fill", &on_fill, nullptr, why)))
        return false;
    if (g_config.battle_time_site != 0)
        rt::set_patch(g_config.battle_time_site, &on_battle_time);
    return hook_role(file, "encounter_touch", &on_encounter_touch, nullptr, why);
}

}  // namespace

// Gelb mode of random-encounters: the battle as for a touch, with the species,
// form and level it rolled instead of a field Pokémon's.
bool wild_battles_direct(lgp::CpuState* ctx, uint32_t species, uint32_t form, uint32_t level, bool* requested) {
    const Config& c = g_config;
    *requested = false;
    if (!g_on.load(std::memory_order_relaxed) || c.redirect != 3 || !c.direct_ok)
        return true;
    const uint32_t n = g_encounters.fetch_add(1, std::memory_order_relaxed) + 1;
    g_our_catch = false;
    g_our_battle = false;
    g_capture_caught = false;
    g_capture_knockout = false;
    g_exp_battle = 0;
    g_flags_battle = 0;
    const lgp::CpuState entry = *ctx;
    const uint64_t scratch = (entry.sp - kScratch) & ~uint64_t{0xF};
    bool down = false;
    if (!team_down(ctx, scratch, &down))
        return false;
    if (down) {
        rt::log(rt::LogLevel::Info, "wild-battles: direct battle %u: no Pokémon can fight, none started", n);
        *ctx = entry;
        return true;
    }
    for (unsigned k = 0; k < c.record_count; ++k) {
        const int i = static_cast<int>(k);
        const uint64_t v = i == c.direct_species ? species
                         : i == c.direct_form    ? form
                         : i == c.direct_level   ? level
                                                 : c.direct_value[k];
        for (uint32_t b = 0; b < c.record_writes[k].size; ++b)
            g_pending.bytes[k][b] = static_cast<uint8_t>(b < 8 ? v >> (8 * b) : 0);
    }
    rt::log(rt::LogLevel::Info, "wild-battles: direct battle %u: species %u form %u level %u", n, species, form, level);
    if (!request_battle(ctx, entry, 0, scratch, n, requested))
        return false;
    *ctx = entry;
    return true;
}

bool wild_battles_direct_ready() { return g_config.redirect == 3 && g_config.direct_ok; }


bool wild_battles_catches() { return g_config.caught; }

const ModImpl& wild_battles_mod() {
    static const ModImpl m = {
        {"wild-battles", "Wild battles",
         "Wild Pokémon you touch battle first, then the catching scene follows (needs the mod file).", true, true},
        &install,
        &g_on,
    };
    return m;
}

}  // namespace lgp::mods::detail

namespace lgp::mods {

uint32_t wild_battles_started() {
    return detail::g_battles_started.load(std::memory_order_relaxed);
}

uint32_t wild_battles_done() {
    return detail::g_battles_done.load(std::memory_order_relaxed);
}

}  // namespace lgp::mods
