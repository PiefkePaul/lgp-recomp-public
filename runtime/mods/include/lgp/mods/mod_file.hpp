// Mod files (docs/knowledge/spiel-mods-hooks.md): what a game mod needs to
// know about one game build, kept out of the repository.
//
// One file per mod, `<game>/mods/<id>.cfg`, written by hand from our own
// analysis of the game (addresses are game data: the file stays in
// $LGP_PRIVATE_DIR and the game folder, never in the repository). Lines:
//
//   # comment
//   hook  <role> <va>          function entry the mod hooks (lgp_lift --mods lifts a door there)
//   value <name> <number>      a constant: an offset, a size, an id
//   patch <name> <va> [rest]   instruction the mod replaces or observes at run time
//                              (lgp_lift --mods lifts a patch site there, lgp/runtime/patch.hpp);
//                              rest is the mod's own (e.g. the new value)
//   <directive> <name> <number> [rest]   other directives (instruction patches, data patches);
//                                         kept for the mod that knows them
//
// Numbers are decimal or 0x hex. <va> is the guest address lgp_lift uses
// (its --base, default 0x08000000 for the first module), not a module offset.
// Roles and names are the mod's own words ([a-z0-9_]).
//
// No dependencies: lgp_lift (recomp/) reads the hook lines too.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lgp::mods {

struct ModLine {
    std::string directive;  // "hook", "value", ...
    std::string name;
    uint64_t number = 0;
    std::string rest;  // after the number, trimmed
    int line = 0;
};

struct ModFile {
    std::string id;  // file name without .cfg
    std::filesystem::path path;
    std::vector<ModLine> lines;

    std::optional<uint64_t> hook(std::string_view role) const;
    std::optional<uint64_t> value(std::string_view name) const;
    std::optional<uint64_t> patch(std::string_view name) const;
    // Every `hook` address, sorted, without duplicates.
    std::vector<uint64_t> hook_entries() const;
    // Every `patch` address that is an instruction (not a `data` line), sorted,
    // without duplicates: the patch sites lgp_lift --mods builds.
    std::vector<uint64_t> patch_entries() const;
};

// Decimal or 0x hex, the whole word, no sign.
bool parse_number(std::string_view s, uint64_t* out);

// `where` names the source in error messages ("wild-battles.cfg").
bool parse_mod_file(std::string_view text, const std::string& where, ModFile* out, std::string* error);
bool load_mod_file(const std::filesystem::path& file, ModFile* out, std::string* error);
// A file, or every *.cfg in a directory (sorted by name). A missing directory
// is no error: no mods.
bool load_mod_files(const std::filesystem::path& file_or_dir, std::vector<ModFile>* out, std::string* error);

}  // namespace lgp::mods
