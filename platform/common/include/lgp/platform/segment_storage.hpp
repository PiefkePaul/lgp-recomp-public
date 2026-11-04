// A game file put together from byte ranges of host files (P7.3): on Android
// the 4.2 GiB romfs.bin travels in split APKs as parts below 2 GiB, stored
// uncompressed, and is read in place (no unpacked copy). The parts are named
// by a spec, so the same code runs with plain files on every host:
//
//   "FILE@OFFSET+LENGTH,FILE@OFFSET+LENGTH,..."   (decimal; FILE without ',')
//   "FILE"                                         (the whole file)
//
// The Android app opens each part with AssetManager.openFd and names it as
// /proc/self/fd/N@offset+length.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "lgp/platform/storage.hpp"

namespace lgp::platform {

struct FileSegment {
    std::string path;
    uint64_t offset = 0;
    uint64_t length = 0;  // whole_file: taken from the file when opened
    bool whole_file = false;
};

// False with *error for a malformed spec (an empty one is malformed too).
bool parse_file_segments(std::string_view spec, std::vector<FileSegment>* out, std::string* error);

// A read-only file over the segments, read one after the other. nullptr with
// *error if a part cannot be opened or is shorter than its range.
std::unique_ptr<IFile> open_segmented_file(const std::vector<FileSegment>& segments, std::string* error);

// `base` plus read-only files at fixed paths in its root ("/romfs.bin"); they
// hide a file of the same name in `base`. Read-only as a whole.
class AddedFilesStorage final : public IStorage {
public:
    struct Added {
        std::string name;  // in the root, without '/'
        std::vector<FileSegment> segments;
        uint64_t size = 0;
    };
    // nullptr with *error if a segment cannot be opened.
    static std::unique_ptr<AddedFilesStorage> create(std::unique_ptr<IStorage> base,
                                                      std::vector<std::pair<std::string, std::vector<FileSegment>>> files,
                                                      std::string* error);

    bool read_only() const override { return true; }
    StorageError open(std::string_view path, uint32_t mode, std::unique_ptr<IFile>* out) override;
    StorageError stat(std::string_view path, DirEntry* out) override;
    StorageError list(std::string_view path, std::vector<DirEntry>* out) override;
    StorageError create_file(std::string_view, uint64_t) override { return StorageError::ReadOnly; }
    StorageError create_directory(std::string_view) override { return StorageError::ReadOnly; }
    StorageError remove_file(std::string_view) override { return StorageError::ReadOnly; }
    StorageError remove_directory(std::string_view, bool) override { return StorageError::ReadOnly; }
    StorageError rename(std::string_view, std::string_view) override { return StorageError::ReadOnly; }
    StorageError commit() override { return StorageError::Ok; }

private:
    AddedFilesStorage(std::unique_ptr<IStorage> base, std::vector<Added> added)
        : base_(std::move(base)), added_(std::move(added)) {}
    const Added* find(std::string_view path) const;

    std::unique_ptr<IStorage> base_;
    std::vector<Added> added_;
};

}  // namespace lgp::platform
