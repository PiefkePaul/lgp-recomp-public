// Origin Private File System (OPFS) as the browser's IStorage (P6): the
// user's own game data, copied there from a local file picker by the page
// (platform/web/js), and the save data. Nothing is ever fetched from a server.
//
// OPFS is mounted into the program's file system with WasmFS
// (-sWASMFS, linked by lgp_platform_web), so the backends are the ordinary
// HostDirectoryStorage on a path below the mount point and every other file
// access of the program (module files, npdm) works on it unchanged.
//
// Layout below the mount point, shared with the page (lgp_web.js):
//   /opfs/lgp/data/exefs/...    modules (main, sdk, ..., main.npdm)
//   /opfs/lgp/data/romfs/...    RomFS tree (or /opfs/lgp/data/romfs.bin)
//   /opfs/lgp/save/...          save data, written by the program
//
// Threading: WasmFS proxies OPFS access to a dedicated worker, and mounting
// blocks until that worker runs, so call these from a pthread (main() under
// PROXY_TO_PTHREAD is one), never from the browser's main thread.
#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "lgp/platform/storage.hpp"

namespace lgp::platform::web {

inline constexpr const char* kOpfsMount = "/opfs";
inline constexpr const char* kGameDataDir = "/opfs/lgp/data";
inline constexpr const char* kExefsDir = "/opfs/lgp/data/exefs";
inline constexpr const char* kSaveDir = "/opfs/lgp/save";

// Mounts OPFS at kOpfsMount. Idempotent and thread-safe; false (and *error)
// if the browser has no OPFS or the mount failed.
bool mount_opfs(std::string* error = nullptr);

// IStorage on a directory below the mount (e.g. kGameDataDir read-only,
// kSaveDir writable). Mounts first; creates the directory when writable.
// nullptr (and *error) if that fails or a read-only directory is missing.
std::unique_ptr<IStorage> open_opfs_storage(std::string_view dir, bool read_only, std::string* error = nullptr);

}  // namespace lgp::platform::web
