#include <cstring>

#include "lgp/platform/ui/layout_skin.hpp"
#include "lgp/platform/ui/sarc.hpp"

namespace lgp::platform::ui {

std::optional<SkinArchive> load_skin_archive(std::string name, std::span<const uint8_t> bytes, std::string* error,
                                             std::vector<std::string>* warnings) {
    auto sarc = Sarc::parse(std::vector<uint8_t>(bytes.begin(), bytes.end()), error);
    if (!sarc)
        return std::nullopt;
    SkinArchive a;
    a.name = std::move(name);
    for (const Sarc::Entry& e : sarc->entries()) {
        const auto f = sarc->file(e);
        if (f.size() >= 4 && std::memcmp(f.data(), "FLYT", 4) == 0) {
            std::string why;
            auto l = parse_bflyt(f, &why);
            std::string stem = e.name.substr(e.name.find_last_of('/') + 1);
            stem = stem.substr(0, stem.find_last_of('.'));
            if (!l) {
                warnings->push_back(a.name + ": layout " + stem + ": " + why);
                continue;
            }
            l->archive = a.name;
            for (const std::string& w : l->warnings)
                warnings->push_back(a.name + ": layout " + stem + ": " + w);
            a.layouts.emplace_back(std::move(stem), std::move(*l));
        } else if (f.size() >= 4 && std::memcmp(f.data(), "FLAN", 4) == 0) {
            std::string why;
            auto anim = parse_bflan(f, &why);
            std::string stem = e.name.substr(e.name.find_last_of('/') + 1);
            stem = stem.substr(0, stem.find_last_of('.'));
            if (!anim)
                warnings->push_back(a.name + ": animation " + stem + ": " + why);
            else
                a.animations.emplace_back(std::move(stem), std::move(*anim));
        } else if (f.size() >= 4 && std::memcmp(f.data(), "BNTX", 4) == 0) {
            std::string why;
            std::vector<std::string> skipped;
            auto images = decode_bntx(f, &why, &skipped);
            if (images.empty() && !why.empty())
                warnings->push_back(a.name + ": " + e.name + ": " + why);
            if (!skipped.empty())
                warnings->push_back(a.name + ": " + std::to_string(skipped.size()) + " textures in unknown formats");
            for (UiImage& img : images)
                a.images.push_back(std::move(img));
        }
    }
    if (a.layouts.empty()) {
        *error = "no layouts";
        return std::nullopt;
    }
    return a;
}

}  // namespace lgp::platform::ui
