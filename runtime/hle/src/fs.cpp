// fsp-srv: IFileSystemProxy, IStorage (RomFS, system data), IFileSystem,
// IFile and IDirectory over the host storages of FsConfig.
#include "lgp/hle/fs.hpp"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "lgp/hle/romfs.hpp"
#include "lgp/runtime/log.hpp"
#include "save_data.hpp"

namespace lgp::hle {
namespace {

using platform::StorageError;
using rt::LogLevel;
namespace fr = fs_result;

// Paths travel in X buffers of FS_MAX_PATH (0x301) bytes, NUL-terminated.
constexpr size_t kMaxPath = 0x301;
// Guest reads and writes go through a host buffer of at most this size.
constexpr uint64_t kChunk = 1ull << 20;

rt::Result to_result(StorageError e) {
    switch (e) {
    case StorageError::Ok: return rt::kSuccess;
    case StorageError::NotFound:
    case StorageError::NotADirectory:
    case StorageError::IsADirectory: return fr::kPathNotFound;
    case StorageError::AlreadyExists: return fr::kPathAlreadyExists;
    case StorageError::NotEmpty: return fr::kDirectoryNotEmpty;
    case StorageError::ReadOnly: return fr::kUnsupportedOperation;
    case StorageError::InvalidPath: return fr::kInvalidPath;
    case StorageError::OutOfRange: return fr::kOutOfRange;
    case StorageError::Io: return fr::kUnexpected;
    }
    return fr::kUnexpected;
}

std::string hex16(uint64_t v) {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016" PRIx64, v);
    return buf;
}

// Path i of the request, normalized; empty on failure (*rc set).
std::string pop_path(const Request& req, size_t i, rt::Result* rc) {
    std::vector<uint8_t> raw;
    if (!req.read_in(i, &raw, kMaxPath)) {
        *rc = rt::kernel_result::kInvalidPointer;
        return {};
    }
    const auto nul = std::find(raw.begin(), raw.end(), uint8_t{0});
    const std::string path(raw.begin(), nul);
    const auto norm = platform::normalize_path(path);
    if (nul == raw.end() || !norm) {
        rt::log(LogLevel::Warn, "fsp-srv: invalid path '%.128s'", path.c_str());
        *rc = fr::kInvalidPath;
        return {};
    }
    *rc = rt::kSuccess;
    return *norm;
}

// Copies len bytes from `read(offset, dst, n)` into the guest at addr.
template <typename ReadFn>
rt::Result copy_to_guest(const Request& req, uint64_t addr, uint64_t len, ReadFn&& read) {
    std::vector<uint8_t> tmp(static_cast<size_t>(std::min(len, kChunk)));
    for (uint64_t done = 0; done < len;) {
        const uint64_t n = std::min(len - done, kChunk);
        const rt::Result rc = read(done, tmp.data(), n);
        if (!rc.ok())
            return rc;
        if (!req.write_guest(addr + done, tmp.data(), n))
            return rt::kernel_result::kInvalidPointer;
        done += n;
    }
    return rt::kSuccess;
}

// ---------------------------------------------------------------- IStorage

class DataStorage final : public Service {
public:
    explicit DataStorage(std::shared_ptr<DataSource> source) : Service("fsp:IStorage"), source_(std::move(source)) {
        on(0, [this](Request& req, Response& resp) {  // Read(s64 offset, s64 size, out buffer 0x46)
            const int64_t offset = req.pop<int64_t>();
            const int64_t size = req.pop<int64_t>();
            const HipcBuffer b = req.out_buffer(0);
            if (offset < 0) {
                resp.result = fr::kInvalidOffset;
            } else if (size < 0 || static_cast<uint64_t>(size) > b.size) {
                resp.result = fr::kInvalidSize;
            } else if (static_cast<uint64_t>(offset) > source_->size() ||
                       static_cast<uint64_t>(size) > source_->size() - static_cast<uint64_t>(offset)) {
                resp.result = fr::kOutOfRange;
            } else {
                const auto base = static_cast<uint64_t>(offset);
                resp.result = copy_to_guest(req, b.addr, static_cast<uint64_t>(size),
                                            [this, base](uint64_t at, uint8_t* dst, uint64_t n) {
                                                return source_->read(base + at, dst, n) ? rt::kSuccess
                                                                                        : fr::kUnexpected;
                                            });
            }
        });
        on(1, [](Request&, Response& resp) { resp.result = fr::kUnsupportedOperation; });  // Write
        on(2, [](Request&, Response&) {});                                                  // Flush
        on(3, [](Request&, Response& resp) { resp.result = fr::kUnsupportedOperation; });  // SetSize
        on(4, [this](Request&, Response& resp) { resp.push<int64_t>(static_cast<int64_t>(source_->size())); });
    }

private:
    std::shared_ptr<DataSource> source_;
};

// ------------------------------------------------------------------- IFile

constexpr uint32_t kModeRead = 1, kModeWrite = 2, kModeAppend = 4;

class File final : public Service {
public:
    // save: the save data the file belongs to, kept mounted while it is open.
    File(std::unique_ptr<platform::IFile> file, uint32_t mode, std::shared_ptr<detail::SaveData> save)
        : Service("fsp:IFile"), file_(std::move(file)), mode_(mode), save_(std::move(save)) {
        if (save_ && (mode_ & kModeWrite))
            save_->writer_opened();
        // Read(u32 option, s64 offset, s64 size, out buffer 0x46) -> s64 bytes read
        on(0, [this](Request& req, Response& resp) {
            req.pop<uint32_t>();
            const int64_t offset = req.pop<int64_t>();
            const int64_t size = req.pop<int64_t>();
            const HipcBuffer b = req.out_buffer(0);
            std::lock_guard lock(mutex_);
            if (!(mode_ & kModeRead)) {
                resp.result = fr::kReadNotPermitted;
            } else if (offset < 0) {
                resp.result = fr::kInvalidOffset;
            } else if (size < 0 || static_cast<uint64_t>(size) > b.size) {
                resp.result = fr::kInvalidSize;
            } else if (static_cast<uint64_t>(offset) > file_->size()) {
                resp.result = fr::kOutOfRange;
            } else {
                const auto base = static_cast<uint64_t>(offset);
                const uint64_t len = std::min(static_cast<uint64_t>(size), file_->size() - base);
                resp.result = copy_to_guest(req, b.addr, len, [this, base](uint64_t at, uint8_t* dst, uint64_t n) {
                    uint64_t got = 0;
                    const StorageError e = file_->read(base + at, dst, n, &got);
                    if (e != StorageError::Ok)
                        return to_result(e);
                    std::memset(dst + got, 0, n - got);  // shrank meanwhile
                    return rt::kSuccess;
                });
                if (resp.result.ok())
                    resp.push<int64_t>(static_cast<int64_t>(len));
            }
        });
        // Write(u32 option, s64 offset, s64 size, in buffer 0x45); option bit 0: flush
        on(1, [this](Request& req, Response& resp) {
            const uint32_t option = req.pop<uint32_t>();
            const int64_t offset = req.pop<int64_t>();
            const int64_t size = req.pop<int64_t>();
            const HipcBuffer b = req.in_buffer(0);
            std::lock_guard lock(mutex_);
            if (!(mode_ & kModeWrite)) {
                resp.result = fr::kWriteNotPermitted;
            } else if (offset < 0) {
                resp.result = fr::kInvalidOffset;
            } else if (size < 0 || static_cast<uint64_t>(size) > b.size) {
                resp.result = fr::kInvalidSize;
            } else if (static_cast<uint64_t>(offset) + static_cast<uint64_t>(size) > file_->size() &&
                       !(mode_ & kModeAppend)) {
                resp.result = fr::kFileExtensionWithoutOpenModeAllowAppend;
            } else {
                resp.result = write(req, static_cast<uint64_t>(offset), b.addr, static_cast<uint64_t>(size));
                if (resp.result.ok() && (option & 1))
                    resp.result = to_result(file_->flush());
            }
        });
        on(2, [this](Request&, Response& resp) {  // Flush
            std::lock_guard lock(mutex_);
            if (mode_ & kModeWrite)
                resp.result = to_result(file_->flush());
        });
        on(3, [this](Request& req, Response& resp) {  // SetSize(s64)
            const int64_t size = req.pop<int64_t>();
            std::lock_guard lock(mutex_);
            if (!(mode_ & kModeWrite))
                resp.result = fr::kWriteNotPermitted;
            else if (size < 0)
                resp.result = fr::kInvalidSize;
            else
                resp.result = to_result(file_->set_size(static_cast<uint64_t>(size)));
        });
        on(4, [this](Request&, Response& resp) {  // GetSize
            std::lock_guard lock(mutex_);
            resp.push<int64_t>(static_cast<int64_t>(file_->size()));
        });
    }

