// Mod "classic-exp" (P8.16, docs/analysis/51-p8-wilde-kaempfe.md): experience
// as in Pokémon Yellow. Off by default, live (the menu's page "Spiel").
//
// After every K.O. the game computes, per team member, a table entry: the
// amount, the effort values and flags (bonus, fought, Exp. Share). Its own
// formula: m = base × level rounded down to a multiple of 7, fought m/12, the
// others m/24 (the Exp. Share is always on), at least 1, times its factors
// (other trainer ×1.5 or ×1.7, can still evolve, affection, GO origin). The mod
// hooks that function and writes, after it, the amount of Generation 1:
//
//   fought:     floor(base × level / 7) / s  (s = the members that fought)
//               × 3/2 when the member has another trainer
//               × 3/2 in a trainer battle
//               at least 1
//   not fought: 0 (the game then shows no text and adds nothing)
//
// and clears the bonus flag (the game's "extra" text). The effort values stay
// the game's. base is the game's base experience of the defeated species, not
// the table of Generation 1. Catching gives no experience: the catch scene's
// amount (catch_exp_amount) is 0 while the switch is on, and a catch in the
// battle gives 0 too: while the server's catch experience runs (hook
// exp_caught_door, optional) the formula's amounts are 0.
//
// Lines in classic-exp.cfg (game data, $LGP_PRIVATE_DIR):
//   hook exp_formula VA          the per-member function (required)
//   hook catch_exp_amount VA     the catch scene's amount (required; shared
//                                with wild-battles, one door)
//   value exp_in_reg N           entry register of the input (base, level …)
//   value exp_out_reg N          entry register of the member's table entry
//   value exp_F_offset N         F = base, level, kind, ot (1 = the player is
//                                the original trainer), fighters (s), fought
//   value exp_F_size N           (input) and amount, bonus (output); size 1,
//   value exp_F_mask N           2, 4 or 8 (default 4), mask default all bits
//                                (a field in bits: the value is shifted down)
//   hook exp_caught_door VA      optional: the server's catch experience (it
//                                runs the formula for a catch in the battle)
//   value exp_wild_kind N        kind of a wild battle (default 0)
//   value exp_trainer_kind N     optional: only this kind is a trainer battle;
//                                without it every kind but wild is one
//   value exp_log N              log the first N calls (default 16)
#include <atomic>
#include <cinttypes>
#include <string>

#include "lgp/lifted/cpu_state.hpp"
#include "lgp/runtime/guest_mem.hpp"
#include "lgp/runtime/log.hpp"
#include "mod_impl.hpp"

