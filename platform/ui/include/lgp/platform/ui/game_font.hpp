// The game's own fonts for the app's menus. Switch games ship OpenType and
// TrueType fonts wrapped as BFOTF/BFTTF: an 8-byte header (magic and size,
// each XORed with a key) and the font XORed word by word with the same key.
// The app reads them from the user's RomFS at run time; no font of the game
// is part of the repository.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace lgp::platform::ui {

// The plain OpenType/TrueType font inside a BFOTF/BFTTF; a file that already
// is a plain font is returned as is. nullopt if neither.
std::optional<std::vector<uint8_t>> unwrap_game_font(std::span<const uint8_t> file);

// True if `font` starts like an OpenType/TrueType font (sfnt version tag).
bool looks_like_sfnt(std::span<const uint8_t> font);

}  // namespace lgp::platform::ui
