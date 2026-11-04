// Where a desktop app keeps its files (P7.1): settings, save data, logs and
// caches go to the user's folders of the platform, never next to the program
// (an installed .app bundle or a folder under Program Files may be read-only).
//
//   Windows: config and data %APPDATA%\<app>, cache %LOCALAPPDATA%\<app>
//   macOS:   config and data ~/Library/Application Support/<app>,
//            cache ~/Library/Caches/<app>
//   iOS:     config and data the app container's Documents folder (visible in
//            the Files app, so saves and logs can be copied off the device;
//            the game dump goes to Documents/game), cache Library/Application
//            Support/cache (kept by iOS, unlike Library/Caches; excluded from
//            backups, exclude_from_backup)
//   tvOS:    all three in Library/Caches (Documents takes no files there and
//            the system may empty Caches while the app is not running)
//   other:   $XDG_CONFIG_HOME/<app>, $XDG_DATA_HOME/<app>, $XDG_CACHE_HOME/<app>
//            (defaults ~/.config, ~/.local/share, ~/.cache)
//
// LGP_APP_DIR=<dir> puts all three into that one folder (tests, a portable
// copy on an external drive).
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace lgp::platform {

struct AppDirs {
    std::filesystem::path config;  // settings
    std::filesystem::path data;    // save data, logs
    std::filesystem::path cache;   // files that can be rebuilt (pipeline cache)
};

// The folders for app `name` (not created); nullopt and *error if the host
// does not say where the user's folders are (no HOME, no APPDATA).
std::optional<AppDirs> app_dirs(std::string_view name, std::string* error);

// The game folder shipped inside the app (Paul, 2026-10-14: the dump may live
// in the app package): "game" in the bundle's resources on Apple platforms
// (the .app itself on iOS/tvOS, Contents/Resources in a macOS .app), next to
// the program elsewhere; on Android <LGP_APP_DIR>/package/game (APK assets
// are not files: LgpActivity copies them to <LGP_APP_DIR>/package, the game
// is never among them). Empty where the program's path is unknown (and in the
// browser). Whether the folder exists is the caller's question.
std::filesystem::path packaged_game_dir();

// Keeps the existing folder `path` out of iCloud and computer backups on iOS
// (the pipeline cache can be rebuilt and is tens of MiB); false if that
// failed. Elsewhere nothing to do (macOS backs up Library/Caches neither,
// tvOS has no backups), true.
bool exclude_from_backup(const std::filesystem::path& path);

}  // namespace lgp::platform
