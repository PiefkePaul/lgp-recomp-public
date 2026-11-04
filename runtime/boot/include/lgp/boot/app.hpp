// App mode of a boot program (P7.1): started without a command line (double
// click), the program takes its options from a settings file in the user's
// folders (lgp/platform/app_dirs.hpp) instead:
//
//   <config>/settings.ini   key = value lines, '#' starts a comment
//     game       = folder with exefs/ and romfs/ (or romfs.bin), the user's dump
//     saves      = save data folder (empty: <data>/saves; relative: below <data>)
//     controller = handheld | joycon-right | joycon-left (player 1)
//     player1    = press | order (player 1 is the first gamepad pressed, or the first connected)
//     fullscreen = 0 | 1
//     aspect     = fit | fill | stretch (picture format: 16:9 with bars, cut to fill, stretched)
//     effects    = full | half (expensive light effects at full or half resolution; empty: the device's default)
//     async      = 0 | 1 (graphics work on a thread of its own, --async-execute; empty: the device's default)
//     smoothing  = off | linear | fxaa (how the image is scaled to the screen, --smoothing; default off)
//     render_scale = 0.25..2 (render targets at that share of their size, --render-scale; menu: 1, 0.75, 0.5;
//                  empty: 1)
//     hud_bounds = 0..100 (widescreen: how far header, footer and corner groups move from the 16:9 frame
//                  towards the edge of a wider or taller screen, percent, --hud-bounds; default 100; only
//                  without a `layouts … hud` line, with one everything but the battle goes to the edge)
//     hud_sides  = T,B,L,R (the battle's HUD bounds per side: top, bottom, left, right, each 0..100 percent of
//                  the extra room there, for the layouts a widescreen `layouts … hud` line names; --hud-sides;
//                  empty: 0,0,0,0 = the 16:9 frame; the menu's screen "Battle HUD bounds" writes H,H,W,W)
//     touch      = 0 | 1 (on-screen controller; empty: the platform decides)
//     touch_size = 50..140, touch_opacity = 25..100 (percent); touch_mirrored = 0 | 1 (stick right)
//     touch_throw = 0 | 1 (a swipe upwards throws a Poké Ball; default 1)
//     touch_throw_pad = 0 | 1 (… also while a gamepad plays player 1; default 0)
//     volume     = 0..100 (percent of the game's sound)
//     ldn        = lan | off (trade and battle with other devices in the same Wi-Fi, --ldn lan; default lan;
//                  ryuldn from older builds reads as lan)
//                  Online through the RyuLDN server is always on when <game>/mods/online-button.cfg exists: with
//                  lan both links, --ldn both (the game's "nearby" takes the LAN, "far away" the server through the
//                  mod online-button, which the app then names), with off the server alone, --ldn ryuldn. Without
//                  the file only the Wi-Fi. ldn_online of older builds is read and dropped.
//     ldn_server = HOST[:PORT] or ws[s]://HOST[:PORT][/PATH]: another RyuLDN server than the build's
//                  (--ldn-server; port 30456 when missing, ws:// goes through a WebSocket bridge, port 80, wss:// with
//                  TLS, 443; empty: the build's LGP_LDN_SERVER_DEFAULT, default_ldn_server())
//     ldn_room   = text: rooms are only found by devices with the same text (--ldn-room; empty: public)
//     docked     = 0 | 1 (1: TV mode, the game renders 1080p; 0: handheld, 720p;
//                  empty: handheld; switchable while the game runs)
//     mods       = game mods that are on, ids separated by spaces (lgp/mods/mods.hpp; empty: none;
//                  without the line: CMake LGP_APP_DEFAULT_MODS, empty unless the build kit set it)
//     (encounter_rate, visible_pokemon: rows of older builds, read and dropped; visible_pokemon = 2 takes
//                  random-encounters, the app's Pokémon Gelb, out of mods)
//     language   = the game's language: ja, en-US, en-GB, fr, fr-CA, de, it, es, es-419, ko, zh-CN or zh-TW
//                  (empty: the device's language, --language device)
//     start_screen = 0 | 1 (1: the main menu before the game, to choose a save; 0: the game starts at once with
//                  the last save, the menu opens with the pause button; default 0)
//     controller_screen = 0 | 1 (1: the game asks for the controller at every start; 0: the app names the mod
//                  skip-controller-screen when <game>/mods/skip-controller-screen.cfg is there; default 0;
//                  audio-free-guard is named whenever <game>/mods/audio-free-guard.cfg is there)
//     hide_field_pokemon = 0 | 1 (1: in Let's Go, with random-encounters, the app's Pokémon Gelb, off, the
//                  field Pokémon are hidden but can still be touched; the app then names random-encounters in
//                  hiding-only mode when <game>/mods/random-encounters.cfg is there; default 0)
//     args       = more options of the command line, split at spaces
//   <data>/game/                    the dump when settings.ini names none (the
//                                   only way on phones: copied in with the Files app
//                                   on iOS, over adb on Android)
//   <data>/saves/                   save data (save slot 1; saves-2/ … more slots,
//                                   chosen on the start screen, start_screen = 1)
//   <data>/backups/                 copies of save slots made from the pause menu
//   <data>/logs/lgp.log             output of the last run (lgp.prev.log: the one before)
//   <cache>/webgpu-pipelines.lgppipes   pipeline cache
//
// This file holds the host-neutral parts (settings, checking a game folder,
// the resulting command line); dialogs and folders come from the platform.
#pragma once

