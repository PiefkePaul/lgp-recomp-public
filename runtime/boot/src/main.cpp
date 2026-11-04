// Boot program (P3.4): starts a recompiled module set on the runtime.
// Built once per set of lifted code (lgp_add_boot_executable in CMake); the
// module files are read at run time and must be the ones the code was lifted
// from (checked against the lifted module table).
//
//   <program> (--exefs DIR | MODULE...) [--npdm FILE] [--game DIR] [--game-file NAME=SPEC] [--save DIR]
//             [--start auto|sdk|homebrew] [--graphics stop|hle|continue]
//             [--renderer soft|webgpu|window] [--canvas SELECTOR] [--frames N] [--fullscreen]
//             [--nvn-unimplemented halt|continue] [--nvn-trace]
//             [--program-id ID] [--memory MIB] [--language CODE] [--region N]
//             [--docked | --handheld] [--nickname NAME] [--time POSIX] [--utc-offset SEC]
//             [--sched free|core-baton|baton] [--clock host|virtual] [--trace-svcs] [--init-arrays] [--watchdog SEC]
//             [--native [--native-base HEX] [--native-config FILE] [--native-cache FILE|none]
//              [--native-image-out FILE | --native-image FILE]]
//             [--frame-stats N[,N...] [--frame-dump FILE.ppm] [--frame-grid]] [--press SPEC] [--input none|sdl] [--controller handheld|joycon-right|joycon-left] [--throw SPEC] [--pad-motion on|off] [--player1 press|order] [--motion-frame SPEC] [--motion-gain G] [--stick DZ,OUTER[,HOLD]] [--touch on|off] [--touch-throw on|off|always] [--input-log] [--motion-pose on|frozen] [--menu on|off] [--menu-script SPEC]
//             [--pipeline-cache FILE|none] [--pipeline-cache-seed FILE] [--time-stats N]
//             [--audio none|sdl] [--volume 0-100] [--video none|ffmpeg|vpx|host] [--no-movie-audio]
//             [--ldn off|lan|ryuldn|both] [--ldn-port N] [--ldn-peer IP[:PORT],...]
//             [--ldn-server HOST[:PORT]|ws[s]://HOST[:PORT][/PATH]] [--ldn-room TEXT] [--ldn-ca FILE]
//             [--log trace|debug|info|warn|error] [--unresolved FILE] [--quiet]
//
// --exefs takes the modules named in the lifted code from DIR (main, sdk, …)
// and DIR/main.npdm if present. --game-file NAME=SPEC adds a read-only file to
// the game storage's root, read from byte ranges of host files
// (FILE@OFFSET+LENGTH,..., lgp/platform/segment_storage.hpp); the Android app
// passes romfs.bin this way from its split APKs. --init-arrays: see BootOptions::run_init_arrays.
// --graphics hle runs the NVN-HLE; --renderer picks where frames go: soft
// (CPU reference, no window, default), webgpu (headless GPU) or window (SDL3
// + WebGPU; the process ends when the window is closed). The last two exist
// only where WebGPU/SDL3 were built (LGP_BOOT_WEBGPU, LGP_BOOT_SDL).
// --canvas SELECTOR (web): webgpu presents to that canvas ("#canvas").
// --paced shows frames no faster than the console would (the game's NVN
// present interval at 60 Hz, lgp/boot/frame_pacer.hpp); default with a window
// or a canvas. --unthrottled presents as fast as the game renders; default
// for soft and headless webgpu (measurements, tests).
// In the browser the page passes OPFS paths (/opfs/lgp/...); prepare_host()
// mounts OPFS before any file is read (lgp/boot/host.hpp).
// --clock virtual decouples guest time from the host (lgp/runtime/clock.hpp):
// it moves by a fixed cost per svc and counter read, jumps over idle time and
// to the next 60 Hz vsync at every present. It implies --sched baton (unless
// --sched says otherwise) and a fixed --time, so the same --press script gives
// the same run (docs/knowledge/virtuelle-gastuhr.md). Default: host.
// --env KEY=VALUE sets an environment variable before anything reads it (the
// browser page has no environment: e.g. --env LGP_WEBGPU_ASYNC_WRITEBACK=1).
// --time-stats-tick US reads the clock for it only every US microseconds
// (default 0, in the browser 1000: Safari leaked with a clock read per switch).
// --time-stats N prints, every N presented frames, where each guest thread
// spent its wall time (guest code, waiting for its slot, kernel waits, HLE
// requests, NVN recording, renderer calls) and how busy the renderer's GPU
// thread was (lgp/runtime/time_stats.hpp, docs/knowledge/zeitaufteilung-gast-threads.md).
// --frames N ends the process after N presented frames. --watchdog SEC logs
// every guest thread (state, wait, frame chain) after SEC seconds and stops.
// --stall-report SEC (default 10, 0 = off) logs the same plus the HLE requests
// in flight and the audout/movie state once when presents stop for SEC
// seconds while the guest is not paused, and keeps running.
// --frame-stats N prints numbers about presented frame N (WebGPU renderers);
// --frame-dump also writes it as PPM (a game image: keep it in LGP_PRIVATE_DIR;
// "{n}" in FILE becomes the frame number). --press scripts player 1 for runs
// without a window: TRIGGER:KEYS[:HOLD],... presses KEYS (a, b, x, y, l, r, zl,
// zr, plus, minus, up, down, left, right, the sticks lup, ldown, lleft, lright,
// rup, rdown, rleft, rright, throw = the Joy-Con throw gesture of Space with
// --controller joycon-right|joycon-left, joined with +) at presented frame N, at Ts seconds
// of guest time, at movie-start/movie-end[#K][+N|+Ts], or once the image has
// stood still (stable:500ms, BASE+after-stable[=DUR]; --graphics hle), for HOLD
// frames or Ts seconds, e.g. --press 300:a,40s:a,movie-end+1s:a,stable:500ms:a
// (lgp/boot/press_script.hpp).
// --keyboard answers the game's software keyboard (swkbd, e.g. the player's
// name): window (default with --renderer window) types into a dialog window,
// menu (default in app programs on iOS and Android, where a second window
// never shows) into a screen of the app menu with the on-screen keyboard,
// host (default in the browser) opens a page dialog, console reads a line
// from stdin, text:NAME always answers NAME; none (default otherwise) gives
// the game its initial text or the account nickname.
// --pipeline-cache FILE (WebGPU renderers) records the shader programs and
// draw pipelines the game needed and creates them ahead of use in later runs, against the
// stutter when an effect shows up (lgp/gpu/webgpu/pipeline_cache.hpp). The
// file is derived from the game: a .lgppipes file outside the repository.
// Default: $LGP_PRIVATE_DIR/cache/webgpu-pipelines.lgppipes if that variable
// is set (the browser page passes one in OPFS), else in app programs the
// app's cache folder; none switches it off.
// --pipeline-cache-seed FILE takes over the records of another .lgppipes file
// whatever device, browser or driver wrote it (e.g. Paul's PC playthrough for
// a fresh browser or the iPad); default <--game>/pipeline-seed.lgppipes if it
// exists (the browser page copies a .lgppipes file of the picked folder there),
// in app programs else pipeline-seed.lgppipes beside the packaged game folder
// (LGP_APP_PIPELINE_SEED).
// App programs (lgp_add_boot_executable(... APP), P7.1): started without
// arguments or with --app [options], they take their options from the app's
// settings file and ask for the game folder (lgp/boot/app.hpp).
// --fullscreen opens the window borderless fullscreen; F11 toggles it.
// --aspect fit|fill|stretch: how the 16:9 game image fills a screen of
// another shape (lgp/platform/screen_fit.hpp): with bars (default), cut to
// fill, or stretched; menus and the touch controller cover the whole window.
// --effects full|half (WebGPU renderers): the game's expensive additive light
// effects (CMake LGP_HALFRES_PROGRAMS) at full or half resolution, softer
// but cheaper on phones (WebGpuBackend::set_halfres_effects); default per
// device kind (CMake LGP_EFFECTS_<KIND>), LGP_WEBGPU_HALFRES_PROGRAMS in the
// environment wins (measurements).
// --async-execute on|off (WebGPU renderers): graphics work on a thread of
// its own, one present ahead (AsyncBackend); default per device kind (CMake
// LGP_ASYNC_EXECUTE_DEFAULT, LGP_ASYNC_EXECUTE_ANDROID[_TV]),
// LGP_WEBGPU_ASYNC_EXECUTE in the environment wins (measurements).
// --smoothing off|linear|fxaa (WebGPU renderers): how the game image is
// scaled onto the screen (platform::webgpu::PresentFilter): nearest texel
// (default), bilinear, or FXAA and bilinear; changes at once in the menu.
// --render-scale S (WebGPU renderers, 0.25..2, default 1): render targets
// at S times their size (WebGpuBackend::set_render_scale, fewer pixels to
// fill on phones, softer picture); LGP_WEBGPU_RENDER_SCALE in the
// environment wins (measurements).
// --input sdl (default with --renderer window) feeds hid from SDL3 gamepads
// and the keyboard; --controller picks what player 1 is (default handheld,
// player 2 is always a single right Joy-Con). Keys: docs/knowledge/host-input-auf-switch-controller.md.
// --player1 press (default) makes the first gamepad on which a button is
// pressed player 1, and again after the game disconnects npad No1 (LGP's
// controller screen); order keeps the connection order.
// --pad-motion off leaves the gamepads' gyro and accelerometer off;
// --motion-frame "-y,x,z" re-expresses player 1's real sensor data in
// another frame (both for tests against the game, docs/analysis/11-pc-abnahme-p5.md).
// --motion-gain G scales the gamepads' real angular velocity (default 1 for
// handheld, 3 for a pad that plays a single Joy-Con: with 1 the Pro
// Controller aimed far too bluntly there, Paul 2026-10-13; a real Joy-Con
// through SDL may want --motion-gain 1).
// --stick DZ,OUTER[,HOLD] sets the sticks' radial dead zone, the length from
// which they count as fully deflected (default 0.15,0.9) and a hysteresis
// that keeps a held stick steady against host jitter (default 0 = off).
// --menu on|off: the app's menus (P7, app mode with a window; lgp/platform/ui/
// app_menu.hpp): start screen with save slots and settings, pause button in
// the game. Default on unless --press runs a script. --menu-script SPEC drives
// them for tests (play, newslot, pause, backup, resume, quit, settings, slot<N>,
// docked/handheld (the TV mode setting), mod:<id>=on|off (a game mod's switch
// in the settings, live mods at once), encounter-rate:rare|medium|gen1 and
// visible-pokemon:off|mix|on (the rows under random-encounters),
// controller:handheld|joycon-right|joycon-left (player 1's controller, at once
// in the game), each with an optional @FRAME).
// --start-screen on|off: the start screen before the game (default: settings.ini
// start_screen, 0 unless set in the menu; on when --menu-script drives it).
// Off, the game starts with the preselected save at once; the start screen
// still comes when there is no save folder or iCloud has another state.
// --touch on|off: the on-screen controller (lgp/platform/touch_pad.hpp) on
// player 1, drawn over the image; default on for Android and iOS builds.
// --touch-throw on|off|always: a quick swipe upwards on free space throws a
// Poké Ball, with direction and strength for a single Joy-Con, as A in
// handheld (TouchPad::set_swipe_action). on: not while a gamepad plays
// player 1, always: then too (default on, the app's settings touch_throw and
// touch_throw_pad).
// --motion-pose frozen (diagnostics): the motion sensors report angle 0 and
// an identity direction, rates and acceleration unchanged. If aiming still
// works, the title reads only the rates (and the L+R reset does not matter).
// --input-log logs every vibration value the game sends (and whether it
// reached a motor) and once per second input statistics (SDL events, stick
// drops, gaps of the main loop and of hid's sampling tick).
// Without a window, hid and audout use the host's own devices if it has
// any (the browser page: Gamepad API, keyboard and Web Audio).
// --audio sdl plays audout through SDL3 (LGP_BOOT_SDL_AUDIO builds; default
// with --renderer window), none keeps audout paced by the host clock only;
// --volume sets its loudness in percent (the app settings, P7).
// --docked starts the console in TV mode (the game renders 1080p), --handheld
// (default) in handheld mode (720p). The app's settings switch it while the
// game runs, as docking does (am OperationModeChanged; menu script steps
// docked/handheld).
// --mods wild-battles,… starts game mods (P8, lgp/mods/mods.hpp) from their
// files in <--game>/mods (--mods-dir DIR elsewhere); the code must have been
// lifted with `lgp_lift --mods` on the same files. --hook-log N logs the first
// N calls of every hook door no mod uses (finding a mod's roles in the game).
// --video picks the decoder for in-game videos (movie:: HLE): ffmpeg
// (LGP_BOOT_FFMPEG builds, their default), host (the platform's own decoder:
// WebCodecs in the browser and MediaCodec on Android, the default there), vpx
// (libvpx, VP9 in software; the default where neither is there, e.g. iOS) or
// none (black pictures).
// The videos' sound tracks are offered to the game (Vorbis decoded on the
// host, LGP plays them through its own audout); --no-movie-audio hides them.
// Without the track the game paces the video by itself and LGP's title
// video runs about three times too fast (docs/analysis/16-videoton.md).
// --ldn lan: local wireless play (trade, battle) over the LAN with other
// instances of this program instead of the offline ldn:u (default off, in
// the app on via settings.ini ldn = lan; not tried with the game yet;
// docs/analysis/44-multiplayer-stand.md). The access
// point listens on UDP --ldn-port (default 39871); stations find it by
// broadcast, by probing their /24 and at each --ldn-peer address.
// --ldn ryuldn: the same over a RyuLDN server instead of the LAN (online,
// docs/knowledge/ldn-ueber-ryuldn.md): --ldn-server names it (port 30456 by
// default; ws://HOST[:PORT][/PATH] goes through a WebSocket bridge such as a
// Cloudflare tunnel), --ldn-room TEXT keeps the rooms among instances with the same text.
// wss:// adds TLS (port 443; the certificate is checked against the system's
// roots, --ldn-ca FILE takes a PEM file instead, e.g. an own CA).
// --ldn both: LAN and server in one run; each ldn session takes the LAN
// unless hle::set_ldn_remote(true) (the online-button mod, or
// LGP_LDN_REMOTE=1) asks for the server.
// Exit code: 0 the process ended by itself, reached the graphics stop or the
// frame limit, 1 it crashed or broke, 2 usage, 3 setup failed.
#include <algorithm>
#include <cassert>
#include <cstring>
#include <exception>
#include <atomic>
#include <chrono>
#include <ctime>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <deque>
#include <functional>
#include <sstream>
#include <iostream>
#include <tuple>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "lgp/boot/boot.hpp"
#include <optional>

#include "lgp/boot/app.hpp"  // parse_hud_sides; the app itself only with LGP_BOOT_APP
#include "lgp/boot/host.hpp"
#include "lgp/platform/code_signature.hpp"
#include "lgp/platform/host_env.hpp"
#include "lgp/boot/press_script.hpp"
#include "lgp/gpu/nvn/async_backend.hpp"
#include "lgp/gpu/nvn/backend.hpp"
#include "lgp/gpu/nvn/hle.hpp"
#include "lgp/hle/hid.hpp"
#include "lgp/platform/null_backends.hpp"
#include "lgp/platform/pad_input.hpp"
#include "lgp/platform/screen_fit.hpp"
#include "lgp/platform/cpu_affinity.hpp"
#include "lgp/platform/host_thread.hpp"
#include "lgp/platform/lan.hpp"
#include "lgp/platform/text_input_backends.hpp"
#include "lgp/platform/ui_text.hpp"
#include "lgp/mods/widescreen.hpp"
#include "lgp/runtime/log.hpp"
#include "lgp/runtime/native.hpp"
#include "lgp/runtime/system.hpp"
#include "lgp/runtime/thread.hpp"
#include "lgp/runtime/time_stats.hpp"
#include "movie_hle.hpp"
#if LGP_FP_STATS
#include "lgp/lifted/fp.hpp"
#endif
#if defined(LGP_BOOT_WEBGPU)
#include "lgp/gpu/webgpu/webgpu_backend.hpp"
#include "lgp/platform/webgpu/webgpu_renderer.hpp"
#endif
#if defined(LGP_BOOT_APP)
#include "lgp/boot/app.hpp"
#include "lgp/platform/app_dirs.hpp"
#include "lgp/platform/system_settings.hpp"
#if defined(LGP_CLOUD_CONTAINER)
#include "lgp/boot/save_backup.hpp"
#include "lgp/platform/cloud_save.hpp"
#endif
#endif
#if defined(LGP_BOOT_ANDROID_IMPORT)
#include "lgp/platform/android/game_import.hpp"
#endif
#if defined(LGP_BOOT_SDL)
#include "lgp/platform/desktop/sdl_dialogs.hpp"
#include "lgp/platform/desktop/sdl_input.hpp"
#include "lgp/platform/desktop/sdl_text_input.hpp"
#include "lgp/platform/desktop/sdl_window.hpp"
#endif
#if defined(LGP_BOOT_SDL_AUDIO)
#include "lgp/platform/desktop/sdl_audio.hpp"
#endif
#if defined(LGP_BOOT_UI)
#include "lgp/platform/ui/app_menu.hpp"
#endif
#if defined(LGP_BOOT_FFMPEG)
#include "lgp/platform/ffmpeg_video.hpp"
#endif
#if defined(LGP_BOOT_VPX)
#include "lgp/platform/fallback_video.hpp"
#include "lgp/platform/vpx_video.hpp"
#endif

#ifndef LGP_BOOT_ENTRIES
#define LGP_BOOT_ENTRIES lgp_lifted_entries
#endif
#define LGP_BOOT_CAT2(a, b) a##b
#define LGP_BOOT_CAT(a, b) LGP_BOOT_CAT2(a, b)

extern const lgp::LiftedEntry LGP_BOOT_ENTRIES[];
extern const size_t LGP_BOOT_CAT(LGP_BOOT_ENTRIES, _count);
extern const lgp::LiftedModule LGP_BOOT_CAT(LGP_BOOT_ENTRIES, _modules)[];
extern const size_t LGP_BOOT_CAT(LGP_BOOT_ENTRIES, _modules_count);

namespace {

using namespace lgp;

// Longest wait for the asynchronous backend's queue when the game pauses (menu,
// background): one present in flight takes milliseconds, more means a worker
// stuck in the driver, and the window thread goes on without it.
[[maybe_unused]] constexpr int64_t kAsyncDrainTimeoutMs = 2000;

[[noreturn]] void usage(const char* msg = nullptr) {
    if (msg != nullptr)
        std::fprintf(stderr, "lgp_boot: %s\n", msg);
    std::fprintf(stderr,
                 "usage: lgp_boot (--exefs DIR | MODULE...) [--npdm FILE] [--game DIR] [--game-file NAME=SPEC] [--save DIR]\n"
                 "                [--start auto|sdk|homebrew] [--graphics stop|hle|continue]\n"
                 "                [--renderer soft|webgpu|window] [--canvas SELECTOR] [--frames N]\n"
                 "                [--paced | --unthrottled] [--fullscreen] [--aspect fit|fill|stretch] [--effects full|half]\n"
                 "                [--async-execute on|off] [--render-scale S] [--smoothing off|linear|fxaa] [--hud-bounds P] [--hud-sides T,B,L,R]\n"
                 "                [--nvn-unimplemented halt|continue] [--nvn-trace]\n"
                 "                [--program-id ID] [--memory MIB] [--language CODE] [--region N]\n"
                 "                [--docked | --handheld] [--nickname NAME] [--time POSIX] [--utc-offset SEC]\n"
                 "                [--sched free|core-baton|baton] [--clock host|virtual] [--trace-svcs] [--init-arrays]\n"
                 "                [--native [--native-base HEX] [--native-config FILE] [--native-cache FILE|none]\n"
                 "                 [--native-image-out FILE | --native-image FILE]]\n"
                 "                [--watchdog SEC] [--stall-report SEC]\n"
                 "                [--frame-stats N[,N...] [--frame-dump FILE.ppm] [--frame-grid]] [--press SPEC] [--input none|sdl] [--controller handheld|joycon-right|joycon-left] [--throw SPEC] [--pad-motion on|off] [--player1 press|order] [--motion-frame SPEC] [--motion-gain G] [--stick DZ,OUTER[,HOLD]] [--touch on|off] [--touch-throw on|off|always] [--input-log] [--motion-pose on|frozen] [--menu on|off] [--menu-script SPEC]\n"
                 "                [--pipeline-cache FILE|none] [--pipeline-cache-seed FILE]\n"
                 "                [--time-stats N] [--time-stats-tick US] [--env KEY=VALUE]\n"
                 "                [--audio none|sdl] [--volume 0-100] [--video none|ffmpeg|vpx|host] [--no-movie-audio]\n"
                 "                [--keyboard none|console|window|menu|host|text:NAME]\n"
                 "                [--ldn off|lan|ryuldn|both] [--ldn-port N] [--ldn-peer IP[:PORT],...]\n"
                 "                [--ldn-server HOST[:PORT]|ws[s]://HOST[:PORT][/PATH]] [--ldn-room TEXT] [--ldn-ca FILE]\n"
                 "                [--mods ID[,ID...]] [--mods-dir DIR] [--hook-log N]\n"
                 "                [--log trace|debug|info|warn|error] [--unresolved FILE] [--quiet]\n");
    boot::finish_host();  // web: the page must still receive this text
    std::exit(2);
}

bool read_file(const std::string& path, std::vector<uint8_t>* out) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

// Same escaping as the fixture end-state files (tests/homebrew), so a boot
// run can be compared with them line by line.
std::string printable(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (c >= 0x20 && c < 0x7F && c != '\\') {
            out += static_cast<char>(c);
        } else {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\x%02x", c);
            out += buf;
        }
    }
    return out;
}

// Gyro gain for slots that present a single Joy-Con (see --motion-gain).
constexpr float kDefaultSingleJoyconGyroGain = 3.0f;

int run(int argc, char** argv);

#if defined(LGP_BOOT_APP)
// What run() needs from app mode for the menus (P7): set by app_start().
struct AppUiContext {
    boot::AppFiles files;
    boot::AppSettings settings;
    std::string save_dir;  // the save folder this run uses (relative to files.data or absolute)
    std::string game_source;  // start screen: where the game comes from
    // Player 1 (SdlInput, once it exists): choose again by the next button
    // press, and the name of the pad on slot 0 for the settings screen.
    std::function<void()> pick_player1;
    std::function<std::string()> player1_name;
    bool system_settings = false;  // the restart settings are in the Settings app (sync_system_settings)
};
std::optional<AppUiContext> g_app_ui;

// iPhone/iPad: controller, graphics and the link settings (those that need a
// restart) are edited in the Settings app (Einstellungen > Apps > LGP,
// platform/ios/Settings.bundle, lgp/platform/system_settings.hpp). What the
// user changed there since the last start goes to settings.ini; a value this
// build cannot use is put back. False where the app has no such page.
bool sync_system_settings(const boot::AppFiles& files, boot::AppSettings* s) {
    std::unique_ptr<platform::ISettingsStore> store = platform::system_settings();
    if (!store)
        return false;
    std::vector<std::pair<std::string, std::string>> values = {
        {"controller", s->controller},
        {"effects", s->effects},
        {"render_scale", s->render_scale.empty() ? "1" : s->render_scale},
        {"async", s->async_execute},
        {"ldn", s->ldn},
        {"ldn_server", s->ldn_server},
        {"ldn_room", s->ldn_room},
    };
    const std::vector<std::pair<std::string, std::string>> before = values;
    std::vector<std::string> taken;
    platform::sync_system_settings(*store, &values, &taken);
    for (const std::string& key : taken) {
        std::string value;
        for (const auto& [k, v] : values)
            if (k == key)
                value = v;
        const auto one_of = [&](std::initializer_list<const char*> ok) {
            return std::any_of(ok.begin(), ok.end(), [&](const char* o) { return value == o; });
        };
        hle::LdnLanConfig parsed;
        bool ok = true;
        if (key == "controller") ok = one_of({"handheld", "joycon-right", "joycon-left"}) && (s->controller = value, true);
        else if (key == "effects") ok = one_of({"", "full", "half"}) && (s->effects = value, true);
        else if (key == "render_scale") ok = one_of({"1", "0.75", "0.5"}) && (s->render_scale = value, true);
        else if (key == "async") ok = one_of({"", "0", "1"}) && (s->async_execute = value, true);
        else if (key == "ldn") ok = one_of({"lan", "off"}) && (s->ldn = value, true);
        else if (key == "ldn_server")
            ok = (value.empty() || hle::parse_ryuldn_server(value, &parsed)) && (s->ldn_server = value, true);
        else if (key == "ldn_room") s->ldn_room = value;
        if (!ok) {
            // Back to what the app uses, so the Settings app does not show a value nobody uses.
            for (const auto& [k, v] : before)
                if (k == key) {
                    store->set(k, v);
                    store->set("lgp.mirror." + k, v);
                }
            std::printf("app: Settings app: %s = \"%s\" not taken\n", key.c_str(), value.c_str());
            continue;
        }
        std::printf("app: Settings app: %s = %s\n", key.c_str(), value.c_str());
    }
    std::string error;
    if (!taken.empty() && !boot::save_app_settings(files.settings, *s, &error))
        std::fprintf(stderr, "lgp_boot: %s\n", error.c_str());
    return true;
}
#if defined(LGP_CLOUD_CONTAINER)
// iOS/tvOS: the saves in iCloud (lgp/boot/save_backup.hpp), one record shared
// by iPhone, iPad and Apple TV; null without an iCloud-signed app.
std::unique_ptr<boot::SaveBackup> g_save_backup;
#endif

// App mode (P7.1, lgp/boot/app.hpp): the program was started without
// arguments (double click; output goes to the log file) or with --app
// (output stays on the console; more options may follow).
constexpr const char* kAppName = "lgp-recomp";
constexpr const char* kAppTitle = "LGP";
#ifndef LGP_BOOT_APP_LOG_ALWAYS
#define LGP_BOOT_APP_LOG_ALWAYS 0
#endif
// The build stamp a game folder must carry (lgp/boot/app.hpp, game_fits_build;
// CMake LGP_APP_GAME_STAMP, set by the build tool for a game outside the app).
#ifndef LGP_BOOT_GAME_STAMP
#define LGP_BOOT_GAME_STAMP ""
#endif

struct AppStart {
    std::vector<std::string> args;  // the command line to run with
    std::filesystem::path log;      // empty: output stays where it is
};

void app_message(const std::string& text) {
    // Into the log first: the dialog blocks, and an app closed while it is
    // shown (iOS: swiped away) would otherwise leave no line behind.
    std::fprintf(stderr, "lgp_boot: %s\n", text.c_str());
    std::fflush(stderr);
#if defined(LGP_BOOT_SDL)
    platform::desktop::show_message(platform::desktop::MessageKind::Error, kAppTitle, text, {"OK"});
#endif
}

