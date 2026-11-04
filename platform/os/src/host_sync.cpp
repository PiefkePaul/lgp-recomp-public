// Durable writes on the host, see lgp/platform/host_sync.hpp.
#include "lgp/platform/host_sync.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#endif

namespace lgp::platform {

std::error_code sync_host_path(const std::filesystem::path& path, bool directory) {
#if defined(_WIN32)
    // FlushFileBuffers needs a handle with write access; a directory handle
    // also needs FILE_FLAG_BACKUP_SEMANTICS. On NTFS flushing the directory
    // makes its new and renamed entries durable.
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, directory ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return {static_cast<int>(GetLastError()), std::system_category()};
    const bool ok = FlushFileBuffers(h) != 0;
    const DWORD err = ok ? 0 : GetLastError();
    CloseHandle(h);
    // File systems that cannot flush a directory (FAT, some network shares):
    // not a failed save, as on POSIX below.
    if (directory && (err == ERROR_INVALID_FUNCTION || err == ERROR_INVALID_PARAMETER || err == ERROR_NOT_SUPPORTED))
        return {};
    return ok ? std::error_code{} : std::error_code{static_cast<int>(err), std::system_category()};
#else
#if defined(__EMSCRIPTEN__)
    if (directory)
        return {};
#endif
    int flags = O_RDONLY;
#if defined(O_DIRECTORY)
    if (directory)
        flags |= O_DIRECTORY;
#endif
#if defined(O_CLOEXEC)
    flags |= O_CLOEXEC;
#endif
    const int fd = ::open(path.c_str(), flags);
    if (fd < 0)
        return {errno, std::generic_category()};
    int rc = -1;
#if defined(__APPLE__) && defined(F_FULLFSYNC)
    rc = ::fcntl(fd, F_FULLFSYNC);
#endif
    if (rc != 0)
        rc = ::fsync(fd);
    const int err = rc == 0 ? 0 : errno;
    ::close(fd);
    // Some file systems cannot sync a directory; that is not a failed save.
    if (directory && (err == EINVAL || err == ENOTSUP || err == EBADF))
        return {};
    return err == 0 ? std::error_code{} : std::error_code{err, std::generic_category()};
#endif
}

}  // namespace lgp::platform
