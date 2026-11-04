// Triangles of the app's own interface (P7, docs/analysis/27-p7-app-oberflaeche.md):
// menus and settings around the game, built by the UI library on the host's
// main thread and drawn by the renderer over the game image (after the touch
// overlay, lgp/platform/overlay.hpp). Neutral on purpose: the renderer knows
// nothing of the library that produced the lists (Dear ImGui in platform/ui).
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace lgp::platform {

// One corner: position in UI units (0..UiDrawData::width/height, y down),
// texture coordinate in the command's texture, colour as R, G, B, A bytes in memory
// order (sRGB-encoded, straight alpha).
struct UiVertex {
    float x = 0, y = 0;
    float u = 0, v = 0;
    uint32_t rgba = 0;
};

// Triangles indices[first_index .. first_index + index_count), each index
// offset by base_vertex, clipped to clip (x0, y0, x1, y1 in UI units).
struct UiCommand {
    float clip[4] = {0, 0, 0, 0};
    uint32_t first_index = 0;
    uint32_t index_count = 0;
    uint32_t base_vertex = 0;
    uint32_t image = 0;  // 0: the atlas (UiDrawData::texture), n: images[n - 1]
    // 0: over what is there by alpha; 1: multiplies it by the colour
    // (destination × source, alpha left out), as layouts of the game do.
    uint8_t blend = 0;
};

// A texture of the lists (the font atlas, or a picture such as one of the
// game's own menu graphics): tightly packed RGBA8. A new version (unique
// among all textures of a producer) replaces the uploaded one.
struct UiTexture {
    uint32_t width = 0, height = 0;
    uint64_t version = 0;
    std::vector<uint8_t> rgba;
};

struct UiDrawData {
    // Size of the UI space; the renderer stretches it over the whole surface.
    float width = 0, height = 0;
    std::vector<UiVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<UiCommand> commands;
    std::shared_ptr<const UiTexture> texture;
    // Further pictures, kept by the producer while it uses them (the
    // renderer may keep one uploaded as long as the same object comes back).
    std::vector<std::shared_ptr<const UiTexture>> images;
};

}  // namespace lgp::platform
