// RomFS images for fsp-srv (P3.4). The game parses its RomFS itself (nn::fs
// RomFsFileSystem over the raw IStorage from OpenDataStorageByCurrentProcess),
// so the runtime has to hand out an image, not a directory.
//
// Two sources: an image file as dumped (romfs.bin), or a directory tree (what
// nstool/hactool extract) from which RomFsImage builds the image virtually:
// header and tables live in host memory, file data is read from the
// directory on demand. A virtual image also lets files be replaced (mods).
//
// Format (switchbrew "NCA", libnx romfs_dev.h): 0x50-byte header of u64
// offset/size pairs (dir hash table, dir table, file hash table, file table,
// file data); entries with u32 links (0xFFFFFFFF = none) and UTF-8 names
// padded to 4 bytes; hash = rotate-right-5 xor per name byte, seeded with
// the parent's offset ^ 123456789.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "lgp/platform/storage.hpp"

namespace lgp::hle {

// Read-only byte source behind an fs IStorage.
class DataSource {
public:
    virtual ~DataSource() = default;
    virtual uint64_t size() const = 0;
    // Exactly len bytes from offset; false if out of range or on a host error.
    virtual bool read(uint64_t offset, void* dst, uint64_t len) = 0;
};

// One host file as a source (an image file).
class FileSource final : public DataSource {
public:
    explicit FileSource(std::unique_ptr<platform::IFile> file) : file_(std::move(file)) {}
    uint64_t size() const override { return file_->size(); }
    bool read(uint64_t offset, void* dst, uint64_t len) override;

private:
    std::mutex mutex_;
    std::unique_ptr<platform::IFile> file_;
};

// Compressed image (romfs.bin written by lgp_romfs_pack --compress): the
// RomFS image cut into fixed-size blocks, each compressed on its own with
// zstd, so any byte range is read by decompressing only its blocks. Layout
// (little endian):
//   header (32 bytes): "LGPROMZ\0", u32 version 1, u32 codec 1 (zstd),
//                      u32 block size, u32 block count, u64 image size
//   blocks:            back to back from offset 32
//   index:             block count + 1 u64 offsets into the file; block i is
//                      [index[i], index[i+1]), stored raw when compression
//                      did not make it smaller (its length is then the
//                      block's uncompressed length)
//   footer (16 bytes): u64 index offset, "LGPZEND\0"
// The index is read once; decompressed blocks are kept in a small cache.
struct CompressedImageFormat {
    static constexpr char kMagic[8] = {'L', 'G', 'P', 'R', 'O', 'M', 'Z', '\0'};
    static constexpr char kEndMagic[8] = {'L', 'G', 'P', 'Z', 'E', 'N', 'D', '\0'};
    static constexpr uint32_t kVersion = 1;
    static constexpr uint32_t kCodecZstd = 1;
    static constexpr uint64_t kHeaderSize = 32;
    static constexpr uint64_t kFooterSize = 16;
    static constexpr uint32_t kMinBlockSize = 4096;
    static constexpr uint32_t kMaxBlockSize = 16u << 20;
};

// True if the file starts with the compressed image magic.
bool is_compressed_image(platform::IFile& file);

// Opens a romfs.bin as a source: a plain image as is (FileSource), a
// compressed one through its block index. nullptr with *error set if a
// compressed image is damaged or this build has no zstd.
std::shared_ptr<DataSource> open_image_source(std::unique_ptr<platform::IFile> file, std::string* error);

// Whether this build reads (and writes) compressed images.
bool compressed_images_supported();

struct CompressOptions {
    uint32_t block_size = 128u << 10;
    int level = 9;        // zstd level, 1..19 (22 with ultra)
    unsigned threads = 0;  // 0: the host's cores
};

// Writes `image` as a compressed image through `write` (called in file
// order). Returns false with *error set if reading, compressing or writing
// fails. `progress`, when set, is told the bytes of `image` done so far.
bool write_compressed_image(DataSource& image, const CompressOptions& options,
                            const std::function<bool(const void* data, size_t len)>& write, std::string* error,
                            const std::function<void(uint64_t done)>& progress = {});

// Looks a file up in a RomFS image by absolute path ("/bin/a.webm") by
// walking the directory and file tables (no hashing). On success *offset is
// the file's offset in the image and *size its length.
// *entry (if given) is the file's entry offset in the file table.
bool romfs_find(DataSource& image, std::string_view path, uint64_t* offset, uint64_t* size,
                uint32_t* entry = nullptr);

// A file of a RomFS image: offset in the image, length, absolute path.
struct RomFsFile {
    uint64_t offset;
    uint64_t size;
    std::string path;
};

// The non-empty files of a RomFS image, sorted by offset (walks the tables).
// False if `image` is no RomFS image or its tables are damaged.
bool romfs_list(DataSource& image, std::vector<RomFsFile>* files);

class RomFsImage final : public DataSource {
public:
    // Builds the image for the directory `root` of `fs`. nullptr with *error
    // set if the tree cannot be listed. `fs` must outlive the image.
    static std::shared_ptr<RomFsImage> build(platform::IStorage& fs, std::string_view root,
                                             platform::StorageError* error);

    uint64_t size() const override { return size_; }
    bool read(uint64_t offset, void* dst, uint64_t len) override;

    size_t file_count() const { return file_count_; }
    size_t directory_count() const { return directory_count_; }

    static constexpr uint64_t kHeaderSize = 0x50;
    static constexpr uint64_t kDataOffset = 0x200;  // where Nintendo's tools put file data
    static constexpr uint32_t kNone = 0xFFFFFFFF;
    static uint32_t hash(uint32_t parent, std::string_view name);
    static uint32_t bucket_count(uint32_t entries);

private:
    struct File {
        uint64_t offset;  // absolute in the image
        uint64_t size;
        std::string path;
    };