    ~File() override {
        if (save_ && (mode_ & kModeWrite))
            save_->writer_closed();
    }

private:
    rt::Result write(const Request& req, uint64_t offset, uint64_t addr, uint64_t len) {
        std::vector<uint8_t> tmp(static_cast<size_t>(std::min(len, kChunk)));
        for (uint64_t done = 0; done < len;) {
            const uint64_t n = std::min(len - done, kChunk);
            if (!req.read_guest(addr + done, tmp.data(), n))
                return rt::kernel_result::kInvalidPointer;
            const StorageError e = file_->write(offset + done, tmp.data(), n);
            if (e != StorageError::Ok)
                return to_result(e);
            done += n;
        }
        return rt::kSuccess;
    }

    std::mutex mutex_;
    std::unique_ptr<platform::IFile> file_;
    const uint32_t mode_;
    const std::shared_ptr<detail::SaveData> save_;
};

// -------------------------------------------------------------- IDirectory

// DirectoryEntry (libnx FsDirectoryEntry): name[0x301], attributes u8,
// pad, type u8 at 0x304 (0 directory, 1 file), pad, s64 size at 0x308.
constexpr size_t kDirEntrySize = 0x310;

class Directory final : public Service {
public:
    explicit Directory(std::vector<platform::DirEntry> entries) : Service("fsp:IDirectory"), entries_(std::move(entries)) {
        on(0, [this](Request& req, Response& resp) {  // Read(out buffer 0x6) -> s64 count
            const HipcBuffer b = req.out_buffer(0);
            std::lock_guard lock(mutex_);
            const uint64_t room = b.size / kDirEntrySize;
            const uint64_t n = std::min<uint64_t>(room, entries_.size() - next_);
            std::vector<uint8_t> out(static_cast<size_t>(n * kDirEntrySize), 0);
            for (uint64_t k = 0; k < n; ++k) {
                const platform::DirEntry& e = entries_[next_ + k];
                uint8_t* p = out.data() + k * kDirEntrySize;
                std::memcpy(p, e.name.data(), std::min<size_t>(e.name.size(), kMaxPath - 1));
                p[0x304] = e.is_directory ? 0 : 1;
                const int64_t size = e.is_directory ? 0 : static_cast<int64_t>(e.size);
                std::memcpy(p + 0x308, &size, 8);
            }
            if (n != 0 && !req.write_guest(b.addr, out.data(), out.size())) {
                resp.result = rt::kernel_result::kInvalidPointer;
                return;
            }
            next_ += n;
            resp.push<int64_t>(static_cast<int64_t>(n));
        });
        on(1, [this](Request&, Response& resp) {  // GetEntryCount
            resp.push<int64_t>(static_cast<int64_t>(entries_.size()));
        });
    }

private:
    std::mutex mutex_;
    const std::vector<platform::DirEntry> entries_;
    uint64_t next_ = 0;
};

// ------------------------------------------------------------- IFileSystem

// A mounted save data as a file system: guest paths are below its working
// copy. Commands of all sessions on one save run under the save's lock.
class FileSystem final : public Service {
public:
    FileSystem(std::shared_ptr<detail::SaveData> save, uint64_t space_size)
        : Service("fsp:IFileSystem"), save_(std::move(save)), fs_(save_->storage()), root_(save_->work_root()),
          space_size_(space_size) {
        locked(0, [this](Request& req, Response& resp) {  // CreateFile(u32 option, s64 size, path)
            req.pop<uint32_t>();
            const int64_t size = req.pop<int64_t>();
            const std::string p = path(req, 0, &resp.result);
            if (!resp.result.ok())
                return;
            resp.result = size < 0 ? fr::kInvalidSize : to_result(fs_.create_file(p, static_cast<uint64_t>(size)));
        });
        locked(1, [this](Request& req, Response& resp) {  // DeleteFile
            with_path(req, resp, [this](const std::string& p) { return fs_.remove_file(p); });
        });
        locked(2, [this](Request& req, Response& resp) {  // CreateDirectory
            with_path(req, resp, [this](const std::string& p) { return fs_.create_directory(p); });
        });
        locked(3, [this](Request& req, Response& resp) {  // DeleteDirectory
            with_path(req, resp, [this](const std::string& p) { return remove_dir(p, false); });
        });
        locked(4, [this](Request& req, Response& resp) {  // DeleteDirectoryRecursively
            with_path(req, resp, [this](const std::string& p) { return remove_dir(p, true); });
        });
        locked(5, [this](Request& req, Response& resp) { rename(req, resp, false); });  // RenameFile
        locked(6, [this](Request& req, Response& resp) { rename(req, resp, true); });   // RenameDirectory
        locked(7, [this](Request& req, Response& resp) {  // GetEntryType -> u32 (0 directory, 1 file)
            const std::string p = path(req, 0, &resp.result);
            if (!resp.result.ok())
                return;
            platform::DirEntry e;
            resp.result = to_result(fs_.stat(p, &e));
            if (resp.result.ok())
                resp.push<uint32_t>(e.is_directory ? 0 : 1);
        });
        locked(8, [this](Request& req, Response& resp) {  // OpenFile(u32 mode, path) -> IFile
            const uint32_t mode = req.pop<uint32_t>();
            const std::string p = path(req, 0, &resp.result);
            if (!resp.result.ok())
                return;
            if ((mode & (kModeRead | kModeWrite)) == 0 || (mode & ~(kModeRead | kModeWrite | kModeAppend)) != 0) {
                resp.result = fr::kInvalidArgument;
                return;
            }
            std::unique_ptr<platform::IFile> f;
            uint32_t host_mode = 0;
            if (mode & kModeRead)
                host_mode |= platform::kOpenRead;
            if (mode & kModeWrite)
                host_mode |= platform::kOpenWrite;
            resp.result = to_result(fs_.open(p, host_mode, &f));
            if (resp.result.ok())
                resp.push_object(std::make_shared<File>(std::move(f), mode, save_));
        });
        locked(9, [this](Request& req, Response& resp) {  // OpenDirectory(u32 mode, path) -> IDirectory
            const uint32_t mode = req.pop<uint32_t>();
            const std::string p = path(req, 0, &resp.result);
            if (!resp.result.ok())
                return;
            std::vector<platform::DirEntry> all;
            resp.result = to_result(fs_.list(p, &all));
            if (!resp.result.ok())
                return;
            std::vector<platform::DirEntry> wanted;
            for (auto& e : all)
                if ((e.is_directory && (mode & 1)) || (!e.is_directory && (mode & 2)))
                    wanted.push_back(std::move(e));
            resp.push_object(std::make_shared<Directory>(std::move(wanted)));
        });
        locked(10, [this](Request&, Response& resp) {  // Commit
            // As nn::fs: files open for writing must be closed first.
            resp.result = save_->writers() > 0 ? fr::kWriteModeFileNotClosed : to_result(save_->commit());
        });
        locked(11, [this](Request& req, Response& resp) {  // GetFreeSpaceSize(path) -> s64
            path(req, 0, &resp.result);
            if (resp.result.ok()) {
                const uint64_t used = used_bytes(root_);
                resp.push<int64_t>(static_cast<int64_t>(used >= space_size_ ? 0 : space_size_ - used));
            }
        });
        locked(12, [this](Request& req, Response& resp) {  // GetTotalSpaceSize(path) -> s64
            path(req, 0, &resp.result);
            if (resp.result.ok())
                resp.push<int64_t>(static_cast<int64_t>(space_size_));
        });
        locked(13, [this](Request& req, Response& resp) {  // CleanDirectoryRecursively
            with_path(req, resp, [this](const std::string& p) {
                std::vector<platform::DirEntry> list;
                StorageError e = fs_.list(p, &list);
                for (size_t k = 0; k < list.size() && e == StorageError::Ok; ++k) {
                    const std::string child = p == "/" ? "/" + list[k].name : p + "/" + list[k].name;
                    e = list[k].is_directory ? fs_.remove_directory(child, true) : fs_.remove_file(child);
                }
                return e;
            });
        });
    }

private:
    template <typename Fn>
    void locked(uint32_t command, Fn fn) {
        on(command, [this, fn](Request& req, Response& resp) {
            std::lock_guard lock(save_->mutex());
            fn(req, resp);
        });
    }

