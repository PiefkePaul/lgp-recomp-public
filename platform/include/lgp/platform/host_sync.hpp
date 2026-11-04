// Durable writes on the host (P5.3): flush a file's data, or a directory's
// entries, from the OS cache to the disk, so a save survives a power loss.
// Used by HostDirectoryStorage::commit for the paths changed since the last
// commit.
//
//   POSIX:   fsync; on macOS fcntl(F_FULLFSYNC), because fsync there leaves
//            the drive's cache alone (falls back to fsync where unsupported).
//   Windows: FlushFileBuffers, on directories through a handle opened with
//            FILE_FLAG_BACKUP_SEMANTICS (NTFS; skipped where the file system
//            cannot, e.g. FAT).
//   Emscripten: files only (WasmFS flushes the OPFS access handle);
//            directories have nothing to flush there.
#pragma once

#include <filesystem>
#include <system_error>

namespace lgp::platform {

// Errors as std::error_code (no_such_file_or_directory if the path is gone).
std::error_code sync_host_path(const std::filesystem::path& path, bool directory);

}  // namespace lgp::platform
