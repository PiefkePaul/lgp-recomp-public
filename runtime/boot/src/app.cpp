// App mode settings and checks (lgp/boot/app.hpp).
#include "lgp/boot/app.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>

#include "lgp/mods/mods.hpp"
#include "lgp/platform/host_sync.hpp"
#include "lgp/platform/ui_text.hpp"

namespace lgp::boot {

namespace {

namespace fs = std::filesystem;

std::string trim(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r");
    if (b == std::string::npos)
        return {};
    const size_t e = s.find_last_not_of(" \t\r");
    return s.substr(b, e - b + 1);
}

std::vector<std::string> split_spaces(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    for (std::string word; in >> word;)
        out.push_back(word);
    return out;
}

bool is_dir(const fs::path& p) {
    std::error_code ec;
    return fs::is_directory(p, ec);
}

bool is_file(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

std::string to_utf8(const fs::path& p) { return path_to_utf8(p); }

}  // namespace

std::string path_to_arg(const fs::path& p) {
    try {
        return p.string();
    } catch (const std::exception&) {
        return path_to_utf8(p);
    }
}

std::string path_to_utf8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

fs::path path_from_utf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

#ifndef LGP_LDN_SERVER_DEFAULT
#define LGP_LDN_SERVER_DEFAULT ""
#endif
#ifndef LGP_APP_DEFAULT_MODS
#define LGP_APP_DEFAULT_MODS ""
#endif

const char* const kGameLanguages[12] = {"de", "en-US", "en-GB", "fr", "fr-CA", "it",
                                        "es", "es-419", "ja", "ko", "zh-CN", "zh-TW"};

const char* default_ldn_server() { return LGP_LDN_SERVER_DEFAULT; }

std::string ldn_server_of(const AppSettings& s) { return s.ldn_server.empty() ? default_ldn_server() : s.ldn_server; }

bool online_button_file(const fs::path& mods_dir) {
    std::error_code ec;
    return fs::is_regular_file(mods_dir / "online-button.cfg", ec);
}

bool skip_controller_screen_file(const fs::path& mods_dir) {
    std::error_code ec;
    return fs::is_regular_file(mods_dir / "skip-controller-screen.cfg", ec);
}

bool random_encounters_file(const fs::path& mods_dir) {
    std::error_code ec;
    return fs::is_regular_file(mods_dir / "random-encounters.cfg", ec);
}

bool audio_free_guard_file(const fs::path& mods_dir) {
    std::error_code ec;
    return fs::is_regular_file(mods_dir / "audio-free-guard.cfg", ec);
}

AppFiles app_files(const fs::path& config, const fs::path& data, const fs::path& cache,
                   const fs::path& packaged_game) {
    AppFiles f;
    f.data = data;
    f.packaged_game = packaged_game;
    f.settings = config / "settings.ini";
    f.default_game = data / "game";
    f.saves = data / "saves";
    f.log = data / "logs" / "lgp.log";
    f.previous_log = data / "logs" / "lgp.prev.log";
    f.pipeline_cache = cache / "webgpu-pipelines.lgppipes";
    f.native_cache = cache / "native-rewrite.lgpnative";
    return f;
}

bool parse_hud_sides(const std::string& text, std::array<int, 4>* out) {
    std::array<int, 4> v{};
    size_t at = 0;
    for (size_t k = 0; k < 4; ++k) {
        const size_t comma = k < 3 ? text.find(',', at) : text.size();
        if (comma == std::string::npos || comma == at || (k == 3 && text.find(',', at) != std::string::npos))
            return false;
        const std::string part = text.substr(at, comma - at);
        char* end = nullptr;
        const long n = std::strtol(part.c_str(), &end, 10);
        if (*end != '\0' || n < 0 || n > 100)
            return false;
        v[k] = int(n);
        at = comma + 1;
    }
    *out = v;
    return true;
}

bool load_app_settings(const fs::path& file, AppSettings* out, std::string* error) {
    *out = AppSettings{};
    out->mods = mods::parse_mod_list(LGP_APP_DEFAULT_MODS);  // a mods line (also an empty one) replaces them
    std::error_code ec;
    if (!fs::exists(file, ec))
        return true;
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        *error = "cannot read " + to_utf8(file);
        return false;
    }
    std::string line;
    bool all_visible = false;
    for (int n = 1; std::getline(in, line); ++n) {
        if (n == 1 && line.rfind("\xEF\xBB\xBF", 0) == 0)  // UTF-8 BOM (Windows editors)
            line.erase(0, 3);
        const std::string t = trim(line);
        if (t.empty() || t[0] == '#')
            continue;
        const size_t eq = t.find('=');
        if (eq == std::string::npos) {
            *error = to_utf8(file) + ":" + std::to_string(n) + ": expected key = value";
            return false;
        }
        const std::string key = trim(t.substr(0, eq));
        const std::string value = trim(t.substr(eq + 1));
        if (key == "game") {
            out->game_dir = value;
        } else if (key == "saves") {
            out->save_dir = value;
        } else if (key == "controller") {
            if (value != "handheld" && value != "joycon-right" && value != "joycon-left") {
                *error = to_utf8(file) + ":" + std::to_string(n) + ": controller: handheld, joycon-right or joycon-left";
                return false;
            }
            out->controller = value;
        } else if (key == "player1") {
            if (value != "press" && value != "order") {
                *error = to_utf8(file) + ":" + std::to_string(n) + ": player1: press or order";
                return false;
            }
            out->player1 = value;
        } else if (key == "fullscreen") {
            if (value != "0" && value != "1") {
                *error = to_utf8(file) + ":" + std::to_string(n) + ": fullscreen: 0 or 1";
                return false;
            }
            out->fullscreen = value == "1";
        } else if (key == "aspect") {
            if (value != "fit" && value != "fill" && value != "stretch") {
                *error = to_utf8(file) + ":" + std::to_string(n) + ": aspect: fit, fill or stretch";
                return false;
            }
            out->aspect = value;
        } else if (key == "effects") {
            if (!value.empty() && value != "full" && value != "half") {
                *error = to_utf8(file) + ":" + std::to_string(n) + ": effects: full, half or empty";
                return false;
            }
            out->effects = value;
        } else if (key == "async") {
            if (!value.empty() && value != "0" && value != "1") {
                *error = to_utf8(file) + ":" + std::to_string(n) + ": async: 0, 1 or empty";
                return false;
            }
            out->async_execute = value;
        } else if (key == "smoothing") {
            if (value != "off" && value != "linear" && value != "fxaa") {
                *error = to_utf8(file) + ":" + std::to_string(n) + ": smoothing: off, linear or fxaa";
                return false;
            }
            out->smoothing = value;
        } else if (key == "render_scale") {
            char* end = nullptr;
            const float f = value.empty() ? 1.0f : std::strtof(value.c_str(), &end);
            if (!value.empty() && (end == value.c_str() || *end != '\0' || !(f >= 0.25f && f <= 2.0f))) {
                *error = to_utf8(file) + ":" + std::to_string(n) + ": render_scale: 0.25..2 or empty";
                return false;
            }
            out->render_scale = value;
        } else if (key == "hud_bounds") {
            char* end = nullptr;
            const long v = std::strtol(value.c_str(), &end, 10);
            if (value.empty() || *end != '\0' || v < 0 || v > 100) {
                *error = to_utf8(file) + ":" + std::to_string(n) + ": hud_bounds: 0..100";
                return false;
            }
            out->hud_bounds = int(v);
        } else if (key == "hud_sides") {
            std::array<int, 4> sides{};
            if (!value.empty() && !parse_hud_sides(value, &sides)) {
                *error = to_utf8(file) + ":" + std::to_string(n) + ": hud_sides: T,B,L,R (each 0..100) or empty";
                return false;
            }
            out->hud_sides = value;
        } else if (key == "touch") {
            if (!value.empty() && value != "0" && value != "1") {
                *error = to_utf8(file) + ":" + std::to_string(n) + ": touch: 0, 1 or empty";
                return false;
            }
            out->touch = value;
        } else if (key == "docked") {
            if (!value.empty() && value != "0" && value != "1") {
                *error = to_utf8(file) + ":" + std::to_string(n) + ": docked: 0, 1 or empty";
                return false;
            }
            out->docked = value;
        } else if (key == "touch_mirrored" || key == "touch_throw" || key == "touch_throw_pad") {
            if (value != "0" && value != "1") {
                *error = to_utf8(file) + ":" + std::to_string(n) + ": " + key + ": 0 or 1";
                return false;
            }
            (key == "touch_mirrored" ? out->touch_mirrored
             : key == "touch_throw"  ? out->touch_throw
                                     : out->touch_throw_pad) = value == "1";
        } else if (key == "touch_size" || key == "touch_opacity" || key == "volume") {
            int v = 0;
            const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), v);
            // The menu's ranges (touch size 50-140 %: larger runs the face
            // buttons into the shoulders, TouchPad::set_scale).
            const int lo = key == "volume" ? 0 : key == "touch_size" ? 50 : 25;
            const int hi = key == "touch_size" ? 140 : 100;
            if (ec != std::errc() || end != value.data() + value.size() || v < lo || v > hi) {
                *error = to_utf8(file) + ":" + std::to_string(n) + ": " + key + ": " + std::to_string(lo) + " to " +
                         std::to_string(hi);
                return false;
            }
            (key == "volume" ? out->volume : key == "touch_size" ? out->touch_size : out->touch_opacity) = v;
        } else if (key == "ldn") {
            if (value != "lan" && value != "ryuldn" && value != "off") {
                *error = to_utf8(file) + ":" + std::to_string(n) + ": ldn: lan, ryuldn or off";
                return false;
            }
            // ryuldn of older builds: the Wi-Fi stays on, the server is
            // always there (below).
            out->ldn = value == "off" ? "off" : "lan";
        } else if (key == "ldn_online") {
            // Older builds had a switch for the server; online is always on
            // now, so the line is read and dropped.
        } else if (key == "ldn_server") {
            out->ldn_server = value;
        } else if (key == "ldn_room") {
            out->ldn_room = value;
        } else if (key == "mods") {
            out->mods = mods::parse_mod_list(value);
        } else if (key == "encounter_rate") {
            // Older builds' frequency row of random-encounters; the app's
            // Pokémon Gelb always takes Gelb's rate, so the line is dropped.
        } else if (key == "language") {
            if (!value.empty() && std::find_if(std::begin(kGameLanguages), std::end(kGameLanguages),
                                               [&](const char* l) { return value == l; }) == std::end(kGameLanguages)) {
                *error = to_utf8(file) + ":" + std::to_string(n) +
                         ": language: ja, en-US, en-GB, fr, fr-CA, de, it, es, es-419, ko, zh-CN, zh-TW or empty";
                return false;
            }
            out->language = value;
        } else if (key == "visible_pokemon") {
            // Older builds' row: 2 ("An", all visible) left random-encounters
            // without attacks, as if it were off, so Pokémon Gelb stays off.
            all_visible = value == "2";
        } else if (key == "start_screen") {
            if (value != "0" && value != "1") {
                *error = to_utf8(file) + ":" + std::to_string(n) + ": start_screen: 0 or 1";
                return false;
            }
            out->start_screen = value == "1";
        } else if (key == "controller_screen") {
            if (value != "0" && value != "1") {
                *error = to_utf8(file) + ":" + std::to_string(n) + ": controller_screen: 0 or 1";
                return false;
            }
            out->controller_screen = value == "1";
        } else if (key == "hide_field_pokemon") {
            if (value != "0" && value != "1") {
                *error = to_utf8(file) + ":" + std::to_string(n) + ": hide_field_pokemon: 0 or 1";
                return false;
            }
            out->hide_field_pokemon = value == "1";
        } else if (key == "args") {
            out->extra_args = split_spaces(value);
        }
    }
    if (all_visible)
        out->mods.erase(std::remove(out->mods.begin(), out->mods.end(), "random-encounters"), out->mods.end());
    return true;
}

