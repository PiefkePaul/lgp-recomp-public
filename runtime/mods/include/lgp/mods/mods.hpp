// Game mods (P8, docs/knowledge/spiel-mods-hooks.md): changes to the game's
// behaviour as host code on hook doors of the lifted code (lgp/runtime/hooks.hpp).
//
// The repository holds only the mechanism and the mods' own logic; which game
// function plays which role is in the private mod file (lgp/mods/mod_file.hpp,
// `<game>/mods/<id>.cfg`). The game must be lifted with the same files
// (`lgp_lift --mods <game>/mods`), or the hooks have no door and the mod is
// not started.
//
// A mod is off unless named (`--mods id,id`, settings.ini `mods = id id`).
// One that cannot start (no file, a role missing, a hook without door) is
// reported and left out; the game runs unchanged. A live mod (ModInfo::live)
// with a file is installed switched off when not named (StartOptions::standby),
// so the menu can switch it on and off while the game runs (set_active).
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "lgp/mods/mod_file.hpp"

namespace lgp {
struct CpuState;
}

namespace lgp::rt {
class GuestMemory;
}

namespace lgp::mods {

struct ModInfo {
    std::string id;           // "wild-battles": file name and --mods value
    // For the settings menu, in English; the app shows them in the interface
    // language (lgp/platform/ui_text.hpp, a German line for each in its table).
    std::string title;
    std::string description;  // one line for the menu
    // false: the mod starts (hooks, log) but does not change the game yet.
    bool ready = false;
    // true: set_active() switches it while the game runs (the mod checks the
    // switch where it acts); false: a change counts from the next start.
    bool live = false;
    // true: a data mod, only files that replace the game's RomFS files
    // (<dir>/<id>/romfs/<path>, romfs_files()), no .cfg, no hooks; it counts
    // from the next start.
    bool data = false;
    // true: not on the menu's page, another setting switches it (online-button
    // follows "Online", boot::app_arguments).
    bool hidden = false;
};

// The mod `id` only runs with (both named; nullptr: none). Without it, `id`
// is not started and its RomFS files are left out: battle-balls needs
// wild-battles, which stores a Pokémon caught in a battle.
const char* needed_mod(std::string_view id);
// Whether `id` may act: it needs no other mod, or both are started (for the
// RomFS files of `id`, asked after start(), hle::FsConfig::romfs_keep).
bool needs_met(std::string_view id);

// Every mod this build knows, in menu order.
const std::vector<ModInfo>& known_mods();
const ModInfo* find_mod(std::string_view id);

// "a,b c" -> {"a", "b", "c"}; empty parts dropped, duplicates kept once.
std::vector<std::string> parse_mod_list(std::string_view list);
std::string join_mod_list(const std::vector<std::string>& ids, char sep = ' ');

// Guest memory at a hook-log door (finding the layout of a role's arguments,
// P8.1): LGP_HOOK_DUMP=VA:EXPR:LEN[:after],... EXPR starts with a register
// (x0-x30, sp, tp = tpidr_el0; ret = x0 after the call, only with :after), then any number of
// >OFF (load the pointer at base+OFF) and +OFF (add). "0x7100001000:x1>0:0x80"
// logs 0x80 bytes of the object x1 points at. :after reads the memory when the
// function has returned, with the registers it was entered with (an output
// block passed by pointer).
struct HookDump {
    struct Step {
        bool load = false;  // >OFF: base = *(uint64_t*)(base + OFF); +OFF: base += OFF
        uint64_t offset = 0;
    };
    static constexpr unsigned kSp = 31;
    static constexpr unsigned kRet = 32;
    static constexpr unsigned kTp = 33;  // tpidr_el0, the thread's TLS block
    uint64_t va = 0;    // the door
    unsigned reg = 0;   // 0-30, kSp, kRet
    std::vector<Step> steps;
    uint32_t length = 0;  // 1 to 4096 bytes
    bool after = false;
    std::string expr;     // as written, for the log
};
bool parse_hook_dumps(std::string_view spec, std::vector<HookDump>* out, std::string* error);
// One EXPR alone (reg and steps of *out), and the address it names on the
// entry registers (`ret` = ret); false if a pointer on the way is unreadable.
bool parse_hook_expr(std::string_view expr, HookDump* out);
bool eval_hook_expr(const HookDump& expr, const CpuState& entry, uint64_t ret, uint64_t* out);
// The log lines of one dump, call k of the door; `entry` holds the registers
// at the entry of the door, `ret` x0 after the call (dumps with `after`):
// "hook dump: VA call k EXPR = ADDR" ("… after: …" for dumps after the call)
// and 16 bytes per line, or why it is unreadable. Every line names the door
// and the call, so lines of two guest threads in the same door can be told apart.
std::vector<std::string> hook_dump_lines(const HookDump& dump, const CpuState& entry, uint32_t call, uint64_t ret = 0);

// Acts at hook-log doors (trying a mod's calls before writing its code,
// Fang-4b): LGP_HOOK_ACT=DOOR[#SEL]:ACT[:after],... SEL picks the door's
// calls: K (call K), K- (K and later), K-M; none = every call. ACT:
//   call:FN(xN=V;...)  BL into FN through the dispatch table, on the door's
//                      registers plus the given ones; all registers are put
//                      back afterwards (a halt stays)
//   set:EXPR=V[/SIZE]  stores V (SIZE 1, 2, 4 or 8 bytes, default 4) at EXPR
//   ret:V              x0 = V, the original does not run (not with :after)
// V is a number (0x…, decimal, -N) or an EXPR as in LGP_HOOK_DUMP on the
// registers of the door's entry; there `ret` is x0 of the last call act
// before it in the same call, after the original x0 of the original. Acts of
// one call run in the order written, every call (not only the logged ones),
// and log "hook act: DOOR call k …".
struct HookAct {
    enum Kind { kCall, kSet, kRet, kRetFloat };
    struct Value {
        bool constant = true;
        uint64_t number = 0;
        HookDump expr;  // reg and steps, when !constant
    };
    uint64_t va = 0;  // the door
    uint32_t first = 1;
    uint32_t last = UINT32_MAX;
    Kind kind = kCall;
    bool after = false;
    uint64_t fn = 0;                                // kCall
    std::vector<std::pair<unsigned, Value>> args;   // kCall: register, value
    HookDump target;                                // kSet: the address
    unsigned size = 4;                              // kSet
    Value value;                                    // kSet, kRet
    float real = 0;                                 // kRetFloat: s0
    std::string text;                               // as written, for the log
};
bool parse_hook_acts(std::string_view spec, std::vector<HookAct>* out, std::string* error);
// Runs the acts of `door_va` for call k that match `after`, on ctx; entry
// holds the registers at the entry of the door, *ret the value of `ret`
// (updated by call acts). False if a ret act took the call (x0, or s0 for retf, is set, the
// original must not run) or the guest halted. log = false leaves out the
// "hook act:" lines (warnings stay), for acts that run on every frame.
bool run_hook_acts(const std::vector<HookAct>& acts, bool after, CpuState* ctx, const CpuState& entry, uint32_t call,
                   uint64_t* ret, bool log = true);

struct StartOptions {
    std::vector<std::string> enabled;  // ids
    std::filesystem::path dir;         // where the mod files are (<game>/mods)
    // > 0: every hook door without a mod hook logs its first N calls with
    // x0-x3 and the return address (finding the roles in the game, P8.1).
    uint32_t hook_log = 0;
    // Dumps on those calls, before the original runs (LGP_HOOK_DUMP).
    std::vector<HookDump> dumps;
    // Acts at those doors (LGP_HOOK_ACT).
    std::vector<HookAct> acts;
    // Live mods not in `enabled` whose file is in `dir` are installed too,
    // switched off (their doors then do not log).
    bool standby = false;
    // The process's guest memory, for mods that patch instructions
    // (set_immediate_patch reads them); nullptr: such mods do not start.
    rt::GuestMemory* memory = nullptr;
    // Width / height of the screen the game is shown on (window, device);
    // 0 = unknown. The widescreen mod sizes the game's picture to it.
    double display_aspect = 0;
    // HUD bounds in percent (widescreen, settings.ini hud_bounds): how far the
    // header, footer and side groups move from the 16:9 frame towards the edge
    // of a wider or taller picture; 100 the edge, 0 they stay in the 16:9
    // frame (nothing is stretched either way).
    int hud_bounds = 100;
    // The battle's HUD bounds per side (top, bottom, left, right), percent of
    // the extra room on that side, for the layouts a `layouts … hud L,…` line
    // names; the rest then always goes to the edge. -1: hud_bounds.
    std::array<int, 4> hud_sides = {-1, -1, -1, -1};
};

struct StartResult {
    std::vector<std::string> active;    // ids that started, switched on
    std::vector<std::string> standby;   // live mods installed switched off (StartOptions::standby)
    std::vector<std::string> messages;  // why a mod did not start, notes; for the log
};

// Installs the hooks of the enabled mods, before guest code runs. False with
// *error only for a wrong call (unknown id, unreadable mod file); a mod that
// lacks something is reported in result->messages and left out.
bool start(const StartOptions& options, StartResult* result, std::string* error);
// Whether the mod was started (switched on or, live, off).
bool enabled(std::string_view id);
// Switches a started live mod on or off; it acts from its next chance (for
// wild-battles: the next touch, a battle already running ends as it is).
// False if the mod is not started or not live.
bool set_active(std::string_view id, bool on);
// Whether the mod is started and switched on.
bool active(std::string_view id);
// The mod file of a started mod, nullptr otherwise (roles and values for
// mods that read more than hooks).
const ModFile* mod_file(std::string_view id);
// wild-battles: battle events requested at a touch / done since the process
// started (the press script's battle-start / battle-end).
uint32_t wild_battles_started();
uint32_t wild_battles_done();
// random-encounters: the chance of an encounter per step, out of 256 (the app's
// switch "Pokémon Gelb" sets kRandomEncounterOriginal). kRandomEncounterOriginal takes
// the rate of the zone the player walks in from the mod file's zone_rate lines
// (Gelb's rate per area; zones without one: step_rate). 0 goes back to the mod
// file's step_rate (default 10). Acts from the next step; may be called before
// start().
constexpr uint32_t kRandomEncounterOriginal = 0x10000;
void set_random_encounter_rate(uint32_t per256);
// The rate in effect (the menu's, else the mod file's; kRandomEncounterOriginal
// as set).
uint32_t random_encounter_rate();
// random-encounters: the share of field Pokémon that are hidden (and may
// attack), in percent (the app's switch "Pokémon Gelb" sets 100). Above 100 goes back to the mod file's hidden_percent (default 100).
// Acts on the next spawns; may be called before start().
void set_random_encounter_hidden_percent(uint32_t percent);
// The share in effect (the menu's, else the mod file's).
uint32_t random_encounter_hidden_percent();
// random-encounters: only hide the field Pokémon (the app's "Hide field
// Pokémon", only in Let's Go, Pokémon Gelb off): no die and no Gelb battles,
// the hidden ones stay touchable (hide_contact 2 does not apply). Acts on the
// next check; may be called before start().
void set_random_encounter_hide_only(bool on);
bool random_encounter_hide_only();
// online-button: called with true when the player chose "far away" with the
// mod on (the session goes to the server), false for "nearby" or with the mod
// off (the LAN). lgp_game passes hle::set_ldn_remote; nullptr: nobody listens.
// Kept across stop().
void set_ldn_route_handler(void (*fn)(bool server));
// The RomFS files that the named mods replace: every file under
// <dir>/<id>/romfs/ as image path ("/bin/x.bin") -> contents, for fsp-srv
// (hle::FsConfig::romfs_replace), which is registered before start() runs.
// Any mod may bring such files. A path two mods replace keeps the later mod's
// file; that and unreadable files are reported in *messages. *owners (if
// given) gets path -> id of the mod whose file it is.
std::map<std::string, std::vector<uint8_t>> romfs_files(const std::filesystem::path& dir,
                                                        const std::vector<std::string>& ids,
                                                        std::vector<std::string>* messages,
                                                        std::map<std::string, std::string>* owners = nullptr);
// Calls that reached a hook-log door since start() (StartOptions::hook_log).
uint64_t hook_log_calls();
// Removes all hooks and patches and forgets the started mods (tests, a new process).
void stop();

}  // namespace lgp::mods