// The user's game folder: the saved one if it still fits, else the one inside
// the app package (built in with LGP_APP_GAME_DIR), else <data>/game, else
// asked for (desktop), copied in from a picked folder (Android) or explained
// (iOS/tvOS: no folder picker, the dump is copied into the app's Documents or,
// on tvOS, Library/Caches folder). Empty if the user gave up.
std::filesystem::path app_game_folder(boot::AppSettings& settings, const boot::AppFiles& files, bool romfs_elsewhere) {
    std::vector<std::string> modules, module_ids;
    for (size_t i = 0; i < LGP_BOOT_CAT(LGP_BOOT_ENTRIES, _modules_count); ++i) {
        const lgp::LiftedModule& m = LGP_BOOT_CAT(LGP_BOOT_ENTRIES, _modules)[i];
        modules.push_back(m.name);
        module_ids.push_back(m.module_id != nullptr ? m.module_id : "");
    }
    // A folder that holds the game and belongs to this build (other game
    // version, or a game folder of another build tool run: refused here with a
    // sentence for the user instead of the boot program's layout error).
    const auto resolve = [&](const std::filesystem::path& picked, std::string* problem,
                             bool elsewhere = false) -> std::filesystem::path {
        auto root = boot::resolve_game_folder(picked, modules, problem, elsewhere);
        if (!root.empty() && !boot::game_fits_build(root, modules, module_ids, LGP_BOOT_GAME_STAMP, problem))
            root.clear();
        return root;
    };
    std::string problem;
    if (!settings.game_dir.empty())
        if (auto root = resolve(boot::path_from_utf8(settings.game_dir), &problem);
            !root.empty())
            return root;
    // The folder whose problem the user is told about: a packaged game that
    // does not fit is a broken build and wins over a missing <data>/game.
    std::filesystem::path checked = files.default_game;
    if (settings.game_dir.empty()) {
        std::error_code ec;
        std::string packaged_problem;
        if (!files.packaged_game.empty() && std::filesystem::is_directory(files.packaged_game, ec)) {
            if (auto root = resolve(files.packaged_game, &packaged_problem); !root.empty())
                return root;
        }
        // An empty <data>/game (made by hand in the Files app, or left after
        // deleting its contents) counts as no game: the user gets the
        // instructions, not "lacks exefs".
        if (std::filesystem::is_directory(files.default_game, ec) &&
            !std::filesystem::is_empty(files.default_game, ec)) {
            if (auto root = resolve(files.default_game, &problem, romfs_elsewhere); !root.empty())
                return root;
        } else {
            problem.clear();
        }
        if (!packaged_problem.empty()) {
            problem = packaged_problem;
            checked = files.packaged_game;
        }
    }
#if defined(LGP_BOOT_SDL) && !defined(LGP_BOOT_NO_FOLDER_PICKER)
    (void)checked;  // the dialog lets the user pick another folder instead
    namespace dlg = platform::desktop;
    std::string text = settings.game_dir.empty()
                           ? platform::tr("Please choose the folder with your game dump. It holds the subfolders "
                                          "exefs and romfs (or the file romfs.bin).")
                           : platform::trf("The saved game folder no longer fits:\n{}\n{}\n\nPlease choose the "
                                           "folder again.",
                                           {settings.game_dir, problem});
    for (;;) {
        if (dlg::show_message(dlg::MessageKind::Info, kAppTitle, text,
                              {platform::tr("Choose folder"), platform::tr("Quit")}) != 0)
            return {};
        std::string error;
        const auto picked =
            dlg::pick_folder(platform::tr("Choose the game folder (with exefs and romfs)"), settings.game_dir, &error);
        if (!picked) {
            if (error.empty())
                return {};  // cancelled
            app_message(platform::trf("The folder picker failed: {}\nYou can also enter the folder in settings.ini "
                                      "(game = ...):\n{}",
                                      {error, boot::path_to_utf8(files.settings)}));
            return {};
        }
        const std::filesystem::path root = resolve(boot::path_from_utf8(*picked), &problem);
        if (!root.empty()) {
            settings.game_dir = boot::path_to_utf8(root);
            if (!boot::save_app_settings(files.settings, settings, &error))
                std::fprintf(stderr, "lgp_boot: %s\n", error.c_str());  // works this time anyway
            return root;
        }
        text = platform::trf("This folder does not fit:\n{}\n{}\n\nPlease choose another folder.", {*picked, problem});
    }
#elif defined(LGP_BOOT_ANDROID_IMPORT)
    // Android: the system folder picker gives no file paths, so the dump is
    // copied into <data>/game (GameImport.java), with progress and a space check.
    std::string text =
        !settings.game_dir.empty()
            ? platform::trf("The saved game folder no longer fits:\n{}\n{}\n\nPlease choose the folder with your "
                            "game dump, it is copied into the app.",
                            {settings.game_dir, problem})
        : !problem.empty()
            ? platform::trf("The game data in the app does not fit:\n{}\n\nPlease choose the folder with your game "
                            "dump, it is copied into the app again.",
                            {problem})
            : platform::tr("Welcome! To start, the app needs your own game dump: a folder with the subfolder exefs "
                           "and the folder romfs or the file romfs.bin.\n\nPlease choose this folder, it is copied "
                           "into the app once.");
    for (;;) {
        std::string error;
        const auto result = platform::android::import_game(text, boot::path_to_utf8(files.default_game), &error);
        if (result == platform::android::ImportResult::Cancelled)
            return {};
        if (result == platform::android::ImportResult::Imported) {
            const std::filesystem::path root = resolve(files.default_game, &problem);
            if (!root.empty()) {
                if (!settings.game_dir.empty()) {  // from now on the copy in the app
                    settings.game_dir.clear();
                    if (!boot::save_app_settings(files.settings, settings, &error))
                        std::fprintf(stderr, "lgp_boot: %s\n", error.c_str());
                }
                return root;
            }
            error = problem;
        }
        text = platform::trf("That did not work:\n{}\n\nPlease choose another folder.", {error});
    }
#else
    if (!settings.game_dir.empty())
        app_message(platform::trf("The game folder from settings.ini does not fit:\n{}\n{}", {settings.game_dir, problem}));
    else if (!problem.empty())
        app_message(platform::trf("The game folder does not fit:\n{}\n{}", {boot::path_to_utf8(checked), problem}));
    else
        app_message(platform::trf("No game found. This app does not contain the game: copy the folder \"game\" "
                                  "(exefs, romfs.bin and the rest the build tool made) into the app once, on a Mac in "
                                  "the Finder (select the device, then Files, then LGP), on Windows in the Apple "
                                  "Devices app (Files), on iPad and iPhone also in the Files app (On My iPad > LGP; "
                                  "on the Apple TV from the Mac with xcrun devicectl device copy to). The app looks "
                                  "in\n{}\nThen start it again.",
                                  {boot::path_to_utf8(files.default_game)}));
    return {};
#endif
}

// -1: run with out->args; otherwise the exit code.
int app_start(int argc, char** argv, AppStart* out) {
    std::string error;
    const auto dirs = platform::app_dirs(kAppName, &error);
    if (!dirs) {
        app_message(platform::trf("No folder for settings and saves: {}", {error}));
        return 3;
    }
    const boot::AppFiles files =
        boot::app_files(dirs->config, dirs->data, dirs->cache, platform::packaged_game_dir());
    std::error_code ec;
    // iOS: no console to keep (devicectl starts it with --app and options).
    if (argc == 1 || LGP_BOOT_APP_LOG_ALWAYS) {
        std::filesystem::create_directories(files.log.parent_path(), ec);
        std::filesystem::rename(files.log, files.previous_log, ec);  // keep the run before
        if (platform::redirect_output(files.log, &error))
            out->log = files.log;
    }
#if LGP_BOOT_APP_LOG_ALWAYS
    // Options after --app with relative paths name files below the app's folder (hosted_start).
    std::filesystem::current_path(files.data, ec);
#endif
    std::printf("app: settings %s, saves %s, cache %s\n", boot::path_to_utf8(files.settings).c_str(),
                boot::path_to_utf8(files.saves).c_str(), boot::path_to_utf8(files.pipeline_cache).c_str());
#if defined(LGP_CLOUD_CONTAINER)
    // Before settings and slots are read: an emptied Library/Caches comes back
    // from iCloud here.
    // --cloud-test-wipe (tests on a device): empties the local save folders and
    // the cloud sequence first, so the start below has to restore from iCloud.
    // Only the save folders; settings.ini and everything else stay.
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--cloud-test-wipe") != 0)
            continue;
        std::vector<std::filesystem::path> wipe;
        for (const auto& e : std::filesystem::directory_iterator(files.data, ec)) {
            const std::string name = boot::path_to_utf8(e.path().filename());
            if (name == "saves" || name.rfind("saves-", 0) == 0 || name == ".lgp-cloud")
                wipe.push_back(e.path());
        }
        for (const auto& path : wipe) {
            std::filesystem::remove_all(path, ec);
            std::printf("cloud saves: test wipe removed %s\n", boot::path_to_utf8(path.filename()).c_str());
        }
        std::copy(argv + i + 1, argv + argc, argv + i);  // not an option of the game
        --argc;
        break;
    }
    // Scripted runs (--press, --clock virtual) start from fixed saves: a
    // restore from iCloud would replace them, and the upload thread adds
    // work nobody measures. LGP_CLOUD_SAVES=0 or --no-cloud-saves switch it
    // off for any run. The options count from settings.ini (args =) and the
    // command line, in the order app_arguments() hands them to the parser
    // (the later --clock wins), read here ahead of the restore.
    const auto cloud_wanted = [&] {
        if (const char* v = std::getenv("LGP_CLOUD_SAVES"); v != nullptr && std::strcmp(v, "0") == 0)
            return false;
        std::vector<std::string> run_args;
        boot::AppSettings peek;
        std::string ignored;
        if (std::filesystem::exists(files.settings, ec) && boot::load_app_settings(files.settings, &peek, &ignored))
            run_args = peek.extra_args;
        run_args.insert(run_args.end(), argv + std::min(argc, 2), argv + argc);  // no arguments: argc == 1
        bool virtual_clock = false;
        for (size_t i = 0; i < run_args.size(); ++i) {
            const std::string_view a = run_args[i];
            if (a == "--no-cloud-saves" || a == "--press" || a.rfind("--press=", 0) == 0)
                return false;
            if (a.rfind("--clock=", 0) == 0)
                virtual_clock = a == "--clock=virtual";
            else if (a == "--clock" && i + 1 < run_args.size())
                virtual_clock = run_args[++i] == "virtual";
        }
        return !virtual_clock;
    };
    if (!cloud_wanted()) {
        std::printf("cloud saves: off for this run\n");
    } else if (auto cloud = platform::make_cloud_save(LGP_CLOUD_CONTAINER)) {
        g_save_backup = std::make_unique<boot::SaveBackup>(std::move(cloud), files.data, LGP_CLOUD_SETTINGS_TAG);
        g_save_backup->restore();
        // The restore may bring settings.ini back (an emptied Library/Caches)
        // or replace it: its args may make this a scripted run after all (no
        // uploads then).
        if (!cloud_wanted()) {
            g_save_backup.reset();
            std::printf("cloud saves: off for this run (restored settings.ini)\n");
        }
    } else {
        std::printf("cloud saves: off (app not signed for %s)\n", LGP_CLOUD_CONTAINER);
    }
#endif
    boot::AppSettings settings;
    if (!boot::load_app_settings(files.settings, &settings, &error)) {
        app_message(platform::trf("The settings cannot be read:\n{}", {error}));
        return 3;
    }
    const bool system_settings = sync_system_settings(files, &settings);  // after the iCloud restore
    // Game files from elsewhere (Android: romfs.bin in the APK's splits,
    // GameBundle.java): "NAME=SPEC;NAME=SPEC", passed on as --game-file. They
    // belong to the game in <data>/game, not to a folder the user picked.
    std::vector<std::string> game_files;
    if (const char* v = std::getenv("LGP_APP_GAME_FILES"); v != nullptr && *v != '\0') {
        const std::string all = v;
        for (size_t pos = 0; pos <= all.size();) {
            const size_t end = std::min(all.find(';', pos), all.size());
            if (end > pos)
                game_files.push_back(all.substr(pos, end - pos));
            pos = end + 1;
        }
    }
    const bool romfs_elsewhere = std::any_of(game_files.begin(), game_files.end(),
                                             [](const std::string& f) { return f.rfind("romfs.bin=", 0) == 0; });
    const std::filesystem::path game = app_game_folder(settings, files, romfs_elsewhere);
    if (game.empty())
        return 0;
    std::filesystem::create_directories(files.pipeline_cache.parent_path(), ec);
    platform::exclude_from_backup(files.pipeline_cache.parent_path());
    const std::vector<std::string> more(argv + std::min(argc, 2), argv + argc);  // lgp_game --app [options]
    out->args = boot::app_arguments(settings, game, files, more);
    // The label and the split RomFS follow the folder that really runs
    // (args = or --app --game may override it).
    const std::filesystem::path running = boot::game_from_arguments(out->args);
    if (running == game && game == files.default_game)
        for (const std::string& f : game_files) {
            out->args.push_back("--game-file");
            out->args.push_back(f);
        }
    g_app_ui = AppUiContext{files, settings, {}, boot::game_source_text(running, files)};
    g_app_ui->system_settings = system_settings;
    std::printf("app: %s\n", g_app_ui->game_source.c_str());
    return -1;
}

#if LGP_BOOT_APP_LOG_ALWAYS
// iOS and Android apps have no console, and their working directory is "/".
// Started with plain options (adb shell am start … -e args "--exefs game/exefs …",
// devicectl … -- --frames 600), the output still goes to <data>/logs/lgp.log and
// relative paths name files below the app's folder, as in app mode.
// -1: run; otherwise the exit code.
int hosted_start(int argc, char** argv, std::filesystem::path* log) {
    std::string error;
    const auto dirs = platform::app_dirs(kAppName, &error);
    if (!dirs) {
        app_message(platform::trf("No folder for settings and saves: {}", {error}));
        return 3;
    }
    const boot::AppFiles files = boot::app_files(dirs->config, dirs->data, dirs->cache, {});
    std::error_code ec;
    std::filesystem::create_directories(files.log.parent_path(), ec);
    std::filesystem::rename(files.log, files.previous_log, ec);  // keep the run before
    if (platform::redirect_output(files.log, &error))
        *log = files.log;
    else
        std::fprintf(stderr, "lgp_boot: %s\n", error.c_str());
    std::string line = "app: arguments";
    for (int i = 1; i < argc; ++i)
        line += std::string(" ") + argv[i];
    std::printf("%s\napp: working directory %s\n", line.c_str(), boot::path_to_utf8(files.data).c_str());
    std::filesystem::current_path(files.data, ec);
    if (ec) {
        std::fprintf(stderr, "lgp_boot: cannot enter %s: %s\n", boot::path_to_utf8(files.data).c_str(),
                     ec.message().c_str());
        return 3;
    }
    return -1;
}
#endif
#endif

}  // namespace

namespace {

// Where the interface language came from, for the log ("LGP_UI_LANGUAGE=de",
// "device locale \"de-DE\"").
std::string g_ui_language_source;

// The app's own menus and dialogs (lgp/platform/ui_text.hpp) in the device's
// language: German on a German device, else English. LGP_UI_LANGUAGE=en|de
// chooses (tests, screenshots). Before the first dialog.
void choose_ui_language() {
    if (const char* v = std::getenv("LGP_UI_LANGUAGE"); v != nullptr && *v != '\0') {
        platform::set_ui_language(platform::ui_language_from_name(v));
        g_ui_language_source = std::string("LGP_UI_LANGUAGE=") + v;
        return;
    }
    std::string locale;
#if defined(LGP_BOOT_SDL)
    locale = platform::desktop::preferred_locale();
#endif
    platform::set_ui_language(platform::ui_language_for_locale(locale));
    g_ui_language_source = "device locale \"" + locale + "\"";
}

}  // namespace

#if LGP_GAME_PGO_GENERATE
// Instrumented build (LGP_GAME_PGO=generate): clang's profile runtime reads
// LLVM_PROFILE_FILE when the library loads and writes the profile in an exit
// handler. Android (and iOS/tvOS) apps set --env too late for the first and
// never run the second (Shield, 2026-10-28: empty folder, then a 0-byte
// file), so the boot program names the file and writes it itself.
extern "C" void __llvm_profile_set_filename(const char*);
extern "C" int __llvm_profile_write_file(void);
namespace {
void write_pgo_profile(const std::filesystem::path& fallback_dir) {
    static std::string file;  // the runtime keeps the pointer
    if (const char* v = std::getenv("LLVM_PROFILE_FILE"); v != nullptr && *v != '\0')
        file = v;
    else if (!fallback_dir.empty())
        file = boot::path_to_utf8(fallback_dir / "game-%p.profraw");
    if (!file.empty())
        __llvm_profile_set_filename(file.c_str());
    const int rc = __llvm_profile_write_file();
    std::fprintf(stderr, "pgo: profile written to %s, rc %d\n", file.empty() ? "default.profraw" : file.c_str(),
                 rc);
}
}  // namespace
#endif

