// IStorage on a host directory (P3.6): the extracted RomFS of the user's own
// dump (read-only) or a save directory on desktop. std::filesystem, so it
// also runs on Emscripten's file systems.
//
// Virtual paths are normalized first (normalize_path) and then appended to
// the root, so no guest path can name anything outside it. Symbolic links
// inside the root are followed; the root is the user's own directory.
//
// commit() makes the changes since the last commit durable (P5.3): it syncs
// every file written and every directory whose entries changed to the disk
// (sync_host_path: fsync, F_FULLFSYNC on macOS, FlushFileBuffers on Windows),
// files first. fsp-srv's save journal calls it before and after switching to
// a new committed state, so a save also survives a power loss.
#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <set>

#include "lgp/platform/storage.hpp"

namespace lgp::platform {

class HostDirectoryStorage final : public IStorage {
public:
    // nullptr unless root exists and is a directory. (No exceptions: the
    // wasm build runs without them.)
    static std::unique_ptr<HostDirectoryStorage> open_root(const std::filesystem::path& root, bool read_only);

    bool read_only() const override { return read_only_; }
    StorageError open(std::string_view path, uint32_t mode, std::unique_ptr<IFile>* out) override;
    StorageError stat(std::string_view path, DirEntry* out) override;
    StorageError list(std::string_view path, std::vector<DirEntry>* out) override;
    StorageError create_file(std::string_view path, uint64_t size) override;
    StorageError create_directory(std::string_view path) override;
    StorageError remove_file(std::string_view path) override;
    StorageError remove_directory(std::string_view path, bool recursive) override;
    StorageError rename(std::string_view from, std::string_view to) override;
    StorageError commit() override;

    const std::filesystem::path& root() const { return root_; }

    // Paths changed since the last commit, shared with the open files.
    struct Dirty {
        std::mutex mutex;
        std::set<std::filesystem::path> files;
        std::set<std::filesystem::path> dirs;
        void file(const std::filesystem::path& p);
        void dir(const std::filesystem::path& p);
    };

private:
    HostDirectoryStorage(std::filesystem::path root, bool read_only)
        : root_(std::move(root)), read_only_(read_only), dirty_(std::make_shared<Dirty>()) {}
    // Host path for a virtual one; false if it does not normalize.
    bool host_path(std::string_view path, std::filesystem::path* out) const;

    std::filesystem::path root_;
    bool read_only_;
    std::shared_ptr<Dirty> dirty_;
};

}  // namespace lgp::platform