    // Guest path i mapped below root_.
    std::string path(const Request& req, size_t i, rt::Result* rc) const {
        const std::string p = pop_path(req, i, rc);
        if (!rc->ok())
            return {};
        return p == "/" ? root_ : root_ + p;
    }

    template <typename Fn>
    void with_path(Request& req, Response& resp, Fn&& fn) {
        const std::string p = path(req, 0, &resp.result);
        if (resp.result.ok())
            resp.result = to_result(fn(p));
    }

    StorageError remove_dir(const std::string& p, bool recursive) {
        // The root of a save cannot be deleted (it is the mount point).
        if (p == root_)
            return StorageError::InvalidPath;
        return fs_.remove_directory(p, recursive);
    }

    void rename(Request& req, Response& resp, bool directory) {
        const std::string from = path(req, 0, &resp.result);
        if (!resp.result.ok())
            return;
        const std::string to = path(req, 1, &resp.result);
        if (!resp.result.ok())
            return;
        platform::DirEntry e;
        resp.result = to_result(fs_.stat(from, &e));
        if (resp.result.ok() && e.is_directory != directory)
            resp.result = fr::kPathNotFound;
        if (resp.result.ok())
            resp.result = to_result(directory ? detail::rename_directory(fs_, from, to) : fs_.rename(from, to));
    }

