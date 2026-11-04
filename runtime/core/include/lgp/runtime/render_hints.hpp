// Hints from game mods to the graphics HLE (lgp::mods widescreen `repeat`
// lines): sampled textures of a given size whose clamped lookups repeat
// instead, so a pattern the mod widened (more of its texture shown) repeats
// rather than smearing its edge. Only file textures (not render targets).
//
// Thread-safe: set while guest threads run (the menu turns a mod on or off).
#pragma once

#include <cstdint>
#include <vector>

namespace lgp::rt {

struct RepeatHint {
    uint32_t width = 0, height = 0;
    bool u = false, v = false;
};

// Replaces the list (empty: none).
void set_repeat_hints(std::vector<RepeatHint> hints);
// For a texture of width x height: which axes repeat. False if none.
bool repeat_hint(uint32_t width, uint32_t height, bool* u, bool* v);

}  // namespace lgp::rt
