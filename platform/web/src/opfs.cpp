#include "lgp/platform/web/opfs.hpp"

#include <emscripten/wasmfs.h>
#include <sys/stat.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <system_error>

#include "lgp/platform/host_directory_storage.hpp"

namespace lgp::platform::web {

namespace {

std::mutex g_mount_mutex;
bool g_mounted = false;

void set_error(std::string* error, std::string text) {
    if (error != nullptr)
        *error = std::move(text);
}

}  // namespace

bool mount_opfs(std::string* error) {
    std::lock_guard<std::mutex> lock(g_mount_mutex);
    if (g_mounted)
        return true;
    backend_t backend = wasmfs_create_opfs_backend();
    if (backend == nullptr) {
        set_error(error, "this browser has no OPFS (navigator.storage.getDirectory)");
        return false;
    }
    if (wasmfs_create_directory(kOpfsMount, 0777, backend) != 0) {
        set_error(error, std::string("cannot mount OPFS at ") + kOpfsMount + ": " + std::strerror(errno));
        return false;
    }
    g_mounted = true;
    return true;
}

std::unique_ptr<IStorage> open_opfs_storage(std::string_view dir, bool read_only, std::string* error) {
    if (!mount_opfs(error))
        return nullptr;
    const std::filesystem::path path{std::string(dir)};
    if (!read_only) {
        std::error_code ec;
        std::filesystem::create_directories(path, ec);
        if (ec) {
            set_error(error, "cannot create " + path.string() + ": " + ec.message());
            return nullptr;
        }
    }
    std::unique_ptr<IStorage> storage = HostDirectoryStorage::open_root(path, read_only);
    if (!storage)
        set_error(error, path.string() + " does not exist (no game data copied to this browser yet?)");
    return storage;
}

}  // namespace lgp::platform::web