    uint64_t used_bytes(const std::string& dir) {
        std::vector<platform::DirEntry> list;
        if (fs_.list(dir, &list) != StorageError::Ok)
            return 0;
        uint64_t sum = 0;
        for (const auto& e : list)
            sum += e.is_directory ? used_bytes(dir + "/" + e.name) : e.size;
        return sum;
    }

    const std::shared_ptr<detail::SaveData> save_;
    platform::IStorage& fs_;
    const std::string root_;  // normalized, never "/"
    const uint64_t space_size_;
};

// --------------------------------------------------------- IFileSystemProxy

// SaveDataAttribute (libnx FsSaveDataAttribute), 0x40 bytes.
struct SaveDataAttribute {
    uint64_t program_id;
    uint64_t uid[2];
    uint64_t system_save_data_id;
    uint8_t type;  // 0 System, 1 Account, 2 Bcat, 3 Device, 4 Temporary, 5 Cache, 6 SystemBcat
    uint8_t rank;
    uint16_t index;
    uint8_t pad[0x1C];
};
static_assert(sizeof(SaveDataAttribute) == 0x40);

// Data archives shared by all sessions: built once (a RomFS image of a large
// tree takes a while), kept for the process.
class DataArchives {
public:
    DataArchives(platform::IStorage* game, std::map<std::string, std::vector<uint8_t>> romfs_replace,
                 std::function<bool(const std::string&)> romfs_keep, FsConfig::RomFsChanges changes)
        : game_(game),
          romfs_replace_(std::move(romfs_replace)),
          romfs_keep_(std::move(romfs_keep)),
          changes_(std::move(changes)) {}