// LGP_MAIN_NAME: the program is a shared library whose entry point the
// platform's glue calls (Android app, cmake/LgpAndroidApk.cmake).
// LGP_BOOT_SDL_MAIN (iOS): SDL owns main() and starts this from inside the
// app's run loop (platform/desktop/src/sdl_main.cpp).
#if defined(LGP_MAIN_NAME)
extern "C" int LGP_MAIN_NAME(int argc, char** argv) {
#elif defined(LGP_BOOT_SDL_MAIN)
extern "C" int lgp_boot_main(int argc, char** argv) {
#else
int main(int argc, char** argv) {
#endif
    // A leading "--" ends the launcher's own options (xcrun devicectl device
    // process launch … <bundle id> -- --app …); whether it is passed on
    // differs between tools, so it is dropped here.
    if (argc > 1 && std::strcmp(argv[1], "--") == 0) {
        argv[1] = argv[0];
        --argc;
        ++argv;
    }
    choose_ui_language();
#if defined(LGP_BOOT_APP)
    if (argc == 1 || std::strcmp(argv[1], "--app") == 0) {
        AppStart app;
        if (const int rc = app_start(argc, argv, &app); rc >= 0)
            return rc;
        std::vector<char*> args{argv[0]};
        for (std::string& a : app.args)
            args.push_back(a.data());
        args.push_back(nullptr);
        const int rc = run(int(args.size()) - 1, args.data());
#if LGP_GAME_PGO_GENERATE
        write_pgo_profile(app.log.empty() ? std::filesystem::path() : app.log.parent_path());
#endif
        // Only for a start by the user (no options after --app): a script's run
        // (am start -e args "--app --frames N") must end, not wait on a dialog.
        if (rc != 0 && !app.log.empty() && argc <= 2)
            app_message(platform::trf("The game ended with an error (code {}).\nLog: {}",
                                      {std::to_string(rc), boot::path_to_utf8(app.log)}));
#if LGP_BOOT_APP_LOG_ALWAYS
        // As below: scripts that start the app with --app wait for this line
        // (tools/android/shield_day.py).
        std::fprintf(stderr, "lgp_boot: exit code %d\n", rc);
#endif
        boot::finish_host();
        return rc;
    }
#endif
#if defined(LGP_BOOT_APP) && LGP_BOOT_APP_LOG_ALWAYS
    std::filesystem::path log;
    if (const int rc = hosted_start(argc, argv, &log); rc >= 0)
        return rc;
#endif
    const int rc = run(argc, argv);
#if LGP_GAME_PGO_GENERATE
    write_pgo_profile({});
#endif
#if defined(LGP_BOOT_APP) && LGP_BOOT_APP_LOG_ALWAYS
    // The log's last line says how the run ended (a usage error ends it within milliseconds).
    std::fprintf(stderr, "lgp_boot: exit code %d\n", rc);
#endif
    boot::finish_host();  // web: hand all output to the page before exit
    return rc;
}

namespace {

// Frames in flight per device kind (P7.4, CMake LGP_FRAMES_IN_FLIGHT_<KIND>):
// Apple TV piled up unthrottled command buffers as driver memory
// (IOAccelerator). The environment wins (--env LGP_WEBGPU_FRAMES_IN_FLIGHT=N,
// 0 = no limit); 0 here leaves the backend's default (no limit).
[[maybe_unused]] void apply_frames_in_flight_default(int frames, const char* kind) {
    if (const char* v = std::getenv("LGP_WEBGPU_FRAMES_IN_FLIGHT"); v != nullptr && *v != '\0')
        return;
    if (frames <= 0)
        return;
    platform::set_env("LGP_WEBGPU_FRAMES_IN_FLIGHT", std::to_string(frames));
    std::printf("frames in flight: %d (default on %s)\n", frames, kind);
}

// LGP_HOST_BLOCK_FILE_LATE=A@N (MiB, frame; docs/analysis/42): after the
// renderer showed frame N, the guest memory from block offset A MiB up to the
// file window moves into the file (HostBlock::extend_file_window). With
// LGP_HOST_BLOCK_FILE_RANGE=1024: and 768@600 the Apple TV loads its first
// scene with those pages anonymous (no stall there, Analyse 42) and still
// ends with the footprint of the window 768:. Only with --renderer window.
struct LateFileWindow {
    uint64_t begin = 0;  // block offset
    uint64_t frame = 0;
    bool pending = false;
    int tries = 0;
};
// A guest thread that does not reach a pause point within the second
// (Shield, 2026-10-29: "a guest thread did not stop" at frame 600) gets
// another chance this many frames later, up to kLateFileWindowTries times.
constexpr int kLateFileWindowTries = 5;
constexpr uint64_t kLateFileWindowRetryFrames = 120;

LateFileWindow parse_late_file_window() {
    LateFileWindow late;
    const char* v = std::getenv("LGP_HOST_BLOCK_FILE_LATE");
    if (v == nullptr || *v == '\0' || std::strcmp(v, "0") == 0)
        return late;
    char* stop = nullptr;
    const unsigned long long mib = std::strtoull(v, &stop, 10);
    const char* at = stop;
    unsigned long long frame = 0;
    if (at != v && *at == '@' && at[1] >= '0' && at[1] <= '9') {
        frame = std::strtoull(at + 1, &stop, 10);
        if (*stop == '\0' && mib < (1ull << 32)) {
            late.begin = uint64_t(mib) << 20;
            late.frame = frame;
            late.pending = true;
            return late;
        }
    }
    std::fprintf(stderr, "host block: LGP_HOST_BLOCK_FILE_LATE=%s is not MIB@FRAME, ignored\n", v);
    return late;
}

// Metal libraries compiled offline (tools/apple/build_metallibs.sh,
// docs/analysis/46-metal-shader-vorkompilieren.md): <game>/metal-<kind>/
// becomes LGP_WGPU_METAL_LIBS unless the environment sets it (empty: off).
// Only wgpu-native with tools/wgpu/metal-precompiled.patch reads it, at the
// first pipeline, so this runs before the renderer exists.
[[maybe_unused]] void apply_metal_libs_default(const std::string& game_dir, const char* kind) {
    if (std::getenv("LGP_WGPU_METAL_LIBS") != nullptr || game_dir.empty())
        return;
    const std::string dir = game_dir + "/metal-" + kind;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec))
        return;
    platform::set_env("LGP_WGPU_METAL_LIBS", dir);
    std::printf("metal libraries: %s\n", dir.c_str());
}

// The <kind> for apply_metal_libs_default(): the device's own with SDL (iOS,
// tvOS), else desktop.
[[maybe_unused]] const char* metal_libs_kind() {
#if defined(LGP_BOOT_SDL)
    return platform::desktop::device_kind_name(platform::desktop::device_kind());
#else
    return "desktop";
#endif
}

#if defined(LGP_BOOT_SDL)
// The window renderer's device kind (Android TV only known at run time).
void apply_frames_in_flight_default() {
    const platform::desktop::DeviceKind kind = platform::desktop::device_kind();
    int frames = LGP_BOOT_FIF_DESKTOP;
    switch (kind) {
    case platform::desktop::DeviceKind::Ios: frames = LGP_BOOT_FIF_IOS; break;
    case platform::desktop::DeviceKind::AppleTv: frames = LGP_BOOT_FIF_APPLE_TV; break;
    case platform::desktop::DeviceKind::Android: frames = LGP_BOOT_FIF_ANDROID; break;
    case platform::desktop::DeviceKind::AndroidTv: frames = LGP_BOOT_FIF_ANDROID_TV; break;
    case platform::desktop::DeviceKind::Desktop: break;
    }
    apply_frames_in_flight_default(frames, platform::desktop::device_kind_name(kind));
}

// Apple TV: the first GiB stays anonymous while the first scene loads, then
// MiB 768-1024 move into the file (LGP_HOST_BLOCK_FILE_LATE). With 768: from
// the start the load stood 7-11 s in 5 of 6 runs; with this 0 of 3, the
// footprint after frame 600 as with 768: (docs/analysis/42). Either variable
// set keeps both as they are, so LGP_HOST_BLOCK_FILE_LATE=0 gives the
// window 768: from the start as before.
void apply_late_file_window_default() {
    if (platform::desktop::device_kind() != platform::desktop::DeviceKind::AppleTv)
        return;
    for (const char* name : {"LGP_HOST_BLOCK_FILE_RANGE", "LGP_HOST_BLOCK_FILE_LATE"})
        if (const char* v = std::getenv(name); v != nullptr && *v != '\0')
            return;
    platform::set_env("LGP_HOST_BLOCK_FILE_RANGE", "1024:");
    platform::set_env("LGP_HOST_BLOCK_FILE_LATE", "768@600");
    std::printf("host block: file window 1024: until frame 600, then 768: (default on apple-tv)\n");
}
#endif

#if defined(LGP_BOOT_APP) && defined(LGP_BOOT_UI)
namespace ui = platform::ui;

// Save slots for the start screen; creates slot 1 if there is none.
std::vector<boot::SaveSlot> app_slots(const boot::AppFiles& files) {
    std::vector<boot::SaveSlot> slots = boot::list_save_slots(files.data);
    if (slots.empty()) {
        std::string error;
        boot::create_save_slot(files.data, &error);
        slots = boot::list_save_slots(files.data);
    }
    return slots;
}

std::vector<ui::MenuSlot> menu_slots(const std::vector<boot::SaveSlot>& slots) {
    std::vector<ui::MenuSlot> out;
    for (const boot::SaveSlot& slot : slots) {
        ui::MenuSlot m;
        m.name = platform::trf("Save {}", {std::to_string(slot.number)});
        if (!slot.has_data) {
            m.detail = platform::tr("empty");
            m.empty = true;
        } else {
            // file_clock -> system_clock without clock_cast (not everywhere yet).
            const auto age = std::filesystem::file_time_type::clock::now() - slot.last_write;
            const std::time_t t = std::chrono::system_clock::to_time_t(
                std::chrono::system_clock::now() -
                std::chrono::duration_cast<std::chrono::system_clock::duration>(age));
            std::tm tm{};
#if defined(_MSC_VER)
            localtime_s(&tm, &t);
#else
            localtime_r(&t, &tm);
#endif
            char buf[48];
            std::strftime(buf, sizeof(buf), platform::tr("last played on %b %d at %H:%M"), &tm);
            m.detail = buf;
        }
        out.push_back(std::move(m));
    }
    return out;
}

// Index of the slot whose folder below data is `folder`, or -1.
int slot_folder_index(const std::filesystem::path& data, const std::vector<boot::SaveSlot>& slots,
                      const std::filesystem::path& folder) {
    std::error_code ec;
    const std::filesystem::path want = std::filesystem::weakly_canonical(folder, ec);
    for (size_t i = 0; i < slots.size(); ++i) {
        std::error_code ec2;
        if (std::filesystem::weakly_canonical(data / slots[i].dir, ec2) == want && !ec && !ec2)
            return int(i);
    }
    return -1;
}

ui::MenuSlot custom_menu_slot(const std::filesystem::path& folder) {
    ui::MenuSlot m;
    m.name = platform::tr("Own folder");
    m.detail = boot::path_to_utf8(folder);
    std::error_code ec;
    m.empty = !std::filesystem::is_directory(folder, ec) || std::filesystem::is_empty(folder, ec);
    return m;
}

int slot_index(const std::vector<boot::SaveSlot>& slots, const boot::AppSettings& settings) {
    const int number = boot::current_save_slot(settings);
    for (size_t i = 0; i < slots.size(); ++i)
        if (slots[i].number == number)
            return int(i);
    return 0;
}

// The font of the game's menus (OpenType fonts wrapped as BFOTF), path in
// its RomFS: RodinNTLG for Latin text, else the narrow UD Kakugo C80.
constexpr const char* kGameMenuFonts[] = {"/bin/font/FOT-RodinNTLGPro-DB.BFOTF",
                                          "/bin/font/FOT-UDKakugoC80Pro-DB.BFOTF"};
// The layout archives of the game's options menu (rows, background), read
// at run time for the app's menus (platform/ui/layout_skin.hpp); the
// optional ones add the choice window (its band, buttons and the context
// menu).
constexpr const char* kGameMenuArchives[] = {"/bin/app/setting/bin/setting_list_00.arc",
                                             "/bin/app/setting/bin/setting_bg_00.arc"};
constexpr const char* kGameMenuOptionalArchives[] = {"/bin/app/system_window/bin/system_window_button_00.arc",
                                                     "/bin/app/context_menu/bin/context_menu.arc"};

// A menu archive from the RomFS (under /bin like the fonts); also without
// the /bin, for a dump unpacked from inside it.
bool read_menu_file(const boot::BootOptions& opt, std::string_view path, std::vector<uint8_t>* out,
                    std::string* error) {
    if (boot::read_game_file(opt, path, out, error))
        return true;
    std::string inner_error;
    return path.rfind("/bin/", 0) == 0 && boot::read_game_file(opt, path.substr(4), out, &inner_error);
}

// The game's menu parts from its RomFS; nullptr (and a line why) without
// them, or with LGP_MENU_SKIN=0.
std::shared_ptr<ui::LayoutSkin> load_menu_skin(const boot::BootOptions& opt) {
    if (const char* env = std::getenv("LGP_MENU_SKIN"); env != nullptr && std::string(env) == "0") {
        std::printf("menu: own look (LGP_MENU_SKIN=0)\n");
        return nullptr;
    }
    auto skin = std::make_shared<ui::LayoutSkin>();
    for (const char* path : kGameMenuArchives) {
        std::vector<uint8_t> bytes;
        std::string error;
        std::vector<std::string> warnings;
        std::string stem = path;
        stem = stem.substr(stem.find_last_of('/') + 1);
        stem = stem.substr(0, stem.find('.'));
        if (!read_menu_file(opt, path, &bytes, &error)) {
            std::printf("menu: own look (%s: %s)\n", path, error.c_str());
            return nullptr;
        }
        auto archive = ui::load_skin_archive(stem, bytes, &error, &warnings);
        for (const std::string& w : warnings)
            std::printf("menu: %s\n", w.c_str());
        if (!archive) {
            std::printf("menu: own look (%s: %s)\n", path, error.c_str());
            return nullptr;
        }
        skin->archives.push_back(std::move(*archive));
    }
    for (const char* path : kGameMenuOptionalArchives) {
        std::vector<uint8_t> bytes;
        std::string error;
        std::vector<std::string> warnings;
        std::string stem = path;
        stem = stem.substr(stem.find_last_of('/') + 1);
        stem = stem.substr(0, stem.find('.'));
        if (!read_menu_file(opt, path, &bytes, &error)) {
            std::printf("menu: without %s (%s)\n", path, error.c_str());
            continue;
        }
        auto archive = ui::load_skin_archive(stem, bytes, &error, &warnings);
        for (const std::string& w : warnings)
            std::printf("menu: %s\n", w.c_str());
        if (archive)
            skin->archives.push_back(std::move(*archive));
        else
            std::printf("menu: without %s (%s)\n", path, error.c_str());
    }
    std::printf("menu: game parts\n");
    return skin;
}

// The menu's render scale choices (ui::MenuSettings::render_scale): 100 %, 75 %, 50 %.
constexpr float kMenuRenderScales[] = {1.0f, 0.75f, 0.5f};
constexpr const char* kMenuRenderScaleNames[] = {"1", "0.75", "0.5"};
int menu_render_scale(float scale) { return scale >= 0.875f ? 0 : scale >= 0.625f ? 1 : 2; }
// The app's mod "Pokémon Gelb" (random-encounters, one switch): Gelb's chance
// per step in each area (the mod file's zone_rate lines; with its slot lines
// Gelb mode, battles straight from Gelb's slots) and no field Pokémon visible.
// The rows of older builds (settings.ini encounter_rate, visible_pokemon) are
// gone; boot::load_app_settings migrates them. With Pokémon Gelb off, "Hide
// field Pokémon" (Let's Go only, Paul 2026-11-02) runs the same mod hiding
// only: no die, no Gelb battles, the hidden ones touchable.
constexpr const char* kRandomEncountersId = "random-encounters";
void apply_random_encounters(bool gelb_on, bool hide_only) {
    mods::set_random_encounter_rate(mods::kRandomEncounterOriginal);
    mods::set_random_encounter_hidden_percent(100);
    mods::set_random_encounter_hide_only(hide_only);
    std::printf(hide_only ? "app: Let's Go with the field Pokémon hidden (touchable, Pokémon Gelb off)\n"
                : gelb_on ? "app: Pokémon Gelb at Gelb's rate per area, no field Pokémon visible\n"
                          : "app: Pokémon Gelb off, the field Pokémon as the game has them\n");
}

// Pokémon Gelb is on in the menu (random-encounters listed and on).
bool menu_gelb_on(const ui::MenuSettings& m) {
    for (const ui::MenuMod& mod : m.mods)
        if (mod.id == kRandomEncountersId)
            return mod.on;
    return false;
}

// "Hide field Pokémon" applies: on, Pokémon Gelb listed (its mod file is there) and off.
bool menu_hide_only(const ui::MenuSettings& m) {
    if (!m.hide_field_pokemon)
        return false;
    for (const ui::MenuMod& mod : m.mods)
        if (mod.id == kRandomEncountersId)
            return !mod.on;
    return false;
}

ui::MenuSettings menu_settings(const boot::AppSettings& s, bool touch_default) {
    ui::MenuSettings m;
    m.touch = s.touch.empty() ? touch_default : s.touch == "1";
    m.touch_size = s.touch_size;
    m.touch_opacity = s.touch_opacity;
    m.touch_mirrored = s.touch_mirrored;
    m.touch_throw = s.touch_throw;
    m.touch_throw_pad = s.touch_throw_pad;
    m.volume = s.volume;
    m.docked = s.docked == "1";
    m.controller = s.controller == "joycon-right" ? 1 : s.controller == "joycon-left" ? 2 : 0;
    m.player1_by_press = s.player1 != "order";
    m.aspect = s.aspect == "fill" ? 1 : s.aspect == "stretch" ? 2 : 0;
    m.effects_half = s.effects == "half";
    m.render_scale = menu_render_scale(s.render_scale.empty() ? 1.0f : std::strtof(s.render_scale.c_str(), nullptr));
    m.async_execute = s.async_execute == "1";
    m.smoothing = s.smoothing == "fxaa" ? 2 : s.smoothing == "linear" ? 1 : 0;
    // The menu's screen moves opposite sides alike: top for the height, right for the width.
    if (std::array<int, 4> sides{}; boot::parse_hud_sides(s.hud_sides, &sides)) {
        m.hud_height = sides[0];
        m.hud_width = sides[3];
    }
    m.ldn_lan = s.ldn != "off";
    m.ldn_server = boot::ldn_server_of(s);
    m.ldn_room = s.ldn_room;
    for (const mods::ModInfo& info : mods::known_mods())
        if (!info.hidden)
            m.mods.push_back({info.id, platform::tr(info.title), platform::tr(info.description),
                              std::find(s.mods.begin(), s.mods.end(), info.id) != s.mods.end(), info.live});
    m.language = 0;
    for (int i = 0; i < 12; ++i)
        if (s.language == boot::kGameLanguages[i])
            m.language = i + 1;
    m.start_screen = s.start_screen;
    m.controller_screen = s.controller_screen;
    m.hide_field_pokemon = s.hide_field_pokemon;
    return m;
}

// The battle's HUD bounds from the menu's screen as --hud-sides T,B,L,R.
std::string menu_hud_sides(const ui::MenuSettings& m) {
    const std::string h = std::to_string(std::clamp(m.hud_height, 0, 100));
    const std::string w = std::to_string(std::clamp(m.hud_width, 0, 100));
    return h + "," + h + "," + w + "," + w;
}

// The ids of the mods switched on in the menu.
std::vector<std::string> menu_mods_on(const ui::MenuSettings& m) {
    std::vector<std::string> ids;
    for (const ui::MenuMod& mod : m.mods)
        if (mod.on || (mod.id == kRandomEncountersId && menu_hide_only(m)))
            ids.push_back(mod.id);
    return ids;
}

// The menu shows the values this run uses (settings.ini plus command-line
// options); only what the user changed in it goes back to settings.ini, so a
// one-off option such as --volume 0 is not stored.
ui::MenuSettings g_menu_seen;
// The menu shows a resolution that is not one of its rows (--render-scale 0.25)
// as the nearest row, so comparing rows cannot tell whether the user picked it.
bool g_render_scale_edited = false;

void store_menu_changes(const ui::MenuSettings& m, boot::AppSettings* s) {
    const ui::MenuSettings& was = g_menu_seen;
    if (m.touch != was.touch) s->touch = m.touch ? "1" : "0";
    if (m.touch_size != was.touch_size) s->touch_size = m.touch_size;
    if (m.touch_opacity != was.touch_opacity) s->touch_opacity = m.touch_opacity;
    if (m.touch_mirrored != was.touch_mirrored) s->touch_mirrored = m.touch_mirrored;
    if (m.touch_throw != was.touch_throw) s->touch_throw = m.touch_throw;
    if (m.touch_throw_pad != was.touch_throw_pad) s->touch_throw_pad = m.touch_throw_pad;
    if (m.volume != was.volume) s->volume = m.volume;
    if (m.docked != was.docked) s->docked = m.docked ? "1" : "0";
    if (m.controller != was.controller)
        s->controller = m.controller == 1 ? "joycon-right" : m.controller == 2 ? "joycon-left" : "handheld";
    if (m.player1_by_press != was.player1_by_press) s->player1 = m.player1_by_press ? "press" : "order";
    if (m.aspect != was.aspect)
        s->aspect = m.aspect == 1 ? "fill" : m.aspect == 2 ? "stretch" : "fit";
    if (m.effects_half != was.effects_half) s->effects = m.effects_half ? "half" : "full";
    if (m.render_scale != was.render_scale) {
        s->render_scale = kMenuRenderScaleNames[std::clamp(m.render_scale, 0, 2)];
        g_render_scale_edited = true;
    }
    if (m.async_execute != was.async_execute) s->async_execute = m.async_execute ? "1" : "0";
    if (m.smoothing != was.smoothing) s->smoothing = m.smoothing == 2 ? "fxaa" : m.smoothing == 1 ? "linear" : "off";
    if (m.hud_height != was.hud_height || m.hud_width != was.hud_width)
        s->hud_sides = menu_hud_sides(m);
    if (m.ldn_lan != was.ldn_lan) s->ldn = m.ldn_lan ? "lan" : "off";
    // The build's server is stored as "" (a later build may bring another).
    if (m.ldn_server != was.ldn_server) s->ldn_server = m.ldn_server == boot::default_ldn_server() ? "" : m.ldn_server;
    if (m.ldn_room != was.ldn_room) s->ldn_room = m.ldn_room;
    for (size_t i = 0; i < m.mods.size() && i < was.mods.size(); ++i) {
        if (m.mods[i].on == was.mods[i].on)
            continue;
        auto it = std::find(s->mods.begin(), s->mods.end(), m.mods[i].id);
        if (m.mods[i].on && it == s->mods.end())
            s->mods.push_back(m.mods[i].id);
        else if (!m.mods[i].on && it != s->mods.end())
            s->mods.erase(it);
    }
    if (m.language != was.language)
        s->language = m.language >= 1 && m.language <= 12 ? boot::kGameLanguages[m.language - 1] : "";
    if (m.start_screen != was.start_screen) s->start_screen = m.start_screen;
    if (m.controller_screen != was.controller_screen) s->controller_screen = m.controller_screen;
    if (m.hide_field_pokemon != was.hide_field_pokemon) s->hide_field_pokemon = m.hide_field_pokemon;
    g_menu_seen = m;
}

// Before the game: creates the cached programs (pipeline cache and seed) and
// waits until their pipelines are built, so the game does not stutter on
// them. Shows Screen::Loading with progress once that takes longer than
// LGP_PREPARE_SCREEN_MS (default 300; a start with warm driver caches is
// usually done before); "Start now" leaves the rest to the background.
// Default on phones, tablets and Android TV only: on the PC (RTX 5070 Ti,
// Vulkan, seed v2) the game built no draw pipeline during play without it
// either, and the screen cost 6-9 s (2026-10-26). Off on the Apple TV until
// measured: it keeps every seed pipeline alive below a ~2 GiB jetsam limit.
// LGP_PREPARE_GRAPHICS=0/1 decides. False: the window was closed.
bool prepare_graphics(ui::AppMenu& menu, platform::desktop::SdlWindow& window, gpu::webgpu::WebGpuBackend& backend) {
    const platform::desktop::DeviceKind kind = platform::desktop::device_kind();
    bool on = kind != platform::desktop::DeviceKind::Desktop && kind != platform::desktop::DeviceKind::AppleTv;
    const char* why = on ? "default for phones and tablets" : "default for this device";
    if (const char* v = std::getenv("LGP_PREPARE_GRAPHICS"); v != nullptr && *v != '\0') {
        on = std::string(v) != "0";
        why = "LGP_PREPARE_GRAPHICS";
    }
    if (!on) {
        std::printf("app: graphics not prepared ahead (%s)\n", why);
        return true;
    }
    long screen_ms = 300;
    if (const char* v = std::getenv("LGP_PREPARE_SCREEN_MS"); v != nullptr && *v != '\0')
        screen_ms = std::max(0L, std::strtol(v, nullptr, 10));
    const auto t0 = std::chrono::steady_clock::now();
    const auto elapsed_ms = [&] {
        return long(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
    };
    bool shown = false, skipped = false;
    size_t programs_left = 0;
    gpu::webgpu::WebGpuBackend::PrebuildProgress p;
    for (;;) {
        // Slices, so the screen keeps drawing while programs are created.
        programs_left = backend.warm_cached_programs(30'000);
        p = backend.prebuild_progress();
        if (programs_left == 0 && p.done >= p.total)
            break;
        if (!shown && elapsed_ms() >= screen_ms) {
            menu.show(ui::Screen::Loading);
            shown = true;
        }
        if (shown) {
            menu.set_progress(p.done, p.total + programs_left);
            const ui::Action action = menu.frame();
            window.renderer().set_ui(menu.draw_data());
            window.renderer().redraw();
            if (action == ui::Action::Play) {
                skipped = true;
                break;
            }
        }
        if (!window.pump_events(shown ? 16 : 5))
            return false;
    }
    if (shown) {
        menu.show(ui::Screen::None);
        window.renderer().set_ui(nullptr);
    }
    std::printf("app: graphics prepared: %zu of %zu pipelines and programs in %ld ms (%s)\n", p.done,
                p.total + programs_left, elapsed_ms(),
                skipped ? "started early, the rest in the background" : shown ? "screen shown" : "no screen");
    std::fflush(stdout);
    return true;
}

// --menu-script: menu actions for tests and scripted runs, comma-separated,
// each run on the next menu frame or, with @N, once N frames were shown in
// the game: frames the game presented plus menu frames drawn while it stands
// paused (so a step after "pause" still comes due): play, newslot, pause,
// backup, resume, quit, pickplayer1, settings (opens the settings screen) and slot<N>
// (taps the N-th entry of the slot list, counted from 0) (e.g.
// "play,pause@3,backup,resume@60,quit@100").
struct MenuStep {
    ui::Action action;
    uint64_t frame;
    ui::Screen screen = ui::Screen::None;  // "settings": opens that screen instead of an action
    int slot = -1;                         // "slot<N>": taps that slot instead
    int docked = -1;                       // "docked"/"handheld": changes the TV mode setting instead
    std::string mod;                       // "mod:<id>=on|off": switches a game mod in the settings instead
    int mod_on = -1;
    int controller = -1;                   // "controller:handheld|joycon-right|joycon-left": that row instead
};

void run_menu_step(ui::AppMenu& menu, const MenuStep& step) {
    if (step.docked >= 0) {
        menu.settings().docked = step.docked == 1;
        menu.press(ui::Action::SettingsChanged);
    } else if (step.mod_on >= 0) {
        bool found = false;
        for (ui::MenuMod& m : menu.settings().mods)
            if (m.id == step.mod) {
                m.on = step.mod_on == 1;
                found = true;
            }
        if (!found)
            std::printf("menu: no mod %s in the settings\n", step.mod.c_str());
        else
            std::printf("menu: script switches mod %s %s\n", step.mod.c_str(), step.mod_on == 1 ? "on" : "off");
        menu.press(ui::Action::SettingsChanged);
    } else if (step.controller >= 0) {
        menu.settings().controller = step.controller;
        std::printf("menu: script sets controller %d\n", step.controller);
        menu.press(ui::Action::SettingsChanged);
    } else if (step.slot >= 0)
        menu.choose_slot(step.slot);
    else if (step.screen != ui::Screen::None)
        menu.show(step.screen);
    else
        menu.press(step.action);
}
// A step due at a game frame (@N, N > 0) holds the game at its N-th present
// until the host loop took that step: the guest thread pauses the process and
// stands there (MenuScript::hold_at), and the host lets it go once the step
// ran (release). So "pause@2" pauses after the game's second frame however
// late the host loop comes round (tests; without a script nothing waits).
struct MenuGate {
    std::mutex mutex;
    uint64_t next = 0;  // frame of the next step; 0: none at a frame
    bool held = false;  // the guest holds a pause of its own (ProcessControl)
    std::atomic<uint64_t> presented{0};  // the game's presents so far
    boot::ProcessControl* control = nullptr;
};

struct MenuScript {
    std::deque<MenuStep> steps;
    uint64_t paused_frames = 0;  // menu frames shown while the game stood paused
    std::shared_ptr<MenuGate> gate = std::make_shared<MenuGate>();
    bool empty() const { return steps.empty(); }
    const MenuStep& front() const { return steps.front(); }
    void pop_front() {
        steps.pop_front();
        set_next();
    }
    void push_back(const MenuStep& step) {
        steps.push_back(step);
        set_next();
    }
    void set_next() {
        std::lock_guard lock(gate->mutex);
        gate->next = steps.empty() ? 0 : steps.front().frame;
    }
    // On the guest thread after present n.
    static void hold_at(MenuGate& g, uint64_t n) {
        g.presented.store(n, std::memory_order_release);
        // Presents come from guest threads (present_thunk); without one nobody
        // would stand at the gate while the pause stays taken.
        rt::KThread* self = rt::Scheduler::current();
        assert(self != nullptr);
        if (self == nullptr)
            return;
        {
            std::lock_guard lock(g.mutex);
            if (g.control == nullptr || g.next == 0 || n < g.next || g.held || g.control->closing())
                return;
            g.control->pause(0);  // the calling thread still runs: never at rest here
            g.held = true;
        }
        rt::active_kernel().scheduler().pause_point(*self);  // stands here until the pause ends
    }
    // On the host after a step ran (its own pause, if any, is taken by then).
    void release() {
        std::lock_guard lock(gate->mutex);
        if (!gate->held)
            return;
        gate->held = false;
        gate->control->resume();
    }
};

bool parse_menu_script(const std::string& spec, MenuScript* out) {
    static const std::pair<const char*, ui::Action> kNames[] = {
        {"play", ui::Action::Play},     {"newslot", ui::Action::NewSlot}, {"pause", ui::Action::OpenPause},
        {"backup", ui::Action::Backup}, {"resume", ui::Action::Resume},   {"quit", ui::Action::QuitGame},
        {"pickplayer1", ui::Action::PickPlayer1},
    };
    std::stringstream in(spec);
    for (std::string step; std::getline(in, step, ',');) {
        const size_t at = step.find('@');
        const std::string name = step.substr(0, at);
        MenuStep m{ui::Action::None, 0};
        for (const auto& [n, a] : kNames)
            if (name == n)
                m.action = a;
        if (name == "settings")
            m.screen = ui::Screen::Settings;
        if (name == "docked" || name == "handheld")
            m.docked = name == "docked" ? 1 : 0;
        if (name.compare(0, 4, "mod:") == 0) {  // mod:<id>=on|off
            const size_t eq = name.find('=');
            if (eq == std::string::npos || eq == 4 || (name.substr(eq + 1) != "on" && name.substr(eq + 1) != "off"))
                return false;
            m.mod = name.substr(4, eq - 4);
            m.mod_on = name.substr(eq + 1) == "on" ? 1 : 0;
        }
        if (name == "controller:handheld" || name == "controller:joycon-right" || name == "controller:joycon-left")
            m.controller = name == "controller:joycon-right" ? 1 : name == "controller:joycon-left" ? 2 : 0;
        if (name.size() > 4 && name.compare(0, 4, "slot") == 0 &&
            name.find_first_not_of("0123456789", 4) == std::string::npos)
            m.slot = std::atoi(name.c_str() + 4);
        if (m.action == ui::Action::None && m.screen == ui::Screen::None && m.slot < 0 && m.docked < 0 &&
            m.mod_on < 0 && m.controller < 0)
            return false;
        if (at != std::string::npos) {
            char* end = nullptr;
            m.frame = std::strtoull(step.c_str() + at + 1, &end, 10);
            if (end == step.c_str() + at + 1 || *end != '\0')
                return false;
        }
        out->push_back(m);
    }
    return true;
}

// The app's page in the Settings app (iOS: "app-settings:", as
// UIApplicationOpenSettingsURLString; the menu offers it only there).
void open_system_settings() {
#if defined(LGP_BOOT_SDL)
    std::string error;
    if (!platform::desktop::open_app_settings(&error))
        std::printf("menu: Settings app not opened (%s)\n", error.c_str());
#endif
}

const char* action_name(ui::Action a) {
    switch (a) {
    case ui::Action::Play: return "play";
    case ui::Action::NewSlot: return "new slot";
    case ui::Action::OpenPause: return "pause";
    case ui::Action::Resume: return "resume";
    case ui::Action::Backup: return "backup";
    case ui::Action::QuitGame: return "quit game";
    case ui::Action::Quit: return "quit";
    case ui::Action::SettingsChanged: return "settings changed";
    case ui::Action::PickPlayer1: return "pick player 1";
    case ui::Action::LoadCloudSaves: return "load cloud saves";
    case ui::Action::OpenSystemSettings: return "open system settings";
    case ui::Action::None: break;
    }
    return "none";
}

void save_settings(const AppUiContext& app) {
    std::string error;
    if (!boot::save_app_settings(app.files.settings, app.settings, &error)) {
        std::fprintf(stderr, "lgp_boot: %s\n", error.c_str());
        return;
    }
#if defined(LGP_CLOUD_CONTAINER)
    // settings.ini is in the backup too. Packed without a save's lock: a
    // guest commit racing it packs again and replaces this upload.
    if (g_save_backup)
        g_save_backup->committed();
#endif
}

#if defined(LGP_BOOT_SDL)
// The pause menu's hold on the game: every guest thread stands in the
// scheduler (boot::ProcessControl, the same pause the app takes in the
// background; pauses nest). Without a control the game stands at its next
// present instead (WebGpuRenderer::set_paused).
struct MenuPause {
    boot::ProcessControl* control = nullptr;
    std::function<void(bool)> on_hold;  // the sound output: no underruns counted while held
    std::function<bool(int64_t)> drain;  // asynchronous execution: what the guest queued before it stopped
    const std::atomic<uint64_t>* presented = nullptr;  // with --menu-script: the game's presents
    bool held = false;
    bool at_rest = false;  // every guest thread stood in the scheduler (no save commit under way)

    void hold(platform::webgpu::WebGpuRenderer& renderer) {
        if (held)
            return;
        held = true;
        if (on_hold)
            on_hold(true);
        if (control == nullptr) {
            renderer.set_paused(true);
            return;
        }
        const auto t0 = std::chrono::steady_clock::now();
        at_rest = control->pause(500);
        // The menu draws over the game's last frame, as in the background; a
        // stuck worker must not freeze the window thread with it.
        const bool drained = !at_rest || !drain || drain(kAsyncDrainTimeoutMs);
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        std::printf("menu: game paused in %lld ms%s%s\n", static_cast<long long>(ms),
                    at_rest ? "" : " (a guest thread did not stop yet)",
                    drained ? "" : " (the GPU queue did not empty, the menu shows an older frame)");
        if (presented != nullptr)
            std::printf("menu: game stands at frame %llu, %llu shown\n",
                        static_cast<unsigned long long>(presented->load(std::memory_order_acquire)),
                        static_cast<unsigned long long>(renderer.frames()));
    }
    // Before copying the save folder: true once no guest thread runs any more
    // (one still inside an svc, e.g. a save commit in fsp-srv, keeps it false).
    // Without a control only the presenting thread stands: never.
    bool settle() {
        if (!held || control == nullptr)
            return false;
        if (!at_rest) {
            at_rest = control->pause(300);  // nested: the resume() below keeps the menu's pause
            control->resume();
        }
        return at_rest;
    }
    void release(platform::webgpu::WebGpuRenderer& renderer) {
        if (!held)
            return;
        held = false;
        at_rest = false;
        if (control == nullptr)
            renderer.set_paused(false);
        else
            control->resume();
        if (on_hold)
            on_hold(false);
    }
};

// One turn of the host loop while the game runs: the pause button, and while
// paused the menu.
// The game's software keyboard drawn by the menu (SdlTextInput::host_drawn):
// a new request opens Screen::Text.
ui::TextPrompt menu_prompt(const platform::desktop::SdlTextInput::Prompt& p) {
    ui::TextPrompt t;
    if (p.message) {
        t.message = true;
        t.header = p.message_request.code;
        t.sub = p.message_request.text;
        return t;
    }
    t.header = p.request.header;
    t.sub = p.request.sub;
    t.guide = p.request.guide;
    t.initial = p.request.initial;
    t.note = p.note;
    t.max_length = p.request.max_length;
    t.password = p.request.password;
    return t;
}

void in_game_menu(ui::AppMenu& menu, platform::desktop::SdlWindow& window,
                  const std::function<void(const ui::MenuSettings&)>& apply_settings,
                  std::atomic<bool>& stop_requested, MenuScript& script, MenuPause& pause,
                  platform::desktop::SdlTextInput* text, uint64_t& text_serial) {
    if (text != nullptr && text->host_drawn()) {
        platform::desktop::SdlTextInput::Prompt p;
        // Also when the text screen was left without an answer: the game still waits.
        if (text->prompt(&p) && (p.serial != text_serial || menu.screen() == ui::Screen::None)) {
            text_serial = p.serial;
            menu.show_text(menu_prompt(p));
            std::printf("menu: %s\n", p.message ? "message" : "text request");
        }
    }
    auto& renderer = window.renderer();
    bool stepped = false;
    // @N counts the game's frames; while the menu holds the game, the menu's
    // frames on top (else no step after a pause would come).
    const uint64_t shown = pause.held ? renderer.frames() + script.paused_frames : renderer.frames();
    if (!script.empty() && shown >= script.front().frame) {
        run_menu_step(menu, script.front());
        script.pop_front();
        stepped = true;
    }
    // The game held for that step (MenuGate) goes on once it ran, after a pause it took.
    struct GateRelease {
        MenuScript& script;
        bool stepped;
        ~GateRelease() {
            if (stepped)
                script.release();
        }
    } gate_release{script, stepped};
    if (!menu.wants_frames())
        return;
    AppUiContext& app = *g_app_ui;
    if (menu.screen() == ui::Screen::Settings)
        menu.set_player1_name(app.player1_name ? app.player1_name() : std::string());
    // The HUD bounds screen draws over the widescreen mod's picture (16:9 without it).
    if (const mods::PictureSize p = mods::widescreen_active(); p.width > 0 && p.height > 0)
        menu.set_picture_aspect(float(p.width) / float(p.height));
    else
        menu.set_picture_aspect(16.0f / 9.0f);
    menu.set_docked_next_start(mods::active("widescreen"));
    const ui::Action action = menu.frame();
    if (action != ui::Action::None)
        std::printf("menu: %s\n", action_name(action));
    switch (action) {
    case ui::Action::OpenPause:
        pause.hold(renderer);
        menu.frame();  // the pause screen right away
        break;
    case ui::Action::Resume:
        menu.frame();  // back to the pause button alone
        renderer.set_ui(menu.draw_data());
        pause.release(renderer);
        return;
    case ui::Action::Backup: {
        if (!pause.settle()) {
            menu.set_status(platform::tr("The game is still busy, please back up again in a moment."));
            std::printf("menu: backup skipped, the game is not at rest\n");
            break;
        }
        std::string error;
        const std::filesystem::path to = boot::backup_save_slot(app.files.data, app.save_dir.empty() ? "saves" : app.save_dir, &error);
        menu.set_status(to.empty() ? platform::trf("Backup failed: {}", {error})
                                   : platform::trf("Backed up to {}",
                                                   {boot::path_to_utf8(to.lexically_relative(app.files.data))}));
        std::printf("menu: backup %s\n", to.empty() ? error.c_str() : boot::path_to_utf8(to).c_str());
        break;
    }
    case ui::Action::QuitGame:
        stop_requested = true;
        renderer.set_ui(nullptr);
        pause.release(renderer);   // the game must run to reach its end
        renderer.request_close();  // also lets a game thread waiting at its present go
        return;
    case ui::Action::TextOk:
    case ui::Action::TextCancel:
        if (text != nullptr) {
            std::string note;
            if (!text->answer(action == ui::Action::TextOk, menu.text_value(), &note)) {
                std::printf("menu: text refused (%s)\n", note.c_str());
                menu.reopen_text(note);  // too short or too long: the same field stays open with the note
            }
        }
        if (menu.screen() != ui::Screen::Text)
            menu.frame();  // the screen behind the dialog right away, not the stale dialog
        break;
    case ui::Action::SettingsChanged:
        store_menu_changes(menu.settings(), &app.settings);
        save_settings(app);
        if (apply_settings)
            apply_settings(menu.settings());  // volume, touch layout, picture format, TV mode now; the rest next start
        break;
    case ui::Action::OpenSystemSettings:
        open_system_settings();
        break;
    case ui::Action::PickPlayer1:
        if (app.pick_player1) {
            app.pick_player1();
            menu.set_status(platform::tr("After closing the menu: press a button on the controller that is to be "
                                         "player 1."));
        }
        break;
    default:
        break;
    }
    renderer.set_ui(menu.draw_data());
    if (pause.held) {
        renderer.redraw();  // nobody else presents while the game stands
        ++script.paused_frames;  // counted even if the surface is gone, so scripts never stall
    } else if (action == ui::Action::TextOk || action == ui::Action::TextCancel ||
               menu.screen() == ui::Screen::Text) {
        renderer.redraw();  // the game waits in the keyboard request and presents nothing
    }
}
#endif
#endif

#if defined(LGP_BOOT_WEBGPU)
// --effects without a value: the device's default (CMake LGP_EFFECTS_<KIND>);
// *from says where it came from (for the log).
std::string effects_default(std::string* from) {
#if defined(LGP_BOOT_EFFECTS_BROWSER)  // the page's canvas
    *from = "default for browser";
    return LGP_BOOT_EFFECTS_BROWSER;
#elif defined(LGP_BOOT_SDL)
    const platform::desktop::DeviceKind kind = platform::desktop::device_kind();
    *from = std::string("default for ") + platform::desktop::device_kind_name(kind);
    switch (kind) {
    case platform::desktop::DeviceKind::Ios: return LGP_BOOT_EFFECTS_IOS;
    case platform::desktop::DeviceKind::AppleTv: return LGP_BOOT_EFFECTS_APPLE_TV;
    case platform::desktop::DeviceKind::Android: return LGP_BOOT_EFFECTS_ANDROID;
    case platform::desktop::DeviceKind::AndroidTv: return LGP_BOOT_EFFECTS_ANDROID_TV;
    case platform::desktop::DeviceKind::Desktop: break;
    }
    return LGP_BOOT_EFFECTS_DESKTOP;
#else
    *from = "default";
    return LGP_BOOT_EFFECTS_DESKTOP;
#endif
}

// --async-execute without a value, with the WebGPU renderer: presents in
// flight by default (CMake LGP_ASYNC_EXECUTE_DEFAULT, on Android
// LGP_ASYNC_EXECUTE_ANDROID or _ANDROID_TV by the device kind).
int async_default(const char** from) {
    *from = "default";
#if defined(LGP_BOOT_SDL)
    switch (platform::desktop::device_kind()) {
    case platform::desktop::DeviceKind::Android:
        *from = "default for android";
        return LGP_BOOT_ASYNC_ANDROID;
    case platform::desktop::DeviceKind::AndroidTv:
        *from = "default for android-tv";
        return LGP_BOOT_ASYNC_ANDROID_TV;
    default: break;
    }
#endif
    return LGP_BOOT_ASYNC_DEFAULT;
}
#endif

int run(int argc, char** argv) {
    // Which FP NaN mode the lifted code was built with (a stale build cache
    // can keep the old OFF default; docs/analysis/18-fp-host-nan-pruefung.md).
#if defined(LGP_BOOT_NATIVE_ONLY)
    std::fprintf(stderr, "lgp_boot: no lifted code, the original code runs natively\n");
#elif defined(LGP_FP_HOST_NAN)
    std::fprintf(stderr, "lgp_boot: lifted code with host NaN fast paths (LGP_FP_HOST_NAN on)\n");
#else
    std::fprintf(stderr, "lgp_boot: lifted code with exact Arm NaNs (LGP_FP_HOST_NAN off)\n");
#endif
    boot::LiftedCode code;
    code.entries = LGP_BOOT_ENTRIES;
    code.entry_count = LGP_BOOT_CAT(LGP_BOOT_ENTRIES, _count);
    code.modules = LGP_BOOT_CAT(LGP_BOOT_ENTRIES, _modules);
    code.module_count = LGP_BOOT_CAT(LGP_BOOT_ENTRIES, _modules_count);

    boot::BootOptions opt;
#if defined(LGP_BOOT_NATIVE_ONLY)
    // Built without lifted code (lgp_add_boot_executable NATIVE): the
    // original code runs natively (docs/analysis/56, stage D).
    opt.native.enabled = true;
#endif
    boot::ProcessControl process_control;  // app lifecycle (--renderer window on phones and tablets)
    std::string native_cache;  // empty: default below $LGP_PRIVATE_DIR; "none": off
    std::string exefs, npdm_path, unresolved_path, renderer_kind = "soft", canvas;
    std::string audio_kind;  // empty: sdl with a window, else none
    int volume = 100;         // --volume: percent (SDL sound only)
    std::string video_kind;  // empty: ffmpeg if built, else the host's, else none
    bool ldn_ryuldn = false;  // --ldn ryuldn (needs --ldn-server)
    bool ldn_off = false;     // the last --ldn was off (the app's start menu keeps the server away then)
    std::string ldn_server;   // --ldn-server without the port
    std::vector<uint64_t> frame_stats;  // log a summary of these presented frames (WebGPU renderers)
    uint64_t time_stats_every = 0;      // --time-stats: time per thread every N frames
    int64_t time_stats_tick = -1;       // --time-stats-tick: clock period in us (-1: host default)
    std::string press_spec;             // --press: scripted player 1
    int pacing = -1;                    // --paced 1, --unthrottled 0, -1: by renderer
    bool fullscreen = false;            // --fullscreen (window renderer)
    platform::AspectMode aspect = platform::AspectMode::Fit;  // --aspect
    std::string effects;  // --effects full|half, empty: the device's default
    int async_execute = -1;  // --async-execute on 1, off 0, -1: the device's default
    float render_scale = 0.0f;  // --render-scale, 0: none given (1)
#if defined(LGP_BOOT_WEBGPU)
    platform::webgpu::PresentFilter smoothing = platform::webgpu::PresentFilter::Nearest;  // --smoothing
#endif
    std::vector<std::function<void(uint64_t)>> present_hooks;  // run on every presented frame
    std::string frame_dump;    // also write that frame as a PPM file (a game image: keep it local)
    bool frame_grid = false;   // also print 32x18 cell means (for comparing builds locally)
    std::string pipeline_cache;  // empty: default below $LGP_PRIVATE_DIR; "none": off
    std::string pipeline_cache_seed;  // empty: <--game>/pipeline-seed.lgppipes if it exists
    std::string input_kind;     // empty: sdl with a window, else none
    std::string keyboard_kind;  // software keyboard; empty: window, host (browser), else none
    platform::ControllerStyle player1 = platform::ControllerStyle::Handheld;
    std::string throw_spec;  // --throw: synthetic Joy-Con throw gesture (tuning)
    std::string motion_frame;  // --motion-frame
    float motion_gain = 0.0f;  // --motion-gain; 0: per style (kDefaultSingleJoyconGyroGain)
    std::string stick_range;   // --stick
    std::string touch_mode;    // --touch on|off (default: on for phones and tablets)
    std::string touch_throw_mode;  // --touch-throw on|off|always (default: on, the app's settings)
    std::string menu_mode;     // --menu on|off (app mode with a window: on unless --press)
    std::string start_screen_mode;  // --start-screen on|off (else settings.ini start_screen, on with --menu-script)
    std::string menu_script_spec;  // --menu-script (tests)
    bool pad_motion = true;    // --pad-motion
    bool player1_by_press = true;  // --player1 press|order
    bool input_log = false;    // --input-log
    bool freeze_motion_pose = false;  // --motion-pose frozen
    bool quiet = false;
    bool sched_given = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc)
                usage(("missing value for " + a).c_str());
            return argv[++i];
        };
        auto number = [&](int base = 0) -> uint64_t {
            const std::string v = next();
            char* end = nullptr;
            const unsigned long long n = std::strtoull(v.c_str(), &end, base);
            if (v.empty() || *end != '\0')
                usage(("not a number: " + v).c_str());
            return n;
        };
        if (a == "--exefs") exefs = next();
        else if (a == "--npdm") npdm_path = next();
        else if (a == "--game") opt.game_dir = next();
        else if (a == "--game-file") {
            const std::string v = next();
            const size_t eq = v.find('=');
            if (eq == 0 || eq == std::string::npos)
                usage("--game-file NAME=FILE@OFFSET+LENGTH,...");
            opt.game_files.emplace_back(v.substr(0, eq), v.substr(eq + 1));
        }
        else if (a == "--save") opt.save_dir = next();
        else if (a == "--start") {
            const std::string v = next();
            if (v == "auto") opt.start = boot::StartKind::Auto;
            else if (v == "sdk") opt.start = boot::StartKind::Sdk;
            else if (v == "homebrew") opt.start = boot::StartKind::Homebrew;
            else usage("--start: auto, sdk or homebrew");
        } else if (a == "--graphics") {
            const std::string v = next();
            if (v == "stop") opt.graphics = boot::GraphicsMode::Stop;
            else if (v == "hle") opt.graphics = boot::GraphicsMode::Hle;
            else if (v == "continue") opt.graphics = boot::GraphicsMode::Continue;
            else usage("--graphics: stop, hle or continue");
        } else if (a == "--renderer") {
            renderer_kind = next();
            if (renderer_kind != "soft" && renderer_kind != "webgpu" && renderer_kind != "window")
                usage("--renderer: soft, webgpu or window");
        } else if (a == "--canvas") canvas = next();
        else if (a == "--frames") opt.nvn.frame_limit = number(10);
        else if (a == "--paced") pacing = 1;
        else if (a == "--fullscreen") fullscreen = true;
        else if (a == "--aspect") {
            if (!platform::parse_aspect_mode(next(), &aspect))
                usage("--aspect: fit, fill or stretch");
        }
        else if (a == "--effects") {
            effects = next();
            if (effects != "full" && effects != "half")
                usage("--effects: full or half");
        }
        else if (a == "--async-execute") {
            const std::string v = next();
            if (v != "on" && v != "off")
                usage("--async-execute: on or off");
            async_execute = v == "on" ? 1 : 0;
        }
        else if (a == "--smoothing") {
#if defined(LGP_BOOT_WEBGPU)
            if (!platform::webgpu::parse_present_filter(next(), &smoothing))
                usage("--smoothing: off, linear or fxaa");
#else
            next();
#endif
        }
        else if (a == "--render-scale") {
            const std::string v = next();
            char* end = nullptr;
            render_scale = std::strtof(v.c_str(), &end);
            if (end == v.c_str() || *end != '\0' || !(render_scale >= 0.25f && render_scale <= 2.0f))
                usage("--render-scale: 0.25..2");
        }
        else if (a == "--unthrottled") pacing = 0;
        else if (a == "--nvn-unimplemented") {
            const std::string v = next();
            if (v == "halt") opt.nvn.unimplemented_continue = false;
            else if (v == "continue") opt.nvn.unimplemented_continue = true;
            else usage("--nvn-unimplemented: halt or continue");
        } else if (a == "--nvn-trace") opt.nvn.trace = true; else if (a == "--program-id") opt.process.program_id = number(16);
        else if (a == "--memory") opt.process.total_memory_size = number(10) << 20;
        else if (a == "--language") opt.system.language = next();
        else if (a == "--region") opt.system.region = static_cast<int32_t>(number(10));
        else if (a == "--docked") opt.system.docked = true;
        else if (a == "--handheld") opt.system.docked = false;
        else if (a == "--nickname") opt.system.user_nickname = next();
        else if (a == "--time") opt.system.start_posix_time = static_cast<int64_t>(number(10));
        else if (a == "--utc-offset") opt.system.utc_offset_seconds = std::atoi(next().c_str());
        else if (a == "--sched") {
            const std::string v = next();
            if (v == "free") opt.process.sched_mode = rt::SchedMode::Free;
            else if (v == "core-baton") opt.process.sched_mode = rt::SchedMode::CoreBaton;
            else if (v == "baton") opt.process.sched_mode = rt::SchedMode::Baton;
            else usage("--sched: free, core-baton or baton");
            sched_given = true;
        } else if (a == "--no-cloud-saves") {
            // Read before the cloud restore (tvOS app); nothing to do here.
        } else if (a == "--clock") {
            const std::string v = next();
            if (v == "host") opt.process.clock_mode = rt::ClockMode::Host;
            else if (v == "virtual") opt.process.clock_mode = rt::ClockMode::Virtual;
            else usage("--clock: host or virtual");
        } else if (a == "--trace-svcs") opt.process.trace_svcs = true;
        else if (a == "--native") opt.native.enabled = true;
        else if (a == "--native-base") opt.native.base = number();
        else if (a == "--native-config") opt.native.config = next();
        else if (a == "--native-cache") native_cache = next();
        else if (a == "--native-image-out") opt.native.image_out = next();
        else if (a == "--native-image") opt.native.image = next();
        else if (a == "--init-arrays") opt.run_init_arrays = true;
        else if (a == "--watchdog") opt.watchdog_seconds = static_cast<uint32_t>(number(10));
        else if (a == "--stall-report") opt.stall_report_seconds = static_cast<uint32_t>(number(10));
        else if (a == "--frame-stats") {
            const std::string v = next();
            for (size_t at = 0; at <= v.size();) {
                const size_t end = std::min(v.find(',', at), v.size());
                frame_stats.push_back(std::strtoull(v.substr(at, end - at).c_str(), nullptr, 10));
                at = end + 1;
            }
        } else if (a == "--press") press_spec = next();
        else if (a == "--time-stats-tick") time_stats_tick = int64_t(number(10));
        else if (a == "--env") {
            const std::string kv = next();
            const size_t eq = kv.find('=');
            if (eq == std::string::npos || !platform::set_env(kv.substr(0, eq), kv.substr(eq + 1)))
                usage("--env: KEY=VALUE");
        }
        else if (a == "--time-stats") {
            time_stats_every = number(10);
            if (time_stats_every == 0)
                usage("--time-stats: frames per report, at least 1");
        }
        else if (a == "--frame-dump") frame_dump = next();
        else if (a == "--frame-grid") frame_grid = true;
        else if (a == "--pipeline-cache") pipeline_cache = next();
        else if (a == "--pipeline-cache-seed") pipeline_cache_seed = next();
        else if (a == "--keyboard") {
            keyboard_kind = next();
            if (keyboard_kind != "none" && keyboard_kind != "console" && keyboard_kind != "window" &&
                keyboard_kind != "menu" && keyboard_kind != "host" && keyboard_kind.rfind("text:", 0) != 0)
                usage("--keyboard: none, console, window, menu, host or text:NAME");
        } else if (a == "--input") {
            input_kind = next();
            if (input_kind != "none" && input_kind != "sdl")
                usage("--input: none or sdl");
        } else if (a == "--controller") {
            const std::string v = next();
            if (v == "handheld") player1 = platform::ControllerStyle::Handheld;
            else if (v == "joycon-right") player1 = platform::ControllerStyle::JoyconRight;
            else if (v == "joycon-left") player1 = platform::ControllerStyle::JoyconLeft;
            else usage("--controller: handheld, joycon-right or joycon-left");
        } else if (a == "--pad-motion") {
            const std::string v = next();
            if (v != "on" && v != "off")
                usage("--pad-motion: on or off");
            pad_motion = v == "on";
        } else if (a == "--player1") {
            const std::string v = next();
            if (v != "press" && v != "order")
                usage("--player1: press or order");
            player1_by_press = v == "press";
        } else if (a == "--motion-frame") {
            motion_frame = next();
            platform::MapperConfig probe;
            if (!platform::parse_motion_frame(motion_frame, probe))
                usage("--motion-frame: three signed axes, e.g. -y,x,z");
        } else if (a == "--motion-gain") {
            const std::string v = next();
            char* end = nullptr;
            motion_gain = std::strtof(v.c_str(), &end);
            if (v.empty() || *end != '\0' || !(motion_gain > 0.0f && motion_gain <= 20.0f))
                usage("--motion-gain: factor > 0 and <= 20, e.g. 2");
        } else if (a == "--stick") {
            stick_range = next();
            platform::MapperConfig probe;
            if (!platform::parse_stick_range(stick_range, probe))
                usage("--stick: DEADZONE,OUTER[,HOLD] with 0 <= DEADZONE < OUTER <= 1, 0 <= HOLD < 1, e.g. 0.15,0.9");
        } else if (a == "--touch") {
            touch_mode = next();
            if (touch_mode != "on" && touch_mode != "off")
                usage("--touch: on or off");
        } else if (a == "--touch-throw") {
            touch_throw_mode = next();
            if (touch_throw_mode != "on" && touch_throw_mode != "off" && touch_throw_mode != "always")
                usage("--touch-throw: on, off or always");
        } else if (a == "--menu-script") {
            menu_script_spec = next();
        } else if (a == "--menu") {
            menu_mode = next();
            if (menu_mode != "on" && menu_mode != "off")
                usage("--menu: on or off");
        } else if (a == "--start-screen") {
            start_screen_mode = next();
            if (start_screen_mode != "on" && start_screen_mode != "off")
                usage("--start-screen: on or off");
        } else if (a == "--input-log") {
            input_log = true;
        } else if (a == "--motion-pose") {
            const std::string v = next();
            if (v != "on" && v != "frozen")
                usage("--motion-pose: on or frozen");
            freeze_motion_pose = v == "frozen";
        } else if (a == "--throw") {
            throw_spec = next();
            platform::MapperConfig probe;
            if (!platform::parse_throw_spec(throw_spec, probe))
                usage("--throw: ms=N,g=G,rps=R,accel=x|y|z|all,gyro=x|y|z|all,rest=[+-]x|y|z,side=[+-]x|y|z|none,"
                      "yaw=[+-]x|y|z|none,sidegain=F,leftyaw=[+-]x|y|z|none,leftgain=F,sideaccel=F,leftaccel=F,dir=-1..1,strength=1..2,tempo=1..2,reach=-1..1,short=MS,long=MS");
        } else if (a == "--movie-audio") {  // the default; kept for old command lines
            opt.movie_audio = true;
        } else if (a == "--no-movie-audio") {
            opt.movie_audio = false;
        } else if (a == "--video") {
            video_kind = next();
            if (video_kind != "none" && video_kind != "ffmpeg" && video_kind != "vpx" && video_kind != "host")
                usage("--video: none, ffmpeg, vpx or host");
        } else if (a == "--audio") {
            audio_kind = next();
            if (audio_kind != "none" && audio_kind != "sdl")
                usage("--audio: none or sdl");
        } else if (a == "--ldn") {
            // Local wireless play over the LAN between instances of this app
            // (docs/analysis/44-multiplayer-stand.md). Off by default on the
            // command line (tests, measurements); the app turns it on
            // (settings.ini ldn =, boot::app_arguments).
            const std::string v = next();
            ldn_off = v == "off";
            if (v == "off") {
                opt.ldn_lan.lan = nullptr;
            } else if (v == "lan" || v == "ryuldn" || v == "both") {
                opt.ldn_lan.lan = platform::make_host_lan();
                if (!opt.ldn_lan.lan)
                    std::fprintf(stderr, "lgp_boot: --ldn %s: no sockets on this host, staying offline\n", v.c_str());
                ldn_ryuldn = v != "lan";
                opt.ldn_lan.ryuldn_dual = v == "both";
            } else {
                usage("--ldn: off, lan, ryuldn or both");
            }
        } else if (a == "--ldn-server") {
            const std::string v = next();
            hle::LdnLanConfig parsed;
            if (!hle::parse_ryuldn_server(v, &parsed))
                usage("--ldn-server: HOST[:PORT], ws://HOST[:PORT][/PATH] or wss://HOST[:PORT][/PATH], port 1 to 65535");
            ldn_server = parsed.ryuldn_server;
            opt.ldn_lan.ryuldn_port = parsed.ryuldn_port;
            opt.ldn_lan.ryuldn_websocket = parsed.ryuldn_websocket;
            opt.ldn_lan.ryuldn_tls = parsed.ryuldn_tls;
            opt.ldn_lan.ryuldn_path = parsed.ryuldn_path;
        } else if (a == "--ldn-room") {
            opt.ldn_lan.ryuldn_room = next();
        } else if (a == "--ldn-ca") {
            opt.ldn_lan.ryuldn_ca_file = next();
        } else if (a == "--ldn-port") {
            const uint64_t port = number(10);
            if (port == 0 || port > 65535)
                usage("--ldn-port: 1 to 65535");
            opt.ldn_lan.port = static_cast<uint16_t>(port);
        } else if (a == "--ldn-peer") {
            const std::string v = next();
            size_t start = 0;
            while (start <= v.size()) {
                const size_t comma = std::min(v.find(',', start), v.size());
                platform::LanEndpoint peer;
                if (!platform::parse_lan_endpoint(v.substr(start, comma - start), &peer))
                    usage("--ldn-peer: IP[:PORT],... e.g. 192.168.1.20");
                opt.ldn_lan.peers.push_back(peer);
                start = comma + 1;
            }
        } else if (a == "--mods") {
            for (const std::string& id : mods::parse_mod_list(next())) {
                if (mods::find_mod(id) == nullptr) {
                    std::string known;
                    for (const mods::ModInfo& m : mods::known_mods())
                        known += " " + m.id;
                    usage(("--mods: unknown mod " + id + " (known:" + known + ")").c_str());
                }
                if (std::find(opt.mods.enabled.begin(), opt.mods.enabled.end(), id) == opt.mods.enabled.end())
                    opt.mods.enabled.push_back(id);
            }
        } else if (a == "--mods-dir") {
            opt.mods.dir = std::filesystem::path(next());
        } else if (a == "--hud-bounds") {
            const std::string v = next();
            char* end = nullptr;
            const long n = std::strtol(v.c_str(), &end, 10);
            if (v.empty() || *end != '\0' || n < 0 || n > 100)
                usage("--hud-bounds: 0..100 (percent)");
            opt.mods.hud_bounds = int(n);
        } else if (a == "--hud-sides") {
            if (!lgp::boot::parse_hud_sides(next(), &opt.mods.hud_sides))
                usage("--hud-sides: T,B,L,R, each 0..100 (percent)");
        } else if (a == "--hook-log") {
            opt.mods.hook_log = static_cast<uint32_t>(std::min<uint64_t>(number(10), 1'000'000));
        } else if (a == "--volume") {
            const std::string v = next();
            char* end = nullptr;
            const long n = std::strtol(v.c_str(), &end, 10);
            if (v.empty() || *end != '\0' || n < 0 || n > 100)
                usage("--volume: 0 to 100");
            volume = static_cast<int>(n);
        }
        else if (a == "--log") {
            const std::string v = next();
            if (v == "trace") rt::set_log_level(rt::LogLevel::Trace);
            else if (v == "debug") rt::set_log_level(rt::LogLevel::Debug);
            else if (v == "info") rt::set_log_level(rt::LogLevel::Info);
            else if (v == "warn") rt::set_log_level(rt::LogLevel::Warn);
            else if (v == "error") rt::set_log_level(rt::LogLevel::Error);
            else usage("--log: trace, debug, info, warn or error");
        } else if (a == "--unresolved") unresolved_path = next();
        else if (a == "--quiet") quiet = true;
        else if (!a.empty() && a[0] == '-') usage(("unknown option " + a).c_str());
        else opt.module_paths.push_back(a);
    }
    if (ldn_ryuldn && ldn_server.empty())
        usage("--ldn ryuldn/both: needs --ldn-server HOST[:PORT] or ws[s]://HOST[:PORT][/PATH]");
    if (ldn_ryuldn)
        opt.ldn_lan.ryuldn_server = ldn_server;
    std::printf("ui language: %s (%s)\n", platform::ui_language_name(platform::ui_language()),
                g_ui_language_source.c_str());
    // Which memory entitlements the app got (iOS, sideloaders: docs/knowledge/ios-ohne-mac.md): the kernel grants
    // one that the code signature carries and the profile allows. Silent where neither has any (desktop builds).
    if (platform::OwnSignature sig; platform::own_signature(&sig) && (!sig.entitlements.empty() || !sig.profile.empty())) {
        const auto yes = [](const std::string& plist, const char* key) {
            return platform::entitlement_true(plist, key) ? "yes" : "no";
        };
        const char* kLimit = "com.apple.developer.kernel.increased-memory-limit";
        const char* kVa = "com.apple.developer.kernel.extended-virtual-addressing";
        platform::ProcessMemory mem;
        const bool have_mem = platform::process_memory(&mem) && mem.available > 0;
        std::printf("code signature: increased-memory-limit signed %s, profile %s; extended-virtual-addressing "
                    "signed %s, profile %s%s",
                    yes(sig.entitlements, kLimit), sig.profile.empty() ? "none" : yes(sig.profile, kLimit),
                    yes(sig.entitlements, kVa), sig.profile.empty() ? "none" : yes(sig.profile, kVa),
                    have_mem ? "; available " : "\n");
        if (have_mem)
            std::printf("%llu MiB\n", static_cast<unsigned long long>(mem.available >> 20));
    }
    // --language device (the app's default): the system's language where the
    // game has it, else en-US; reproducible runs keep en-US.
    if (opt.system.language == "device") {
        std::string locale;
#if defined(LGP_BOOT_SDL)
        if (opt.process.clock_mode != rt::ClockMode::Virtual)
            locale = platform::desktop::preferred_locale();
#endif
        opt.system.language = hle::language_for_locale(locale);
        std::printf("language: %s (device locale \"%s\")\n", opt.system.language.c_str(), locale.c_str());
    }
    if (opt.process.clock_mode == rt::ClockMode::Virtual) {
        // Reproducible runs: one guest thread at a time and no host date.
        if (!sched_given)
            opt.process.sched_mode = rt::SchedMode::Baton;
        else if (opt.process.sched_mode != rt::SchedMode::Baton)
            std::fprintf(stderr, "lgp_boot: --clock virtual without --sched baton is not reproducible\n");
        if (!opt.system.start_posix_time)
            opt.system.start_posix_time = boot::kVirtualClockPosixTime;
        std::printf("clock: virtual, sched %s, time %" PRId64 "\n", rt::sched_mode_name(opt.process.sched_mode),
                    *opt.system.start_posix_time);
    }
    if (opt.native.enabled) {
        // The rewrite takes seconds on a game; the cache keeps it per program
        // build (stamp: size and time of this executable).
        if (native_cache.empty())
            if (const char* dir = std::getenv("LGP_PRIVATE_DIR"); dir != nullptr && *dir != '\0')
                native_cache = std::string(dir) + "/cache/native-rewrite.lgpnative";
        if (native_cache != "none")
            opt.native.cache = native_cache;
        std::error_code ec;
        const std::filesystem::path self(argc > 0 ? argv[0] : "");
        const auto size = std::filesystem::file_size(self, ec);
        const auto time = ec ? std::filesystem::file_time_type{} : std::filesystem::last_write_time(self, ec);
        if (ec)
            std::fprintf(stderr, "lgp_boot: no stamp of %s for the native cache (%s)\n", self.string().c_str(),
                         ec.message().c_str());
        else
            opt.native.cache_salt = std::to_string(size) + ":" +
                                    std::to_string(static_cast<long long>(time.time_since_epoch().count()));
    }
    if (std::string error; !boot::prepare_host(&error)) {
        std::fprintf(stderr, "lgp_boot: %s\n", error.c_str());
        return 3;
    }
    if (!exefs.empty()) {
        if (!opt.module_paths.empty())
            usage("--exefs and module files exclude each other");
        for (size_t i = 0; i < code.module_count; ++i)
            opt.module_paths.push_back((std::filesystem::path(exefs) / code.modules[i].name).string());
        if (code.module_count == 0) {
            // No lifted code to name the modules (native start): the ExeFS's
            // modules in rtld's load order.
            std::vector<std::string> names = {"rtld", "main"};
            for (int i = 0; i < 10; ++i)
                names.push_back("subsdk" + std::to_string(i));
            names.push_back("sdk");
            for (const std::string& n : names)
                if (std::filesystem::is_regular_file(std::filesystem::path(exefs) / n))
                    opt.module_paths.push_back((std::filesystem::path(exefs) / n).string());
        }
        if (npdm_path.empty() && std::filesystem::exists(std::filesystem::path(exefs) / "main.npdm"))
            npdm_path = (std::filesystem::path(exefs) / "main.npdm").string();
    }
    if (opt.module_paths.empty())
        usage("no modules");
    if (!npdm_path.empty()) {
        std::vector<uint8_t> bytes;
        std::string error;
        if (!read_file(npdm_path, &bytes)) {
            std::fprintf(stderr, "lgp_boot: cannot read %s\n", npdm_path.c_str());
            return 3;
        }
        opt.npdm = boot::parse_npdm(bytes, &error);
        if (!opt.npdm) {
            std::fprintf(stderr, "lgp_boot: %s: %s\n", npdm_path.c_str(), error.c_str());
            return 3;
        }
        std::printf("npdm: %s, program 0x%016" PRIx64 ", main thread stack 0x%x priority %u core %u\n",
                    opt.npdm->name.c_str(), opt.npdm->program_id, opt.npdm->main_thread_stack_size,
                    opt.npdm->main_thread_priority, opt.npdm->main_thread_core);
    }
    if (!opt.save_dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(opt.save_dir, ec);
    }

    // Renderer for --graphics hle. All of these outlive boot().
    std::unique_ptr<platform::NullRenderer> null_renderer;
#if defined(LGP_BOOT_WEBGPU)
    std::unique_ptr<platform::webgpu::WebGpuRenderer> gpu_renderer;
#endif
#if defined(LGP_BOOT_SDL)
    std::unique_ptr<platform::desktop::SdlWindow> window;
    std::unique_ptr<platform::desktop::SdlInput> sdl_input;
    std::unique_ptr<platform::desktop::SdlTextInput> sdl_text;
#endif
#if defined(LGP_BOOT_APP) && defined(LGP_BOOT_UI)
    std::unique_ptr<ui::AppMenu> menu;
    MenuScript menu_script;
    if (!menu_script_spec.empty() && !parse_menu_script(menu_script_spec, &menu_script))
        usage("--menu-script: play, newslot, pause, backup, resume, quit, settings, slot<N>, docked, handheld or "
              "mod:<id>=on|off, encounter-rate:rare|medium|gen1, visible-pokemon:off|mix|on, "
              "controller:handheld|joycon-right|joycon-left, each with an optional @FRAME");
#endif
    // After the renderers: destroyed before them (the WebGPU backend releases
    // its objects through the renderer's GPU thread).
    std::unique_ptr<gpu::nvn::Backend> backend;
    // LGP_WEBGPU_ASYNC_EXECUTE: runs the backend's work behind the guest
    // (lgp/gpu/nvn/async_backend.hpp); goes before the backend.
    std::unique_ptr<gpu::nvn::AsyncBackend> async_backend;
#if defined(LGP_BOOT_SDL)
    // After the backend: the window's lifecycle handler uses it, so it is
    // detached before the backend goes (the window outlives both).
    struct LifecycleDetach {
        std::unique_ptr<platform::desktop::SdlWindow>& window;
        ~LifecycleDetach() {
            if (window)
                window->set_lifecycle_handler(nullptr);
        }
    } lifecycle_detach{window};
#endif
    std::atomic<bool> stop_requested{false};
    // Controller mapping of every input backend (SDL, the browser's host input).
    const auto input_slots = [&] {
        std::vector<platform::MapperConfig> slots = platform::default_slots();
        slots[0].style = player1;
        if (!throw_spec.empty())
            platform::parse_throw_spec(throw_spec, slots[0]);
        if (!motion_frame.empty())
            platform::parse_motion_frame(motion_frame, slots[0]);
        if (!stick_range.empty())
            for (platform::MapperConfig& slot : slots)
                platform::parse_stick_range(stick_range, slot);
        for (platform::MapperConfig& slot : slots) {
            const bool single = slot.style == platform::ControllerStyle::JoyconRight ||
                                slot.style == platform::ControllerStyle::JoyconLeft;
            slot.gyro_gain = motion_gain > 0.0f ? motion_gain : single ? kDefaultSingleJoyconGyroGain : 1.0f;
        }
        return slots;
    };
    bool windowed = false;
#if defined(LGP_BOOT_WEBGPU)
    gpu::webgpu::WebGpuBackend* wgpu_backend = nullptr;
#endif
    if (opt.graphics == boot::GraphicsMode::Hle) {
        constexpr platform::SurfaceSize kSize{1280, 720};  // handheld resolution
        if (renderer_kind == "soft") {
            null_renderer = std::make_unique<platform::NullRenderer>(kSize);
            backend = std::make_unique<gpu::nvn::SoftBackend>(*null_renderer);
        } else if (renderer_kind == "webgpu") {
#if defined(LGP_BOOT_WEBGPU)
            platform::webgpu::RendererOptions ro;
            ro.size = kSize;
            ro.canvas_selector = canvas;
#if defined(LGP_BOOT_FIF_BROWSER)
            apply_frames_in_flight_default(LGP_BOOT_FIF_BROWSER, "browser");  // the page's canvas
#endif
            apply_metal_libs_default(opt.game_dir, metal_libs_kind());
            gpu_renderer = std::make_unique<platform::webgpu::WebGpuRenderer>(ro);
            if (!gpu_renderer->ok()) {
                std::fprintf(stderr, "lgp_boot: no WebGPU: %s\n", gpu_renderer->error().c_str());
                return 3;
            }
            gpu_renderer->set_aspect_mode(aspect);
            gpu_renderer->set_present_filter(smoothing);
            std::printf("renderer: webgpu (%s%s)\n", gpu_renderer->adapter_name().c_str(),
                        gpu_renderer->on_gpu_thread() ? ", own GPU thread" : "");
            backend = std::make_unique<gpu::webgpu::WebGpuBackend>(*gpu_renderer);
            wgpu_backend = static_cast<gpu::webgpu::WebGpuBackend*>(backend.get());
#else
            usage("--renderer webgpu: this build has no WebGPU");
#endif
        } else {
#if defined(LGP_BOOT_SDL)
            platform::desktop::WindowOptions wo;
            wo.title = "lgp";
            wo.width = kSize.width;
            wo.height = kSize.height;
            wo.fullscreen = fullscreen;
            // --unthrottled: no vsync wait in the window either (FIFO would
            // snap the frame rate to 60/n Hz, e.g. 30 fps for 17-33 ms frames).
            if (pacing == 0)
                wo.renderer.present_mode = WGPUPresentMode_Mailbox;
            std::string error;
            apply_metal_libs_default(opt.game_dir, metal_libs_kind());
            window = platform::desktop::SdlWindow::create(wo, &error);
            if (!window) {
                std::fprintf(stderr, "lgp_boot: no window: %s\n", error.c_str());
                return 3;
            }
            std::printf("renderer: window (%s, %s)\n", window->video_driver().c_str(),
                        window->renderer().adapter_name().c_str());
            window->renderer().set_aspect_mode(aspect);
            if (aspect != platform::AspectMode::Fit)
                std::printf("aspect: %s\n", platform::aspect_mode_name(aspect));
            window->renderer().set_present_filter(smoothing);
            if (smoothing != platform::webgpu::PresentFilter::Nearest)
                std::printf("smoothing: %s\n", platform::webgpu::present_filter_name(smoothing));
            apply_frames_in_flight_default();
            apply_late_file_window_default();
            backend = std::make_unique<gpu::webgpu::WebGpuBackend>(window->renderer());
            wgpu_backend = static_cast<gpu::webgpu::WebGpuBackend*>(backend.get());
            windowed = true;
#if defined(LGP_BOOT_APP) && defined(LGP_BOOT_UI)
            // The app's menus (P7): start screen now, pause button in the game.
            // Scripted runs (--press) start the game right away.
            if (!menu_script.empty() && (!g_app_ui || menu_mode == "off"))
                std::fprintf(stderr, "lgp_boot: --menu-script needs the app mode (--app) with its menus; ignored\n");
            if (g_app_ui && (menu_mode == "on" || (menu_mode.empty() && press_spec.empty()))) {
                menu = ui::AppMenu::create(window->sdl_window(), &error);
                if (!menu)
                    std::fprintf(stderr, "lgp_boot: no menus, continuing without: %s\n", error.c_str());
                else
                    menu->set_touch_layout_supported(true);  // SdlInput::set_touch_layout
                if (menu && g_app_ui->system_settings)
                    menu->set_system_settings(true);
                if (menu)
                    menu->set_server_check([](const std::string& text) -> std::string {
                        hle::LdnLanConfig parsed;
                        return hle::parse_ryuldn_server(text, &parsed)
                                   ? ""
                                   : platform::tr("Not like this: HOST[:PORT] or ws[s]://HOST[:PORT][/PATH].");
                    });
            }
            if (g_app_ui && !menu) {
                // Without menus (scripted runs, or menus that failed) the app's
                // Pokémon Gelb holds too; the start screen sets it otherwise.
                const auto& mods_on = g_app_ui->settings.mods;
                const bool gelb = std::find(mods_on.begin(), mods_on.end(), kRandomEncountersId) != mods_on.end();
                apply_random_encounters(gelb, g_app_ui->settings.hide_field_pokemon && !gelb);
            }
            if (menu) {
                AppUiContext& app = *g_app_ui;
                // The game's own menu font from the user's RomFS (stays on
                // the device); without it the system font.
                bool game_font = false;
                for (const char* path : kGameMenuFonts) {
                    std::vector<uint8_t> font;
                    std::string font_error;
                    if (boot::read_game_file(opt, path, &font, &font_error) &&
                        menu->set_game_font(std::move(font), &font_error)) {
                        std::printf("menu: game font %s\n", path);
                        game_font = true;
                        break;
                    }
                    std::printf("menu: no font %s (%s)\n", path, font_error.c_str());
                }
                if (!game_font)
                    std::printf("menu: system font\n");
                menu->set_skin(load_menu_skin(opt));
                menu->set_game_source(app.game_source);
#if defined(LGP_CLOUD_CONTAINER)
                // Without backup (--no-cloud-saves): a warning only where the
                // system may delete the saves (tvOS, Library/Caches).
                const bool saves_deletable = std::string_view(LGP_CLOUD_SETTINGS_TAG) == "tvos";
                menu->set_cloud_note(g_save_backup ? g_save_backup->note()
                                     : saves_deletable
                                         ? platform::tr("Warning: saves are not backed up in iCloud, tvOS may "
                                                        "delete them.")
                                         : "");
                if (g_save_backup && g_save_backup->cloud_choice() != 0)
                    menu->set_cloud_choice(platform::tr("iCloud has another state (another device). It replaces "
                                                        "the saves here, which are kept as a backup."));
#endif
                std::vector<boot::SaveSlot> slots = app_slots(app.files);
                // A save folder that is no slot (saves = elsewhere, or --save):
                // offered first and kept unless another slot is picked.
                const std::filesystem::path configured(opt.save_dir);  // narrow like argv (path_to_arg)
                // The folder this run would use (settings.ini or --save) picks the slot.
                const int configured_slot =
                    opt.save_dir.empty() ? -1 : slot_folder_index(app.files.data, slots, configured);
                const bool custom = !opt.save_dir.empty() && configured_slot < 0;
                const int offset = custom ? 1 : 0;
                const auto offer = [&](int index) {
                    std::vector<ui::MenuSlot> list = menu_slots(slots);
                    if (custom)
                        list.insert(list.begin(), custom_menu_slot(configured));
                    menu->set_slots(std::move(list), index);
                };
                const int preselected =
                    custom ? 0 : configured_slot >= 0 ? configured_slot : slot_index(slots, app.settings);
                offer(preselected);
                bool picked = false;  // a new slot was made here: the user's own choice (as is a tap)
                ui::MenuSettings shown_graphics;  // the graphics values the start screen showed
                {
                    ui::MenuSettings m = menu_settings(app.settings, platform::desktop::SdlInputOptions::default_touch());
                    if (!touch_mode.empty())
                        m.touch = touch_mode == "on";
                    if (!touch_throw_mode.empty()) {
                        m.touch_throw = touch_throw_mode != "off";
                        m.touch_throw_pad = touch_throw_mode == "always";
                    }
                    m.volume = volume;
                    m.docked = opt.system.docked;
                    m.aspect = int(aspect);
                    // settings.ini plus --ldn: what this run would use (the
                    // server alone, --ldn ryuldn, is the Wi-Fi off)
                    m.ldn_lan = opt.ldn_lan.lan && (opt.ldn_lan.ryuldn_server.empty() || opt.ldn_lan.ryuldn_dual);
                    for (ui::MenuMod& mod : m.mods)  // settings.ini plus --mods
                        mod.on = std::find(opt.mods.enabled.begin(), opt.mods.enabled.end(), mod.id) !=
                                 opt.mods.enabled.end();
                    // boot::app_arguments names random-encounters for "Hide field
                    // Pokémon" too: Pokémon Gelb's row is on only when settings.ini
                    // names it (PC 02.11.: with menus the hiding ran as Gelb).
                    if (const auto& named = app.settings.mods;
                        app.settings.hide_field_pokemon &&
                        std::find(named.begin(), named.end(), kRandomEncountersId) == named.end())
                        for (ui::MenuMod& mod : m.mods)
                            if (mod.id == kRandomEncountersId)
                                mod.on = false;
                    // Only mods with their <id>.cfg (game addresses) in the mods folder, or for a
                    // data mod its <id>/romfs/: a switch without one would do nothing. One that
                    // is on stays, so it can be turned off.
                    const std::filesystem::path mods_dir =
                        !opt.mods.dir.empty() ? opt.mods.dir
                        : opt.game_dir.empty() ? std::filesystem::path()
                                               : std::filesystem::path(opt.game_dir) / "mods";
                    std::erase_if(m.mods, [&](const ui::MenuMod& mod) {
                        if (mod.on) return false;
                        if (mods_dir.empty()) return true;
                        std::error_code ec;
                        const mods::ModInfo* info = mods::find_mod(mod.id);
                        return info && info->data
                                   ? !std::filesystem::is_directory(mods_dir / mod.id / "romfs", ec)
                                   : !std::filesystem::is_regular_file(mods_dir / (mod.id + ".cfg"), ec);
                    });
                    m.controller = player1 == platform::ControllerStyle::JoyconRight  ? 1
                                   : player1 == platform::ControllerStyle::JoyconLeft ? 2
                                                                                      : 0;
                    m.player1_by_press = player1_by_press;  // settings.ini, or --player1 after --app
                    // Graphics: what this run uses (settings.ini, options, else the device's default).
                    std::string effects_from;
                    m.effects_half = (!effects.empty() ? effects : effects_default(&effects_from)) == "half";
                    m.render_scale = menu_render_scale(render_scale > 0.0f ? render_scale : 1.0f);
                    const char* async_from = nullptr;
                    m.async_execute = async_execute >= 0 ? async_execute > 0 : async_default(&async_from) > 0;
                    m.smoothing = int(smoothing);
                    menu->settings() = m;
                    g_menu_seen = m;
                    shown_graphics = m;
                }
                if (slots.empty() && !custom)
                    menu->set_status(platform::trf("No save folder under {} (write permission, disk space?)",
                                     {boot::path_to_utf8(app.files.data)}));
                window->set_event_handler([m = menu.get()](const SDL_Event& e) { m->handle_event(e); });
                // Without the start screen the game starts with the preselected
                // save, as "Continue" would; it still comes when it has to ask.
                bool needed = slots.empty() && !custom;
#if defined(LGP_CLOUD_CONTAINER)
                if (g_save_backup && g_save_backup->cloud_choice() != 0)
                    needed = true;
#endif
                const bool start_screen = start_screen_mode == "on" ||
                                          (start_screen_mode.empty() && (app.settings.start_screen || !menu_script.empty()));
                bool play = !start_screen && !needed;
                if (play)
                    std::printf("menu: start screen skipped (start_screen = 0)\n");
                else
                    menu->show(ui::Screen::Start);
                while (!play) {
                    if (!window->pump_events(16))
                        return 0;  // window closed on the start screen
                    if (!menu_script.empty()) {
                        run_menu_step(*menu, menu_script.front());
                        menu_script.pop_front();
                    }
                    const ui::Action action = menu->frame();
                    if (action != ui::Action::None)
                        std::printf("menu: %s\n", action_name(action));
                    switch (action) {
                    case ui::Action::Play:
                        if (slots.empty() && !(custom && menu->slot() == 0)) {
                            std::printf("app: no save folder under %s\n", boot::path_to_utf8(app.files.data).c_str());
                            menu->set_status(platform::trf("No save folder under {} (write permission, disk space?)",
                                             {boot::path_to_utf8(app.files.data)}));
                            break;
                        }
                        play = true;
                        break;
                    case ui::Action::QuitGame:  // menu script "quit" before the game started
                    case ui::Action::Quit:
                        return 0;
                    case ui::Action::NewSlot:
                        if (const std::string dir = boot::create_save_slot(app.files.data, &error); dir.empty()) {
                            menu->set_status(platform::trf("No new save: {}", {error}));
                        } else {
                            slots = app_slots(app.files);
                            int index = 0;
                            for (size_t i = 0; i < slots.size(); ++i)
                                if (slots[i].dir == dir)
                                    index = int(i) + offset;
                            offer(index);
                            picked = true;
                        }
                        break;
                    case ui::Action::SettingsChanged:
                        store_menu_changes(menu->settings(), &app.settings);
                        save_settings(app);
                        break;
                    case ui::Action::OpenSystemSettings:
                        open_system_settings();
                        break;
#if defined(LGP_CLOUD_CONTAINER)
                    case ui::Action::LoadCloudSaves:
                        if (g_save_backup && g_save_backup->use_cloud_state(&error)) {
                            slots = app_slots(app.files);
                            offer(std::max(0, std::min(menu->slot(), int(slots.size()) + offset - 1)));
                            menu->set_cloud_choice({});
                            menu->set_cloud_note(g_save_backup->note());
                        } else {
                            menu->set_status(platform::trf("iCloud state not loaded: {}", {error}));
                        }
                        break;
#endif
                    default:
                        break;
                    }
                    window->renderer().set_ui(menu->draw_data());
                    window->renderer().redraw();
                }
                // The chosen slot and settings for this run (settings.ini keeps them).
                store_menu_changes(menu->settings(), &app.settings);
                if (custom && menu->slot() == 0) {
                    // opt.save_dir is narrow like argv, settings.ini and app.save_dir UTF-8.
                    app.save_dir = boot::path_to_utf8(configured);  // as configured; settings.ini keeps its saves =
                    if (menu->slot_chosen())  // ... unless "Eigener Ordner" was tapped (a --save folder)
                        app.settings.save_dir = app.save_dir;
                    save_settings(app);
                    std::printf("app: save folder %s\n", opt.save_dir.c_str());
                } else {
                    const int pick = std::clamp(menu->slot() - offset, 0, int(slots.size()) - 1);
                    const boot::SaveSlot& chosen = slots[size_t(pick)];
                    // settings.ini only follows a slot picked in the menu (also a
                    // tap on the preselected one), not a one-off --save that
                    // happened to name a slot.
                    if (picked || menu->slot_chosen())
                        app.settings.save_dir = chosen.dir == "saves" ? std::string() : chosen.dir;
                    app.save_dir = chosen.dir;
                    save_settings(app);
                    opt.save_dir = boot::path_to_arg(app.files.data / chosen.dir);
                    std::error_code ec;
                    std::filesystem::create_directories(app.files.data / chosen.dir, ec);
                    std::printf("app: save slot %d (%s)\n", chosen.number, opt.save_dir.c_str());
                }
                touch_mode = menu->settings().touch ? "on" : "off";
                touch_throw_mode = !menu->settings().touch_throw    ? "off"
                                   : menu->settings().touch_throw_pad ? "always"
                                                                      : "on";
                volume = menu->settings().volume;
                opt.system.docked = menu->settings().docked;
                aspect = platform::AspectMode(std::clamp(menu->settings().aspect, 0, 2));
                window->renderer().set_aspect_mode(aspect);
                smoothing = platform::webgpu::PresentFilter(std::clamp(menu->settings().smoothing, 0, 2));
                window->renderer().set_present_filter(smoothing);
                opt.mods.enabled = menu_mods_on(menu->settings());  // the game has not started yet
                if (menu->settings().hud_height != g_menu_seen.hud_height ||
                    menu->settings().hud_width != g_menu_seen.hud_width)
                    boot::parse_hud_sides(menu_hud_sides(menu->settings()), &opt.mods.hud_sides);
                apply_random_encounters(menu_gelb_on(menu->settings()), menu_hide_only(menu->settings()));
                {
                    // Our server is always on with the mod online-button that
                    // sends "far away" to it (boot::app_arguments does the same;
                    // the mod is hidden, so menu_mods_on() above does not list
                    // it): with the Wi-Fi both links, without it the server
                    // alone. Without the mod's file, with a server the menu
                    // cannot parse, or after an --ldn off on the command line
                    // (it wins, as in app_arguments) only the Wi-Fi.
                    const ui::MenuSettings& ms = menu->settings();
                    const std::filesystem::path mods_dir =
                        opt.mods.dir.empty() ? std::filesystem::path(opt.game_dir) / "mods" : opt.mods.dir;
                    hle::LdnLanConfig online;
                    const bool go_online = !ldn_off && hle::parse_ryuldn_server(ms.ldn_server, &online) &&
                                           boot::online_button_file(mods_dir);
                    if (go_online) {
                        opt.ldn_lan.ryuldn_server = online.ryuldn_server;
                        opt.ldn_lan.ryuldn_port = online.ryuldn_port;
                        opt.ldn_lan.ryuldn_websocket = online.ryuldn_websocket;
                        opt.ldn_lan.ryuldn_tls = online.ryuldn_tls;
                        opt.ldn_lan.ryuldn_path = online.ryuldn_path;
                        opt.ldn_lan.ryuldn_room = ms.ldn_room;
                    } else {
                        opt.ldn_lan.ryuldn_server.clear();
                    }
                    opt.ldn_lan.ryuldn_dual = go_online && ms.ldn_lan;
                    if (go_online && std::find(opt.mods.enabled.begin(), opt.mods.enabled.end(), "online-button") ==
                                         opt.mods.enabled.end())
                        opt.mods.enabled.push_back("online-button");
                    // The game's controller screen at start only when asked for
                    // (boot::app_arguments does the same; the mod is hidden).
                    if (!ms.controller_screen && boot::skip_controller_screen_file(mods_dir) &&
                        std::find(opt.mods.enabled.begin(), opt.mods.enabled.end(), "skip-controller-screen") ==
                            opt.mods.enabled.end())
                        opt.mods.enabled.push_back("skip-controller-screen");
                    if (boot::audio_free_guard_file(mods_dir) &&
                        std::find(opt.mods.enabled.begin(), opt.mods.enabled.end(), "audio-free-guard") ==
                            opt.mods.enabled.end())
                        opt.mods.enabled.push_back("audio-free-guard");
                    std::printf("app: ldn %s\n", go_online ? (opt.ldn_lan.ryuldn_dual ? "both" : "ryuldn")
                                                  : ms.ldn_lan ? "lan"
                                                               : "off");
                    if ((go_online || ms.ldn_lan) && !opt.ldn_lan.lan)
                        opt.ldn_lan.lan = platform::make_host_lan();  // nullptr without UDP: stays offline
                    else if (!go_online && !ms.ldn_lan)
                        opt.ldn_lan.lan = nullptr;
                }
                const int c = menu->settings().controller;
                player1 = c == 1   ? platform::ControllerStyle::JoyconRight
                          : c == 2 ? platform::ControllerStyle::JoyconLeft
                                   : platform::ControllerStyle::Handheld;
                player1_by_press = menu->settings().player1_by_press;
                // Graphics changed on the start screen: this run already (the game has not started).
                if (menu->settings().effects_half != shown_graphics.effects_half)
                    effects = menu->settings().effects_half ? "half" : "full";
                if (g_render_scale_edited)
                    render_scale = kMenuRenderScales[std::clamp(menu->settings().render_scale, 0, 2)];
                if (menu->settings().async_execute != shown_graphics.async_execute)
                    async_execute = menu->settings().async_execute ? 1 : 0;
                menu->set_in_game(true);
                menu->show(ui::Screen::None);
                window->renderer().set_ui(nullptr);
            }
#endif
            // The widescreen mod sizes the game's picture to the window (at start).
            if (const auto size = window->renderer().surface_size(); size.height > 0)
                opt.mods.display_aspect = double(size.width) / double(size.height);
            // Its picture is shown with the picture's own aspect ratio: the game can
            // squeeze it into a 16:9 crop of the window texture (iPhone, 21.10.).
            window->renderer().set_image_aspect([]() -> float {
                const auto p = mods::widescreen_active();
                return p.height > 0 ? float(p.width) / float(p.height) : 0.0f;
            });
            if (input_kind.empty() || input_kind == "sdl") {
                platform::desktop::SdlInputOptions io;
                if (touch_mode == "on") io.touch = true;
                if (touch_mode == "off") io.touch = false;
                io.touch_throw = touch_throw_mode != "off";
                io.touch_throw_with_gamepad = touch_throw_mode == "always";
                io.slots = input_slots();
                io.motion = pad_motion;
                io.player1 = player1_by_press ? platform::PlayerPick::Mode::Press : platform::PlayerPick::Mode::Order;
                // Scripted or reproducible runs (--press, --clock virtual): a
                // phone lying on the desk would feed sensor noise into player 1.
                if (!press_spec.empty() || opt.process.clock_mode == rt::ClockMode::Virtual)
                    io.device_motion = false;
                io.log = input_log;
                sdl_input = platform::desktop::SdlInput::create(io, &error);
                if (!sdl_input) {
                    std::fprintf(stderr, "lgp_boot: no input: %s\n", error.c_str());
                    return 3;
                }
                opt.input = sdl_input.get();
                if (const auto size = window->renderer().surface_size(); size.height > 0)
                    sdl_input->set_touch_aspect(float(size.width) / float(size.height));
                sdl_input->set_touch_window_points(window->height_points());
                sdl_input->set_touch_insets(window->safe_insets());  // later changes come as events
                // Touches on the game image follow the aspect mode (fit/fill/stretch);
                // with the widescreen mod the game's 1280x720 layout sits in the
                // middle of the wider picture.
                sdl_input->set_touch_image_mapper([r = &window->renderer()](float wx, float wy, float* ix, float* iy) {
                    const bool inside = r->window_to_image(wx, wy, ix, iy);
                    mods::widescreen_touch(ix, iy, mods::widescreen_active());
                    // `touch render 1`: the game reads touches in its render size.
                    const auto [sx, sy] = mods::widescreen_touch_scale(mods::widescreen_active());
                    hle::set_touch_scale(sx, sy);
                    return inside;
                });
#if defined(LGP_BOOT_APP)
                // The app's touch layout (settings.ini, the menu's settings screen).
                if (g_app_ui)
                    sdl_input->set_touch_layout(float(g_app_ui->settings.touch_size) / 100.0f,
                                                float(g_app_ui->settings.touch_opacity) / 100.0f,
                                                g_app_ui->settings.touch_mirrored);
                if (g_app_ui && touch_throw_mode.empty())
                    sdl_input->set_touch_throw(g_app_ui->settings.touch_throw, g_app_ui->settings.touch_throw_pad);
                if (g_app_ui) {
                    g_app_ui->pick_player1 = [in = sdl_input.get()] { in->pick_player1(); };
                    g_app_ui->player1_name = [in = sdl_input.get()] { return in->player1_name(); };
                }
#endif
                std::printf("input: sdl (keyboard, %zu gamepads%s)\n", sdl_input->gamepad_count(),
                            io.touch ? ", touch" : "");
            }
            if (keyboard_kind.empty() || keyboard_kind == "window" || keyboard_kind == "menu")
                sdl_text = std::make_unique<platform::desktop::SdlTextInput>(window->sdl_window());
#if defined(LGP_BOOT_APP) && defined(LGP_BOOT_UI)
            // One full-screen window (iOS, Android) or --keyboard menu: the
            // menu draws the keyboard; a dialog window would never show.
            if (sdl_text && menu &&
                (keyboard_kind == "menu" || platform::desktop::SdlTextInput::needs_host_drawn()))
                sdl_text->set_host_drawn(true);
#endif
            // Typing for the software keyboard goes to it, not to the controllers.
            void* menu_ptr = nullptr;
#if defined(LGP_BOOT_APP) && defined(LGP_BOOT_UI)
            menu_ptr = menu.get();
#endif
            window->set_event_handler([in = sdl_input.get(), text = sdl_text.get(), menu_ptr](const SDL_Event& e) {
#if defined(LGP_BOOT_APP) && defined(LGP_BOOT_UI)
                // While the game asks for text over the game image, the
                // keyboard dialog comes first (Escape cancels it, not the game).
                auto* m = static_cast<ui::AppMenu*>(menu_ptr);
                const bool typing = text != nullptr && !text->host_drawn() && text->active() && m != nullptr &&
                                    m->screen() == ui::Screen::None;
                if (m != nullptr && !typing && m->handle_event(e))
                    return;
#else
                (void)menu_ptr;
#endif
                if (text != nullptr && text->handle_event(e))
                    return;
                if (in != nullptr)
                    in->handle_event(e);
            });
            // Phones and tablets: in the background the game stops and the GPU
            // runs dry (iOS ends apps whose GPU work runs there); back in the
            // foreground it continues. Desktops never call this.
            opt.control = &process_control;
            window->set_lifecycle_handler([w = window.get(), control = &process_control, gb = wgpu_backend,
                                           &async_backend](bool background) {
                if (control->closing())
                    return;  // ending: no new pause, the game must reach its last present
                if (!background) {
                    gb->hold_prebuilds(false);
                    platform::reapply_thread_cpu_policy();
                    control->resume();
                    std::printf("app: foreground, game continues\n");
                    std::fflush(stdout);
                    return;
                }
                const auto t0 = std::chrono::steady_clock::now();
                // Pipeline workers too: warming the cache after a cold start
                // kept ~2 cores busy in the background on the OnePlus.
                const size_t compiling = gb->hold_prebuilds(true);
                const bool at_rest = control->pause(1000);
                // What the guest queued before it stopped; a stuck worker must
                // not freeze the window thread (Android: ANR), so with a limit.
                const bool drained = !at_rest || !async_backend || async_backend->drain_for(kAsyncDrainTimeoutMs);
                if (!drained)
                    std::printf("app: background, the GPU queue did not empty in %lld ms\n",
                                static_cast<long long>(kAsyncDrainTimeoutMs));
                if (at_rest && drained)
                    w->renderer().wait_idle();  // nothing submits any more
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - t0).count();
                std::printf("app: background, game paused in %lld ms, %zu pipeline(s) still compiling%s\n",
                            static_cast<long long>(ms), compiling,
                            at_rest ? "" : " (a guest thread did not stop; GPU not drained)");
                std::fflush(stdout);
            });
#else
            usage("--renderer window: this build has no SDL3 window");
#endif
        }
        // Asynchronous execution (docs/knowledge/zeitaufteilung-gast-threads.md):
        // LGP_WEBGPU_ASYNC_EXECUTE=N, N presents in flight (1 = one frame ahead,
        // 0 = off). Unset: CMake LGP_ASYNC_EXECUTE_DEFAULT with the WebGPU
        // renderer (1 on macOS, measured there; 0 elsewhere), on Android
        // LGP_ASYNC_EXECUTE_ANDROID or _ANDROID_TV by the device kind.
        int async_presents = 0;
        const char* async_from = "LGP_WEBGPU_ASYNC_EXECUTE";
        if (const char* v = std::getenv("LGP_WEBGPU_ASYNC_EXECUTE"); v != nullptr && *v != '\0') {
            async_presents = std::atoi(v);
        } else if (async_execute >= 0) {
            async_presents = async_execute;
            async_from = "--async-execute";
        } else {
#if defined(LGP_BOOT_WEBGPU)
            if (wgpu_backend != nullptr)
                async_presents = async_default(&async_from);
#endif
        }
        if (async_presents > 0) {
            gpu::nvn::AsyncBackend::Options ao;
            ao.max_presents_in_flight = uint32_t(std::clamp(async_presents, 1, 4));
            async_backend = std::make_unique<gpu::nvn::AsyncBackend>(*backend, ao);
            std::printf("backend: asynchronous execution (%u present(s) in flight, %s)\n", ao.max_presents_in_flight,
                        async_from);
        }
        opt.nvn.backend = async_backend ? static_cast<gpu::nvn::Backend*>(async_backend.get()) : backend.get();
        if (pacing < 0 ? windowed || !canvas.empty() : pacing == 1) {
            opt.nvn.pace_hz = 60;
            std::printf("present: paced (present interval at 60 Hz)\n");
        } else {
            const char* surface = "";
#if defined(LGP_BOOT_SDL)
            if (window) {
                const WGPUPresentMode m = window->renderer().present_mode();
                surface = m == WGPUPresentMode_Fifo      ? " (window: FIFO, waits for vsync)"
                          : m == WGPUPresentMode_Mailbox ? " (window: mailbox, no vsync wait)"
                                                         : " (window: immediate, no vsync wait)";
            }
#endif
            std::printf("present: unthrottled%s\n", surface);
        }
        opt.nvn.stop_requested = &stop_requested;
#if defined(LGP_BOOT_WEBGPU)
        if (wgpu_backend != nullptr) {
            // --effects: LGP_WEBGPU_HALFRES_PROGRAMS (the backend reads it) wins.
            if (const char* env = std::getenv("LGP_WEBGPU_HALFRES_PROGRAMS"); env == nullptr || *env == '\0') {
                std::string from = "--effects";
                const std::string mode = !effects.empty() ? effects : effects_default(&from);
                wgpu_backend->set_halfres_effects(mode == "half", LGP_BOOT_HALFRES_PROGRAMS);
                if (mode == "half")
                    std::printf("effects: half resolution (%s; programs %s)\n", from.c_str(),
                                LGP_BOOT_HALFRES_PROGRAMS);
            }
#if defined(LGP_BOOT_SDL)
            // "Play with Pikachu" (Paul 2026-10-22): only touches there, so the
            // on-screen controls hide while a program of that scene draws and
            // come back 30 frames after its last draw (CMake
            // LGP_TOUCH_ONLY_PROGRAMS; LGP_TOUCH_ONLY_PROGRAMS in the
            // environment wins, empty: off).
            if (sdl_input) {
                const char* env = std::getenv("LGP_TOUCH_ONLY_PROGRAMS");
                const std::string programs = env != nullptr ? env : LGP_BOOT_TOUCH_ONLY_PROGRAMS;
                if (!programs.empty()) {
                    wgpu_backend->watch_programs(programs);
                    present_hooks.push_back(
                        [wgpu_backend, input = sdl_input.get(), quiet = std::make_shared<int>(-1)](uint64_t) {
                            if (wgpu_backend->take_watched_drawn()) {
                                if (*quiet < 0)
                                    input->set_touch_only_scene(true);
                                *quiet = 0;
                            } else if (*quiet >= 0 && ++*quiet >= 30) {
                                input->set_touch_only_scene(false);
                                *quiet = -1;
                            }
                        });
                }
            }
#endif
            // --render-scale: LGP_WEBGPU_RENDER_SCALE (the backend reads it) wins.
            if (render_scale > 0.0f) {
                wgpu_backend->set_render_scale(render_scale);
                if (const char* env = std::getenv("LGP_WEBGPU_RENDER_SCALE"); (env == nullptr || *env == '\0') &&
                                                                              render_scale != 1.0f)
                    std::printf("render scale: %.2f (--render-scale)\n", double(render_scale));
            }
            opt.nvn.attach_memory = [wgpu_backend, async = async_backend.get()](const gpu::nvn::GuestMemoryView* m) {
                if (m == nullptr && async != nullptr)
                    async->drain();  // queued lists still read guest memory
                wgpu_backend->set_guest_memory(m);
            };
            if (pipeline_cache.empty())
                if (const char* dir = std::getenv("LGP_PRIVATE_DIR"); dir != nullptr && *dir != '\0')
                    pipeline_cache = std::string(dir) + "/cache/webgpu-pipelines.lgppipes";
#if defined(LGP_BOOT_APP)
            // The game program keeps one without LGP_PRIVATE_DIR too: in the
            // app's cache folder (lgp/boot/app.hpp).
            if (pipeline_cache.empty()) {
                std::string error;
                if (const auto dirs = platform::app_dirs(kAppName, &error)) {
                    const auto file = boot::app_files(dirs->config, dirs->data, dirs->cache).pipeline_cache;
                    std::error_code ec;
                    std::filesystem::create_directories(file.parent_path(), ec);
                    platform::exclude_from_backup(file.parent_path());
                    pipeline_cache = boot::path_to_arg(file);  // like app_arguments
                }
            }
#endif
            if (!pipeline_cache.empty() && pipeline_cache != "none") {
                std::string error;
                if (pipeline_cache_seed.empty() && !opt.game_dir.empty()) {
                    std::error_code ec;
                    const std::string seed = opt.game_dir + "/pipeline-seed.lgppipes";
                    if (std::filesystem::is_regular_file(seed, ec))
                        pipeline_cache_seed = seed;
                }
#if defined(LGP_BOOT_APP)
                // Else the seed the build put into the app package
                // (LGP_APP_PIPELINE_SEED), beside the packaged game folder.
                if (pipeline_cache_seed.empty()) {
                    std::error_code ec;
                    const auto packaged = platform::packaged_game_dir();
                    const auto seed = packaged.parent_path() / "pipeline-seed.lgppipes";
                    if (!packaged.empty() && std::filesystem::is_regular_file(seed, ec))
                        pipeline_cache_seed = boot::path_to_utf8(seed);
                }
#endif
                const bool cache_set = wgpu_backend->set_pipeline_cache(pipeline_cache, &error, pipeline_cache_seed);
                if (cache_set)
                    std::printf("pipeline cache: %s (%" PRIu64 " programs, %" PRIu64 " pipelines from earlier runs, %" PRIu64
                                " records from seed %s)\n",
                                pipeline_cache.c_str(), wgpu_backend->stats().pipeline_cache_programs,
                                wgpu_backend->stats().pipeline_cache_records, wgpu_backend->stats().pipeline_cache_seeded,
                                pipeline_cache_seed.empty() ? "-" : pipeline_cache_seed.c_str());
                else
                    std::printf("pipeline cache: off (%s: %s)\n", pipeline_cache.c_str(), error.c_str());
#if defined(LGP_BOOT_APP) && defined(LGP_BOOT_UI)
                if (cache_set && menu && !prepare_graphics(*menu, *window, *wgpu_backend))
                    return 0;  // window closed while preparing
#endif
            }
            // --time-stats N: also GPU memory and backpressure every N frames
            // (docs/knowledge/webgpu-backend-desktop-und-browser.md).
            if (time_stats_every != 0)
                present_hooks.push_back([wgpu_backend, every = time_stats_every,
                                         last = std::make_shared<gpu::webgpu::WebGpuBackend::Stats>()](uint64_t n) {
                    if (n % every != 0)
                        return;
                    const auto st = wgpu_backend->stats();
                    const auto peaks = wgpu_backend->take_window_peaks();
                    const double mib = 1.0 / (1024.0 * 1024.0);
                    std::printf("gpu memory, frames %" PRIu64 "-%" PRIu64 ": buffers live %" PRIu64 " (%.1f MiB, %" PRIu64
                                " created, %" PRIu64 " destroyed), textures live %" PRIu64 " (~%.1f MiB), queue writes %.2f MiB/frame, "
                                "frames in flight %" PRIu64 " (max %" PRIu64 "), presents waited %" PRIu64 " (%.1f ms/frame), "
                                "compute write-backs %" PRIu64 " (waited for %" PRIu64 ", %.1f ms/frame), sync waits %" PRIu64
                                " (%" PRIu64 " with write-backs pending), peak in window: buffers %.1f MiB, "
                                "textures ~%.1f MiB\n",
                                n - every + 1, n, st.buffers_created - st.buffers_destroyed,
                                double(st.buffer_bytes_created - st.buffer_bytes_destroyed) * mib,
                                st.buffers_created - last->buffers_created, st.buffers_destroyed - last->buffers_destroyed,
                                st.textures_created - st.textures_destroyed,
                                double(st.texture_bytes_created - st.texture_bytes_destroyed) * mib,
                                double(st.queue_write_bytes - last->queue_write_bytes) * mib / double(every),
                                st.frames_in_flight, st.frames_in_flight_max, st.frame_waits - last->frame_waits,
                                double(st.frame_wait_us - last->frame_wait_us) / 1000.0 / double(every),
                                st.writebacks - last->writebacks, st.writeback_waits - last->writeback_waits,
                                double(st.writeback_wait_us - last->writeback_wait_us) / 1000.0 / double(every),
                                st.finish_calls - last->finish_calls, st.finish_pending - last->finish_pending,
                                double(peaks.buffer_bytes) * mib, double(peaks.texture_bytes) * mib);
                    if (st.buffer_pool_reused + st.buffer_pool_created != 0) {  // LGP_WEBGPU_BUFFER_POOL=1
                        const uint64_t reused = st.buffer_pool_reused - last->buffer_pool_reused;
                        const uint64_t created = st.buffer_pool_created - last->buffer_pool_created;
                        std::printf("gpu buffer pool, frames %" PRIu64 "-%" PRIu64 ": per-list buffers reused %" PRIu64
                                    ", created %" PRIu64 " (%.0f%% reused), %.1f MiB free in the pool\n",
                                    n - every + 1, n, reused, created,
                                    reused + created != 0 ? 100.0 * double(reused) / double(reused + created) : 0.0,
                                    double(st.buffer_pool_free_bytes) * mib);
                    }
                    // What new content costs (stutter at a battle start, an effect's first use).
                    std::printf("gpu work, frames %" PRIu64 "-%" PRIu64 ": programs created %" PRIu64 " (%.1f ms), "
                                "draw pipelines created %" PRIu64 " (%.1f ms; prebuilt waited %" PRIu64 ", late %" PRIu64
                                ", draws skipped while compiling %" PRIu64 "), compute pipelines waited for %" PRIu64
                                " (%.1f ms), textures created %" PRIu64 ", texture uploads %.1f ms (%.2f MiB written)\n",
                                n - every + 1, n, st.programs_created - last->programs_created,
                                double(st.program_create_us - last->program_create_us) / 1000.0,
                                st.draw_pipelines_created - last->draw_pipelines_created,
                                double(st.draw_pipeline_create_us - last->draw_pipeline_create_us) / 1000.0,
                                st.prebuilds_waited - last->prebuilds_waited, st.prebuilds_late - last->prebuilds_late,
                                st.draws_skipped_compiling - last->draws_skipped_compiling,
                                st.compute_pipeline_waits - last->compute_pipeline_waits,
                                double(st.compute_pipeline_wait_us - last->compute_pipeline_wait_us) / 1000.0,
                                st.textures_created - last->textures_created,
                                double(st.texture_upload_us - last->texture_upload_us) / 1000.0,
                                double(st.texture_write_bytes - last->texture_write_bytes) * mib);
                    // Where the dispatches sit among the draws (Analyse 20, lever 1).
                    std::printf("gpu lists, frames %" PRIu64 "-%" PRIu64 ": lists %" PRIu64 ", with dispatches %" PRIu64
                                "; their draws before the first dispatch %" PRIu64 ", between first and last %" PRIu64
                                ", after the last %" PRIu64 "; compute first: dispatches %" PRIu64 " ahead, %" PRIu64
                                " in order, lists %" PRIu64 ", read back ahead %" PRIu64 "\n",
                                n - every + 1, n, st.lists - last->lists, st.dispatch_lists - last->dispatch_lists,
                                st.draws_before_dispatch - last->draws_before_dispatch,
                                st.draws_between_dispatches - last->draws_between_dispatches,
                                st.draws_after_dispatch - last->draws_after_dispatch,
                                st.dispatches_first - last->dispatches_first,
                                st.dispatches_in_order - last->dispatches_in_order,
                                st.lists_compute_first - last->lists_compute_first,
                                st.lists_written_back_first - last->lists_written_back_first);
                    // What the frames asked of the backend (memory beneath the
                    // API, such as Metal's command buffer pool, grows with it).
                    const uint64_t lists = st.lists - last->lists;
                    std::printf("gpu load, frames %" PRIu64 "-%" PRIu64 ": lists %" PRIu64 ", commands %.1f/list (max %" PRIu64
                                "), draws %.1f/frame, render passes %.1f/frame, "
                                "dispatches %.1f/frame, buffer-to-texture copies %" PRIu64 ", texture copies %" PRIu64
                                ", bind groups created %" PRIu64 " (reused %" PRIu64 "), constant uploads %.2f MiB/frame, "
                                "geometry uploads %.2f MiB/frame, textures evicted %" PRIu64 "\n",
                                n - every + 1, n, lists,
                                double(st.list_commands - last->list_commands) / double(std::max<uint64_t>(lists, 1)),
                                peaks.list_commands, double(st.draws - last->draws) / double(every),
                                double(st.render_passes - last->render_passes) / double(every),
                                double(st.dispatches - last->dispatches) / double(every), st.copies - last->copies,
                                st.texture_copies - last->texture_copies,
                                st.bind_groups_created - last->bind_groups_created,
                                st.bind_groups_reused - last->bind_groups_reused,
                                double(st.cbuf_upload_bytes - last->cbuf_upload_bytes) * mib / double(every),
                                double(st.geometry_upload_bytes - last->geometry_upload_bytes) * mib / double(every),
                                st.textures_evicted - last->textures_evicted);
                    // Why render passes begin and end (P7.3: tile GPUs load and
                    // store the attachments of every pass).
                    {
                        std::vector<std::pair<uint64_t, std::string>> causes;
                        for (const auto& [why, count] : st.pass_reasons) {
                            const auto was = last->pass_reasons.find(why);
                            const uint64_t d = count - (was != last->pass_reasons.end() ? was->second : 0);
                            if (d != 0) causes.emplace_back(d, why);
                        }
                        std::sort(causes.rbegin(), causes.rend());
                        std::string list;
                        for (const auto& [count, why] : causes)
                            list += (list.empty() ? "" : ", ") + why + " " + std::to_string(count);
                        const uint64_t draw_passes = st.draw_passes - last->draw_passes;
                        std::printf("gpu passes, frames %" PRIu64 "-%" PRIu64 ": render passes %" PRIu64
                                    " (%.1f/frame), draw passes %" PRIu64 " (%.1f draws each), new attachment sets %"
                                    PRIu64 "; %s\n",
                                    n - every + 1, n, st.render_passes - last->render_passes,
                                    double(st.render_passes - last->render_passes) / double(every), draw_passes,
                                    draw_passes != 0 ? double(st.draws - last->draws) / double(draw_passes) : 0.0,
                                    st.attachment_sets_new - last->attachment_sets_new, list.empty() ? "-" : list.c_str());
                    }
                    // LGP_WEBGPU_DEBUG=gputime: GPU execution time per frame
                    // and the compute programs that take most of it (by hash,
                    // as in the compute pipeline log; tools/analyze/wgsl_stats.py).
                    if (st.gpu_time_passes != last->gpu_time_passes) {
                        const auto per_frame = [&](uint64_t ns) { return double(ns) / 1e6 / double(every); };
                        std::vector<std::pair<uint64_t, uint64_t>> programs;  // ns, hash
                        for (const auto& [hash, t] : st.gpu_time_programs) {
                            const auto was = last->gpu_time_programs.find(hash);
                            const uint64_t ns = t.ns - (was != last->gpu_time_programs.end() ? was->second.ns : 0);
                            if (ns != 0) programs.emplace_back(ns, hash);
                        }
                        std::sort(programs.rbegin(), programs.rend());
                        std::string top;
                        for (size_t i = 0; i < programs.size() && i < 5; ++i) {
                            char item[64];
                            std::snprintf(item, sizeof(item), "%s%016llx %.2f", i ? ", " : "",
                                          (unsigned long long)programs[i].second, per_frame(programs[i].first));
                            top += item;
                        }
                        std::printf("gpu time, frames %" PRIu64 "-%" PRIu64 ": compute %.2f ms/frame (%" PRIu64
                                    " passes), render %.2f ms/frame (%" PRIu64 " passes), untimed %" PRIu64
                                    "; compute programs ms/frame: %s\n",
                                    n - every + 1, n, per_frame(st.gpu_time_compute.ns - last->gpu_time_compute.ns),
                                    st.gpu_time_compute.passes - last->gpu_time_compute.passes,
                                    per_frame(st.gpu_time_render.ns - last->gpu_time_render.ns),
                                    st.gpu_time_render.passes - last->gpu_time_render.passes,
                                    st.gpu_time_untimed - last->gpu_time_untimed, top.empty() ? "-" : top.c_str());
                        // The render passes that take most of it, by target
                        // (own line: tools parse the one above).
                        std::vector<std::tuple<uint64_t, uint64_t, std::string>> targets;  // ns, passes, target
                        for (const auto& [name, t] : st.gpu_time_targets) {
                            const auto was = last->gpu_time_targets.find(name);
                            const bool had = was != last->gpu_time_targets.end();
                            const uint64_t ns = t.ns - (had ? was->second.ns : 0);
                            const uint64_t passes = t.passes - (had ? was->second.passes : 0);
                            if (passes != 0) targets.emplace_back(ns, passes, name);
                        }
                        std::sort(targets.rbegin(), targets.rend());
                        std::string rank;
                        for (size_t i = 0; i < targets.size() && i < 10; ++i) {
                            const auto& [ns, passes, name] = targets[i];
                            char item[160];
                            std::snprintf(item, sizeof(item), "%s%s %.2f (%.1f passes)", i ? ", " : "", name.c_str(),
                                          per_frame(ns), double(passes) / double(every));
                            rank += item;
                        }
                        std::printf("gpu time by target, frames %" PRIu64 "-%" PRIu64 ": %zu targets, ms/frame: %s\n",
                                    n - every + 1, n, targets.size(), rank.empty() ? "-" : rank.c_str());
                        // Estimate: a pass's time split over its programs by draws.
                        std::vector<std::tuple<uint64_t, uint64_t, uint64_t>> draw_programs;  // ns, draws, hash
                        for (const auto& [hash, t] : st.gpu_time_draw_programs) {
                            const auto was = last->gpu_time_draw_programs.find(hash);
                            const bool had = was != last->gpu_time_draw_programs.end();
                            const uint64_t draws = t.passes - (had ? was->second.passes : 0);
                            if (draws != 0) draw_programs.emplace_back(t.ns - (had ? was->second.ns : 0), draws, hash);
                        }
                        std::sort(draw_programs.rbegin(), draw_programs.rend());
                        std::string drawn;
                        for (size_t i = 0; i < draw_programs.size() && i < 10; ++i) {
                            const auto& [ns, draws, hash] = draw_programs[i];
                            char item[96];
                            std::snprintf(item, sizeof(item), "%s%016llx %.2f (%.1f draws)", i ? ", " : "",
                                          (unsigned long long)hash, per_frame(ns), double(draws) / double(every));
                            drawn += item;
                        }
                        std::printf("gpu time by draw program, frames %" PRIu64 "-%" PRIu64
                                    ": %zu programs, ms/frame (pass time split by draws): %s\n",
                                    n - every + 1, n, draw_programs.size(), drawn.empty() ? "-" : drawn.c_str());
                    }
                    *last = st;
                });
            // Numbers only (no image leaves the machine): share of non-black
            // pixels, distinct colours and the mean colour of frame N.
            if (!frame_stats.empty())
                present_hooks.push_back([wgpu_backend, async = async_backend.get(), frame_stats, frame_dump,
                                         frame_grid](uint64_t n) {
                    if (std::find(frame_stats.begin(), frame_stats.end(), n) == frame_stats.end())
                        return;
                    // On the worker right after the queued work (frame n itself,
                    // not the one before; never beside the worker's next task).
                    // With several guest threads running at once (not --sched
                    // baton) another thread may queue work between the present
                    // and here; the single baton (--clock virtual) runs none.
                    std::vector<uint8_t> px;
                    uint32_t w = 0, h = 0;
                    bool read = false;
                    const auto read_frame = [&] { read = wgpu_backend->read_presented(&px, &w, &h); };
                    if (async != nullptr)
                        async->run_in_order(read_frame);
                    else
                        read_frame();
                    if (!read || px.empty()) {
                        std::printf("frame %" PRIu64 ": not readable\n", n);
                        return;
                    }
                    if (!frame_dump.empty()) {
                        std::string path = frame_dump;
                        if (const size_t at = path.find("{n}"); at != std::string::npos)
                            path.replace(at, 3, std::to_string(n));
                        if (FILE* f = std::fopen(path.c_str(), "wb")) {
                            std::fprintf(f, "P6\n%u %u\n255\n", w, h);
                            for (size_t i = 0; i + 3 < px.size(); i += 4) std::fwrite(&px[i], 1, 3, f);
                            std::fclose(f);
                        }
                    }
                    uint64_t lit = 0, sum[3] = {0, 0, 0};
                    std::vector<uint32_t> colours;
                    for (size_t i = 0; i + 3 < px.size(); i += 4) {
                        if (px[i] | px[i + 1] | px[i + 2])
                            ++lit;
                        for (int c = 0; c < 3; ++c) sum[c] += px[i + c];
                        if (i % 64 == 0) colours.push_back(px[i] | (px[i + 1] << 8) | (px[i + 2] << 16));
                    }
                    std::sort(colours.begin(), colours.end());
                    colours.erase(std::unique(colours.begin(), colours.end()), colours.end());
                    const uint64_t total = px.size() / 4;
                    std::printf("frame %" PRIu64 ": %ux%u, non-black %.1f %%, colours (sampled) %zu, mean %llu %llu %llu\n",
                                n, w, h, 100.0 * double(lit) / double(total), colours.size(),
                                (unsigned long long)(sum[0] / total), (unsigned long long)(sum[1] / total),
                                (unsigned long long)(sum[2] / total));
                    // --frame-grid: mean colour of each of 32x18 cells, for a local
                    // comparison of two builds (the log stays in LGP_PRIVATE_DIR).
                    if (frame_grid && w >= 32 && h >= 18) {
                        std::string line = "  grid 32x18:";
                        for (uint32_t cy = 0; cy < 18; ++cy)
                            for (uint32_t cx = 0; cx < 32; ++cx) {
                                uint64_t acc[3] = {0, 0, 0}, cnt = 0;
                                for (uint32_t y = cy * h / 18; y < (cy + 1) * h / 18; ++y)
                                    for (uint32_t x = cx * w / 32; x < (cx + 1) * w / 32; ++x, ++cnt)
                                        for (int c = 0; c < 3; ++c) acc[c] += px[(size_t(y) * w + x) * 4 + c];
                                char cell[8];
                                std::snprintf(cell, sizeof(cell), " %02x%02x%02x", unsigned(acc[0] / cnt),
                                              unsigned(acc[1] / cnt), unsigned(acc[2] / cnt));
                                line += cell;
                            }
                        std::printf("%s\n", line.c_str());
                    }
                    const auto st = wgpu_backend->stats();
                    std::printf("  backend: clears %llu, draws %llu, skipped %llu, invalid programs %llu, invalid "
                                "pipelines %llu, uploads %llu, copies %llu, unbound textures %llu, dispatches %llu "
                                "(vertex streams from dispatch output %llu, constant buffers through global memory %llu, "
                                "lists written back %llu, waits for them %llu in %.1f ms)%c",
                                (unsigned long long)st.clears, (unsigned long long)st.draws,
                                (unsigned long long)st.skipped_commands, (unsigned long long)st.invalid_programs,
                                (unsigned long long)st.invalid_pipelines, (unsigned long long)st.uploads,
                                (unsigned long long)st.copies, (unsigned long long)st.unbound_textures,
                                (unsigned long long)st.dispatches, (unsigned long long)st.compute_vertex_streams,
                                (unsigned long long)st.compute_uniform_slots, (unsigned long long)st.writebacks,
                                (unsigned long long)st.writeback_waits, st.writeback_wait_us / 1000.0, 10);
                    std::printf("  textures: copies %llu (blits %llu), ASTC textures %llu, ASTC levels decoded on the CPU "
                                "%llu, ASTC error blocks %llu, 3D textures %llu; sampled: ASTC %llu, 3D %llu\n",
                                (unsigned long long)st.texture_copies, (unsigned long long)st.blits,
                                (unsigned long long)st.astc_textures, (unsigned long long)st.astc_cpu_decoded,
                                (unsigned long long)st.astc_error_blocks, (unsigned long long)st.textures_3d,
                                (unsigned long long)st.astc_bindings, (unsigned long long)st.bindings_3d);
                    // Totals since start (docs/knowledge/webgpu-backend-draw-kosten.md).
                    std::printf("  uploads: constant buffers %llu (%llu bytes, reused %llu), vertices/indices %llu "
                                "(%llu bytes, reused %llu, unchanged since an earlier list %llu), bind groups "
                                "created %llu, reused %llu%c",
                                (unsigned long long)st.cbuf_uploads, (unsigned long long)st.cbuf_upload_bytes,
                                (unsigned long long)st.cbuf_reused, (unsigned long long)st.geometry_uploads,
                                (unsigned long long)st.geometry_upload_bytes, (unsigned long long)st.geometry_reused,
                                (unsigned long long)st.geometry_cached,
                                (unsigned long long)st.bind_groups_created, (unsigned long long)st.bind_groups_reused, 10);
                    std::printf("  shaders: programs created %llu (%.1f ms), draw pipelines created %llu (%.1f ms); "
                                "textures from pools %.1f ms (%llu checks: read and fingerprint %.1f ms for %.1f "
                                "MiB, deswizzle and convert %.1f ms, writeTexture %llu calls %.1f ms for %.1f MiB; "
                                "%llu pitch-linear %.1f ms for %.1f MiB)%c",
                                (unsigned long long)st.programs_created, st.program_create_us / 1000.0,
                                (unsigned long long)st.draw_pipelines_created, st.draw_pipeline_create_us / 1000.0,
                                st.texture_upload_us / 1000.0, (unsigned long long)st.texture_checks,
                                st.texture_read_us / 1000.0, st.texture_read_bytes / 1048576.0,
                                st.texture_convert_us / 1000.0, (unsigned long long)st.texture_writes,
                                st.texture_write_us / 1000.0, st.texture_write_bytes / 1048576.0,
                                (unsigned long long)st.texture_linear_checks, st.texture_linear_us / 1000.0,
                                st.texture_linear_bytes / 1048576.0, 10);
                    std::printf("  pipeline cache: %" PRIu64 " programs at start, %" PRIu64 " created ahead, %" PRIu64
                                " taken over by the game, %" PRIu64 " compute pipelines from the background (%" PRIu64
                                " dispatches waited for one, %.1f ms)\n",
                                st.pipeline_cache_programs, st.programs_warmed, st.programs_adopted,
                                st.compute_pipelines_prebuilt, st.compute_pipeline_waits,
                                st.compute_pipeline_wait_us / 1000.0);
                    std::printf("  pipeline cache: %" PRIu64 " recipes at start, %" PRIu64 " pipelines created ahead "
                                "and used (%" PRIu64 " waited for), %" PRIu64 " too late, %" PRIu64 " failed, "
                                "%.1f ms in the background\n",
                                st.pipeline_cache_records, st.pipelines_prebuilt, st.prebuilds_waited,
                                st.prebuilds_late, st.prebuilds_failed, st.prebuild_us / 1000.0);
                    for (const auto& rt : wgpu_backend->rendered_textures()) {
                        std::vector<uint8_t> tp;
                        uint32_t tw = 0, th = 0;
                        if (!wgpu_backend->read_texture(rt.id, &tp, &tw, &th) || tp.empty())
                            continue;
                        uint64_t lit_t = 0;
                        for (size_t i = 0; i + 3 < tp.size(); i += 4)
                            lit_t += (tp[i] | tp[i + 1] | tp[i + 2]) != 0;
                        std::printf("  target #%u %ux%u format %u: non-black %.1f %%%c", rt.id, tw, th, rt.format,
                                    100.0 * double(lit_t) / double(tp.size() / 4), 10);
                    }
                    for (const auto& [reason, count] : st.skip_reasons)
                        std::printf("  skipped %llu: %s%c", (unsigned long long)count, reason.c_str(), 10);
                    for (const auto& [name, count] : st.diagnostics)
                        std::printf("  diag %llu: %s%c", (unsigned long long)count, name.c_str(), 10);
                    std::fflush(stdout);
                });
        }
#endif
    }

    if (input_kind == "sdl" && !windowed)
        usage("--input sdl needs --graphics hle --renderer window");
    // --press: scripted player 1 (lgp/boot/press_script.hpp), sampled at every
    // hid poll from presented frames, guest time and movie:: events; outlives boot().
    std::unique_ptr<boot::ScriptedInput> scripted;
    if (!press_spec.empty()) {
        std::vector<boot::Press> presses;
        std::string error;
        if (!boot::parse_press_script(press_spec, &presses, &error))
            usage(("--press: " + error).c_str());
        auto frames = std::make_shared<std::atomic<uint64_t>>(0);
        // stable / +after-stable: hash the presented image (never kept) at most
        // every kImageInterval of guest time; a change shows up that much later.
        auto image = std::make_shared<boot::ImageWatch>();
        if (boot::uses_image(presses)) {
            if (opt.graphics != boot::GraphicsMode::Hle)
                usage("--press: stable and +after-stable need --graphics hle");
            constexpr double kImageInterval = 1.0 / 30;
            std::function<bool(std::vector<uint8_t>*)> read;
#if defined(LGP_BOOT_WEBGPU)
            if (wgpu_backend != nullptr)
                read = [wgpu_backend, async = async_backend.get()](std::vector<uint8_t>* px) {
                    if (async == nullptr)
                        return wgpu_backend->read_presented(px, nullptr, nullptr);
                    bool ok = false;  // on the worker, as for --frame-stats
                    async->run_in_order([&] { ok = wgpu_backend->read_presented(px, nullptr, nullptr); });
                    return ok;
                };
#endif
            if (null_renderer != nullptr)
                read = [r = null_renderer.get()](std::vector<uint8_t>* px) {
                    platform::SurfaceSize size;
                    *px = r->last_image(&size);
                    return true;
                };
            auto last = std::make_shared<double>(-1.0);  // presenting thread only
            present_hooks.push_back([image, read, last](uint64_t) {
                const double now = double(rt::system_tick()) / double(rt::kCounterFrequency);
                if (*last >= 0 && now - *last < kImageInterval)
                    return;
                *last = now;
                std::vector<uint8_t> px;
                if (read(&px) && !px.empty())
                    image->observe(boot::hash_image(px.data(), px.size()), now);
            });
            std::printf("input: watching the presented image (hash every %.0f ms)\n", kImageInterval * 1000);
        }
        platform::ControllerState idle;
        idle.style = player1;
        // p2: player 2 on npad No2, a single Joy-Con when player 1 is handheld.
        const size_t players = boot::press_players(presses);
        platform::ControllerState idle2;
        idle2.style = player1 == platform::ControllerStyle::Handheld ? platform::ControllerStyle::JoyconRight : player1;
        const size_t count = presses.size();
        const auto throws_of = [&presses](uint8_t player) {
            return std::any_of(presses.begin(), presses.end(), [player](const boot::Press& p) {
                return p.player == player && (p.buttons & boot::kPressThrow) != 0;
            });
        };
        const bool throws = throws_of(0);
        const bool throws2 = throws_of(1);
        boot::PressScript press_script(std::move(presses));
        press_script.on_fire = [](size_t i, uint64_t frame, double seconds) {
            std::printf("input: press %zu at frame %" PRIu64 ", %.2f s\n", i + 1, frame, seconds);
        };
        // KEYS "throw": the synthetic Joy-Con gesture of the window's Space
        // key (ControllerMapper, tuned by --throw), timed by guest seconds so
        // --clock virtual runs repeat; the Joy-Con then also rests in the hand
        // (gravity) between throws.
        boot::ScriptedInput::MotionSource motion;
        if (throws) {
            if (player1 != platform::ControllerStyle::JoyconRight && player1 != platform::ControllerStyle::JoyconLeft)
                usage("--press: throw needs --controller joycon-right or joycon-left");
            auto mapper = std::make_shared<platform::ControllerMapper>(input_slots()[0]);
            motion = [mapper](bool throwing, double seconds) {
                platform::HostPad pad;
                pad.connected = true;
                pad.throw_gesture = throwing;
                return mapper->map(pad, uint64_t(seconds * 1e9) + 1).motion;  // 0 means "no throw running"
            };
        }
        // p2:…:throw: the same gesture on player 2's Joy-Con, the shake that
        // lets player 2 join in LGP.
        boot::ScriptedInput::MotionSource motion2;
        if (throws2) {
            platform::MapperConfig slot2 = input_slots()[1];
            slot2.style = idle2.style;
            if (!throw_spec.empty())
                platform::parse_throw_spec(throw_spec, slot2);  // --throw tunes both players' scripted throws
            auto mapper = std::make_shared<platform::ControllerMapper>(slot2);
            motion2 = [mapper](bool throwing, double seconds) {
                platform::HostPad pad;
                pad.connected = true;
                pad.throw_gesture = throwing;
                return mapper->map(pad, uint64_t(seconds * 1e9) + 1).motion;
            };
        }
        scripted = std::make_unique<boot::ScriptedInput>(std::move(press_script), idle, [frames, image] {
            boot::PressClock now;
            now.frame = frames->load();
            now.seconds = double(rt::system_tick()) / double(rt::kCounterFrequency);
            now.events[size_t(boot::PressEvent::MovieStart)] = boot::MovieHle::video_starts();
            now.events[size_t(boot::PressEvent::MovieEnd)] = boot::MovieHle::video_ends();
            now.events[size_t(boot::PressEvent::BattleStart)] = mods::wild_battles_started();
            now.events[size_t(boot::PressEvent::BattleEnd)] = mods::wild_battles_done();
            now.image_changed = image->last_change();
            return now;
        }, std::move(motion), players, idle2, std::move(motion2));
        // touch@X/Y: picture pixels as a finger on the window's game picture
        // would give them (the widescreen mod's mapping included).
        scripted->set_touch_mapper([](float x, float y) {
            mods::PictureSize picture = mods::widescreen_active();
            if (picture.width == 0 || picture.height == 0)
                picture = {1280, 720};
            float u = x / float(picture.width), v = y / float(picture.height);
            mods::widescreen_touch(&u, &v, picture);
            const auto [sx, sy] = mods::widescreen_touch_scale(picture);
            hle::set_touch_scale(sx, sy);
            platform::TouchPoint p;
            p.x = std::clamp(u, 0.0f, 1.0f) * platform::kTouchScreenWidth;
            p.y = std::clamp(v, 0.0f, 1.0f) * platform::kTouchScreenHeight;
            return p;
        });
        opt.input = scripted.get();
        input_kind = "script";
        std::printf("input: script (%zu presses%s)\n", count, players > 1 ? ", players 1 and 2" : "");
        present_hooks.push_back([frames](uint64_t n) { frames->store(n); });
    }
    if (time_stats_every != 0) {
        const uint32_t tick = time_stats_tick >= 0 ? uint32_t(time_stats_tick) : boot::time_stats_tick_us();
        if (tick != 0) {
            rt::set_time_stats_tick_us(tick);
            std::printf("time stats: clock ticks every %u us\n", tick);
        }
        rt::enable_time_stats(true);
        // Slot handovers between guest threads and how long the woken one
        // took to run (wakeup latency, P7.3).
        rt::time_stats_add_source("wakeups", [](uint64_t* busy, uint64_t* calls) {
            rt::scheduler_wake_latency(busy, calls);
        });
#if defined(LGP_BOOT_WEBGPU)
        platform::webgpu::WebGpuRenderer* timed = gpu_renderer.get();
#if defined(LGP_BOOT_SDL)
        if (timed == nullptr && window)
            timed = &window->renderer();  // --renderer window: the window owns it
#endif
        if (timed != nullptr) {
            rt::time_stats_add_source("gpu thread", [r = timed](uint64_t* busy, uint64_t* calls) {
                *busy = r->run_busy_ns();
                *calls = r->run_calls();
            });
            // What "gpu thread" and the guest's "gpu" hold besides encoding
            // (Analyse 39): waits for another caller's task, surface acquire,
            // the blit's submit, the present, the lists' submits, the warm-up.
            // Waits after a task for the GPU, counted in device polls: far
            // more polls than waits mean the loop spins (P7.3, OnePlus).
            rt::time_stats_add_source("gpu wait polls", [r = timed](uint64_t* busy, uint64_t* calls) {
                *busy = r->run_wait_ns();
                *calls = r->run_wait_polls();
            });
            rt::time_stats_add_source("gpu lock wait", [r = timed](uint64_t* busy, uint64_t* calls) {
                *busy = r->run_lock_wait_ns();
                *calls = r->run_lock_waits();
            });
            rt::time_stats_add_source("gpu surface acquire", [r = timed](uint64_t* busy, uint64_t* calls) {
                *busy = r->surface_acquire_ns();
                *calls = r->surface_presents();
            });
            rt::time_stats_add_source("gpu surface submit", [r = timed](uint64_t* busy, uint64_t* calls) {
                *busy = r->surface_submit_ns();
                *calls = r->surface_presents();
            });
            rt::time_stats_add_source("gpu surface present", [r = timed](uint64_t* busy, uint64_t* calls) {
                *busy = r->surface_present_ns();
                *calls = r->surface_presents();
            });
        }
        if (wgpu_backend != nullptr) {
            rt::time_stats_add_source("gpu list submits", [b = wgpu_backend](uint64_t* busy, uint64_t* calls) {
                uint64_t steps = 0, step_ns = 0;
                b->submit_times(calls, busy, &steps, &step_ns);
            });
            rt::time_stats_add_source("gpu warm-up", [b = wgpu_backend](uint64_t* busy, uint64_t* calls) {
                uint64_t submits = 0, submit_ns = 0;
                b->submit_times(&submits, &submit_ns, calls, busy);
            });
        }
#endif
        // The guest's "gpu" column by backend call (lists recorded, presents,
        // finish waits, texture writes ...).
        for (size_t i = 0; i < gpu::nvn::backend_call_times().size(); ++i)
            rt::time_stats_add_source(std::string("gpu call ") + gpu::nvn::backend_call_times()[i].kind,
                                      [i](uint64_t* busy, uint64_t* calls) {
                                          const gpu::nvn::BackendCallTime t = gpu::nvn::backend_call_times()[i];
                                          *busy = t.ns;
                                          *calls = t.calls;
                                      });
        if (async_backend)
            rt::time_stats_add_source("gpu queue", [a = async_backend.get()](uint64_t* busy, uint64_t* calls) {
                const auto st = a->stats();
                *busy = st.worker_busy_ns;
                *calls = st.tasks;
            });
        if (async_backend)
            present_hooks.push_back([a = async_backend.get(), every = time_stats_every,
                                     last = std::make_shared<gpu::nvn::AsyncBackend::Stats>()](uint64_t n) {
                if (n % every != 0)
                    return;
                const auto st = a->stats();
                const auto per_frame = [every](uint64_t ns) { return double(ns) / 1e6 / double(every); };
                std::printf("gpu queue, frames %" PRIu64 "-%" PRIu64 ": %" PRIu64 " tasks, waits for presents %" PRIu64
                            " (%.2f ms/frame), for fences/finish %" PRIu64 " (%.2f ms/frame), queue full %" PRIu64
                            ", longest queue %" PRIu64 "\n",
                            n - every + 1, n, st.tasks - last->tasks, st.present_waits - last->present_waits,
                            per_frame(st.present_wait_ns - last->present_wait_ns), st.fence_waits - last->fence_waits,
                            per_frame(st.fence_wait_ns - last->fence_wait_ns), st.queue_full_waits - last->queue_full_waits,
                            st.max_queued);
                *last = st;
            });
        present_hooks.push_back([every = time_stats_every, control = &process_control](uint64_t n) {
            if (n % every != 0)
                return;
            for (const std::string& line : rt::time_stats_report(n - every + 1, every))
                std::printf("%s\n", line.c_str());
            // What committing and releasing guest memory cost in this window
            // (all of it counts as guest time; the load stall with the file
            // window, docs/analysis/42): calls, MiB and ms per kind.
            {
                using Op = platform::HostBlock::Op;
                static platform::HostBlock::OpTotals last[size_t(Op::Count)];
                std::string line;
                for (size_t i = 0; i < size_t(Op::Count); ++i) {
                    const platform::HostBlock::OpTotals t = platform::HostBlock::op_totals(Op(i));
                    const uint64_t calls = t.calls - last[i].calls;
                    if (calls != 0) {
                        char item[128];
                        std::snprintf(item, sizeof(item), "%s%s %" PRIu64 " (%.1f MiB, %.2f ms)",
                                      line.empty() ? "" : ", ", platform::HostBlock::op_name(Op(i)), calls,
                                      double(t.bytes - last[i].bytes) / (1024.0 * 1024.0),
                                      double(t.ns - last[i].ns) / 1e6);
                        line += item;
                    }
                    last[i] = t;
                }
                if (!line.empty())
                    std::printf("host block ops, frames %" PRIu64 "-%" PRIu64 ": %s\n", n - every + 1, n,
                                line.c_str());
            }
            // CPU time and page faults of the process in this window: whether
            // guest time is kernel time (page faults) or user time.
            if (platform::ProcessCpu cpu; platform::process_cpu(&cpu)) {
                static platform::ProcessCpu last{};  // first window: since the start
                std::printf("host cpu, frames %" PRIu64 "-%" PRIu64 ": user %.1f ms, system %.1f ms, faults %" PRIu64
                            ", pageins %" PRIu64 ", cow %" PRIu64 ", syscalls %" PRIu64 ", mach %" PRIu64
                            ", csw %" PRIu64 "\n",
                            n - every + 1, n, double(cpu.user_ns - last.user_ns) / 1e6,
                            double(cpu.system_ns - last.system_ns) / 1e6, cpu.faults - last.faults,
                            cpu.pageins - last.pageins, cpu.cow - last.cow, cpu.syscalls - last.syscalls,
                            cpu.mach - last.mach, cpu.csw - last.csw);
                last = cpu;
            }
            // GPU waits the guest asks for (nvnQueueFinish, nvnSyncWait):
            // with the graphics thread each one waits for its queue.
            {
                static gpu::nvn::WaitStats last{};
                const gpu::nvn::WaitStats w = gpu::nvn::wait_stats();
                const auto per_frame = [every](uint64_t ns) { return double(ns) / 1e6 / double(every); };
                std::printf("nvn waits, frames %" PRIu64 "-%" PRIu64 ": queue finish %" PRIu64
                            " (%.2f ms/frame), sync wait %" PRIu64 " (%.2f ms/frame)\n",
                            n - every + 1, n, w.finish_calls - last.finish_calls,
                            per_frame(w.finish_ns - last.finish_ns), w.sync_calls - last.sync_calls,
                            per_frame(w.sync_ns - last.sync_ns));
                last = w;
            }
            // Native start: entries into guest code and gate passes, to
            // relate the system time and its calls to them.
            if (const rt::native::Stats ns = rt::native::stats(); ns.entries != 0) {
                static rt::native::Stats last{};
                std::printf("native, frames %" PRIu64 "-%" PRIu64 ": entries %" PRIu64 ", svcs %" PRIu64
                            ", host calls %" PRIu64 ", sysregs %" PRIu64 ", kicks %" PRIu64 "\n",
                            n - every + 1, n, ns.entries - last.entries, ns.svcs - last.svcs,
                            ns.host_calls - last.host_calls, ns.sysregs - last.sysregs, ns.kicks - last.kicks);
                last = ns;
            }
            // The whole process as the OS counts it (jetsam on iOS/tvOS
            // compares the footprint with its limit, P7.4).
            if (platform::ProcessMemory mem; platform::process_memory(&mem)) {
                const double mib = 1.0 / (1024.0 * 1024.0);
                // Guest share: only with a lifecycle control (--renderer window).
                char guest[64] = "";
                if (uint64_t resident = 0; control->guest_resident(&resident))
                    std::snprintf(guest, sizeof(guest), ", guest resident %.1f MiB", double(resident) * mib);
                // GPU resources as Metal counts them (Apple): graphics memory in
                // the footprint beyond this is not Metal resources.
                char gpu[128] = "";
                if (platform::GpuMemory g; platform::gpu_memory(&g))
                    std::snprintf(gpu, sizeof(gpu), ", metal allocated %.1f MiB (working set %.1f MiB)",
                                  double(g.allocated) * mib, double(g.working_set) * mib);
                // Purgeable (Apple): volatile is graphics memory the system may
                // drop without a kill; nonvolatile counts in the footprint.
                char purge[96] = "";
                if (mem.has_purgeable)
                    std::snprintf(purge, sizeof(purge), ", purgeable %.1f MiB, volatile %.1f MiB",
                                  double(mem.purgeable_nonvolatile) * mib, double(mem.purgeable_volatile) * mib);
                // The machine around it: a kill under system-wide shortage
                // leaves the process below its own limit.
                char sysmem[128] = "";
                if (platform::SystemMemory sys; platform::system_memory(&sys))
                    std::snprintf(sysmem, sizeof(sysmem),
                                  ", system free %.1f MiB, file cache %.1f MiB, compressed %.1f MiB, level %d %%",
                                  double(sys.free) * mib, double(sys.file_cache) * mib, double(sys.compressed) * mib,
                                  sys.level);
                std::printf("host memory, frame %" PRIu64 ": footprint %.1f MiB, peak %.1f MiB, available %.1f MiB%s%s%s%s\n",
                            n, double(mem.footprint) * mib, double(mem.peak) * mib, double(mem.available) * mib, guest,
                            gpu, purge, sysmem);
                // Where the footprint sits (Darwin, Linux/Android; LGP_HOST_MEMORY_PARTS=1,
                // docs/analysis/32-metal-speicher-je-pipeline.md): parts of 1 MiB and more.
                static const bool parts_wanted = [] {
                    const char* v = std::getenv("LGP_HOST_MEMORY_PARTS");
                    return v != nullptr && std::string(v) == "1";
                }();
                if (std::vector<platform::MemoryPart> parts; parts_wanted && platform::memory_breakdown(&parts)) {
                    std::string line;
                    for (const platform::MemoryPart& p : parts) {
                        if (p.bytes < (1u << 20))
                            continue;
                        char item[96];
                        std::snprintf(item, sizeof(item), "%s%s %.1f", line.empty() ? "" : ", ", p.name.c_str(),
                                      double(p.bytes) * mib);
                        line += item;
                    }
                    std::printf("host memory parts, frame %" PRIu64 " (MiB): %s\n", n, line.c_str());
                }
            }
            // LGP_HOST_BLOCK_REGIONS=MiB: guest memory per bucket, resident
            // and (Darwin) dirty, to pick LGP_HOST_BLOCK_FILE_RANGE. Buckets
            // without resident pages are left out.
            static const uint64_t region_mib = [] {
                // MiB, 1 to 65536; anything else turns the line off.
                const char* v = std::getenv("LGP_HOST_BLOCK_REGIONS");
                const unsigned long long n = v != nullptr && *v >= '0' && *v <= '9' ? std::strtoull(v, nullptr, 10) : 0;
                return n <= 65536 ? uint64_t(n) : uint64_t(0);
            }();
            if (boot::ProcessControl::GuestRegions g; region_mib > 0 && control->guest_regions(region_mib << 20, &g)) {
                const uint64_t bucket = region_mib << 20;
                const bool dirty = platform::HostBlock::dirty_known();
                std::string line = "guest regions, frame " + std::to_string(n) + " (" + std::to_string(region_mib) +
                                   " MiB, resident" + (dirty ? "/dirty" : "") + " MiB";
                if (g.file_end > g.file_begin)
                    line += ", file " + std::to_string(g.file_begin >> 20) + "-" + std::to_string(g.file_end >> 20);
                line += "):";
                for (size_t i = 0; i < g.regions.size(); ++i) {
                    const auto& r = g.regions[i];
                    if (r.resident == 0 && r.dirty == 0)
                        continue;
                    char cell[96];
                    if (dirty)
                        std::snprintf(cell, sizeof(cell), " %llu@0x%llx=%.0f/%.0f", (unsigned long long)(i * region_mib),
                                      (unsigned long long)(g.guest_start + i * bucket), double(r.resident) / (1 << 20),
                                      double(r.dirty) / (1 << 20));
                    else
                        std::snprintf(cell, sizeof(cell), " %llu@0x%llx=%.0f", (unsigned long long)(i * region_mib),
                                      (unsigned long long)(g.guest_start + i * bucket), double(r.resident) / (1 << 20));
                    line += cell;
                }
                std::printf("%s\n", line.c_str());
            }
            std::fflush(stdout);
        });
    }
