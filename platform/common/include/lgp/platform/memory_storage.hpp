// IStorage in host RAM (P3.6): tests, and the base for a web backend that
// loads files into memory before the game starts.
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "lgp/platform/storage.hpp"

namespace lgp::platform {

class MemoryStorage final : public IStorage {
public:
    explicit MemoryStorage(bool read_only = false) : read_only_(read_only) { dirs_.insert("/"); }

    // Setup before use (ignores read_only): creates missing parent directories.
    void add_file(std::string_view path, std::vector<uint8_t> data);

    bool read_only() const override { return read_only_; }
    StorageError open(std::string_view path, uint32_t mode, std::unique_ptr<IFile>* out) override;
    StorageError stat(std::string_view path, DirEntry* out) override;
    StorageError list(std::string_view path, std::vector<DirEntry>* out) override;
    StorageError create_file(std::string_view path, uint64_t size) override;
    StorageError create_directory(std::string_view path) override;
    StorageError remove_file(std::string_view path) override;
    StorageError remove_directory(std::string_view path, bool recursive) override;
    StorageError rename(std::string_view from, std::string_view to) override;
    StorageError commit() override { return StorageError::Ok; }

    // Upper bound for one file: sizes come from guest code and must not
    // exhaust the host. Below that, a failed allocation is reported as Io
    // natively; the wasm build has no exceptions and aborts instead.
    static constexpr uint64_t kMaxFileSize = 1ull << 30;

private:
    struct Blob {
        std::mutex mutex;
        std::vector<uint8_t> data;
    };
    class File;

    StorageError check_parent(const std::string& path) const;
    bool has_children(const std::string& dir) const;

    bool read_only_;
    mutable std::mutex mutex_;
    // Open files share the data with the map, so a rename keeps them valid.
    std::map<std::string, std::shared_ptr<Blob>> files_;
    std::set<std::string> dirs_;
};

}  // namespace lgp::platform
