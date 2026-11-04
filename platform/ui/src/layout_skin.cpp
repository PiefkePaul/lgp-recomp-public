#include "lgp/platform/ui/layout_skin.hpp"

namespace lgp::platform::ui {

namespace {

// "setting_bg_00.bflim" and "setting_bg_00" name the same texture.
std::string_view stem(std::string_view name) {
    const size_t slash = name.find_last_of('/');
    if (slash != std::string_view::npos)
        name.remove_prefix(slash + 1);
    const size_t dot = name.find_last_of('.');
    return dot == std::string_view::npos ? name : name.substr(0, dot);
}

}  // namespace

const Layout* SkinArchive::layout(std::string_view name) const {
    for (const auto& [n, l] : layouts)
        if (n == name)
            return &l;
    return nullptr;
}

const LayoutAnimation* SkinArchive::animation(std::string_view name) const {
    for (const auto& [n, a] : animations)
        if (n == name)
            return &a;
    return nullptr;
}

int SkinArchive::image(std::string_view name) const {
    const std::string_view want = stem(name);
    for (size_t i = 0; i < images.size(); ++i)
        if (stem(images[i].name) == want)
            return int(i);
    return -1;
}

const SkinArchive* LayoutSkin::archive(std::string_view name) const {
    for (const SkinArchive& a : archives)
        if (a.name == name)
            return &a;
    return nullptr;
}

const SkinArchive* LayoutSkin::archive_with(std::string_view name) const {
    for (const SkinArchive& a : archives)
        if (a.layout(name) != nullptr)
            return &a;
    return nullptr;
}

}  // namespace lgp::platform::ui