#if defined(LGP_BOOT_APP) && defined(LGP_BOOT_UI)
    // --menu-script steps at a game frame hold the game there (MenuGate);
    // last, so the hooks above see the frame first.
    if (menu && !menu_script.empty() && opt.control != nullptr) {
        menu_script.gate->control = opt.control;
        present_hooks.push_back([gate = menu_script.gate](uint64_t n) { MenuScript::hold_at(*gate, n); });
    }
#endif
    if (!present_hooks.empty())
        opt.nvn.on_present = [hooks = present_hooks](uint64_t n) {
            for (const auto& h : hooks) h(n);
        };
    // Sound output for audout (P5.2); outlives boot().
#if defined(LGP_BOOT_SDL_AUDIO)
    std::unique_ptr<platform::desktop::SdlAudio> sdl_audio;
#endif
    // Without a window the host's own devices, if it has any (the browser:
    // Gamepad API, keyboard, Web Audio; lgp/boot/host.hpp).
    const boot::HostDevices host_devices = boot::host_devices(input_slots());
    if (input_kind.empty() && !windowed && host_devices.input != nullptr) {
        opt.input = host_devices.input;
        std::printf("input: host\n");
    }
    if (audio_kind.empty() && !windowed && host_devices.audio != nullptr) {
        opt.audio = host_devices.audio;
        audio_kind = "host";
        std::printf("audio: host\n");
    }
    // Software keyboard (swkbd): without one the game gets its initial text
    // or the account nickname.
    std::unique_ptr<platform::ITextInput> own_text;
    if (keyboard_kind.rfind("text:", 0) == 0) {
        own_text = std::make_unique<platform::FixedTextInput>(keyboard_kind.substr(5));
        opt.text_input = own_text.get();
    } else if (keyboard_kind == "console") {
        own_text = std::make_unique<platform::ConsoleTextInput>(std::cin, std::cout);
        opt.text_input = own_text.get();
    } else if (keyboard_kind == "window" || keyboard_kind == "menu" || (keyboard_kind.empty() && windowed)) {
#if defined(LGP_BOOT_SDL)
        opt.text_input = sdl_text.get();
        if (sdl_text && sdl_text->host_drawn()) {
            keyboard_kind = "menu";
        } else if (sdl_text && keyboard_kind == "menu") {
            usage("--keyboard menu needs the app menu (--app, not --menu off)");
        } else if (sdl_text && platform::desktop::SdlTextInput::needs_host_drawn()) {
            // One full-screen window and no menu to draw the keyboard: a dialog
            // window would never show and the game would wait for it forever.
            opt.text_input = nullptr;
            if (keyboard_kind == "window")
                usage("--keyboard window: this platform shows no second window, use the app menu");
        }
#endif
        if (opt.text_input == nullptr && !keyboard_kind.empty())
            usage("--keyboard window|menu needs --renderer window");
        if (keyboard_kind.empty() && opt.text_input != nullptr)
            keyboard_kind = "window";
    } else if (keyboard_kind == "host" || (keyboard_kind.empty() && host_devices.text != nullptr)) {
        if (host_devices.text == nullptr)
            usage("--keyboard host: this platform has no text input");
        opt.text_input = host_devices.text;
        keyboard_kind = "host";
    }
    std::printf("keyboard: %s\n", opt.text_input != nullptr ? keyboard_kind.substr(0, keyboard_kind.find(':')).c_str()
                                                             : "none (initial text or nickname)");
    if (audio_kind.empty())
        audio_kind = windowed ? "sdl" : "none";
    if (audio_kind == "sdl") {
#if defined(LGP_BOOT_SDL_AUDIO)
        std::string error;
        sdl_audio = platform::desktop::SdlAudio::create(&error);
        if (sdl_audio) {
            opt.audio = sdl_audio.get();
            sdl_audio->set_gain(float(volume) / 100.0f);
            std::printf("audio: sdl (%s)\n", sdl_audio->driver().c_str());
        } else {
            std::fprintf(stderr, "lgp_boot: no sound, continuing without: %s\n", error.c_str());
        }
#else
        usage("--audio sdl: this build has no SDL3 audio");
#endif
    }

    // Decoder for in-game videos (movie:: HLE); outlives boot().
