#include "lgp/platform/ui/game_font.hpp"

#include <array>
#include <cstring>

namespace lgp::platform::ui {

bool looks_like_sfnt(std::span<const uint8_t> f) {
    if (f.size() < 12)
        return false;
    const bool tag = std::memcmp(f.data(), "OTTO", 4) == 0 || std::memcmp(f.data(), "true", 4) == 0 ||
                     std::memcmp(f.data(), "ttcf", 4) == 0 || (f[0] == 0 && f[1] == 1 && f[2] == 0 && f[3] == 0);
    if (!tag)
        return false;
    if (std::memcmp(f.data(), "ttcf", 4) == 0)
        return true;
    const unsigned tables = unsigned(f[4]) << 8 | f[5];  // big endian, as all of sfnt
    return tables >= 4 && tables <= 64;
}

std::optional<std::vector<uint8_t>> unwrap_game_font(std::span<const uint8_t> file) {
    if (looks_like_sfnt(file))
        return std::vector<uint8_t>(file.begin(), file.end());
    if (file.size() < 8 + 12)
        return std::nullopt;
    // The key Nintendo's shared fonts use (0x06186249, applied to little-
    // endian words; the header magic then reads 0x18029A7F), in both byte
    // orders; the result must look like a font.
    const std::array<std::array<uint8_t, 4>, 2> keys = {{{0x49, 0x62, 0x18, 0x06}, {0x06, 0x18, 0x62, 0x49}}};
    for (const auto& key : keys) {
        std::array<uint8_t, 12> head;
        for (size_t i = 0; i < head.size(); ++i)
            head[i] = file[8 + i] ^ key[i % 4];
        if (!looks_like_sfnt(head))
            continue;
        // Size from the header (in either byte order), else the rest of the file.
        uint8_t s[4];
        for (size_t i = 0; i < 4; ++i)
            s[i] = file[4 + i] ^ key[i];
        const uint64_t le = uint32_t(s[0]) | uint32_t(s[1]) << 8 | uint32_t(s[2]) << 16 | uint32_t(s[3]) << 24;
        const uint64_t be = uint32_t(s[3]) | uint32_t(s[2]) << 8 | uint32_t(s[1]) << 16 | uint32_t(s[0]) << 24;
        const uint64_t rest = file.size() - 8;
        const uint64_t size = le != 0 && le <= rest ? le : be != 0 && be <= rest ? be : rest;
        std::vector<uint8_t> out(static_cast<size_t>(size));
        for (size_t i = 0; i < out.size(); ++i)
            out[i] = file[8 + i] ^ key[i % 4];
        return out;
    }
    return std::nullopt;
}

}  // namespace lgp::platform::ui
