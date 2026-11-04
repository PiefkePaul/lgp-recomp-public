// Mod "skip-controller-screen" (Paul 2026-10-28): the game starts with the
// controller set in the app (handheld by default) instead of asking for it on
// its controller screen at every start. The screen stays reachable from the
// game's own options.
//
// At boot the game creates its controller check with a mode that always shows
// the screen (it disconnects npads No1 and No2 and waits for the player).
// Mode 0, which the game never uses itself, checks quietly: it shows the
// screen only with two or more controllers connected or a style the game does
// not support, else it reports done at once. The mod gives the boot's check
// mode 0; the other callers (options and the like) keep theirs. Found in
// Ghidra, static; whether the game runs on cleanly without the screen is to
// be checked on the game.
//
// Lines in skip-controller-screen.cfg (game data, $LGP_PRIVATE_DIR):
//   patch boot_mode VA    the MOV (MOVZ/MOVN or bitmask) of the mode the boot
//                         gives its controller check (required)
//   value boot_mode N     the mode it gets instead (default 0, the quiet check)
// Without the mod the boot keeps the game's own mode. The app names the mod
// itself unless settings.ini says controller_screen = 1 (boot::app_arguments).
#include <atomic>
#include <cinttypes>
#include <string>

#include "lgp/lifted/cpu_state.hpp"
#include "lgp/runtime/guest_mem.hpp"
#include "lgp/runtime/log.hpp"
#include "lgp/runtime/patch.hpp"
#include "mod_impl.hpp"

namespace lgp::mods::detail {

namespace {

uint64_t g_mode = 0;
std::atomic<bool> g_on{false};  // ModImpl::on
std::atomic<uint32_t> g_passes{0};

void on_boot_mode(lgp::CpuState* ctx, void*) {
    const uint64_t va = ctx->pc - 4;
    const uint32_t insn = rt::load<uint32_t>(va);
    unsigned rd = 0;
    uint64_t own = 0;
    if (!rt::decode_move_immediate(insn, &rd, &own)) {
        rt::log(rt::LogLevel::Error, "skip-controller-screen: patch boot_mode 0x%" PRIx64 " is no MOV (0x%08x)", va,
                insn);
        ctx->halt = kHaltUndefined;
        ctx->halt_info = insn;
        return;
    }
    const bool on = g_on.load(std::memory_order_relaxed);
    const uint64_t value = on ? ((insn >> 31) ? g_mode : (g_mode & 0xFFFF'FFFFu)) : own;
    if (rd != 31)
        ctx->x[rd] = value;
    if (g_passes.fetch_add(1, std::memory_order_relaxed) < 4)
        rt::log(rt::LogLevel::Info, "skip-controller-screen: boot controller check mode %" PRIu64 " (game: %" PRIu64 ")",
                value, own);
}

bool install(const ModFile& file, const StartOptions&, std::string* why) {
    const std::optional<uint64_t> site = file.patch("boot_mode");
    if (!site) {
        *why = "no `patch boot_mode` in " + file.path.filename().string();
        return false;
    }
    g_mode = file.value("boot_mode").value_or(0);
    g_passes = 0;
    rt::set_patch(*site, &on_boot_mode);
    return true;
}

}  // namespace

const ModImpl& skip_controller_screen_mod() {
    static const ModImpl m = {
        {"skip-controller-screen", "Start without controller screen",
         "The game starts with the controller set here instead of asking at every start (needs the mod file).",
         true, false, false, true},
        &install,
        &g_on,
    };
    return m;
}

}  // namespace lgp::mods::detail
