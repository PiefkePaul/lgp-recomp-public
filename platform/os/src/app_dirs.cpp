// User folders of a desktop app (app_dirs.hpp).
#include "lgp/platform/app_dirs.hpp"

#include <cstdlib>
#include <string>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <mach-o/dyld.h>
#if TARGET_OS_IPHONE && !TARGET_OS_TV
#include <CoreFoundation/CoreFoundation.h>
#endif
#include <vector>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <system_error>
#endif

namespace lgp::platform {

namespace {

namespace fs = std::filesystem;

// Environment variable as a path; empty if unset or empty. Windows reads the
// wide variable so that user names outside the ANSI code page survive.
fs::path env_path(const char* name) {
#if defined(_WIN32)
    const std::wstring wide(name, name + std::char_traits<char>::length(name));
    wchar_t* v = nullptr;
    size_t len = 0;
    if (_wdupenv_s(&v, &len, wide.c_str()) != 0 || v == nullptr)
        return {};
    fs::path p = *v != L'\0' ? fs::path(v) : fs::path();
    std::free(v);
    return p;
#else
    const char* v = std::getenv(name);
    return v != nullptr && *v != '\0' ? fs::path(v) : fs::path();
#endif
}

}  // namespace

std::optional<AppDirs> app_dirs(std::string_view name, std::string* error) {
    const fs::path app{std::string(name)};
    if (const fs::path one = env_path("LGP_APP_DIR"); !one.empty())
        return AppDirs{one, one, one};
#if defined(_WIN32)
    const fs::path roaming = env_path("APPDATA");
    fs::path local = env_path("LOCALAPPDATA");
    if (roaming.empty()) {
        *error = "APPDATA is not set";
        return std::nullopt;
    }
    if (local.empty())
        local = roaming;
    return AppDirs{roaming / app, roaming / app, local / app};
#else
    const fs::path home = env_path("HOME");
#if defined(__APPLE__)
    if (home.empty()) {
        *error = "HOME is not set";
        return std::nullopt;
    }
#if TARGET_OS_TV
    // tvOS: Documents exists but refuses writes and there is no Application
    // Support; only Library/Caches (and tmp) can be written, and the system
    // may empty Caches while the app is not running (P7.4,
    // docs/knowledge/tvos-als-zielplattform.md). Everything goes there.
    (void)app;
    const fs::path caches = home / "Library" / "Caches";
    return AppDirs{caches, caches, caches / "cache"};
#elif TARGET_OS_IPHONE
    // HOME is the app's sandbox container; the container is the app's own,
    // so no <app> subfolder. The cache (the pipeline cache, costly to rebuild:
    // seconds of stutter per new scene) goes to Application Support, which
    // iOS keeps; it may empty Library/Caches when storage runs low.
    (void)app;
    return AppDirs{home / "Documents", home / "Documents", home / "Library" / "Application Support" / "cache"};
#else
    const fs::path support = home / "Library" / "Application Support" / app;
    return AppDirs{support, support, home / "Library" / "Caches" / app};
#endif
#else
    // XDG base directories; a relative value is invalid and ignored.
    auto xdg = [&](const char* var, const char* fallback) -> fs::path {
        const fs::path v = env_path(var);
        if (!v.empty() && v.is_absolute())
            return v / app;
        return home.empty() ? fs::path() : home / fallback / app;
    };
    AppDirs dirs{xdg("XDG_CONFIG_HOME", ".config"), xdg("XDG_DATA_HOME", ".local/share"),
                 xdg("XDG_CACHE_HOME", ".cache")};
    if (dirs.config.empty() || dirs.data.empty() || dirs.cache.empty()) {
        *error = "HOME is not set";
        return std::nullopt;
    }
    return dirs;
#endif
#endif
}

fs::path packaged_game_dir() {
    fs::path exe;
#if defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buf(size + 1, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0)
        return {};
    // The path the program was started by; it may hold links or "..".
    std::error_code canon_ec;
    exe = fs::weakly_canonical(fs::path(buf.data()), canon_ec);
    if (canon_ec)
        exe = fs::path(buf.data());
#elif defined(_WIN32)
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0)
            return {};
        if (n < buf.size()) {
            buf.resize(n);
            break;
        }
        buf.resize(buf.size() * 2);
    }
    exe = fs::path(buf);
#elif defined(__linux__) && !defined(__ANDROID__)
    std::error_code ec;
    exe = fs::read_symlink("/proc/self/exe", ec);
    if (ec)
        return {};
#elif defined(__ANDROID__)
    // APK assets are not files: LgpActivity copies what the build put there
    // (the pipeline cache seed, LGP_APP_PIPELINE_SEED) to <app folder>/package.
    // The game itself is never in the APK, so this "game" does not exist.
    const fs::path app = env_path("LGP_APP_DIR");
    return app.empty() ? fs::path() : app / "package" / "game";
#else
    return {};  // Emscripten (OPFS)
#endif
    const fs::path dir = exe.parent_path();
#if defined(__APPLE__) && !TARGET_OS_IPHONE
    // macOS .app: <name>.app/Contents/MacOS/<exe> -> Contents/Resources/game.
    if (dir.filename() == "MacOS" && dir.parent_path().filename() == "Contents")
        return dir.parent_path() / "Resources" / "game";
#endif
    return dir / "game";
}

bool exclude_from_backup(const fs::path& path) {
#if defined(__APPLE__) && TARGET_OS_IPHONE && !TARGET_OS_TV
    // NSURLIsExcludedFromBackupKey through CoreFoundation; the flag sits on
    // the folder and covers everything in it.
    const std::string s = path.string();
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        kCFAllocatorDefault, reinterpret_cast<const UInt8*>(s.data()), static_cast<CFIndex>(s.size()), true);
    if (url == nullptr)
        return false;
    const Boolean ok = CFURLSetResourcePropertyForKey(url, kCFURLIsExcludedFromBackupKey, kCFBooleanTrue, nullptr);
    CFRelease(url);
    return ok;
#else
    (void)path;
    return true;
#endif
}

}  // namespace lgp::platform
