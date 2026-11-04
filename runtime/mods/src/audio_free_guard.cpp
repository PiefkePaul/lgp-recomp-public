// Mod "audio-free-guard" (hidden, audio hang after a reopen): keeps the
// game's count of free audio buffers from wrapping.
//
// The game's sound frame refills its output in a batch: it takes the number
// of free buffers (total - queued, unsigned) once and appends that many,
// counting down to 0 without fetching released buffers in between. A
// re-create of the output that the game's stall detector starts in the middle
// of such a batch can leave queued above total (seen: 10 of 4); free then
// reads 0xfffffffa and the batch appends about 4 G times, the sound hangs
// for good and audout's queue grows without end (Ghidra and a PC run with a
// forced device loss; the runtime releases the buffers, the game never asks
// again). The mod sets free to 0 instead: the game then fetches released
// buffers, queued drops back to total by itself (the extra buffers were
// really appended and are released as they play), and the next batch is
// normal. Found in Ghidra, static; checked on the game only as noted in
// docs/knowledge/horizon-audout-hwopus.md.
//
// Lines in audio-free-guard.cfg (game data, $LGP_PRIVATE_DIR):
//   patch free VA        an instruction after free is computed into the
//                        register and before the game uses it; the mod runs
//                        before it, the instruction itself still runs
//                        (required; lift with --patch-site VA or --mods)
//   value free_reg N     that register, as W (default 8)
// free counts as wrapped when it is negative as a signed 32-bit value (total
// is small: 4 in LGP, at most the 32 buffers an output holds). In LGP the
// site is the STR that hands free to the caller through its out pointer.
// The app names the mod itself when the file is in <game>/mods.
#include <atomic>
#include <cinttypes>
#include <string>

#include "lgp/lifted/cpu_state.hpp"
#include "lgp/runtime/log.hpp"
#include "lgp/runtime/patch.hpp"
#include "mod_impl.hpp"

namespace lgp::mods::detail {

namespace {

unsigned g_free_reg = 8;
std::atomic<bool> g_on{false};  // ModImpl::on
std::atomic<uint32_t> g_clamps{0};

void on_free(lgp::CpuState* ctx, void*) {
    if (!g_on.load(std::memory_order_relaxed))
        return;
    const uint32_t free = static_cast<uint32_t>(ctx->x[g_free_reg]);
    if (static_cast<int32_t>(free) >= 0)
        return;
    ctx->x[g_free_reg] = 0;  // a W write clears the upper half
    const uint32_t n = g_clamps.fetch_add(1, std::memory_order_relaxed);
    if (n < 8 || (n & (n + 1)) == 0)  // the first 8, then at powers of two
        rt::log(rt::LogLevel::Warn,
                "audio-free-guard: free audio buffers read 0x%08x (more queued than the output holds), set to 0 "
                "(%u times)",
                free, n + 1);
}

bool install(const ModFile& file, const StartOptions&, std::string* why) {
    const std::optional<uint64_t> site = file.patch("free");
    if (!site) {
        *why = "no `patch free` in " + file.path.filename().string();
        return false;
    }
    const uint64_t reg = file.value("free_reg").value_or(8);
    if (reg > 30) {
        *why = "`value free_reg` must be a register 0..30";
        return false;
    }
    g_free_reg = static_cast<unsigned>(reg);
    g_clamps = 0;
    rt::set_patch(*site, &on_free, nullptr, /*observe=*/true);
    return true;
}

}  // namespace

const ModImpl& audio_free_guard_mod() {
    static const ModImpl m = {
        {"audio-free-guard", "Audio hang guard",
         "Keeps the game's sound from hanging after the audio output was reopened (needs the mod file).", true,
         false, false, true},
        &install,
        &g_on,
    };
    return m;
}

}  // namespace lgp::mods::detail