namespace lgp::mods::detail {

namespace {

struct Field {
    uint64_t offset = 0;
    uint32_t size = 4;
    uint64_t mask = ~uint64_t{0};
};

enum FieldId { kBase, kLevel, kKind, kOt, kFighters, kFought, kAmount, kBonus, kFields };
constexpr const char* kFieldNames[kFields] = {"base",     "level",  "kind",   "ot",
                                              "fighters", "fought", "amount", "bonus"};

struct Config {
    unsigned in_reg = 0;
    unsigned out_reg = 0;
    Field fields[kFields];
    uint64_t wild_kind = 0;
    bool trainer_kind_set = false;
    uint64_t trainer_kind = 0;
    uint32_t log = 16;
};

Config g_config;
std::atomic<bool> g_on{false};  // ModImpl::on: the menu's switch
std::atomic<uint32_t> g_calls{0};
// Inside the server's catch experience (hook exp_caught_door).
std::atomic<bool> g_in_catch{false};

uint64_t size_mask(uint32_t size) { return size >= 8 ? ~uint64_t{0} : (uint64_t{1} << (8 * size)) - 1; }

unsigned shift_of(uint64_t mask) {
    unsigned s = 0;
    while (s < 63 && !(mask >> s & 1))
        ++s;
    return s;
}

uint64_t load_raw(uint64_t va, uint32_t size) {
    switch (size) {
    case 1: return rt::load<uint8_t>(va);
    case 2: return rt::load<uint16_t>(va);
    case 4: return rt::load<uint32_t>(va);
    default: return rt::load<uint64_t>(va);
    }
}

void store_raw(uint64_t va, uint32_t size, uint64_t v) {
    switch (size) {
    case 1: rt::store<uint8_t>(va, static_cast<uint8_t>(v)); break;
    case 2: rt::store<uint16_t>(va, static_cast<uint16_t>(v)); break;
    case 4: rt::store<uint32_t>(va, static_cast<uint32_t>(v)); break;
    default: rt::store<uint64_t>(va, v); break;
    }
}

uint64_t read(const Field& f, uint64_t base) {
    const uint64_t mask = f.mask & size_mask(f.size);
    return (load_raw(base + f.offset, f.size) & mask) >> shift_of(mask);
}

void write(const Field& f, uint64_t base, uint64_t v) {
    const uint64_t mask = f.mask & size_mask(f.size);
    const unsigned shift = shift_of(mask);
    const uint64_t max = mask >> shift;
    const uint64_t old = load_raw(base + f.offset, f.size);
    store_raw(base + f.offset, f.size, (old & ~mask) | ((v > max ? max : v) << shift & mask));
}

// The amount of Generation 1 (Yellow) for one member.
uint64_t classic_exp_amount(uint64_t base, uint64_t level, uint64_t count, bool fought, bool own, bool trainer) {
    if (!fought)
        return 0;
    uint64_t v = base * level / 7 / (count == 0 ? 1 : count);
    if (!own)
        v = v * 3 / 2;
    if (trainer)
        v = v * 3 / 2;
    return v == 0 ? 1 : v;
}

// hook exp_formula: the game's entry first (its effort values and flags), then
// our amount and no bonus.
void on_exp_formula(lgp::CpuState* ctx, lgp::LiftedFn original, void*) {
    const uint64_t in = ctx->x[g_config.in_reg];
    const uint64_t out = ctx->x[g_config.out_reg];
    original(ctx);
    if (ctx->halt != kHaltNone || !g_on.load(std::memory_order_relaxed))
        return;
    const Field* f = g_config.fields;
    const uint64_t base = read(f[kBase], in), level = read(f[kLevel], in), kind = read(f[kKind], in);
    const uint64_t count = read(f[kFighters], in);
    const bool own = read(f[kOt], in) != 0, fought = read(f[kFought], in) != 0;
    const bool caught = g_in_catch.load(std::memory_order_relaxed);
    const bool trainer = g_config.trainer_kind_set ? kind == g_config.trainer_kind : kind != g_config.wild_kind;
    const uint64_t amount = caught ? 0 : classic_exp_amount(base, level, count, fought, own, trainer);
    const uint64_t game = read(f[kAmount], out);
    write(f[kAmount], out, amount);
    write(f[kBonus], out, 0);
    if (const uint32_t n = g_calls.fetch_add(1, std::memory_order_relaxed); n < g_config.log)
        rt::log(rt::LogLevel::Info,
                "classic-exp: base %" PRIu64 " level %" PRIu64 " kind %" PRIu64 " ot %d fighters %" PRIu64
                " fought %d%s: %" PRIu64 " instead of %" PRIu64,
                base, level, kind, own, count, fought, caught ? " caught" : "", amount, game);
}

// hook exp_caught_door: the formula runs inside it for a catch in the battle.
void on_exp_caught(lgp::CpuState* ctx, lgp::LiftedFn original, void*) {
    if (!g_on.load(std::memory_order_relaxed)) {
        original(ctx);
        return;
    }
    g_in_catch = true;
    original(ctx);  // a guest fault stops the game, nothing left to reset
    g_in_catch = false;
}

bool catch_exp_zero() { return g_on.load(std::memory_order_relaxed); }

bool read_field(const ModFile& file, FieldId id, Field* f, std::string* why) {
    const std::string name = std::string("exp_") + kFieldNames[id];
    const std::optional<uint64_t> offset = file.value(name + "_offset");
    if (!offset) {
        *why = "no `value " + name + "_offset` in " + file.path.filename().string();
        return false;
    }
    f->offset = *offset;
    if (const std::optional<uint64_t> size = file.value(name + "_size")) {
        if (*size != 1 && *size != 2 && *size != 4 && *size != 8) {
            *why = "value " + name + "_size must be 1, 2, 4 or 8";
            return false;
        }
        f->size = static_cast<uint32_t>(*size);
    }
    if (const std::optional<uint64_t> mask = file.value(name + "_mask"))
        f->mask = *mask;
    if ((f->mask & size_mask(f->size)) == 0) {
        *why = "value " + name + "_mask has no bit inside the field";
        return false;
    }
    return true;
}

bool read_reg(const ModFile& file, const char* name, unsigned* reg, std::string* why) {
    const std::optional<uint64_t> v = file.value(name);
    if (!v || *v > 30) {
        *why = std::string(v ? "" : "no ") + "`value " + name + "`" + (v ? " must be x0…x30" : "") + " in " +
               file.path.filename().string();
        return false;
    }
    *reg = static_cast<unsigned>(*v);
    return true;
}

bool install(const ModFile& file, const StartOptions&, std::string* why) {
    Config c;
    if (!read_reg(file, "exp_in_reg", &c.in_reg, why) || !read_reg(file, "exp_out_reg", &c.out_reg, why))
        return false;
    for (int id = 0; id < kFields; ++id)
        if (!read_field(file, FieldId(id), &c.fields[id], why))
            return false;
    c.wild_kind = file.value("exp_wild_kind").value_or(0);
    if (const std::optional<uint64_t> t = file.value("exp_trainer_kind")) {
        c.trainer_kind_set = true;
        c.trainer_kind = *t;
    }
    c.log = static_cast<uint32_t>(file.value("exp_log").value_or(16));
    g_config = c;
    g_calls = 0;
    g_in_catch = false;
    if (file.hook("exp_caught_door") && !hook_role(file, "exp_caught_door", &on_exp_caught, nullptr, why))
        return false;
    return hook_role(file, "exp_formula", &on_exp_formula, nullptr, why) &&
           zero_catch_exp(file, &catch_exp_zero, why);
}

}  // namespace

const ModImpl& classic_exp_mod() {
    static const ModImpl m = {
        {"classic-exp", "EXP as in Yellow",
         "Experience by the formula of Pokémon Yellow: only fighters, divided by their number, no bonus, catching "
         "gives no EXP (needs the mod file).",
         true, true},
        &install,
        &g_on,
    };
    return m;
}

}  // namespace lgp::mods::detail
