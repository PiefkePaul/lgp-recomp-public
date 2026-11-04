// Lists what SARC archives hold, for finding the game's interface files
// (layouts, textures, fonts) in the user's RomFS without unpacking it:
//
//   lgp_sarc_list FILE|DIR... [--match WORD,...]
//
// One line per entry: archive, path inside, size, first four bytes. Nested
// archives (SARC, also Yaz0-compressed) are listed too. With a directory,
// every file under it that is a SARC or Yaz0. --match keeps archives whose
// path or entries contain one of the words (case-insensitive). Names and
// sizes only; nothing is extracted.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "lgp/platform/ui/sarc.hpp"

namespace fs = std::filesystem;
using lgp::platform::ui::Sarc;

namespace {

std::vector<std::string> g_match;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

bool matches(const std::string& text) {
    if (g_match.empty())
        return true;
    const std::string l = lower(text);
    for (const std::string& m : g_match)
        if (l.find(m) != std::string::npos)
            return true;
    return false;
}

std::string magic(const uint8_t* p, size_t n) {
    std::string s;
    for (size_t i = 0; i < std::min<size_t>(4, n); ++i)
        s.push_back(p[i] >= 0x20 && p[i] < 0x7F ? char(p[i]) : '.');
    return s;
}

void list(const std::string& where, std::vector<uint8_t> bytes) {
    std::string error;
    auto sarc = Sarc::parse(std::move(bytes), &error);
    if (!sarc)
        return;
    bool any = matches(where);
    for (const auto& e : sarc->entries())
        any = any || matches(e.name);
    if (!any)
        return;
    for (const auto& e : sarc->entries()) {
        const auto f = sarc->file(e);
        std::printf("%s\t%s\t%zu\t%s\n", where.c_str(), e.name.c_str(), f.size(), magic(f.data(), f.size()).c_str());
        if (f.size() >= 4 && (magic(f.data(), 4) == "SARC" || magic(f.data(), 4) == "Yaz0"))
            list(where + "!" + e.name, std::vector<uint8_t>(f.begin(), f.end()));
    }
}

void list_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    char head[4] = {};
    if (!in.read(head, 4))
        return;
    const std::string m(head, 4);
    if (m != "SARC" && m != "Yaz0")
        return;
    in.seekg(0);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    list(path.generic_string(), std::move(bytes));
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<fs::path> inputs;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--match" && i + 1 < argc) {
            std::string words = lower(argv[++i]);
            size_t at = 0;
            while (at <= words.size()) {
                const size_t comma = words.find(',', at);
                const std::string w = words.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
                if (!w.empty())
                    g_match.push_back(w);
                if (comma == std::string::npos)
                    break;
                at = comma + 1;
            }
        } else {
            inputs.emplace_back(a);
        }
    }
    if (inputs.empty()) {
        std::fprintf(stderr, "usage: lgp_sarc_list FILE|DIR... [--match WORD,...]\n");
        return 2;
    }
    std::printf("archive\tentry\tsize\tmagic\n");
    for (const fs::path& p : inputs) {
        std::error_code ec;
        if (fs::is_directory(p, ec)) {
            for (const auto& e : fs::recursive_directory_iterator(p, fs::directory_options::skip_permission_denied, ec))
                if (e.is_regular_file(ec))
                    list_file(e.path());
        } else {
            list_file(p);
        }
    }
    return 0;
}