#if defined(LGP_BOOT_FFMPEG)
    platform::FfmpegVideo ffmpeg_video;
    if (video_kind.empty() || video_kind == "ffmpeg") {
        opt.video = &ffmpeg_video;
        video_kind = "ffmpeg";
    }
#else
    if (video_kind == "ffmpeg")
        usage("--video ffmpeg: this build has no FFmpeg");
#endif
#if defined(LGP_BOOT_VPX)
    platform::VpxVideo vpx_video;
    std::optional<platform::FallbackVideo> host_or_vpx;
#endif
    if ((video_kind.empty() || video_kind == "host") && host_devices.video != nullptr) {
        opt.video = host_devices.video;
#if defined(LGP_BOOT_VPX)
        // A host decoder that fails before its first picture (an output
        // format MediaCodec gives that is not converted, e.g. on a Shield)
        // hands the movie to libvpx instead of showing black (P7.3). Not with
        // an explicit --video host, which tests the host decoder alone.
        if (video_kind.empty()) {
            opt.video = &host_or_vpx.emplace(*host_devices.video, vpx_video);
            std::printf("video: host (libvpx if it fails)\n");
        } else {
            std::printf("video: host\n");
        }
#else
        std::printf("video: host\n");
#endif
        video_kind = "host";
    } else if (video_kind == "host") {
        usage("--video host: this platform has no video decoder");
    }
    // libvpx: VP9 in software where neither FFmpeg nor the host decodes (iOS).
