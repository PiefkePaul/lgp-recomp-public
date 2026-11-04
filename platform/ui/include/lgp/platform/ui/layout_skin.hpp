// The game's menu parts as the app's menus use them: per layout archive
// (SARC from the RomFS, e.g. app/setting/bin/setting_list_00.arc) its
// layouts (blyt/*.bflyt), animations (anim/*.bflan) and decoded textures
// (timg/*.bntx). Built at run time from the user's game only; without it the
// menus keep their own look.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "lgp/platform/ui/bntx.hpp"
#include "lgp/platform/ui/layout.hpp"

namespace lgp::platform::ui {

struct SkinArchive {
    std::string name;  // file stem of the archive
    std::vector<std::pair<std::string, Layout>> layouts;  // by file stem
    std::vector<UiImage> images;
    std::vector<std::pair<std::string, LayoutAnimation>> animations;  // by file stem (anim/*.bflan)

    const Layout* layout(std::string_view stem) const;
    const LayoutAnimation* animation(std::string_view stem) const;
    int image(std::string_view name) const;  // -1: none
};

struct LayoutSkin {
    std::vector<SkinArchive> archives;

    const SkinArchive* archive(std::string_view name) const;
    // The archive that has the layout `stem` (the first one).
    const SkinArchive* archive_with(std::string_view stem) const;
};

// One archive's layouts and textures (lgp_platform_ui_assets: decodes the
// textures). nullopt and *error if it is no SARC with layouts; what could
// not be read is listed in *warnings (names and counts only).
std::optional<SkinArchive> load_skin_archive(std::string name, std::span<const uint8_t> sarc, std::string* error,
                                             std::vector<std::string>* warnings);

}  // namespace lgp::platform::ui