    explicit RomFsImage(platform::IStorage& fs) : fs_(fs) {}
    bool read_file(size_t index, uint64_t offset, uint8_t* dst, uint64_t len);

    platform::IStorage& fs_;
    std::vector<uint8_t> header_;  // kHeaderSize bytes at 0
    std::vector<uint8_t> tables_;  // at tables_offset_
    uint64_t tables_offset_ = 0;
    uint64_t size_ = 0;
    std::vector<File> files_;  // non-empty files, sorted by offset
    size_t file_count_ = 0;
    size_t directory_count_ = 0;

    std::mutex mutex_;
    std::map<size_t, std::unique_ptr<platform::IFile>> open_;  // small cache of open files
};

// An image with some of its files replaced (data mods, `<game>/mods/<id>/romfs/`):
// the file table lives in memory with the entries of the replaced files
// pointing past the end of the base image, where their new contents follow.
// Everything else reads through to the base. Files can change size. A file
// the base does not have is added if its directory exists (the file table then
// moves behind the contents and the header points there); new directories
// cannot be added.
class RomFsOverlay final : public DataSource {
public:
    // Keys are image paths ("/bin/field/x.bin"). nullptr with *error if the
    // base is no RomFS image or lacks the directory of a file.
    static std::shared_ptr<RomFsOverlay> build(std::shared_ptr<DataSource> base,
                                               const std::map<std::string, std::vector<uint8_t>>& files,
                                               std::string* error);

    uint64_t size() const override { return size_; }
    bool read(uint64_t offset, void* dst, uint64_t len) override;
    size_t added() const { return added_; }  // files the base did not have

private:
    std::shared_ptr<DataSource> base_;
    uint64_t base_size_ = 0;
    std::vector<uint8_t> header_;   // patched copy, only when files were added
    uint64_t dirs_offset_ = 0;      // the directory table
    std::vector<uint8_t> dirs_;     // patched copy
    uint64_t hash_offset_ = 0;      // the file hash table
    std::vector<uint8_t> hash_;     // patched copy
    uint64_t table_offset_ = 0;     // the file table (behind the contents when grown)
    std::vector<uint8_t> table_;    // patched copy
    size_t added_ = 0;
    uint64_t extra_offset_ = 0;     // where the replaced contents start
    std::vector<uint8_t> extra_;
    uint64_t size_ = 0;
};

// Files changed as the game reads them (a mod's rewrite of game files, e.g.
// the widescreen mod's layouts, hle::FsConfig::romfs_changes). On the first
// read that touches a file, `wants` is asked with its first bytes (up to 16);
// if it says yes, the whole file is read and handed to `change`, which must
// keep the size. Only the changed byte runs are kept; every read of the file
// gets them laid over the base. `epoch` (optional) names a generation per
// file, asked when it is decided: 0 = never again; else a read that finds
// another number than the one it was decided under decides the file anew
// (a setting the change depends on moved, e.g. the battle's HUD bounds).
class RomFsRewriter final : public DataSource {
public:
    using Wants = std::function<bool(const std::string& path, std::span<const uint8_t> head)>;
    using Change = std::function<bool(const std::string& path, std::vector<uint8_t>& bytes)>;
    using Epoch = std::function<uint64_t(const std::string& path)>;
    // nullptr if `base` is no RomFS image.
    static std::shared_ptr<RomFsRewriter> wrap(std::shared_ptr<DataSource> base, Wants wants, Change change,
                                               Epoch epoch = nullptr);

    uint64_t size() const override { return base_->size(); }
    bool read(uint64_t offset, void* dst, uint64_t len) override;
    size_t file_count() const { return files_.size(); }
    size_t changed() const;  // files changed so far

private:
    struct Run {
        uint64_t at;  // offset in the file
        std::vector<uint8_t> bytes;
    };
    struct File {
        uint64_t offset;
        uint64_t size;
        std::string path;
        bool decided = false;
        uint64_t epoch = 0;     // Epoch when decided (0: not followed)
        std::vector<Run> runs;  // sorted, not overlapping
    };
    void decide(File& f);

    std::shared_ptr<DataSource> base_;
    Wants wants_;
    Change change_;
    Epoch epoch_;
    std::vector<File> files_;  // sorted by offset
    mutable std::mutex mutex_;
    size_t changed_ = 0;
};

// Diagnosis (LGP_ROMFS_READ_LOG=1): reads through to `base` and logs the
// path of each file the first time a read touches it, to find which files a
// screen loads (game file names: the log stays private).
class RomFsReadLog final : public DataSource {
public:
    // nullptr if `base` is no RomFS image. With `each` (non-empty) a file
    // whose path contains it is logged again on every read that starts at its
    // first byte (line `romfs read again: PATH (N times)`), to see whether the
    // game reads it anew, e.g. per battle.
    static std::shared_ptr<RomFsReadLog> wrap(std::shared_ptr<DataSource> base, std::string each = {});

    uint64_t size() const override { return base_->size(); }
    bool read(uint64_t offset, void* dst, uint64_t len) override;
    size_t file_count() const { return files_.size(); }

private:
    struct File {
        uint64_t offset;  // absolute in the image
        uint64_t size;
        std::string path;
        bool seen = false;
        uint32_t starts = 0;  // reads from its first byte (`each` only)
    };
    std::shared_ptr<DataSource> base_;
    std::string each_;
    std::vector<File> files_;  // non-empty files, sorted by offset
    std::mutex mutex_;
};

}  // namespace lgp::hle
