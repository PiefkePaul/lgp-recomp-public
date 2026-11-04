// battle-balls: Poké Balls in the bag during wild battles. A data mod: its
// battle_pocket.dat in <game>/mods/battle-balls/romfs/ puts the balls on a bag
// page in battle (the game's own file has them on 0). The battle server still
// has the ball branch with the catch formula; trainer battles block the ball.
// The file is made by tools/items/battle_pocket.py from the game's file and
// stays private (docs/analysis/55-fangen-im-kampf.md).
#include "mod_impl.hpp"

namespace lgp::mods::detail {

namespace {

std::atomic<bool> g_on{false};

bool install(const ModFile&, const StartOptions&, std::string*) { return true; }

}  // namespace

const ModImpl& battle_balls_mod() {
    static const ModImpl m = {
        {"battle-balls", "Catch in battle",
         "Poké Balls in the bag of wild battles: catching starts from the battle menu (how: Catch style in "
         "battle), and one defeated is gone. Off: the Let's Go catch scene after a won battle. Needs the file in "
         "the folder mods and Wild battles, from the next start.",
         true, false, true},
        &install,
        &g_on,
    };
    return m;
}

}  // namespace lgp::mods::detail
