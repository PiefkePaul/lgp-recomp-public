// Reading Nintendo SARC archives and Yaz0 compression (public formats), for
// the app's menus in the game's own look: the layouts, textures and fonts of
// the game's interface sit in SARC archives in its RomFS. Nothing of the game
// is part of the repository; the app reads the user's files at run time.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace lgp::platform::ui {

// Yaz0 ("Yaz0", size, groups of 8 literal/back-reference codes). nullopt if
// the data is not Yaz0 or is cut short.
std::optional<std::vector<uint8_t>> yaz0_decompress(std::span<const uint8_t> data);

class Sarc {
public:
    struct Entry {
        std::string name;  // path inside the archive ("" for nameless entries)
        uint32_t hash = 0;
        uint32_t begin = 0, end = 0;  // into data()
    };
    // Parses a SARC (also Yaz0-compressed); nullopt and *error if it is none.
    static std::optional<Sarc> parse(std::vector<uint8_t> bytes, std::string* error = nullptr);

    const std::vector<Entry>& entries() const { return entries_; }
    // The file's bytes, empty if there is no such name.
    std::span<const uint8_t> file(const std::string& name) const;
    std::span<const uint8_t> file(const Entry& e) const;
    const std::vector<uint8_t>& data() const { return bytes_; }

private:
    std::vector<uint8_t> bytes_;
    std::vector<Entry> entries_;
};

// The hash SARC uses for names (key 0x65 in all known archives).
uint32_t sarc_name_hash(const std::string& name, uint32_t key = 0x65);

}  // namespace lgp::platform::ui
