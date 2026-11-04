// Textures of the game's interface for the app's menus: BNTX containers
// (the NX texture format: "BNTX" header, "NX  " table, one "BRTI" block per
// texture with format, size, block-linear layout and the offsets of its mip
// levels) as they sit in the layout archives (timg/__Combined.bntx). Level 0
// of each texture is decoded to RGBA8 with the project's block-linear and
// BCn/ASTC decoders. Read from the user's RomFS at run time only.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace lgp::platform::ui {

struct UiImage {
    std::string name;
    uint32_t width = 0, height = 0;
    uint32_t format = 0;  // BNTX format word (type << 8 | kind), for diagnostics
    std::vector<uint8_t> rgba;  // width * height * 4, straight alpha
};

// All textures of a BNTX; those in formats this decoder lacks are left out
// and named in *skipped. Empty and *error if the data is no BNTX.
std::vector<UiImage> decode_bntx(std::span<const uint8_t> data, std::string* error,
                                 std::vector<std::string>* skipped = nullptr);

}  // namespace lgp::platform::ui
