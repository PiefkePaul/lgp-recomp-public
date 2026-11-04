// App mode of the boot program (lgp/boot/app.hpp, P7.1): settings file,
// game folder check, the resulting command line, and the user folders of
// the platform (lgp/platform/app_dirs.hpp).
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "lgp/boot/app.hpp"
#include "lgp/platform/ui_text.hpp"
#include "lgp/platform/app_dirs.hpp"
#include "lgp/platform/host_env.hpp"

using namespace lgp;
namespace fs = std::filesystem;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        ++g_failures;
    }
}

void write(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

std::string read(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

bool has_pair(const std::vector<std::string>& args, const std::string& key, const std::string& value) {
    for (size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == key && args[i + 1] == value)
            return true;
    return false;
}

// The value of the last `key` (the boot program's parser lets it win).
std::string last_value(const std::vector<std::string>& args, const std::string& key) {
    std::string v;
    for (size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == key)
            v = args[i + 1];
    return v;
}

void test_settings(const fs::path& dir) {
    std::string error;
    boot::AppSettings s;
    s.controller = "joycon-left";
    check(boot::load_app_settings(dir / "missing.ini", &s, &error), "missing file loads");
    check(s.controller == "handheld" && s.game_dir.empty() && !s.fullscreen, "missing file gives defaults");

    s.game_dir = "D:/Spiele/LGP Dump/ä";
    s.save_dir = "";
    s.controller = "joycon-right";
    s.player1 = "order";
    s.fullscreen = true;
    s.extra_args = {"--unthrottled", "--stick", "0.2,0.9"};
    const fs::path file = dir / "cfg" / "settings.ini";
    check(boot::save_app_settings(file, s, &error), "save creates the folder and the file");
    check(!fs::exists(dir / "cfg" / "settings.ini.tmp"), "no temporary file left");
    boot::AppSettings back;
    check(boot::load_app_settings(file, &back, &error), "saved file loads");
    check(back.game_dir == s.game_dir, "game folder survives (UTF-8, spaces)");
    check(back.controller == "joycon-right" && back.fullscreen, "controller and fullscreen survive");
    check(back.player1 == "order", "player 1 choice survives");
    check(back.extra_args == s.extra_args, "args survive");
    check(back.touch.empty() && back.volume == 100 && back.touch_size == 100 && back.touch_throw &&
              !back.touch_throw_pad,
          "menu settings default");

    s.touch = "0";
    s.touch_size = 140;
    s.touch_opacity = 60;
    s.touch_mirrored = true;
    s.touch_throw = false;
    s.touch_throw_pad = true;
    s.volume = 35;
    s.docked = "1";
    check(boot::save_app_settings(file, s, &error), "save with menu settings");
    check(boot::load_app_settings(file, &back, &error), "menu settings load");
    check(back.touch == "0" && back.touch_size == 140 && back.touch_opacity == 60 && back.touch_mirrored &&
              !back.touch_throw && back.touch_throw_pad && back.volume == 35 && back.docked == "1",
          "menu settings survive");
    check(back.aspect == "fit", "picture format defaults to fit");
    s.aspect = "fill";
    check(boot::save_app_settings(file, s, &error) && boot::load_app_settings(file, &back, &error) &&
              back.aspect == "fill",
          "picture format survives");
    write(dir / "aspect.ini", "aspect = stretch\n");
    check(boot::load_app_settings(dir / "aspect.ini", &back, &error) && back.aspect == "stretch", "aspect = stretch");
    write(dir / "bad9.ini", "aspect = zoom\n");
    check(!boot::load_app_settings(dir / "bad9.ini", &back, &error), "aspect must be fit, fill or stretch");
    check(back.effects.empty(), "effects default to the device's");
    s.effects = "half";
    check(boot::save_app_settings(file, s, &error) && boot::load_app_settings(file, &back, &error) &&
              back.effects == "half",
          "effects survive");
    s.effects.clear();
    write(dir / "bad10.ini", "effects = low\n");
    check(!boot::load_app_settings(dir / "bad10.ini", &back, &error), "effects must be full, half or empty");
    check(back.async_execute.empty(), "asynchronous execution defaults to the device's default");
    s.async_execute = "1";
    check(boot::save_app_settings(file, s, &error) && boot::load_app_settings(file, &back, &error) &&
              back.async_execute == "1",
          "async survives");
    s.async_execute.clear();
    write(dir / "bad12.ini", "async = yes\n");
    check(!boot::load_app_settings(dir / "bad12.ini", &back, &error), "async must be 0, 1 or empty");
    check(back.render_scale.empty(), "render scale defaults to 1");
    s.render_scale = "0.75";
    check(boot::save_app_settings(file, s, &error) && boot::load_app_settings(file, &back, &error) &&
              back.render_scale == "0.75",
          "render scale survives");
    s.render_scale.clear();
    write(dir / "bad14.ini", "render_scale = 3\n");
    check(!boot::load_app_settings(dir / "bad14.ini", &back, &error), "render_scale must be in 0.25..2");
    write(dir / "bad15.ini", "render_scale = 75%\n");
    check(!boot::load_app_settings(dir / "bad15.ini", &back, &error), "render_scale must be a number");
    check(back.hud_bounds == 100, "HUD bounds default to 100 %");
    s.hud_bounds = 25;
    check(boot::save_app_settings(file, s, &error) && boot::load_app_settings(file, &back, &error) &&
              back.hud_bounds == 25,
          "HUD bounds survive");
    s.hud_bounds = 100;
    write(dir / "bad17.ini", "hud_bounds = 120\n");
    check(!boot::load_app_settings(dir / "bad17.ini", &back, &error), "hud_bounds must be in 0..100");
    s.hud_sides = "0,10,50,100";
    check(boot::save_app_settings(file, s, &error) && boot::load_app_settings(file, &back, &error) &&
              back.hud_sides == "0,10,50,100",
          "HUD sides survive");
    s.hud_sides.clear();
    write(dir / "bad18.ini", "hud_sides = 0,10,50\n");
    check(!boot::load_app_settings(dir / "bad18.ini", &back, &error), "hud_sides needs four values");
    std::array<int, 4> sides{};
    check(boot::parse_hud_sides("5,0,100,42", &sides) && sides == std::array<int, 4>{5, 0, 100, 42}, "sides parse");
    check(!boot::parse_hud_sides("5,0,101,42", &sides) && !boot::parse_hud_sides("5,0,1,2,3", &sides) &&
              !boot::parse_hud_sides("5,,1,2", &sides),
          "bad sides refused");
    check(back.smoothing == "off", "smoothing defaults to off");
    s.smoothing = "fxaa";
    check(boot::save_app_settings(file, s, &error) && boot::load_app_settings(file, &back, &error) &&
              back.smoothing == "fxaa",
          "smoothing survives");
    s.smoothing = "off";
    write(dir / "bad16.ini", "smoothing = bicubic\n");
    check(!boot::load_app_settings(dir / "bad16.ini", &back, &error), "smoothing must be off, linear or fxaa");
    check(back.language.empty(), "language defaults to the device's");
    s.language = "de";
    check(boot::save_app_settings(file, s, &error) && boot::load_app_settings(file, &back, &error) &&
              back.language == "de",
          "language survives");
    s.language.clear();
    write(dir / "bad-language.ini", "language = nl\n");
    check(!boot::load_app_settings(dir / "bad-language.ini", &back, &error), "language must be one of the game's");
    // Older builds' random-encounters rows: read and dropped; "visible_pokemon = 2"
    // (all visible, no attacks) takes the mod out, the rest keeps it.
    write(dir / "old-encounters.ini", "mods = random-encounters wild-battles\nencounter_rate = rare\nvisible_pokemon = 1\n");
    check(boot::load_app_settings(dir / "old-encounters.ini", &back, &error) && back.mods.size() == 2,
          "old encounter rows load, the mod stays");
    write(dir / "old-encounters.ini", "visible_pokemon = 2\nmods = random-encounters wild-battles\n");
    check(boot::load_app_settings(dir / "old-encounters.ini", &back, &error) && back.mods.size() == 1 &&
              back.mods[0] == "wild-battles",
          "visible_pokemon = 2 takes random-encounters out");
    check(boot::save_app_settings(dir / "old-encounters.ini", back, &error) &&
              read(dir / "old-encounters.ini").find("visible_pokemon") == std::string::npos,
          "saving drops the old row");
    back.mods.push_back("random-encounters");  // switched on again in the menu
    check(boot::save_app_settings(dir / "old-encounters.ini", back, &error) &&
              boot::load_app_settings(dir / "old-encounters.ini", &back, &error) && back.mods.size() == 2,
          "switched on again, it stays on");
    check(boot::load_app_settings(file, &back, &error) && !back.start_screen, "start_screen defaults to 0");
    s.start_screen = true;
    check(boot::save_app_settings(file, s, &error) && boot::load_app_settings(file, &back, &error) &&
              back.start_screen,
          "start_screen survives");
    s.start_screen = false;
    write(dir / "bad-start.ini", "start_screen = on\n");
    check(!boot::load_app_settings(dir / "bad-start.ini", &back, &error), "start_screen must be 0 or 1");
    check(!back.controller_screen, "controller_screen defaults to 0");
    s.controller_screen = true;
    check(boot::save_app_settings(file, s, &error) && boot::load_app_settings(file, &back, &error) &&
              back.controller_screen,
          "controller_screen survives");
    s.controller_screen = false;
    write(dir / "bad-cscreen.ini", "controller_screen = yes\n");
    check(!boot::load_app_settings(dir / "bad-cscreen.ini", &back, &error), "controller_screen must be 0 or 1");
    check(!back.hide_field_pokemon, "hide_field_pokemon defaults to 0");
    s.hide_field_pokemon = true;
    check(boot::save_app_settings(file, s, &error) && boot::load_app_settings(file, &back, &error) &&
              back.hide_field_pokemon,
          "hide_field_pokemon survives");
    s.hide_field_pokemon = false;
    write(dir / "bad-hide.ini", "hide_field_pokemon = on\n");
    check(!boot::load_app_settings(dir / "bad-hide.ini", &back, &error), "hide_field_pokemon must be 0 or 1");
    write(dir / "bad13.ini", "docked = tv\n");
    check(!boot::load_app_settings(dir / "bad13.ini", &back, &error), "docked must be 0, 1 or empty");
    s.mods = {"wild-battles", "from-a-newer-build"};
    check(boot::save_app_settings(file, s, &error), "save with mods");
    check(boot::load_app_settings(file, &back, &error) && back.mods == s.mods, "mods survive, unknown ids too");
    write(dir / "mods.ini", "mods = wild-battles,  wild-battles\n");
    check(boot::load_app_settings(dir / "mods.ini", &back, &error) && back.mods.size() == 1, "mods once");
    s.mods.clear();
    check(back.ldn == "lan", "local play over the LAN defaults to on in the app");
    s.ldn = "off";
    check(boot::save_app_settings(file, s, &error) && boot::load_app_settings(file, &back, &error) &&
              back.ldn == "off",
          "ldn = off survives");
    s.ldn = "lan";
    s.ldn_server = "lgp.example.org:30456";
    s.ldn_room = "zwei Woerter";
    check(boot::save_app_settings(file, s, &error) && boot::load_app_settings(file, &back, &error) &&
              back.ldn == "lan" && back.ldn_server == s.ldn_server && back.ldn_room == s.ldn_room,
          "server and room survive");
    s.ldn_server.clear();
    s.ldn_room.clear();
    write(dir / "old_ryuldn.ini", "ldn = ryuldn\nldn_server = ws://h.example\n");
    check(boot::load_app_settings(dir / "old_ryuldn.ini", &back, &error) && back.ldn == "lan" &&
              back.ldn_server == "ws://h.example",
          "ldn = ryuldn of older builds reads as lan");
    write(dir / "bad10.ini", "ldn = wifi\n");
    check(!boot::load_app_settings(dir / "bad10.ini", &back, &error), "ldn must be lan, ryuldn or off");
    // Online is always on now; the old switch is read and dropped, also its 0.
    write(dir / "old_online.ini", "ldn_online = 0\nldn = off\n");
    check(boot::load_app_settings(dir / "old_online.ini", &back, &error) && back.ldn == "off",
          "ldn_online of older builds is dropped without an error");
    check(boot::save_app_settings(file, back, &error) && read(file).find("ldn_online") == std::string::npos,
          "ldn_online is not written any more");
    back = {};
    check(boot::ldn_server_of(back) == boot::default_ldn_server(), "an empty ldn_server is the build's server");
    back.ldn_server = "other.example";
    check(boot::ldn_server_of(back) == "other.example", "ldn_server overrides the build's server");
    write(dir / "bad4.ini", "volume = 101\n");
    check(!boot::load_app_settings(dir / "bad4.ini", &back, &error), "volume above 100 is an error");
    write(dir / "bad5.ini", "touch = yes\n");
    check(!boot::load_app_settings(dir / "bad5.ini", &back, &error), "touch must be 0, 1 or empty");
    write(dir / "bad6.ini", "touch_mirrored = yes\n");
    check(!boot::load_app_settings(dir / "bad6.ini", &back, &error), "touch_mirrored must be 0 or 1");
    write(dir / "bad6b.ini", "touch_throw = on\n");
    check(!boot::load_app_settings(dir / "bad6b.ini", &back, &error), "touch_throw must be 0 or 1");
    write(dir / "bad7.ini", "touch_size = 200\n");
    check(!boot::load_app_settings(dir / "bad7.ini", &back, &error), "touch_size above 140 is an error");
    write(dir / "bad8.ini", "touch_opacity = 101\n");
    check(!boot::load_app_settings(dir / "bad8.ini", &back, &error), "touch_opacity above 100 is an error");

    write(dir / "bom.ini", "\xEF\xBB\xBF# comment\r\ngame = C:\\Games\\LGP\r\n  future_key = 1\r\nfullscreen=0\r\n");
    check(boot::load_app_settings(dir / "bom.ini", &back, &error), "BOM, CRLF and unknown keys load");
    check(back.game_dir == "C:\\Games\\LGP" && !back.fullscreen, "values from a Windows editor");

    // Saving keeps the user's comments, order and keys this build does not know.
    write(dir / "keep.ini", "# my notes\nfuture_key = 1\nvolume = 20\n\n# below\nvolume = 30\ngame = old\n");
    check(boot::load_app_settings(dir / "keep.ini", &back, &error), "keep.ini loads");
    back.volume = 70;
    back.game_dir = "new";
    check(boot::save_app_settings(dir / "keep.ini", back, &error), "save over a hand-written file");
    {
        std::ifstream in(dir / "keep.ini", std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        check(text.rfind("# my notes\nfuture_key = 1\nvolume = 70\n\n# below\ngame = new\n", 0) == 0,
              "comments, unknown key and order kept, known keys replaced in place, the second volume dropped");
        check(text.find("controller = ") != std::string::npos && text.find("args =") != std::string::npos,
              "missing keys appended");
    }
    check(boot::load_app_settings(dir / "keep.ini", &back, &error) && back.volume == 70 && back.game_dir == "new",
          "saved values load");

    write(dir / "bad1.ini", "game\n");
    check(!boot::load_app_settings(dir / "bad1.ini", &back, &error) && error.find(":1:") != std::string::npos,
          "a line without = is an error with its line number");
    write(dir / "bad2.ini", "controller = pro\n");
    check(!boot::load_app_settings(dir / "bad2.ini", &back, &error), "an unknown controller is an error");
    write(dir / "bad3.ini", "fullscreen = yes\n");
    check(!boot::load_app_settings(dir / "bad3.ini", &back, &error), "fullscreen must be 0 or 1");
    write(dir / "bad4.ini", "player1 = first\n");
    check(!boot::load_app_settings(dir / "bad4.ini", &back, &error), "player1 must be press or order");
}

void test_game_folder(const fs::path& dir) {
    const std::vector<std::string> modules = {"main", "sdk"};
    const fs::path game = dir / "game";
    std::string problem;
    check(boot::resolve_game_folder(game, modules, &problem).empty() && !problem.empty(), "missing folder");
    fs::create_directories(game);
    check(boot::resolve_game_folder(game, modules, &problem).empty() && problem.find("exefs") != std::string::npos,
          "folder without exefs");
    write(game / "exefs" / "main", "x");
    check(boot::resolve_game_folder(game, modules, &problem).empty() && problem.find("sdk") != std::string::npos,
          "exefs without a module names it");
    write(game / "exefs" / "sdk", "x");
    check(boot::resolve_game_folder(game, modules, &problem).empty() && problem.find("romfs") != std::string::npos,
          "folder without romfs");
    check(boot::resolve_game_folder(game, modules, &problem, true) == game,
          "romfs from elsewhere (Android split APKs): exefs is enough");
    write(game / "romfs.bin", "x");
    check(boot::resolve_game_folder(game, modules, &problem) == game, "exefs + romfs.bin");
    fs::remove(game / "romfs.bin");
    fs::create_directories(game / "romfs");
    check(boot::resolve_game_folder(game, modules, &problem) == game, "exefs + romfs/");
    check(boot::resolve_game_folder(game / "exefs", modules, &problem) == game, "picked exefs/ means its parent");
    check(boot::resolve_game_folder(game / "romfs", modules, &problem) == game, "picked romfs/ means its parent");

    // Belongs to this build: build ids of NSO modules, then the build stamp.
    std::string nso(0x100, '\0');
    nso.replace(0, 4, "NSO0");
    for (int i = 0; i < 32; ++i)
        nso[0x40 + i] = static_cast<char>(i == 0 ? 0xab : i);
    write(game / "exefs" / "main", nso);
    const std::string id = "ab0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
    check(boot::game_fits_build(game, modules, {id, ""}, "", &problem), "matching build id fits");
    check(boot::game_fits_build(game, modules, {}, "", &problem), "no ids: nothing to check");
    const std::string other = "cd" + id.substr(2);
    problem.clear();
    check(!boot::game_fits_build(game, modules, {other, ""}, "", &problem) &&
              problem.find("main") != std::string::npos && problem.find("ab01020304050607") != std::string::npos &&
              problem.find("cd01020304050607") != std::string::npos,
          "another build id is refused, naming the module and both ids");
    check(boot::game_fits_build(game, modules, {"", other}, "", &problem), "sdk is no NSO here: left to the boot check");
    problem.clear();
    check(!boot::game_fits_build(game, modules, {id, ""}, "k1", &problem) && !problem.empty(),
          "a build stamp without the file is refused");
    write(game / boot::kBuildStampFile, "k2\n");
    check(!boot::game_fits_build(game, modules, {id, ""}, "k1", &problem), "another build stamp is refused");
    write(game / boot::kBuildStampFile, "k1\n");
    check(boot::game_fits_build(game, modules, {id, ""}, "k1", &problem), "the same build stamp fits");
}

void test_arguments(const fs::path& dir) {
    const boot::AppFiles files = boot::app_files(dir / "c", dir / "d", dir / "k");
    check(files.settings == dir / "c" / "settings.ini", "settings below config");
    check(files.default_game == dir / "d" / "game", "default game folder below data");
    check(files.saves == dir / "d" / "saves" && files.log == dir / "d" / "logs" / "lgp.log", "saves and log below data");
    check(files.pipeline_cache == dir / "k" / "webgpu-pipelines.lgppipes", "pipeline cache below cache");
    check(files.native_cache == dir / "k" / "native-rewrite.lgpnative", "native cache below cache");
    check(files.packaged_game.empty(), "no packaged game unless the platform names one");
    check(boot::app_files(dir / "c", dir / "d", dir / "k", dir / "app" / "game").packaged_game ==
              dir / "app" / "game",
          "packaged game folder as given");
    const boot::AppFiles packaged = boot::app_files(dir / "c", dir / "d", dir / "k", dir / "app" / "game");
    check(boot::game_source_text(dir / "app" / "game", packaged) == "Game: included in the app",
          "game source: app package");
    check(boot::game_source_text(dir / "d" / "game", packaged) == "Game: the app's folder \"game\"",
          "game source: the app's game folder");
    check(boot::game_source_text(dir / "elsewhere", files) == "Game: " + boot::path_to_utf8(dir / "elsewhere"),
          "game source: other folder by path");
    // German on a German device (lgp/platform/ui_text.hpp).
    platform::set_ui_language(platform::UiLanguage::German);
    check(boot::game_source_text(dir / "app" / "game", packaged) == "Spiel: in der App enthalten",
          "game source in German");
    platform::set_ui_language(platform::UiLanguage::English);
    {
        boot::AppSettings o;
        o.extra_args = {"--game", "other"};
        const std::vector<std::string> a = boot::app_arguments(o, dir / "app" / "game", packaged);
        check(boot::game_from_arguments(a) == dir / "d" / "other", "the last --game wins (args =)");
        check(boot::game_source_text(boot::game_from_arguments(a), packaged) ==
                  "Game: " + boot::path_to_utf8(dir / "d" / "other"),
              "game source follows an --game override");
        const std::vector<std::string> b =
            boot::app_arguments(boot::AppSettings{}, dir / "app" / "game", packaged, {"--game", "run"});
        check(boot::game_from_arguments(b) == dir / "d" / "run", "the last --game wins (after --app)");
        check(boot::game_from_arguments({"--frames", "1"}).empty(), "no --game");
    }

    boot::AppSettings s;
    s.extra_args = {"--renderer", "soft"};
    const fs::path game = dir / "game";
    std::vector<std::string> args = boot::app_arguments(s, game, files);
    check(has_pair(args, "--exefs", (game / "exefs").string()), "--exefs");
    check(has_pair(args, "--game", game.string()), "--game");
    check(has_pair(args, "--save", files.saves.string()), "--save defaults to the app's saves");
    check(has_pair(args, "--graphics", "hle") && has_pair(args, "--renderer", "window"), "game window");
    check(has_pair(args, "--pipeline-cache", files.pipeline_cache.string()), "pipeline cache always on");
    check(has_pair(args, "--native-cache", files.native_cache.string()), "native cache always on");
    check(has_pair(args, "--controller", "handheld"), "controller");
    check(has_pair(args, "--language", "device"), "the device's language by default");
    check(std::find(args.begin(), args.end(), "--player1") == args.end(), "player 1 by press is the default");
    check(std::find(args.begin(), args.end(), "--fullscreen") == args.end(), "windowed by default");
    check(std::find(args.begin(), args.end(), "--aspect") == args.end(), "fit needs no --aspect");
    check(std::find(args.begin(), args.end(), "--effects") == args.end(), "effects: the device's default");
    check(std::find(args.begin(), args.end(), "--mods") == args.end(), "no mods by default");
    check(has_pair(args, "--ldn", "lan"), "local play over the LAN is on in the app");
    s.ldn = "off";
    args = boot::app_arguments(s, game, files);
    check(std::find(args.begin(), args.end(), "--ldn") == args.end(), "ldn = off passes no --ldn");
    s.ldn_server = "lgp.example.org";
    s.ldn_room = "pauls geraete";
    args = boot::app_arguments(s, game, files);
    check(std::find(args.begin(), args.end(), "--ldn") == args.end() &&
              std::find(args.begin(), args.end(), "--ldn-server") == args.end() &&
              std::find(args.begin(), args.end(), "--mods") == args.end(),
          "without the online-button file the server is of no use: Wi-Fi off, no link");
    s.ldn = "lan";
    args = boot::app_arguments(s, game, files);
    check(last_value(args, "--ldn") == "lan" && std::count(args.begin(), args.end(), "--ldn") == 1 &&
              std::find(args.begin(), args.end(), "--ldn-server") == args.end(),
          "without the online-button file only the Wi-Fi (far away would stay Nintendo's)");
    fs::create_directories(game / "mods");
    std::ofstream(game / "mods" / "online-button.cfg") << "# test\n";
    args = boot::app_arguments(s, game, files);
    check(last_value(args, "--ldn") == "both" && std::count(args.begin(), args.end(), "--ldn") == 1 &&
              has_pair(args, "--ldn-server", "lgp.example.org") && has_pair(args, "--ldn-room", "pauls geraete") &&
              has_pair(args, "--mods", "online-button"),
          "Wi-Fi and the online-button file: both links in one run, the mod named, without any switch");
    s.ldn = "off";
    args = boot::app_arguments(s, game, files);
    check(last_value(args, "--ldn") == "ryuldn" && has_pair(args, "--mods", "online-button"),
          "Wi-Fi off with the file: the server alone, far away through the mod");
    s.ldn = "lan";
    s.mods = {"wild-battles"};
    args = boot::app_arguments(s, game, files);
    check(has_pair(args, "--mods", "wild-battles,online-button"), "online-button after the mods switched on");
    s.mods = {"online-button"};
    args = boot::app_arguments(s, game, files);
    check(has_pair(args, "--mods", "online-button"), "online-button named once");
    s.mods.clear();
    s.ldn_server.clear();
    s.ldn_room.clear();
    args = boot::app_arguments(s, game, files);
    if (std::string(boot::default_ldn_server()).empty())
        check(last_value(args, "--ldn") == "lan" && std::find(args.begin(), args.end(), "--mods") == args.end(),
              "without any server it stays in the LAN");
    else
        check(last_value(args, "--ldn") == "both" && has_pair(args, "--ldn-server", boot::default_ldn_server()),
              "without ldn_server the build's server");
    fs::remove(game / "mods" / "online-button.cfg");
    s.mods = {"from-a-newer-build", "wild-battles"};
    args = boot::app_arguments(s, game, files);
    check(has_pair(args, "--mods", "wild-battles"), "--mods with the ids this build knows");
    s.mods = {"from-a-newer-build"};
    args = boot::app_arguments(s, game, files);
    check(std::find(args.begin(), args.end(), "--mods") == args.end(), "no --mods without a known id");
    s.mods.clear();
    // The game's controller screen: skipped by the mod when its file is there.
    std::ofstream(game / "mods" / "skip-controller-screen.cfg") << "# test\n";
    args = boot::app_arguments(s, game, files);
    check(has_pair(args, "--mods", "skip-controller-screen"), "with its file the mod skips the controller screen");
    s.mods = {"wild-battles"};
    args = boot::app_arguments(s, game, files);
    check(has_pair(args, "--mods", "wild-battles,skip-controller-screen"), "skip-controller-screen after the others");
    s.mods.clear();
    s.controller_screen = true;
    args = boot::app_arguments(s, game, files);
    check(std::find(args.begin(), args.end(), "--mods") == args.end(), "controller_screen = 1 keeps the screen");
    s.controller_screen = false;
    fs::remove(game / "mods" / "skip-controller-screen.cfg");
    // The audio hang guard: named whenever its file is there, no setting.
    std::ofstream(game / "mods" / "audio-free-guard.cfg") << "# test\n";
    args = boot::app_arguments(s, game, files);
    check(has_pair(args, "--mods", "audio-free-guard"), "with its file the audio guard is on");
    s.controller_screen = true;
    args = boot::app_arguments(s, game, files);
    check(has_pair(args, "--mods", "audio-free-guard"), "the audio guard does not follow controller_screen");
    s.controller_screen = false;
    fs::remove(game / "mods" / "audio-free-guard.cfg");
    // Hide field Pokémon (Let's Go, Pokémon Gelb off): random-encounters, only with its file.
    s.hide_field_pokemon = true;
    args = boot::app_arguments(s, game, files);
    check(std::find(args.begin(), args.end(), "--mods") == args.end(), "hiding needs the mod file");
    std::ofstream(game / "mods" / "random-encounters.cfg") << "# test\n";
    args = boot::app_arguments(s, game, files);
    check(has_pair(args, "--mods", "random-encounters"), "hiding names random-encounters");
    s.mods = {"random-encounters"};
    args = boot::app_arguments(s, game, files);
    check(has_pair(args, "--mods", "random-encounters"), "with Pokémon Gelb on it is named once");
    s.mods.clear();
    s.hide_field_pokemon = false;
    args = boot::app_arguments(s, game, files);
    check(std::find(args.begin(), args.end(), "--mods") == args.end(), "without hiding not named");
    fs::remove(game / "mods" / "random-encounters.cfg");
    // The throw's animation only with Catch in battle.
    s.mods = {"wild-battles", "battle-throw"};
    args = boot::app_arguments(s, game, files);
    check(has_pair(args, "--mods", "wild-battles"), "without battle-balls battle-throw is left out");
    s.mods = {"wild-battles", "battle-balls", "battle-throw"};
    args = boot::app_arguments(s, game, files);
    check(has_pair(args, "--mods", "wild-battles,battle-balls,battle-throw"), "with battle-balls it is named");
    s.mods.clear();
    args = boot::app_arguments(s, game, files);
    check(args.size() >= 2 && args[args.size() - 2] == "--renderer" && args.back() == "soft",
          "extra args come last (they win)");

    s.save_dir = (dir / "own saves").string();
    s.fullscreen = true;
    args = boot::app_arguments(s, game, files);
    check(has_pair(args, "--save", (dir / "own saves").string()), "own save folder");
    check(std::find(args.begin(), args.end(), "--fullscreen") != args.end(), "fullscreen");
    s.player1 = "order";
    check(has_pair(boot::app_arguments(s, game, files), "--player1", "order"), "player 1 in connection order");
    s.player1 = "press";
    s.aspect = "fill";
    args = boot::app_arguments(s, game, files);
    check(has_pair(args, "--aspect", "fill"), "picture format");
    s.aspect = "fit";
    s.effects = "full";
    check(has_pair(boot::app_arguments(s, game, files), "--effects", "full"), "effects chosen in the settings");
    s.effects.clear();
    check(std::find(args.begin(), args.end(), "--async-execute") == args.end(), "async: the device's default");
    s.async_execute = "0";
    check(has_pair(boot::app_arguments(s, game, files), "--async-execute", "off"), "async off");
    s.async_execute = "1";
    check(has_pair(boot::app_arguments(s, game, files), "--async-execute", "on"), "async on");
    s.async_execute.clear();
    check(std::find(args.begin(), args.end(), "--render-scale") == args.end(), "render scale: 1 by default");
    s.render_scale = "0.5";
    check(has_pair(boot::app_arguments(s, game, files), "--render-scale", "0.5"), "render scale chosen");
    s.render_scale.clear();
    check(std::find(args.begin(), args.end(), "--hud-bounds") == args.end(), "HUD bounds: 100 % by default");
    s.hud_bounds = 50;
    check(has_pair(boot::app_arguments(s, game, files), "--hud-bounds", "50"), "HUD bounds chosen");
    s.hud_bounds = 100;
    s.hud_sides = "0,0,30,60";
    check(has_pair(boot::app_arguments(s, game, files), "--hud-sides", "0,0,30,60"), "HUD sides chosen");
    s.hud_sides.clear();
    check(has_pair(args, "--hud-sides", "0,0,0,0"), "the battle's HUD bounds: the 16:9 frame by default");
    check(std::find(args.begin(), args.end(), "--smoothing") == args.end(), "smoothing: off by default");
    s.smoothing = "linear";
    check(has_pair(boot::app_arguments(s, game, files), "--smoothing", "linear"), "smoothing chosen");
    s.smoothing = "off";

    // Relative paths start in the data folder (iOS: the container moves with
    // every reinstall), in settings.ini and in the options after --app.
    check(files.data == dir / "d", "data folder");
    s.save_dir = "rel saves";
    s.extra_args = {"--frame-dump", "dumps/a.ppm"};
    args = boot::app_arguments(s, game, files, {"--save", "run", "--pipeline-cache", "none", "--frames", "10"});
    check(has_pair(args, "--save", (dir / "d" / "rel saves").string()), "relative saves = below data");
    check(has_pair(args, "--frame-dump", (dir / "d" / "dumps" / "a.ppm").string()), "relative path in args =");
    check(has_pair(args, "--save", (dir / "d" / "run").string()), "relative --save after --app below data");
    check(has_pair(args, "--pipeline-cache", "none"), "--pipeline-cache none stays none");
    check(has_pair(args, "--frames", "10"), "other options stay");
    check(args.size() >= 2 && args[args.size() - 2] == "--frames", "options after --app come last");
    args = boot::app_arguments(s, game, files, {"--save", (dir / "abs").string()});
    check(has_pair(args, "--save", (dir / "abs").string()), "absolute --save stays");
}

void test_save_slots(const fs::path& dir) {
    const fs::path data = dir / "slots";
    fs::remove_all(data);
    fs::create_directories(data);
    std::string error;
    check(boot::list_save_slots(data).empty(), "no slots yet");
    check(boot::create_save_slot(data, &error) == "saves", "first slot is saves");
    check(boot::create_save_slot(data, &error) == "saves-2", "second slot is saves-2");
    fs::create_directories(data / "saves-10");
    fs::create_directories(data / "saves-x");      // not a slot
    fs::create_directories(data / "saves-1");      // not a slot (slot 1 is saves)
    write(data / "saves-2" / "save" / "data.bin", "123");
    const std::vector<boot::SaveSlot> slots = boot::list_save_slots(data);
    check(slots.size() == 3 && slots[0].dir == "saves" && slots[1].dir == "saves-2" && slots[2].dir == "saves-10",
          "slots sorted by number, others ignored");
    check(!slots[0].has_data && slots[1].has_data, "slots with data");
    check(boot::create_save_slot(data, &error) == "saves-11", "a new slot after the highest");

    boot::AppSettings s;
    check(boot::current_save_slot(s) == 1, "empty saves = slot 1");
    s.save_dir = "saves-2";
    check(boot::current_save_slot(s) == 2, "saves-2 = slot 2");
    s.save_dir = "/elsewhere";
    check(boot::current_save_slot(s) == 0, "another folder is no slot");

    const fs::path backup = boot::backup_save_slot(data, "saves-2", &error);
    check(!backup.empty() && fs::exists(backup / "save" / "data.bin"), "backup copies the slot");
    check(backup.parent_path() == data / "backups", "backups below data");
    const fs::path second = boot::backup_save_slot(data, "saves-2", &error);
    check(!second.empty() && second != backup, "a second backup gets its own folder");
    check(boot::backup_save_slot(data, "saves-99", &error).empty() && !error.empty(), "missing slot: error");
    // A save folder outside the slots (absolute, trailing separator): the
    // backup still lands in <data>/backups, named after the folder.
    const fs::path own = dir / "own-saves";
    fs::create_directories(own);
    std::ofstream(own / "x.bin") << "x";
    const fs::path own_backup = boot::backup_save_slot(data, boot::path_to_utf8(own) + "/", &error);
    check(!own_backup.empty() && own_backup.parent_path() == data / "backups" &&
              boot::path_to_utf8(own_backup.filename()).rfind("own-saves-", 0) == 0 && fs::exists(own_backup / "x.bin"),
          "absolute save folder: backup below data");
    check(boot::backup_save_slot(data, ".", &error).empty() && error.find("into itself") != std::string::npos,
          "the data folder itself: no backup into itself");
    // <data>/backups unusable (here a file): an error, no exception, nothing left behind.
    const fs::path blocked = dir / "blocked";
    fs::create_directories(blocked / "saves");
    std::ofstream(blocked / "saves" / "x.bin") << "x";
    std::ofstream(blocked / "backups") << "not a folder";
    error.clear();
    check(boot::backup_save_slot(blocked, "saves", &error).empty() && !error.empty(), "backups not a folder: error");

    boot::AppSettings a;
    a.touch = "0";
    a.volume = 40;
    const boot::AppFiles files = boot::app_files(dir / "c", data, dir / "k");
    const std::vector<std::string> args = boot::app_arguments(a, dir / "game", files);
    check(has_pair(args, "--touch", "off") && has_pair(args, "--volume", "40"), "touch and volume options");
    const auto has = [](const std::vector<std::string>& v, const char* s) { return std::find(v.begin(), v.end(), s) != v.end(); };
    check(!has(args, "--docked") && !has(args, "--handheld"), "docked empty: the boot program's default");
    a.docked = "1";
    check(has(boot::app_arguments(a, dir / "game", files), "--docked"), "docked = 1: TV mode");
    a.docked = "0";
    check(has(boot::app_arguments(a, dir / "game", files), "--handheld"), "docked = 0: handheld");
}

void test_app_dirs(const fs::path& dir) {
    std::string error;
    platform::set_env("LGP_APP_DIR", (dir / "portable").string());
    auto d = platform::app_dirs("lgp-test", &error);
    check(d && d->config == dir / "portable" && d->data == dir / "portable" && d->cache == dir / "portable",
          "LGP_APP_DIR puts everything into one folder");
    platform::set_env("LGP_APP_DIR", "");
    d = platform::app_dirs("lgp-test", &error);
    check(d.has_value(), "the platform names its folders");
    if (d) {
        check(d->config.filename() == "lgp-test" && d->data.filename() == "lgp-test" &&
                  d->cache.filename() == "lgp-test",
              "folders end in the app name");
        check(d->config.is_absolute() && d->cache.is_absolute(), "folders are absolute");
        std::printf("app dirs: config %s, data %s, cache %s\n", d->config.string().c_str(), d->data.string().c_str(),
                    d->cache.string().c_str());
    }
    const fs::path packaged = platform::packaged_game_dir();
#if defined(__ANDROID__) || defined(__EMSCRIPTEN__)
    check(packaged.empty(), "no packaged game folder where the package is no folder");
#else
    check(packaged.is_absolute() && packaged.filename() == "game", "packaged game folder: <program dir>/game");
    std::printf("packaged game: %s\n", packaged.string().c_str());
#endif
#if !defined(_WIN32) && !defined(__APPLE__)
    platform::set_env("XDG_CACHE_HOME", (dir / "xdg-cache").string());
    platform::set_env("XDG_CONFIG_HOME", "relative/ignored");
    d = platform::app_dirs("lgp-test", &error);
    check(d && d->cache == dir / "xdg-cache" / "lgp-test", "XDG_CACHE_HOME is used");
    check(d && d->config.is_absolute(), "a relative XDG_CONFIG_HOME is ignored");
#endif
}

}  // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / "lgp_test_boot_app";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    test_settings(dir);
    test_game_folder(dir);
    test_arguments(dir);
    test_save_slots(dir);
    test_app_dirs(dir);
    fs::remove_all(dir, ec);
    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("boot app: all checks passed\n");
    return 0;
}
