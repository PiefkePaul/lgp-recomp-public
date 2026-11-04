// The free stand-in for the standard system font (cmake/LgpFreeFont.cmake):
// a TrueType file compiled into lgp_free_font. pl:u serves it when the game
// storage has no /systemdata/0100000000000811 from the user's firmware.
// Only defined when LGP_FREE_FONT_AVAILABLE (lgp_runtime_hle then defines
// LGP_HLE_FREE_FONT=1).
#pragma once

#include <cstdint>
#include <span>

namespace lgp::hle {

std::span<const uint8_t> free_system_font();

}  // namespace lgp::hle
