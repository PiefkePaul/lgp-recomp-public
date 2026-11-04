// IStorage: files the runtime reads and writes on the host (P3.6). Used by
// the fs service for the game's RomFS/ExeFS (read-only, from the user's own
// dump) and for save data (P5.3).
//
// Paths are virtual and backend-independent: absolute, '/'-separated, UTF-8,
// without "." or ".." components, backslashes, NUL or ':' (normalize_path).
// A backend maps them below its own root and never outside of it, whatever
// the guest sends: path names come from game code.
//
// Calls are synchronous and may block; they run on guest threads, which may
// block in every build (ADR-0002: PROXY_TO_PTHREAD on the web).
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lgp::platform {

enum class StorageError {
    Ok,
    NotFound,
    AlreadyExists,
    NotADirectory,
    IsADirectory,
    NotEmpty,
    ReadOnly,       // read-only backend, or a file opened without Write
    InvalidPath,    // fails normalize_path
    OutOfRange,     // offset or size beyond what the backend allows
    Io,             // host error
};
const char* storage_error_name(StorageError e);

enum OpenMode : uint32_t {
    kOpenRead = 1,
    kOpenWrite = 2,
};

struct DirEntry {
    std::string name;  // last component only
    bool is_directory = false;
    uint64_t size = 0;  // files only
};

class IFile {
public:
    virtual ~IFile() = default;
    virtual uint64_t size() const = 0;
    // Reads up to len bytes; *got < len only at the end of the file.
    virtual StorageError read(uint64_t offset, void* dst, uint64_t len, uint64_t* got) = 0;
    // Writes len bytes, growing the file if needed (zero-filled gap).
    virtual StorageError write(uint64_t offset, const void* src, uint64_t len) = 0;
    virtual StorageError set_size(uint64_t size) = 0;
    virtual StorageError flush() = 0;
};

class IStorage {
public:
    virtual ~IStorage() = default;
    virtual bool read_only() const = 0;

    virtual StorageError open(std::string_view path, uint32_t mode, std::unique_ptr<IFile>* out) = 0;
    virtual StorageError stat(std::string_view path, DirEntry* out) = 0;
    // Sorted by name.
    virtual StorageError list(std::string_view path, std::vector<DirEntry>* out) = 0;
    // The parent directory must exist.
    virtual StorageError create_file(std::string_view path, uint64_t size) = 0;
    virtual StorageError create_directory(std::string_view path) = 0;
    virtual StorageError remove_file(std::string_view path) = 0;
    // recursive = false: the directory must be empty.
    virtual StorageError remove_directory(std::string_view path, bool recursive) = 0;
    virtual StorageError rename(std::string_view from, std::string_view to) = 0;
    // Make everything written so far durable (fs Commit for save data).
    virtual StorageError commit() = 0;
};

// "/a//b/" -> "/a/b"; "" and "/" -> "/". nullopt for relative paths, "." or
// ".." components, '\\', ':' or NUL.
std::optional<std::string> normalize_path(std::string_view path);

}  // namespace lgp::platform
