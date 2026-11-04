// SARC archives and Yaz0 (platform/ui/sarc.hpp), on archives built here: the
// game's own archives stay with the user.
#include <cstring>
#include <string>
#include <vector>

#include "../runtime/check.hpp"
#include "lgp/platform/ui/game_font.hpp"
#include "lgp/platform/ui/sarc.hpp"
#include "ui_fixtures.hpp"

using namespace lgp::platform::ui;
using lgp::test::make_sarc;

namespace {

void test_sarc() {
    std::string error;
    auto sarc = Sarc::parse(make_sarc({{"blyt/menu.bflyt", "FLYT-layout"}, {"timg/window.bflim", "pixels"}}), &error);
    CHECK(sarc.has_value());
    if (!sarc)
        return;
    CHECK_EQ(sarc->entries().size(), 2u);
    CHECK(sarc->entries()[0].name == "blyt/menu.bflyt");
    const auto f = sarc->file("timg/window.bflim");
    CHECK(std::string(f.begin(), f.end()) == "pixels");
    CHECK(sarc->file("missing").empty());
    CHECK(!Sarc::parse(std::vector<uint8_t>(64, 0), &error).has_value());
    CHECK(error == "not a SARC archive");
}

void test_yaz0() {
    // "abcabcabcabcX": 3 literals, a back-reference (distance 3, length 9),
    // a literal.
    const std::vector<uint8_t> z = {'Y', 'a', 'z', '0', 0, 0, 0, 13, 0, 0, 0, 0, 0, 0, 0, 0,
                                    0xE8,  // 1110 1000: lit lit lit ref lit
                                    'a', 'b', 'c', 0x70, 0x02, 'X'};
    const auto out = yaz0_decompress(z);
    CHECK(out.has_value());
    if (out)
        CHECK(std::string(out->begin(), out->end()) == "abcabcabcabcX");
    std::vector<uint8_t> cut(z.begin(), z.end() - 3);
    CHECK(!yaz0_decompress(cut).has_value());
}

// A BFOTF as games ship it: header (magic, size) and font XORed with the
// shared-font key, little-endian words.
void test_game_font() {
    std::vector<uint8_t> otf = {'O', 'T', 'T', 'O', 0, 10, 0, 0x80, 0, 3, 0, 0x20};
    for (int i = 0; i < 37; ++i)
        otf.push_back(uint8_t(i * 7));
    const uint8_t key[4] = {0x49, 0x62, 0x18, 0x06};
    std::vector<uint8_t> file = {0x36, 0xF8, 0x1A, 0x1E};  // 0x18029A7F ^ key
    const uint32_t size = uint32_t(otf.size());
    for (int i = 0; i < 4; ++i)
        file.push_back(uint8_t(size >> (24 - 8 * i)) ^ key[i]);  // the size big-endian
    for (size_t i = 0; i < otf.size(); ++i)
        file.push_back(otf[i] ^ key[i % 4]);
    while (file.size() % 4 != 0)
        file.push_back(0);  // padding after the font
    const auto out = unwrap_game_font(file);
    CHECK(out.has_value());
    if (out)
        CHECK(*out == otf);
    CHECK(unwrap_game_font(otf).has_value());  // a plain font stays as is
    CHECK(!unwrap_game_font(std::vector<uint8_t>(64, 0x55)).has_value());
}

}  // namespace

int main() {
    RUN(test_sarc);
    RUN(test_yaz0);
    RUN(test_game_font);
    return finish();
}
