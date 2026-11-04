#include "lgp/mods/mod_file.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <sstream>

namespace lgp::mods {

namespace fs = std::filesystem;

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.remove_suffix(1);
    return s;
}

std::string_view next_word(std::string_view& s) {
    s = trim(s);
    size_t n = 0;
    while (n < s.size() && s[n] != ' ' && s[n] != '\t')
        ++n;
    const std::string_view w = s.substr(0, n);
    s.remove_prefix(n);
    return w;
}

bool is_name(std::string_view s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    });
}

}  // namespace

// Not strtoull: it takes "0x" alone as 0 on some C libraries, not on others.
bool parse_number(std::string_view s, uint64_t* out) {
    int base = 10;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s.remove_prefix(2);
        base = 16;
    }
    if (s.empty())
        return false;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), *out, base);
    return ec == std::errc() && end == s.data() + s.size();
}

std::optional<uint64_t> ModFile::hook(std::string_view role) const {
    for (const ModLine& l : lines)
        if (l.directive == "hook" && l.name == role)
            return l.number;
    return std::nullopt;
}

std::optional<uint64_t> ModFile::value(std::string_view name) const {
    for (const ModLine& l : lines)
        if (l.directive == "value" && l.name == name)
            return l.number;
    return std::nullopt;
}

std::optional<uint64_t> ModFile::patch(std::string_view name) const {
    for (const ModLine& l : lines)
        if (l.directive == "patch" && l.name == name)
            return l.number;
    return std::nullopt;
}

namespace {
std::vector<uint64_t> addresses(const std::vector<ModLine>& lines, std::string_view directive) {
    std::vector<uint64_t> v;
    for (const ModLine& l : lines)
        if (l.directive == directive)
            v.push_back(l.number);
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}
}  // namespace

std::vector<uint64_t> ModFile::hook_entries() const { return addresses(lines, "hook"); }
std::vector<uint64_t> ModFile::patch_entries() const {
    // `patch NAME VA data …` (widescreen) writes a value in guest memory, often
    // rodata: no instruction there to lift as a patch site.
    std::vector<ModLine> code;
    for (const ModLine& l : lines)
        if (l.directive == "patch" && l.rest.substr(0, l.rest.find_first_of(" \t")) != "data")
            code.push_back(l);
    return addresses(code, "patch");
}

bool parse_mod_file(std::string_view text, const std::string& where, ModFile* out, std::string* error) {
    out->lines.clear();
    int n = 0;
    for (size_t pos = 0; pos <= text.size();) {
        ++n;
        const size_t nl = text.find('\n', pos);
        std::string_view line = text.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
        pos = nl == std::string_view::npos ? text.size() + 1 : nl + 1;
        if (n == 1 && line.substr(0, 3) == "\xEF\xBB\xBF")  // UTF-8 BOM (Windows editors)
            line.remove_prefix(3);
        if (const size_t hash = line.find('#'); hash != std::string_view::npos)
            line = line.substr(0, hash);
        line = trim(line);
        if (line.empty())
            continue;
        const auto fail = [&](const std::string& why) {
            *error = where + ":" + std::to_string(n) + ": " + why;
            return false;
        };
        ModLine l;
        l.line = n;
        l.directive = std::string(next_word(line));
        l.name = std::string(next_word(line));
        const std::string_view number = next_word(line);
        l.rest = std::string(trim(line));
        if (!is_name(l.directive))
            return fail("expected a directive (hook, value, ...)");
        if (!is_name(l.name))
            return fail("expected a name of a-z, 0-9 and _ after " + l.directive);
        if (!parse_number(number, &l.number))
            return fail(l.directive + " " + l.name + ": expected a number (decimal or 0x hex)");
        if ((l.directive == "hook" || l.directive == "value") && !l.rest.empty())
            return fail(l.directive + " " + l.name + ": unexpected text after the number");
        if (l.directive == "hook" && (l.number & 3) != 0)
            return fail("hook " + l.name + ": an A64 function entry is 4-byte aligned");
        if (l.directive == "patch" && (l.number & 3) != 0)
            return fail("patch " + l.name + ": an A64 instruction is 4-byte aligned");
        for (const ModLine& o : out->lines)
            if (o.directive == l.directive && o.name == l.name &&
                (l.directive == "hook" || l.directive == "value" || l.directive == "patch"))
                return fail(l.directive + " " + l.name + " twice (first in line " + std::to_string(o.line) + ")");
        out->lines.push_back(std::move(l));
    }
    return true;
}

bool load_mod_file(const fs::path& file, ModFile* out, std::string* error) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        *error = "cannot read " + file.string();
        return false;
    }
    std::ostringstream text;
    text << in.rdbuf();
    out->id = file.stem().string();
    out->path = file;
    return parse_mod_file(text.str(), file.filename().string(), out, error);
}

bool load_mod_files(const fs::path& file_or_dir, std::vector<ModFile>* out, std::string* error) {
    std::error_code ec;
    if (!fs::is_directory(file_or_dir, ec)) {
        if (!fs::exists(file_or_dir, ec))
            return true;
        ModFile f;
        if (!load_mod_file(file_or_dir, &f, error))
            return false;
        out->push_back(std::move(f));
        return true;
    }
    std::vector<fs::path> files;
    for (const fs::directory_entry& e : fs::directory_iterator(file_or_dir, ec))
        if (e.path().extension() == ".cfg" && e.is_regular_file(ec))
            files.push_back(e.path());
    if (ec) {
        *error = "cannot list " + file_or_dir.string() + ": " + ec.message();
        return false;
    }
    std::sort(files.begin(), files.end());
    for (const fs::path& p : files) {
        ModFile f;
        if (!load_mod_file(p, &f, error))
            return false;
        out->push_back(std::move(f));
    }
    return true;
}

}  // namespace lgp::mods