bool save_app_settings(const fs::path& file, const AppSettings& s, std::string* error) {
    std::string args;
    for (const std::string& a : s.extra_args)
        args += " " + a;
    const std::pair<const char*, std::string> values[] = {
        {"game", s.game_dir},
        {"saves", s.save_dir},
        {"controller", s.controller},
        {"player1", s.player1},
        {"fullscreen", s.fullscreen ? "1" : "0"},
        {"aspect", s.aspect},
        {"effects", s.effects},
        {"async", s.async_execute},
        {"render_scale", s.render_scale},
        {"smoothing", s.smoothing},
        {"hud_bounds", std::to_string(s.hud_bounds)},
        {"hud_sides", s.hud_sides},
        {"touch", s.touch},
        {"touch_size", std::to_string(s.touch_size)},
        {"touch_opacity", std::to_string(s.touch_opacity)},
        {"touch_mirrored", s.touch_mirrored ? "1" : "0"},
        {"touch_throw", s.touch_throw ? "1" : "0"},
        {"touch_throw_pad", s.touch_throw_pad ? "1" : "0"},
        {"volume", std::to_string(s.volume)},
        {"ldn", s.ldn},
        {"ldn_server", s.ldn_server},
        {"ldn_room", s.ldn_room},
        {"docked", s.docked},
        {"mods", mods::join_mod_list(s.mods)},
        {"language", s.language},
        {"start_screen", s.start_screen ? "1" : "0"},
        {"controller_screen", s.controller_screen ? "1" : "0"},
        {"hide_field_pokemon", s.hide_field_pokemon ? "1" : "0"},
        {"args", args},
    };
    auto line_for = [](const std::pair<const char*, std::string>& kv) {
        // "args =" without a trailing blank when empty, as before.
        return std::string(kv.first) + (kv.second.empty() ? " =" : kv.second[0] == ' ' ? " =" : " = ") + kv.second;
    };
    // The existing file keeps its comments, order and keys this build does not
    // know (written by a newer build or by hand); known keys are replaced in
    // place, the first time they appear, and later copies dropped.
    std::vector<std::string> lines;
    bool written[std::size(values)] = {};
    {
        std::error_code exists_ec;
        const bool exists = fs::exists(file, exists_ec);
        std::ifstream in(file, std::ios::binary);
        if (exists && !in) {
            // Rewriting it from scratch would drop comments and unknown keys.
            *error = "cannot read " + to_utf8(file);
            return false;
        }
        std::string line;
        for (int n = 1; in && std::getline(in, line); ++n) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            std::string t = trim(line);
            if (n == 1 && t.rfind("\xEF\xBB\xBF", 0) == 0)
                t = trim(t.substr(3));
            const size_t eq = t.find('=');
            size_t known = std::size(values);
            if (!t.empty() && t[0] != '#' && eq != std::string::npos)
                for (size_t i = 0; i < std::size(values); ++i)
                    if (trim(t.substr(0, eq)) == values[i].first)
                        known = i;
            // Keys of older builds whose meaning load_app_settings folded into
            // others (the mods line); kept, they would apply again on every load.
            if (!t.empty() && t[0] != '#' && eq != std::string::npos &&
                (trim(t.substr(0, eq)) == "encounter_rate" || trim(t.substr(0, eq)) == "visible_pokemon"))
                continue;
            if (known == std::size(values)) {
                lines.push_back(line);
            } else if (!written[known]) {
                lines.push_back(line_for(values[known]));
                written[known] = true;
            }
        }
        if (in.bad()) {
            *error = "cannot read " + to_utf8(file);
            return false;
        }
    }
    if (lines.empty())
        lines.push_back("# lgp settings (docs/knowledge/desktop-app-huelle.md)");
    for (size_t i = 0; i < std::size(values); ++i)
        if (!written[i])
            lines.push_back(line_for(values[i]));
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    fs::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            *error = "cannot write " + to_utf8(tmp);
            return false;
        }
        for (const std::string& l : lines)
            out << l << "\n";
        if (!out.flush()) {
            *error = "cannot write " + to_utf8(tmp);
            return false;
        }
    }
    // On the disk before the rename: else a power loss (or a killed Android
    // emulator, E3) can leave the renamed file empty.
    if (const std::error_code sync = platform::sync_host_path(tmp, false)) {
        *error = "cannot write " + to_utf8(tmp) + ": " + sync.message();
        fs::remove(tmp, ec);
        return false;
    }
    fs::rename(tmp, file, ec);  // replaces an existing file, also on Windows
    if (!ec)
        platform::sync_host_path(file.parent_path().empty() ? fs::path(".") : file.parent_path(), true);  // the rename
    if (ec) {
        *error = "cannot replace " + to_utf8(file) + ": " + ec.message();
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

fs::path resolve_game_folder(const fs::path& picked, const std::vector<std::string>& modules, std::string* problem,
                             bool romfs_elsewhere) {
    if (picked.empty() || !is_dir(picked)) {
        *problem = platform::tr("The folder does not exist.");
        return {};
    }
    fs::path root = picked;
    const fs::path name = picked.filename().empty() ? picked.parent_path().filename() : picked.filename();
    if ((name == "exefs" || name == "romfs") && !is_dir(picked / "exefs"))
        root = fs::path(picked).lexically_normal().parent_path();
    if (!is_dir(root / "exefs")) {
        *problem = platform::tr("The folder lacks the subfolder exefs (the game's program files).");
        return {};
    }
    for (const std::string& m : modules)
        if (!is_file(root / "exefs" / m)) {
            *problem = platform::trf("exefs lacks the file {}.", {m});
            return {};
        }
    if (!romfs_elsewhere && !is_dir(root / "romfs") && !is_file(root / "romfs.bin")) {
        *problem = platform::tr("The folder lacks romfs (subfolder) or romfs.bin (the game data).");
        return {};
    }
    return root;
}

bool game_fits_build(const fs::path& root, const std::vector<std::string>& modules,
                     const std::vector<std::string>& module_ids, const std::string& build_stamp,
                     std::string* problem) {
    for (std::size_t i = 0; i < modules.size() && i < module_ids.size(); ++i) {
        if (module_ids[i].empty())
            continue;
        // NSO header: magic at 0, module (build) id at 0x40, 32 bytes, never compressed.
        std::ifstream in(root / "exefs" / modules[i], std::ios::binary);
        unsigned char head[0x60] = {};
        if (!in.read(reinterpret_cast<char*>(head), sizeof(head)) || std::memcmp(head, "NSO0", 4) != 0)
            continue;  // not an NSO: the boot program's layout check decides
        static const char* digits = "0123456789abcdef";
        std::string id;
        for (int b = 0x40; b < 0x60; ++b) {
            id.push_back(digits[head[b] >> 4]);
            id.push_back(digits[head[b] & 15]);
        }
        if (id != module_ids[i]) {
            *problem = platform::trf("exefs/{} is from another version of the game (build ID {}, this app needs {}).",
                                     {modules[i], id.substr(0, 16), module_ids[i].substr(0, 16)});
            return false;
        }
    }
    if (!build_stamp.empty()) {
        std::ifstream in(root / kBuildStampFile, std::ios::binary);
        std::string have((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        while (!have.empty() && std::isspace(static_cast<unsigned char>(have.back())))
            have.pop_back();
        if (have != build_stamp) {
            *problem = platform::tr("The game folder belongs to another build of this app. Copy the game folder "
                                    "of this build into the app again.");
            return false;
        }
    }
    return true;
}

namespace {

// "saves" -> 1, "saves-<n>" -> n (n >= 2), else 0.
int slot_number(const std::string& dir) {
    if (dir == "saves")
        return 1;
    if (dir.rfind("saves-", 0) != 0 || dir.size() == 6)
        return 0;
    int n = 0;
    const auto [end, ec] = std::from_chars(dir.data() + 6, dir.data() + dir.size(), n);
    return ec == std::errc() && end == dir.data() + dir.size() && n >= 2 && n < 1000 ? n : 0;
}

}  // namespace

fs::path game_from_arguments(const std::vector<std::string>& args) {
    fs::path game;
    for (std::size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == "--game")
            game = fs::path(args[i + 1]);  // narrow like the command line
    return game;
}

std::string game_source_text(const fs::path& game, const AppFiles& files) {
    const auto same = [&](const fs::path& folder) {
        if (folder.empty())
            return false;
        std::error_code ec, ec2;
        const fs::path a = fs::weakly_canonical(game, ec), b = fs::weakly_canonical(folder, ec2);
        return !ec && !ec2 && a == b;
    };
    if (same(files.packaged_game))
        return platform::tr("Game: included in the app");
    if (same(files.default_game))
        return platform::tr("Game: the app's folder \"game\"");
    return platform::trf("Game: {}", {path_to_utf8(game)});
}

std::vector<SaveSlot> list_save_slots(const fs::path& data) {
    std::vector<SaveSlot> slots;
    std::error_code ec;
    for (fs::directory_iterator it(data, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec))
            continue;
        const std::string name = path_to_utf8(it->path().filename());
        const int n = slot_number(name);
        if (n == 0)
            continue;
        SaveSlot slot;
        slot.number = n;
        slot.dir = name;
        slot.last_write = fs::file_time_type::min();
        for (fs::recursive_directory_iterator f(it->path(), ec), fend; !ec && f != fend; f.increment(ec)) {
            if (!f->is_regular_file(ec))
                continue;
            slot.has_data = true;
            slot.last_write = std::max(slot.last_write, f->last_write_time(ec));
        }
        ec.clear();
        slots.push_back(slot);
    }
    std::sort(slots.begin(), slots.end(), [](const SaveSlot& a, const SaveSlot& b) { return a.number < b.number; });
    return slots;
}

int current_save_slot(const AppSettings& s) { return s.save_dir.empty() ? 1 : slot_number(s.save_dir); }

std::string create_save_slot(const fs::path& data, std::string* error) {
    int next = 1;
    for (const SaveSlot& slot : list_save_slots(data))
        next = std::max(next, slot.number + 1);
    const std::string dir = next == 1 ? "saves" : "saves-" + std::to_string(next);
    std::error_code ec;
    fs::create_directories(data / dir, ec);
    if (ec) {
        *error = "cannot create " + to_utf8(data / dir) + ": " + ec.message();
        return {};
    }
    return dir;
}

fs::path backup_save_slot(const fs::path& data, const std::string& dir, std::string* error) {
    const fs::path from = data / path_from_utf8(dir);
    if (!is_dir(from)) {
        *error = to_utf8(from) + " does not exist";
        return {};
    }
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#if defined(_MSC_VER)
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
    // Named after the folder's last part: an absolute or nested save folder
    // still lands in <data>/backups.
    fs::path last = path_from_utf8(dir).lexically_normal();
    if (last.filename().empty())
        last = last.parent_path();
    std::string name = to_utf8(last.filename());
    if (name.empty() || name == "." || name == "..")
        name = "saves";
    fs::path to = data / "backups" / path_from_utf8(name + "-" + stamp);
    std::error_code ec;
    for (int i = 2; fs::exists(to, ec); ++i)  // two backups in one second
        to = data / "backups" / path_from_utf8(name + "-" + stamp + "-" + std::to_string(i));
    if (ec) {
        *error = "cannot use " + to_utf8(data / "backups") + ": " + ec.message();
        return {};
    }
    // A save folder that holds <data>/backups (saves = . or a parent of the
    // data folder) would copy into its own copy.
    const fs::path from_c = fs::weakly_canonical(from, ec);
    const fs::path to_c = fs::weakly_canonical(to, ec);
    if (!ec) {
        const fs::path rel = to_c.lexically_relative(from_c);
        if (!rel.empty() && *rel.begin() != "..") {
            *error = "cannot back up " + to_utf8(from) + " into itself (" + to_utf8(to) + ")";
            return {};
        }
    }
    ec.clear();
    fs::create_directories(to, ec);
    if (!ec)
        fs::copy(from, to, fs::copy_options::recursive, ec);
    if (ec) {
        *error = "cannot copy " + to_utf8(from) + " to " + to_utf8(to) + ": " + ec.message();
        // `to` did not exist before, so a half-written copy is ours to drop.
        std::error_code ignored;
        fs::remove_all(to, ignored);
        return {};
    }
    return to;
}

std::vector<std::string> app_arguments(const AppSettings& s, const fs::path& game_dir, const AppFiles& files,
                                       const std::vector<std::string>& more) {
    const auto below_data = [&](const std::string& value) {
        const fs::path p = path_from_utf8(value);
        return p.is_relative() ? (files.data / p).make_preferred() : p;
    };
    const fs::path saves = s.save_dir.empty() ? files.saves : below_data(s.save_dir);
    std::vector<std::string> args = {
        "--exefs",          path_to_arg(game_dir / "exefs"),
        "--game",           path_to_arg(game_dir),
        "--save",           path_to_arg(saves),
        "--graphics",       "hle",
        "--renderer",       "window",
        "--pipeline-cache", path_to_arg(files.pipeline_cache),
        "--native-cache",   path_to_arg(files.native_cache),
        "--controller",     s.controller,
    };
    if (s.player1 == "order")
        args.insert(args.end(), {"--player1", "order"});
    args.insert(args.end(), {"--language", s.language.empty() ? "device" : s.language});
    if (s.fullscreen)
        args.push_back("--fullscreen");
    if (!s.aspect.empty() && s.aspect != "fit")
        args.insert(args.end(), {"--aspect", s.aspect});
    if (!s.effects.empty())
        args.insert(args.end(), {"--effects", s.effects});
    if (!s.async_execute.empty())
        args.insert(args.end(), {"--async-execute", s.async_execute == "1" ? "on" : "off"});
    if (!s.render_scale.empty())
        args.insert(args.end(), {"--render-scale", s.render_scale});
    if (!s.smoothing.empty() && s.smoothing != "off")
        args.insert(args.end(), {"--smoothing", s.smoothing});
    if (s.hud_bounds != 100)
        args.insert(args.end(), {"--hud-bounds", std::to_string(s.hud_bounds)});
    // The battle's HUD bounds start at the 16:9 frame (Paul, 30.10.2026).
    args.insert(args.end(), {"--hud-sides", s.hud_sides.empty() ? "0,0,0,0" : s.hud_sides});
    if (!s.touch.empty())
        args.insert(args.end(), {"--touch", s.touch == "1" ? "on" : "off"});
    if (s.volume != 100)
        args.insert(args.end(), {"--volume", std::to_string(s.volume)});
    // Local play over the LAN is on in the app (the command line keeps it
    // off); an --ldn off in args = or after --app still wins (parsed later).
    // Online through our RyuLDN server is always on when the game's mods
    // folder has the file of the mod online-button (named below): with the
    // Wi-Fi on, one run has both links (--ldn both), "nearby" in the game
    // takes the LAN, "far away" the server; with the Wi-Fi off the server
    // alone (--ldn ryuldn), then "nearby" goes over the server too. Without
    // the file "far away" stays Nintendo's and the server would be of no use,
    // so the Wi-Fi goes alone; likewise without a server (none in the build,
    // none set).
    const bool online_button = !ldn_server_of(s).empty() && online_button_file(game_dir / "mods");
    if (online_button) {
        args.insert(args.end(), {"--ldn", s.ldn == "lan" ? "both" : "ryuldn", "--ldn-server", ldn_server_of(s)});
        if (!s.ldn_room.empty())
            args.insert(args.end(), {"--ldn-room", s.ldn_room});
    } else if (s.ldn == "lan") {
        args.insert(args.end(), {"--ldn", "lan"});
    }
    if (!s.docked.empty())
        args.push_back(s.docked == "1" ? "--docked" : "--handheld");
    std::vector<std::string> known_mods;
    for (const std::string& id : s.mods)
        if (mods::find_mod(id) != nullptr)
            known_mods.push_back(id);
    // The throw's animation only with Catch in battle (Paul 2026-11-02): it
    // shows a throw that the balls in the battle bag make possible.
    if (std::find(known_mods.begin(), known_mods.end(), "battle-balls") == known_mods.end())
        known_mods.erase(std::remove(known_mods.begin(), known_mods.end(), "battle-throw"), known_mods.end());
    // The game's online button goes to our server.
    if (online_button && std::find(known_mods.begin(), known_mods.end(), "online-button") == known_mods.end())
        known_mods.push_back("online-button");
    // The game starts with the app's controller, not its controller screen.
    if (!s.controller_screen && skip_controller_screen_file(game_dir / "mods") &&
        std::find(known_mods.begin(), known_mods.end(), "skip-controller-screen") == known_mods.end())
        known_mods.push_back("skip-controller-screen");
    // The game's sound must not hang after its audio output was reopened.
    if (audio_free_guard_file(game_dir / "mods") &&
        std::find(known_mods.begin(), known_mods.end(), "audio-free-guard") == known_mods.end())
        known_mods.push_back("audio-free-guard");
    // Let's Go with the field Pokémon hidden: random-encounters in hiding-only
    // mode (lgp_game sets it from settings.ini), only with its mod file.
    if (s.hide_field_pokemon && random_encounters_file(game_dir / "mods") &&
        std::find(known_mods.begin(), known_mods.end(), "random-encounters") == known_mods.end())
        known_mods.push_back("random-encounters");
    if (!known_mods.empty())
        args.insert(args.end(), {"--mods", mods::join_mod_list(known_mods, ',')});
    const size_t own = args.size();
    args.insert(args.end(), s.extra_args.begin(), s.extra_args.end());
    const size_t from_argv = args.size();
    args.insert(args.end(), more.begin(), more.end());
    static const char* const kPathOptions[] = {"--save",          "--game",           "--exefs",
                                               "--pipeline-cache", "--pipeline-cache-seed", "--frame-dump"};
    for (size_t i = own; i + 1 < args.size(); ++i)
        for (const char* option : kPathOptions) {
            if (args[i] != option)
                continue;
            const std::string& value = args[i + 1];
            // Values from settings.ini are UTF-8; the ones after --app come from
            // argv in the narrow native encoding (the ANSI code page on Windows),
            // which path_from_utf8 would garble. Absolute paths stay as given.
            const fs::path p = i + 1 >= from_argv ? fs::path(value) : path_from_utf8(value);
            const bool off = args[i] == std::string("--pipeline-cache") && value == "none";
            if (!off && p.is_relative())
                args[i + 1] = path_to_arg((files.data / p).make_preferred());  // \ on Windows
            ++i;
            break;
        }
    return args;
}

}  // namespace lgp::boot
