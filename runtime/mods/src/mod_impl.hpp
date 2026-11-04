// What each mod implements (src/<mod>.cpp), registered in mods.cpp.
#pragma once

#include <atomic>
#include <string>

#include "lgp/lifted/cpu_state.hpp"
#include "lgp/mods/mod_file.hpp"
#include "lgp/mods/mods.hpp"

namespace lgp::mods::detail {

struct ModImpl {
    ModInfo info;
    // Installs the mod's hooks (lgp::rt::set_hook). False with *why when the
    // file lacks something the mod needs; the caller then removes what was
    // installed.
    bool (*install)(const ModFile& file, const StartOptions& options, std::string* why);
    // The switch the mod checks where it acts (set by start, set_active, stop).
    std::atomic<bool>* on;
};

const ModImpl& wild_battles_mod();
// Whether the installed wild-battles.cfg stores a Pokémon caught in a
// battle (`from post_result`, way A); battle-balls needs that.
bool wild_battles_catches();
// The Gelb mode of random-encounters: a wild battle as for a touch, but with
// this species, form and level (wild-battles.cfg `value direct_species` …).
// Runs on the hook's registers (they come back as they were). *requested:
// the battle event exists. False if a guest call halted.
bool wild_battles_direct(lgp::CpuState* ctx, uint32_t species, uint32_t form, uint32_t level, bool* requested);
// Whether the installed wild-battles.cfg can make such a battle.
bool wild_battles_direct_ready();
const ModImpl& original_encounters_mod();
const ModImpl& battle_balls_mod();
const ModImpl& battle_throw_mod();
const ModImpl& widescreen_mod();
const ModImpl& random_encounters_mod();
const ModImpl& classic_exp_mod();
const ModImpl& online_button_mod();
const ModImpl& skip_controller_screen_mod();
const ModImpl& audio_free_guard_mod();

// set_hook for the role; false with *why when the role is missing or has no door.
bool hook_role(const ModFile& file, const char* role, void (*fn)(lgp::CpuState*, lgp::LiftedFn, void*),
               void* user, std::string* why);

// hook catch_exp_amount, shared by the mods that take the catch scene's
// experience away (one door, one hook): the amount is 0 while any of their
// tests is true. False with *why as hook_role; stop() forgets the tests.
bool zero_catch_exp(const ModFile& file, bool (*test)(), std::string* why);

// BL from a hook into the guest function at va, on the hook's registers (the
// caller sets the arguments, sp and saves what it needs back). The call goes
// through the dispatch table, so a hook on va runs too. Lifted code returns by
// RET, x30 is not used. Nothing with a destructor may live across the call: a
// guest fault unwinds with longjmp. False if va has no function (ctx halts
// with kHaltBadJump) or the guest halted.
bool call_guest(lgp::CpuState* ctx, uint64_t va);

}  // namespace lgp::mods::detail
