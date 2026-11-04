// original-encounters: the wild Pokémon of an area (species, how often, levels)
// as in Pokémon Yellow. A data mod: its files in <game>/mods/original-encounters/
// romfs/ replace the game's spawn tables when the RomFS opens (fsp-srv), so it
// needs no hooks and counts from the next start. The tables are made from public
// data by a local tool and stay private like the mod files
// (docs/analysis/54-zufallsbegegnungen.md).
#include "mod_impl.hpp"

namespace lgp::mods::detail {

namespace {

std::atomic<bool> g_on{false};

bool install(const ModFile&, const StartOptions&, std::string*) { return true; }

}  // namespace

const ModImpl& original_encounters_mod() {
    static const ModImpl m = {
        {"original-encounters", "Encounters as in Yellow",
         "Species, frequency and levels of the wild Pokémon as in Pokémon Yellow (needs the data in the folder mods, "
         "from the next start).",
         true, false, true},
        &install,
        &g_on,
    };
    return m;
}

}  // namespace lgp::mods::detail