#include <array>
#include <filesystem>
#include <string>
#include <vector>

namespace lgp::boot {

struct AppSettings {
    std::string game_dir;
    std::string save_dir;
    std::string controller = "handheld";
    std::string player1 = "press";
    bool fullscreen = false;
    std::string aspect = "fit";  // --aspect
    std::string effects;  // --effects: "", "full" or "half" ("": the device's default)
    std::string async_execute;  // --async-execute: "", "0" or "1" ("": the device's default)
    std::string render_scale;  // --render-scale: "" (1) or a number in 0.25..2
    std::string smoothing = "off";  // --smoothing: off | linear | fxaa
    int hud_bounds = 100;  // --hud-bounds: 0..100 percent (widescreen)
    std::string hud_sides;  // --hud-sides: "" or T,B,L,R, each 0..100 (the battle's HUD bounds)
    std::string touch;  // "", "0" or "1"
    int touch_size = 100;
    int touch_opacity = 100;
    bool touch_mirrored = false;
    bool touch_throw = true;
    bool touch_throw_pad = false;
    int volume = 100;
    std::string ldn = "lan";  // --ldn lan: lan | off
    std::string ldn_server;   // --ldn-server ("": default_ldn_server())
    std::string ldn_room;     // --ldn-room
    std::string docked;  // "", "0" or "1"
    std::vector<std::string> mods;  // --mods; ids this build does not know are kept, not passed on
    std::string language;  // --language ("": the device's, --language device)
    bool start_screen = false;  // the main menu before the game (--start-screen; menu only)
    bool controller_screen = false;  // the game's controller screen at start (else the mod skip-controller-screen)
    bool hide_field_pokemon = false;  // Let's Go without visible field Pokémon (Pokémon Gelb off)
    std::vector<std::string> extra_args;
};

// Languages of the game (settings.ini language =, the menu), in menu order.
extern const char* const kGameLanguages[12];

// The RyuLDN server of this build (CMake LGP_LDN_SERVER_DEFAULT; may be
// empty), and the one settings use: ldn_server, else the build's.
const char* default_ldn_server();
std::string ldn_server_of(const AppSettings& settings);
// Whether the mod online-button can start: its file online-button.cfg is in
// mods_dir (<game>/mods). With online on, only then does a run take both
// links (--ldn both) and the mod; without it "far away" stays Nintendo's and
// online goes alone (--ldn ryuldn).
bool online_button_file(const std::filesystem::path& mods_dir);
// Whether the mod skip-controller-screen can start: its file is in mods_dir.
// Unless settings.ini asks for the game's controller screen (controller_screen
// = 1), a run then names the mod: the game starts with the app's controller.
bool skip_controller_screen_file(const std::filesystem::path& mods_dir);
// Whether the hidden mod audio-free-guard can start: its file is in mods_dir.
// A run then always names it (no setting): the game's sound does not hang
// after its audio output was reopened.
bool audio_free_guard_file(const std::filesystem::path& mods_dir);
// Whether random-encounters can start: its file is in mods_dir. With
// hide_field_pokemon and Pokémon Gelb off a run names it (hiding only).
bool random_encounters_file(const std::filesystem::path& mods_dir);

// Files of the app below the platform's folders (AppDirs from lgp_platform).
struct AppFiles {
    std::filesystem::path data;            // relative paths of the app's options start here
    std::filesystem::path settings;        // <config>/settings.ini
    std::filesystem::path packaged_game;   // game folder inside the app package (may be empty or missing)
    std::filesystem::path default_game;    // <data>/game
    std::filesystem::path saves;           // <data>/saves
    std::filesystem::path log;             // <data>/logs/lgp.log
    std::filesystem::path previous_log;    // <data>/logs/lgp.prev.log
    std::filesystem::path pipeline_cache;  // <cache>/webgpu-pipelines.lgppipes
    std::filesystem::path native_cache;    // <cache>/native-rewrite.lgpnative (native start only)
};
AppFiles app_files(const std::filesystem::path& config, const std::filesystem::path& data,
                   const std::filesystem::path& cache, const std::filesystem::path& packaged_game = {});

// Missing file: defaults and true. Unknown keys are ignored (newer settings
// read by an older build); a malformed line or value is an error.
// "T,B,L,R" with each 0..100 into out (top, bottom, left, right); false if malformed.
bool parse_hud_sides(const std::string& text, std::array<int, 4>* out);

bool load_app_settings(const std::filesystem::path& file, AppSettings* out, std::string* error);
// Writes the settings (through a temporary file, so a crash keeps the old one).
// An existing file keeps its comments, order and unknown keys; known keys are
// replaced in place, missing ones appended.
bool save_app_settings(const std::filesystem::path& file, const AppSettings& settings, std::string* error);

// The game folder for a folder the user picked: the folder itself, or its
// parent if the user picked its exefs/ or romfs/ subfolder. It must hold
// exefs/<module> for every module of the lifted code, and romfs/ or
// romfs.bin (not with romfs_elsewhere: the Android app's romfs.bin comes from
// its split APKs, --game-file). Empty path and *problem (one sentence for the
// user, in the interface language: lgp/platform/ui_text.hpp) otherwise.
std::filesystem::path resolve_game_folder(const std::filesystem::path& picked,
                                          const std::vector<std::string>& modules, std::string* problem,
                                          bool romfs_elsewhere = false);

// Whether the game folder `root` (from resolve_game_folder) belongs to this
// build. module_ids are the hex build ids the code was lifted for, one per
// module (empty entries are not checked); an exefs/<module> that is an NSO
// with another id is from another version of the game. build_stamp, when not
// empty, must be the content of <root>/lgp-build-stamp.txt: a build whose game
// is not inside the app (build tool, --game-separate) writes both, so a game
// folder of an earlier build (other mod files) is refused instead of running
// against code lifted with other mod hooks. False and *problem (one sentence
// in the interface language) if not.
constexpr const char* kBuildStampFile = "lgp-build-stamp.txt";
bool game_fits_build(const std::filesystem::path& root, const std::vector<std::string>& modules,
                     const std::vector<std::string>& module_ids, const std::string& build_stamp,
                     std::string* problem);

// One line for the start screen saying where the game comes from: the app
// package, the app's own game folder, or the folder's path (UTF-8, in the
// interface language).
std::string game_source_text(const std::filesystem::path& game, const AppFiles& files);
// The game folder the boot program will use: the last --game in `args`
// (an --game in args = or after --app overrides the chosen one). Empty if none.
std::filesystem::path game_from_arguments(const std::vector<std::string>& args);

// Folder paths in settings.ini are UTF-8 (fs::path's own narrow form is the
// ANSI code page on Windows).
std::string path_to_utf8(const std::filesystem::path& p);
std::filesystem::path path_from_utf8(const std::string& s);
// A path as the boot program's options take it: narrow like its command line
// (the ANSI code page on Windows); UTF-8 if the code page cannot hold it
// (then it will not open there; Verdacht: only with unusual user names).
std::string path_to_arg(const std::filesystem::path& p);

// Save slots: folders <data>/saves (slot 1) and <data>/saves-<n> (n >= 2),
// sorted by number. `dir` is relative to <data> (the value of saves =).
struct SaveSlot {
    int number = 1;
    std::string dir;                              // "saves", "saves-2", ...
    std::filesystem::file_time_type last_write;   // newest file inside; min() if empty
    bool has_data = false;                        // any file inside
};
std::vector<SaveSlot> list_save_slots(const std::filesystem::path& data);
// The slot settings.ini points at (saves = empty or relative "saves[-n]"), else 0.
int current_save_slot(const AppSettings& settings);
// Creates the next free slot folder; its dir, or empty and *error.
std::string create_save_slot(const std::filesystem::path& data, std::string* error);
// Copies a save folder (with the save journal files; dir relative to <data>
// or absolute) to <data>/backups/<last part of dir>-<YYYYMMDD-HHMMSS>; the
// copy's path, or empty and *error.
std::filesystem::path backup_save_slot(const std::filesystem::path& data, const std::string& dir,
                                       std::string* error);

// Command line of the app's run (without the program name): the game window
// with sound, controllers and the pipeline cache, then settings.extra_args,
// then `more` (the options after --app; later ones win). Relative folders and
// files in them (saves, --save, --game, --exefs, --pipeline-cache,
// --pipeline-cache-seed, --frame-dump) are taken below files.data: the app's
// working folder is not its own (on iOS the container's path even changes
// with every reinstall). `--pipeline-cache none` stays none.
std::vector<std::string> app_arguments(const AppSettings& settings, const std::filesystem::path& game_dir,
                                       const AppFiles& files, const std::vector<std::string>& more = {});

}  // namespace lgp::boot
