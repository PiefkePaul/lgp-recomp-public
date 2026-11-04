// Mod "online-button" (docs/knowledge/ldn-ueber-ryuldn.md, Abschnitt
// "Online-Knopf"): the game's choice "far away" (Nintendo's online play) leads
// into the flow of "nearby" (local wireless, ldn), and that session goes
// through our RyuLDN server instead of the LAN. Off by default and not on the
// menu's page: the app names it while "Online" is on (boot::app_arguments).
//
// The menu frame of the choice writes, after the player confirmed, a next
// state and an online flag and then changes state: nearby gives one pair,
// far away another. The mod gives the far-away branch the nearby pair and
// tells the ldn links which way the next session goes (set_ldn_route_handler,
// in lgp_game hle::set_ldn_remote: with --ldn both the server, else the
// only link there is). Nintendo's online play itself (nifm, a Nintendo
// Account, NEX) is not reached any more.
//
// Lines in online-button.cfg (game data, $LGP_PRIVATE_DIR):
//   patch far_state VA    the MOV (MOVZ/MOVN or bitmask) of the far branch's
//                         next state (required)
//   patch far_flag VA     the MOV of its online flag (required)
//   patch near_state VA   optional: the MOV of the nearby branch's next state;
//                         observed only, a pass sends the session to the LAN
//                         again (without it a far choice keeps the server
//                         until the next start)
//   value far_state N     the state the far branch gets (default 2, nearby's)
//   value far_flag N      the flag it gets (default 0, nearby's)
//   value log N           log the first N choices (default 16)
// Without the mod the far branch keeps the game's own values.
#include <atomic>
#include <cinttypes>
#include <string>

#include "lgp/lifted/cpu_state.hpp"
#include "lgp/runtime/guest_mem.hpp"
#include "lgp/runtime/log.hpp"
#include "lgp/runtime/patch.hpp"
#include "mod_impl.hpp"

namespace lgp::mods {

namespace {
std::atomic<void (*)(bool)> g_route{nullptr};
}  // namespace

void set_ldn_route_handler(void (*fn)(bool server)) { g_route.store(fn, std::memory_order_release); }

namespace detail {

namespace {

struct Config {
    uint64_t far_state = 2;
    uint64_t far_flag = 0;
    uint32_t log = 16;
};

Config g_config;
std::atomic<bool> g_on{false};  // ModImpl::on: the menu's switch
std::atomic<uint32_t> g_choices{0};

void route(bool server) {
    if (void (*fn)(bool) = g_route.load(std::memory_order_acquire))
        fn(server);
}

// The MOV at the patch site with `value` instead of its own (or its own:
// keep), as the instruction would have written it.
void write_move(lgp::CpuState* ctx, const char* name, bool replace, uint64_t value) {
    const uint64_t va = ctx->pc - 4;
    const uint32_t insn = rt::load<uint32_t>(va);
    unsigned rd = 0;
    uint64_t own = 0;
    if (!rt::decode_move_immediate(insn, &rd, &own)) {
        rt::log(rt::LogLevel::Error, "online-button: patch %s 0x%" PRIx64 " is no MOV (0x%08x)", name, va, insn);
        ctx->halt = kHaltUndefined;
        ctx->halt_info = insn;
        return;
    }
    if (replace)
        own = (insn >> 31) ? value : (value & 0xFFFF'FFFFu);
    if (rd != 31)
        ctx->x[rd] = own;
}

void on_far_state(lgp::CpuState* ctx, void*) {
    const bool on = g_on.load(std::memory_order_relaxed);
    write_move(ctx, "far_state", on, g_config.far_state);
    if (ctx->halt != kHaltNone)
        return;
    route(on);
    if (const uint32_t n = g_choices.fetch_add(1, std::memory_order_relaxed); n < g_config.log)
        rt::log(rt::LogLevel::Info, "online-button: far away chosen, %s",
                on ? "nearby flow through the server" : "switched off, the game's online play");
}

void on_far_flag(lgp::CpuState* ctx, void*) {
    write_move(ctx, "far_flag", g_on.load(std::memory_order_relaxed), g_config.far_flag);
}

void on_near_state(lgp::CpuState*, void*) {  // observed: the instruction still runs
    route(false);
    if (const uint32_t n = g_choices.fetch_add(1, std::memory_order_relaxed); n < g_config.log)
        rt::log(rt::LogLevel::Info, "online-button: nearby chosen, the LAN");
}

bool install(const ModFile& file, const StartOptions&, std::string* why) {
    const std::optional<uint64_t> far_state = file.patch("far_state"), far_flag = file.patch("far_flag");
    if (!far_state || !far_flag) {
        *why = std::string("no `patch ") + (far_state ? "far_flag" : "far_state") + "` in " +
               file.path.filename().string();
        return false;
    }
    Config c;
    c.far_state = file.value("far_state").value_or(c.far_state);
    c.far_flag = file.value("far_flag").value_or(c.far_flag);
    c.log = static_cast<uint32_t>(file.value("log").value_or(c.log));
    g_config = c;
    g_choices = 0;
    rt::set_patch(*far_state, &on_far_state);
    rt::set_patch(*far_flag, &on_far_flag);
    if (const std::optional<uint64_t> near = file.patch("near_state"))
        rt::set_patch(*near, &on_near_state, nullptr, true);
    return true;
}

}  // namespace

const ModImpl& online_button_mod() {
    static const ModImpl m = {
        {"online-button", "Online button",
         "\"Far away\" plays over our own server instead of Nintendo's (on with \"Online\", needs the mod file).",
         true, false, false, true},
        &install,
        &g_on,
    };
    return m;
}

}  // namespace detail
}  // namespace lgp::mods