#if defined(LGP_BOOT_VPX)
    if (video_kind.empty() || video_kind == "vpx") {
        opt.video = &vpx_video;
        video_kind = "vpx";
        std::printf("video: vpx\n");
    }
#else
    if (video_kind == "vpx")
        usage("--video vpx: this build has no libvpx");
#endif
    opt.input_log = input_log;
    opt.freeze_motion_pose = freeze_motion_pose;
    (void)pad_motion;  // SDL input only
    (void)player1_by_press;
#if defined(LGP_CLOUD_CONTAINER)
    if (g_save_backup)
        opt.on_save_commit = [] { g_save_backup->committed(); };
#endif
    boot::BootResult r;
    if (windowed) {
#if defined(LGP_BOOT_SDL)
        // SDL wants its events on the main thread; the process runs beside it.
        std::atomic<bool> done{false};
#if defined(LGP_BOOT_APP) && defined(LGP_BOOT_UI)
        MenuPause menu_pause;
        uint64_t menu_text_serial = 0;  // the software keyboard request the menu shows
        menu_pause.control = opt.control;
        if (menu_script.gate->control != nullptr)
            menu_pause.presented = &menu_script.gate->presented;
        if (async_backend)
            menu_pause.drain = [a = async_backend.get()](int64_t ms) { return a->drain_for(ms); };
#if defined(LGP_BOOT_SDL_AUDIO)
        if (sdl_audio)
            menu_pause.on_hold = [a = sdl_audio.get()](bool held) { a->set_held(held); };
#endif
        if (menu)
            menu->set_backup_available(opt.control != nullptr);  // a copy needs the whole guest at rest
        // The menu's HUD screen as this run started (settings.ini or
        // --hud-sides, which may hold four different sides): only an edit on
        // the screen hands the game its mirrored pairs.
        int hud_height_applied = menu ? menu->settings().hud_height : -1;
        int hud_width_applied = menu ? menu->settings().hud_width : -1;
        const auto apply_menu = [&](const ui::MenuSettings& m) {
            window->renderer().set_aspect_mode(platform::AspectMode(std::clamp(m.aspect, 0, 2)));
            window->renderer().set_present_filter(platform::webgpu::PresentFilter(std::clamp(m.smoothing, 0, 2)));
#if defined(LGP_BOOT_SDL_AUDIO)
            if (sdl_audio)
                sdl_audio->set_gain(float(m.volume) / 100.0f);
#endif
            // The widescreen mod does not follow a switch while the game runs
            // (layouts change as the game reads them, cameras keep their
            // format): the TV mode from the next start (settings.ini).
            static int docked_deferred = -1;
            if (opt.control != nullptr && mods::active("widescreen") && m.docked != opt.system.docked) {
                if (docked_deferred != int(m.docked)) {
                    docked_deferred = int(m.docked);
                    std::printf("menu: %s mode from the next start (widescreen)\n", m.docked ? "TV" : "handheld");
                }
            } else if (opt.control != nullptr && opt.control->set_docked(m.docked)) {
                std::printf("menu: %s mode\n", m.docked ? "TV" : "handheld");
            }
            if (sdl_input) {
                sdl_input->set_touch_enabled(m.touch);
                sdl_input->set_touch_layout(float(m.touch_size) / 100.0f, float(m.touch_opacity) / 100.0f,
                                            m.touch_mirrored);
                sdl_input->set_touch_throw(m.touch_throw, m.touch_throw_pad);
                sdl_input->set_player1_mode(m.player1_by_press ? platform::PlayerPick::Mode::Press
                                                               : platform::PlayerPick::Mode::Order);
                // The controller at once: hid shows the game the new style at
                // its next update, as a swap of controllers on the console.
                const platform::ControllerStyle style = m.controller == 1   ? platform::ControllerStyle::JoyconRight
                                                        : m.controller == 2 ? platform::ControllerStyle::JoyconLeft
                                                                            : platform::ControllerStyle::Handheld;
                if (style != player1) {
                    player1 = style;
                    sdl_input->set_player1_controller(input_slots()[0]);
                    std::printf("menu: controller %s\n", m.controller == 1   ? "joycon-right"
                                                         : m.controller == 2 ? "joycon-left"
                                                                             : "handheld");
                }
            }
            // The battle's HUD bounds from the menu's screen: its layouts change
            // anew when the game reads them next (the next battle).
            if (m.hud_height != hud_height_applied || m.hud_width != hud_width_applied) {
                hud_height_applied = m.hud_height;
                hud_width_applied = m.hud_width;
                const std::string sides = menu_hud_sides(m);
                std::array<int, 4> v{};
                if (boot::parse_hud_sides(sides, &v) && mods::set_widescreen_hud_sides(v))
                    std::printf("menu: battle HUD bounds %s\n", sides.c_str());
            }
            // Live mods at once; one not started in this run (no mod file, the
            // game lifted without --mods) says so once.
            static std::vector<std::string> unloaded_said;
            for (const ui::MenuMod& mod : m.mods) {
                // Pokémon Gelb off with "Hide field Pokémon": the same mod, hiding only.
                const bool hide_only = mod.id == kRandomEncountersId && menu_hide_only(m);
                const bool want = mod.on || hide_only;
                if (mod.id == kRandomEncountersId && hide_only != mods::random_encounter_hide_only()) {
                    mods::set_random_encounter_hide_only(hide_only);
                    std::printf("menu: hiding field Pokémon only %s\n", hide_only ? "on (Let's Go)" : "off");
                }
                if (!mod.live || want == mods::active(mod.id))
                    continue;
                if (mods::set_active(mod.id, want)) {
                    std::printf("menu: mod %s %s%s\n", mod.id.c_str(), want ? "on" : "off",
                                hide_only ? " (hiding only)" : "");
                } else if (want && std::find(unloaded_said.begin(), unloaded_said.end(), mod.id) == unloaded_said.end()) {
                    unloaded_said.push_back(mod.id);
                    std::printf("menu: mod %s is not loaded in this run\n", mod.id.c_str());
                    if (menu)
                        menu->set_status(platform::trf("{}: not loaded in this run (mod file missing or the game "
                                                       "was translated without --mods).",
                                                       {mod.title}));
                }
            }
        };
        // Script steps due at frame 0 (pause@0) are taken before the game runs:
        // a short game can reach its end before the host loop's first turn, and
        // a pause taken then finds no process any more (ProcessControl keeps a
        // pause taken before attach and applies it there).
        while (menu && !menu_script.empty() && menu_script.front().frame == 0)
            in_game_menu(*menu, *window, apply_menu, stop_requested, menu_script, menu_pause, sdl_text.get(),
                         menu_text_serial);
#endif
        LateFileWindow late_window = parse_late_file_window();
        std::thread runner([&] {
            platform::set_current_thread_name("lgp-boot");
            r = boot::boot(opt, code);
            done = true;
        });
        // Closing ends the run at the next present; a guest that never gets
        // there (hung before its first frame) is stopped after this long.
        constexpr auto kCloseGrace = std::chrono::seconds(5);
        std::chrono::steady_clock::time_point closed_at{};
        bool stop_forced = false;
        while (!done.load()) {
            if (late_window.pending && opt.control != nullptr && !stop_requested.load() &&
                window->renderer().frames() >= late_window.frame) {
                // As the background pause: the guest stops, the GPU queue
                // empties, then nothing touches guest memory while it moves.
                late_window.pending = false;
                const auto t0 = std::chrono::steady_clock::now();
                const bool at_rest = opt.control->pause(1000);
                const bool drained = !at_rest || !async_backend || async_backend->drain_for(kAsyncDrainTimeoutMs);
                platform::HostBlock::MoveStats moved;
                std::string error;
                bool ok = false;
                if (at_rest && drained) {
                    window->renderer().wait_idle();
                    ok = opt.control->extend_file_window(late_window.begin, &moved, &error);
                } else {
                    error = at_rest ? "the GPU queue did not empty" : "a guest thread did not stop";
                }
                opt.control->resume();
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - t0).count();
                if (ok)
                    std::printf("host block: file window from MiB %llu at frame %llu: %.1f MiB committed, %.1f MiB "
                                "written, move %.1f ms, game paused %lld ms\n",
                                (unsigned long long)(late_window.begin >> 20),
                                (unsigned long long)window->renderer().frames(), double(moved.committed) / 1048576.0,
                                double(moved.written) / 1048576.0, double(moved.ns) / 1e6, static_cast<long long>(ms));
                else if (!at_rest && ++late_window.tries < kLateFileWindowTries) {
                    late_window.pending = true;
                    late_window.frame = window->renderer().frames() + kLateFileWindowRetryFrames;
                    std::printf("host block: LGP_HOST_BLOCK_FILE_LATE not applied yet: %s, try %d of %d at frame "
                                "%llu\n",
                                error.c_str(), late_window.tries + 1, kLateFileWindowTries,
                                (unsigned long long)late_window.frame);
                } else
                    std::printf("host block: LGP_HOST_BLOCK_FILE_LATE not applied: %s\n", error.c_str());
                std::fflush(stdout);
            }
            if (!stop_requested.load()) {
                if (!window->pump_events(10)) {
                    stop_requested = true;  // ends at the next present
                    closed_at = std::chrono::steady_clock::now();
#if defined(LGP_BOOT_APP) && defined(LGP_BOOT_UI)
                    menu_pause.release(window->renderer());  // a game held by the pause menu must run to its end
#endif
                    // Closed while in the background (Android onDestroy): a held
                    // pause would keep the guest from that present and runner.join()
                    // would wait forever. Held pipeline workers stay held: a draw
                    // takes a queued job itself (claim_prebuild), the backend's
                    // destructor drops the queue, and releasing them would let
                    // warm-all compile every cached program first.
                    process_control.close();
                    if (sdl_text)
                        sdl_text->cancel();  // a guest thread waiting for text would never present
                }
                if (sdl_input) {
                    sdl_input->service();
                    // Every frame: the touch controller can be switched on and off in the menu.
                    std::vector<platform::OverlayCircle> overlay = sdl_input->touch_overlay();
#if defined(LGP_BOOT_APP) && defined(LGP_BOOT_UI)
                    // The text screen: the game waits for the text and the system keyboard
                    // covers the lower buttons; taps there type (Android emulator, E3).
                    if (menu && menu->screen() == ui::Screen::Text)
                        overlay.clear();
#endif
                    window->renderer().set_overlay(std::move(overlay));
                }
                if (sdl_text)
                    sdl_text->service();
#if defined(LGP_BOOT_APP) && defined(LGP_BOOT_UI)
                if (menu)
                    in_game_menu(*menu, *window, apply_menu, stop_requested, menu_script, menu_pause, sdl_text.get(),
                                 menu_text_serial);
#endif
            } else {
                if (!stop_forced && opt.control != nullptr && closed_at != std::chrono::steady_clock::time_point{} &&
                    std::chrono::steady_clock::now() - closed_at > kCloseGrace) {
                    stop_forced = true;
                    std::printf("app: no present within %lld s of closing, stopping the guest\n",
                                static_cast<long long>(kCloseGrace.count()));
                    std::fflush(stdout);
                    opt.control->stop();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        runner.join();
#endif
    } else {
        r = boot::boot(opt, code);
    }
    rt::time_stats_clear_sources();
    if (r.native.image == boot::BootResult::NativeStats::Image::Written) {
        // --native-image-out: the image is all this run makes.
        std::printf("native: image %s written, %" PRIu64 " ranges, %" PRIu64 " bytes; rewrite %" PRIu64
                    " words, %" PRIu64 " host calls\n",
                    opt.native.image_out.c_str(), r.native.image_ranges, r.native.image_bytes, r.native.words,
                    r.native.host_calls);
        return 0;
    }
    if (!r.started) {
        std::fprintf(stderr, "lgp_boot: %s\n", r.error.c_str());
        return 3;
    }
    if (!quiet)
        for (const std::string& line : r.debug_output)
            std::printf("debug: %s\n", printable(line).c_str());
    std::printf("start: %s\n", boot::start_kind_name(r.kind));
    if (r.native.used)
        std::printf("native: rewrite %" PRIu64 " ms, %" PRIu64 " words, %" PRIu64 " gate stubs, %" PRIu64
                    " stack-slot stubs, %" PRIu64 " host calls; unpatched %" PRIu64 " x18 sites, %" PRIu64
                    " gate sites; %" PRIu64 " modules not executable; cache %s\n",
                    r.native.rewrite_ms, r.native.words, r.native.gate_stubs, r.native.spill_stubs, r.native.host_calls,
                    r.native.x18_unpatched, r.native.gate_unpatched, r.native.modules_not_executable,
                    r.native.cache == boot::BootResult::NativeStats::Cache::Hit    ? "hit"
                    : r.native.cache == boot::BootResult::NativeStats::Cache::Miss ? "miss"
                                                                                    : "off");
    if (r.native.image == boot::BootResult::NativeStats::Image::Mapped)
        std::printf("native: code mapped from the image, %" PRIu64 " ranges, %" PRIu64 " bytes\n",
                    r.native.image_ranges, r.native.image_bytes);
    else if (r.native.image == boot::BootResult::NativeStats::Image::Checked)
        std::printf("native: code checked against the image and made executable (no file mapping), %" PRIu64
                    " ranges, %" PRIu64 " bytes\n",
                    r.native.image_ranges, r.native.image_bytes);
    for (const std::string& m : r.native.modules_left_out)
        std::printf("native: %s left out, the movie HLE replaces it\n", m.c_str());
    std::printf("exit: %s", rt::exit_reason_name(r.exit.reason));
    if (r.exit.reason == rt::ProcessExit::Reason::Crash)
        std::printf(" (halt %u, info 0x%" PRIx64 ", pc 0x%" PRIx64 ", thread %" PRIu64 ")", r.exit.halt,
                    r.exit.halt_info, r.exit.pc, r.exit.thread_id);
    std::printf("\n");
#if defined(LGP_BOOT_SDL)
    if (window && window->renderer().error_count() != 0)
        std::printf("renderer: %llu WebGPU errors\n", (unsigned long long)window->renderer().error_count());
#endif
#if LGP_FP_STATS
    {
        const auto& fs = lgp::lifted::fp::g_fp_stats;
        std::printf("fp: soft path %" PRIu64 " of %" PRIu64 " operations (fpcr seen: 0x%08x)\n", fs.soft.load(),
                    fs.ops.load(), fs.fpcr_seen.load());
    }
#endif
    if (!r.graphics_stop.empty())
        std::printf("graphics stop: %s (called from 0x%" PRIx64 ")\n", r.graphics_stop.c_str(),
                    r.graphics_stop_caller);
    if (opt.graphics == boot::GraphicsMode::Hle) {
        std::printf("nvn: frames %" PRIu64 "%s, procs %zu (%zu implemented), unimplemented called %zu\n", r.frames,
                    r.frame_limit_reached ? " (limit)" : "", r.nvn_procs, r.nvn_procs_implemented,
                    r.nvn_unimplemented.size());
        if (opt.nvn.pace_hz > 0)
            std::printf("present: waited %" PRIu64 " ms for the display\n", r.paced_wait_ns / 1'000'000);
        for (size_t i = 0; i < r.nvn_unimplemented.size() && i < 40; ++i)
            std::printf("  %s\n", r.nvn_unimplemented[i].c_str());
        if (r.nvn_untranslated != 0) {
            std::printf("nvn: programs without shaders (translator gaps) %" PRIu64 "\n", r.nvn_untranslated);
            for (const std::string& reason : r.nvn_untranslated_reasons)
                std::printf("  %s\n", reason.c_str());
        }
    }
    for (const std::string& m : r.mods_messages)
        std::printf("mods: %s\n", m.c_str());
    if (!r.mods_active.empty())
        std::printf("mods: on %s\n", mods::join_mod_list(r.mods_active).c_str());
    if (opt.mods.hook_log > 0)
        std::printf("hook log: %" PRIu64 " calls\n", r.hook_log_calls);
    for (const std::string& m : r.skipped_initializers)
        std::printf("skipped initializer: %s\n", m.c_str());
    std::printf("unresolved imports: %zu\n", r.unresolved_imports.size());
    for (size_t i = 0; i < r.unresolved_imports.size() && i < 20; ++i)
        std::printf("  %s\n", r.unresolved_imports[i].c_str());
    std::printf("unresolved branch targets: %zu\n", r.unresolved.size());
    for (size_t i = 0; i < r.unresolved.size() && i < 20; ++i)
        std::printf("  0x%" PRIx64 " from 0x%" PRIx64 " (%" PRIu64 "x)\n", r.unresolved[i].target, r.unresolved[i].from_pc,
                    r.unresolved[i].count);
    if (!unresolved_path.empty()) {
        std::ofstream f(unresolved_path, std::ios::binary);
        f << r.unresolved_config;
    }
    return r.ok() ? 0 : 1;
}

}  // namespace