    // `image` (a file) or `dir` (a tree) of the game storage; nullptr if neither.
    std::shared_ptr<DataSource> open(const std::string& image, const std::string& dir) {
        std::lock_guard lock(mutex_);
        auto it = cache_.find(image);
        if (it != cache_.end())
            return it->second;
        std::shared_ptr<DataSource> src;
        if (game_ != nullptr) {
            std::unique_ptr<platform::IFile> f;
            if (game_->open(image, platform::kOpenRead, &f) == StorageError::Ok) {
                const bool compressed = is_compressed_image(*f);
                std::string why;
                src = open_image_source(std::move(f), &why);
                if (!src)
                    rt::log(LogLevel::Error, "fsp-srv: %s: %s", image.c_str(), why.c_str());
                else if (compressed)
                    rt::log(LogLevel::Info, "fsp-srv: %s is compressed (%" PRIu64 " bytes as an image)",
                            image.c_str(), src->size());
            } else {
                platform::DirEntry e;
                if (game_->stat(dir, &e) == StorageError::Ok && e.is_directory) {
                    StorageError err = StorageError::Ok;
                    auto img = RomFsImage::build(*game_, dir, &err);
                    if (img)
                        rt::log(LogLevel::Info, "fsp-srv: built RomFS image of %s: %zu files, %zu directories",
                                dir.c_str(), img->file_count(), img->directory_count());
                    else
                        rt::log(LogLevel::Error, "fsp-srv: cannot build RomFS image of %s: %s", dir.c_str(),
                                platform::storage_error_name(err));
                    src = std::move(img);
                }
            }
        }
        if (src && image == "/romfs.bin" && romfs_keep_) {
            for (auto it = romfs_replace_.begin(); it != romfs_replace_.end();)
                it = romfs_keep_(it->first) ? std::next(it) : romfs_replace_.erase(it);
            romfs_keep_ = nullptr;
        }
        // A mod's changes: files changed at mount join the replaced ones.
        std::vector<std::string> at_mount;
        const bool changing = src && image == "/romfs.bin" && changes_.begin && changes_.change &&
                              changes_.begin(&at_mount);
        if (changing) {
            const auto start = std::chrono::steady_clock::now();
            size_t changed = 0;
            for (const std::string& path : at_mount) {
                std::vector<uint8_t> bytes;
                if (auto r = romfs_replace_.find(path); r != romfs_replace_.end()) {
                    bytes = r->second;
                } else {
                    uint64_t offset = 0, size = 0;
                    if (!romfs_find(*src, path, &offset, &size))
                        continue;
                    bytes.resize(size);
                    if (!src->read(offset, bytes.data(), size)) {
                        rt::log(LogLevel::Warn, "fsp-srv: cannot read %s to change it", path.c_str());
                        continue;
                    }
                }
                if (changes_.change(path, bytes)) {
                    romfs_replace_[path] = std::move(bytes);
                    ++changed;
                }
            }
            if (!at_mount.empty())
                rt::log(LogLevel::Info, "fsp-srv: %zu of %zu RomFS files changed by mods at mount (%.0f ms)", changed,
                        at_mount.size(),
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        }
        if (src && image == "/romfs.bin" && !romfs_replace_.empty()) {
            std::string why;
            if (auto overlay = RomFsOverlay::build(src, romfs_replace_, &why)) {
                const size_t added = overlay->added();
                src = std::move(overlay);
                rt::log(LogLevel::Info, "fsp-srv: %zu RomFS files replaced by mods, %zu added",
                        romfs_replace_.size() - added, added);
            } else {
                rt::log(LogLevel::Error, "fsp-srv: mod files for the RomFS left out: %s", why.c_str());
            }
            romfs_replace_.clear();  // the overlay holds its own copy; the image is cached from here on
        }
        if (changing && changes_.wants) {
            auto wants = [w = changes_.wants, skip = std::set<std::string>(at_mount.begin(), at_mount.end())](
                             const std::string& path, std::span<const uint8_t> head) {
                return skip.count(path) == 0 && w(path, head);
            };
            const auto start = std::chrono::steady_clock::now();
            if (auto rewriter = RomFsRewriter::wrap(src, std::move(wants), changes_.change, changes_.epoch)) {
                rt::log(LogLevel::Info, "fsp-srv: mods may change %zu RomFS files as they are read (listed in %.0f ms)",
                        rewriter->file_count(),
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
                src = std::move(rewriter);
            }
        }
        if (src && image == "/romfs.bin")
            changes_ = {};  // asked once
        if (src && image == "/romfs.bin") {
            const char* env = std::getenv("LGP_ROMFS_READ_LOG");
            if (env != nullptr && env[0] != '\0' && env[0] != '0') {
                // "1": the first read of each file; other text: also every
                // read again of the files whose path contains it.
                const std::string each = std::string(env) == "1" ? std::string() : std::string(env);
                if (auto logged = RomFsReadLog::wrap(src, each)) {
                    rt::log(LogLevel::Info, "fsp-srv: logging the first read of each of %zu RomFS files",
                            logged->file_count());
                    src = std::move(logged);
                }
            }
        }
        if (src)
            cache_.emplace(image, src);
        return src;
    }

private:
    platform::IStorage* game_;
    std::map<std::string, std::vector<uint8_t>> romfs_replace_;
    std::function<bool(const std::string&)> romfs_keep_;
    FsConfig::RomFsChanges changes_;
    std::mutex mutex_;
    std::map<std::string, std::shared_ptr<DataSource>> cache_;
};

class FileSystemProxy final : public Service {
public:
    FileSystemProxy(const FsConfig& config, std::shared_ptr<DataArchives> archives,
                    std::shared_ptr<detail::SaveRegistry> saves)
        : Service("fsp-srv"), config_(config), archives_(std::move(archives)), saves_(std::move(saves)) {
        on(1, [](Request&, Response&) {});  // SetCurrentProcess(u64, pid)
        on(51, [this](Request& req, Response& resp) {  // OpenSaveDataFileSystem(u8 space, attribute)
            const uint8_t space = req.pop<uint8_t>();
            const auto attr = req.pop<SaveDataAttribute>();
            open_save(space, attr, resp);
        });
        on(200, [this](Request&, Response& resp) {  // OpenDataStorageByCurrentProcess
            open_data(resp, "/romfs.bin", "/romfs");
        });
        on(202, [this](Request& req, Response& resp) {  // OpenDataStorageByDataId(u8 storage, u64 data id)
            const uint8_t storage = req.pop<uint8_t>();
            const uint64_t data_id = req.pop<uint64_t>();
            rt::log(LogLevel::Debug, "fsp-srv: OpenDataStorageByDataId(%u, %016" PRIx64 ")", storage, data_id);
            open_data(resp, "/systemdata/" + hex16(data_id) + ".bin", "/systemdata/" + hex16(data_id));
        });
        on(1005, [](Request&, Response& resp) { resp.push<uint32_t>(0); });  // GetGlobalAccessLogMode
    }

private:
    void open_data(Response& resp, const std::string& image, const std::string& dir) {
        auto src = archives_->open(image, dir);
        if (!src) {
            rt::log(LogLevel::Warn, "fsp-srv: no %s or %s in the game storage", image.c_str(), dir.c_str());
            resp.result = fr::kTargetNotFound;
            return;
        }
        resp.push_object(std::make_shared<DataStorage>(std::move(src)));
    }

    void open_save(uint8_t space, const SaveDataAttribute& attr, Response& resp) {
        const uint64_t program = attr.program_id != 0 ? attr.program_id : config_.program_id;
        std::string root;
        switch (attr.type) {
        case 0: root = "/system/" + hex16(attr.system_save_data_id); break;
        case 1: root = "/" + hex16(program) + "/account/" + hex16(attr.uid[0]) + hex16(attr.uid[1]); break;
        case 2: root = "/" + hex16(program) + "/bcat"; break;
        case 3: root = "/" + hex16(program) + "/device"; break;
        case 4: root = "/" + hex16(program) + "/temporary"; break;
        case 5: root = "/" + hex16(program) + "/cache/" + std::to_string(attr.index); break;
        default:
            rt::log(LogLevel::Warn, "fsp-srv: save data type %u not supported", attr.type);
            resp.result = fr::kInvalidArgument;
            return;
        }
        rt::log(LogLevel::Info, "fsp-srv: OpenSaveDataFileSystem(space %u) -> %s", space, root.c_str());
        if (config_.save == nullptr) {
            rt::log(LogLevel::Warn, "fsp-srv: no save storage configured");
            resp.result = fr::kTargetNotFound;
            return;
        }
        // Created on first use: the game never creates its save data itself
        // (that is the system's job when the application starts).
        for (size_t at = 1; at <= root.size(); ++at) {
            if (at != root.size() && root[at] != '/')
                continue;
            const StorageError e = config_.save->create_directory(root.substr(0, at));
            if (e != StorageError::Ok && e != StorageError::AlreadyExists) {
                rt::log(LogLevel::Error, "fsp-srv: cannot create %s: %s", root.substr(0, at).c_str(),
                        platform::storage_error_name(e));
                resp.result = to_result(e);
                return;
            }
        }
        // Temporary storage is not journaled on the console either.
        StorageError err = StorageError::Ok;
        auto save = saves_->mount(*config_.save, root, attr.type != 4, &err);
        if (!save) {
            resp.result = to_result(err);
            return;
        }
        resp.push_object(std::make_shared<FileSystem>(std::move(save), config_.save_space_size));
    }

    const FsConfig config_;
    std::shared_ptr<DataArchives> archives_;
    std::shared_ptr<detail::SaveRegistry> saves_;
};

}  // namespace

void register_fs(ServiceManager& sm, FsConfig config) {
    // The replaced RomFS files go to the archives alone, not into every session's copy of the config.
    auto archives = std::make_shared<DataArchives>(config.game, std::move(config.romfs_replace),
                                                   std::move(config.romfs_keep), std::move(config.romfs_changes));
    config.romfs_replace.clear();
    config.romfs_keep = nullptr;
    config.romfs_changes = {};
    auto saves = std::make_shared<detail::SaveRegistry>(config.on_commit);
    sm.register_service("fsp-srv", [config, archives, saves] {
        return std::make_shared<FileSystemProxy>(config, archives, saves);
    });
}

}  // namespace lgp::hle
