#include "lgp/platform/host_directory_storage.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <system_error>
#include <vector>

#include "lgp/platform/host_sync.hpp"

namespace lgp::platform {
namespace fs = std::filesystem;

namespace {

constexpr uint64_t kMaxFileSize = 1ull << 40;

// C++20 spells UTF-8 paths as std::u8string.
std::string utf8(const fs::path& p) {
    const std::u8string s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}

fs::path from_utf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

StorageError from_error(const std::error_code& ec) {
    if (ec == std::errc::no_such_file_or_directory)
        return StorageError::NotFound;
    if (ec == std::errc::file_exists)
        return StorageError::AlreadyExists;
    if (ec == std::errc::not_a_directory)
        return StorageError::NotADirectory;
    if (ec == std::errc::is_a_directory)
        return StorageError::IsADirectory;
    if (ec == std::errc::directory_not_empty)
        return StorageError::NotEmpty;
    if (ec == std::errc::read_only_file_system || ec == std::errc::permission_denied)
        return StorageError::ReadOnly;
    return StorageError::Io;
}

// Parent of a host path must be an existing directory.
StorageError check_parent(const fs::path& p) {
    std::error_code ec;
    const fs::file_status st = fs::status(p.parent_path(), ec);
    if (st.type() == fs::file_type::not_found)
        return StorageError::NotFound;
    if (ec)
        return from_error(ec);
    return st.type() == fs::file_type::directory ? StorageError::Ok : StorageError::NotADirectory;
}

class HostFile final : public IFile {
public:
    HostFile(fs::path path, std::fstream stream, bool writable, std::shared_ptr<HostDirectoryStorage::Dirty> dirty)
        : path_(std::move(path)), stream_(std::move(stream)), writable_(writable), dirty_(std::move(dirty)) {}

    uint64_t size() const override {
        std::error_code ec;
        const uint64_t n = fs::file_size(path_, ec);
        return ec ? 0 : n;
    }

    ~HostFile() override {
        if (raw_ != nullptr)
            std::fclose(raw_);
    }

    StorageError read(uint64_t offset, void* dst, uint64_t len, uint64_t* got) override {
        std::lock_guard lock(mutex_);
        *got = 0;
        const uint64_t size = this->size();
        if (offset >= size || len == 0)
            return StorageError::Ok;
        const uint64_t n = std::min(len, size - offset);
        return read_direct(offset, dst, n, got);
    }

    StorageError write(uint64_t offset, const void* src, uint64_t len) override {
        if (!writable_)
            return StorageError::ReadOnly;
        if (offset > kMaxFileSize || len > kMaxFileSize - offset)
            return StorageError::OutOfRange;
        dirty_->file(path_);
        std::lock_guard lock(mutex_);
        const uint64_t size = this->size();
        if (offset > size) {
            // Zero-fill the gap; seeking past the end does not do that portably.
            stream_.flush();
            std::error_code ec;
            fs::resize_file(path_, offset, ec);
            if (ec)
                return from_error(ec);
        }
        stream_.clear();
        stream_.seekp(static_cast<std::streamoff>(offset));
        stream_.write(static_cast<const char*>(src), static_cast<std::streamsize>(len));
        // size() asks the file system, so nothing may stay in the stream buffer.
        stream_.flush();
        return stream_.good() ? StorageError::Ok : StorageError::Io;
    }

    StorageError set_size(uint64_t size) override {
        if (!writable_)
            return StorageError::ReadOnly;
        if (size > kMaxFileSize)
            return StorageError::OutOfRange;
        dirty_->file(path_);
        std::lock_guard lock(mutex_);
        stream_.flush();
        std::error_code ec;
        fs::resize_file(path_, size, ec);
        return ec ? from_error(ec) : StorageError::Ok;
    }

    StorageError flush() override {
        std::lock_guard lock(mutex_);
        stream_.flush();
        return stream_.good() ? StorageError::Ok : StorageError::Io;
    }

private:
    // Reads bypass the stream (writes keep using it):
    // - std::filebuf fills its 4 KiB buffer once per underflow, and in the
    //   browser every underlying read is a round trip to the OPFS worker
    //   (WasmFS), which made reading game data ≈ 3 MB/s. An unbuffered FILE*
    //   reads a whole range in one call; writes always flush, so it never sees
    //   stale data.
    // - WasmFS reads a file opened read-only through Blob.slice (an async
    //   browser read per call, milliseconds each) and one opened for writing
    //   through a synchronous access handle. So the file is opened "r+" where
    //   that works (also for read-only storage: nothing is written through it),
    //   and "r" otherwise (read-only files on the desktop).
    StorageError read_direct(uint64_t offset, void* dst, uint64_t n, uint64_t* got) {
        if (raw_ == nullptr) {
#if defined(_WIN32)
            raw_ = _wfopen(path_.c_str(), L"r+b");
            if (raw_ == nullptr)
                raw_ = _wfopen(path_.c_str(), L"rb");
#else
            raw_ = std::fopen(path_.c_str(), "r+b");
            if (raw_ == nullptr)
                raw_ = std::fopen(path_.c_str(), "rb");
#endif
            if (raw_ == nullptr)
                return StorageError::Io;
            std::setvbuf(raw_, nullptr, _IONBF, 0);
        }
#if defined(_WIN32)
        const int sought = _fseeki64(raw_, static_cast<long long>(offset), SEEK_SET);
#else
        const int sought = fseeko(raw_, static_cast<off_t>(offset), SEEK_SET);
#endif
        if (sought != 0)
            return StorageError::Io;
        *got = std::fread(dst, 1, static_cast<size_t>(n), raw_);
        return std::ferror(raw_) ? StorageError::Io : StorageError::Ok;
    }

    fs::path path_;
    std::mutex mutex_;
    std::FILE* raw_ = nullptr;
    std::fstream stream_;
    bool writable_;
    std::shared_ptr<HostDirectoryStorage::Dirty> dirty_;
};

}  // namespace

std::unique_ptr<HostDirectoryStorage> HostDirectoryStorage::open_root(const fs::path& root, bool read_only) {
    std::error_code ec;
    fs::path canonical = fs::canonical(root, ec);
    if (ec || !fs::is_directory(canonical, ec))
        return nullptr;
    return std::unique_ptr<HostDirectoryStorage>(new HostDirectoryStorage(std::move(canonical), read_only));
}

bool HostDirectoryStorage::host_path(std::string_view path, fs::path* out) const {
    const auto p = normalize_path(path);
    if (!p)
        return false;
    // "/a/b" -> root/a/b. The normalized path has no "..", ":" or "\\", and
    // every component is a plain name, so the result stays below root_.
    // "/" must be root_ itself, not root_ with a trailing separator: the
    // remove/rename guards compare against root_.
    *out = *p == "/" ? root_ : root_ / from_utf8(p->substr(1));
    return true;
}

StorageError HostDirectoryStorage::open(std::string_view path, uint32_t mode, std::unique_ptr<IFile>* out) {
    fs::path p;
    if (!host_path(path, &p))
        return StorageError::InvalidPath;
    const bool writable = (mode & kOpenWrite) != 0;
    if (writable && read_only_)
        return StorageError::ReadOnly;
    std::error_code ec;
    const fs::file_status st = fs::status(p, ec);
    if (st.type() == fs::file_type::not_found)
        return StorageError::NotFound;
    if (st.type() == fs::file_type::directory)
        return StorageError::IsADirectory;
    if (st.type() != fs::file_type::regular)
        return StorageError::Io;
    std::fstream stream(p, std::ios::binary | std::ios::in | (writable ? std::ios::out : std::ios::openmode{}));
    if (!stream.is_open())
        return writable ? StorageError::ReadOnly : StorageError::Io;
    *out = std::make_unique<HostFile>(p, std::move(stream), writable, dirty_);
    if (writable)
        dirty_->file(p);
    return StorageError::Ok;
}

StorageError HostDirectoryStorage::stat(std::string_view path, DirEntry* out) {
    fs::path p;
    if (!host_path(path, &p))
        return StorageError::InvalidPath;
    std::error_code ec;
    const fs::file_status st = fs::status(p, ec);
    if (st.type() == fs::file_type::not_found)
        return StorageError::NotFound;
    if (ec)
        return from_error(ec);
    const bool dir = st.type() == fs::file_type::directory;
    const std::string name = p == root_ ? std::string() : utf8(p.filename());
    *out = {name, dir, dir ? 0 : static_cast<uint64_t>(fs::file_size(p, ec))};
    return ec ? from_error(ec) : StorageError::Ok;
}

StorageError HostDirectoryStorage::list(std::string_view path, std::vector<DirEntry>* out) {
    fs::path p;
    if (!host_path(path, &p))
        return StorageError::InvalidPath;
    std::error_code ec;
    const fs::file_status st = fs::status(p, ec);
    if (st.type() == fs::file_type::not_found)
        return StorageError::NotFound;
    if (st.type() != fs::file_type::directory)
        return StorageError::NotADirectory;
    out->clear();
    for (fs::directory_iterator it(p, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        const bool dir = it->is_directory(e2);
        out->push_back({utf8(it->path().filename()), dir, dir ? 0 : static_cast<uint64_t>(it->file_size(e2))});
    }
    if (ec)
        return from_error(ec);
    std::sort(out->begin(), out->end(), [](const DirEntry& a, const DirEntry& b) { return a.name < b.name; });
    return StorageError::Ok;
}

StorageError HostDirectoryStorage::create_file(std::string_view path, uint64_t size) {
    fs::path p;
    if (!host_path(path, &p) || p == root_)
        return StorageError::InvalidPath;
    if (read_only_)
        return StorageError::ReadOnly;
    if (size > kMaxFileSize)
        return StorageError::OutOfRange;
    std::error_code ec;
    if (fs::exists(fs::symlink_status(p, ec)))
        return StorageError::AlreadyExists;
    if (const StorageError e = check_parent(p); e != StorageError::Ok)
        return e;
    {
        std::ofstream f(p, std::ios::binary);
        if (!f.is_open())
            return StorageError::Io;
    }
    fs::resize_file(p, size, ec);
    dirty_->file(p);
    dirty_->dir(p.parent_path());
    return ec ? from_error(ec) : StorageError::Ok;
}

StorageError HostDirectoryStorage::create_directory(std::string_view path) {
    fs::path p;
    if (!host_path(path, &p))
        return StorageError::InvalidPath;
    if (read_only_)
        return StorageError::ReadOnly;
    std::error_code ec;
    if (fs::exists(fs::symlink_status(p, ec)))
        return StorageError::AlreadyExists;
    if (const StorageError e = check_parent(p); e != StorageError::Ok)
        return e;
    fs::create_directory(p, ec);
    dirty_->dir(p);
    dirty_->dir(p.parent_path());
    return ec ? from_error(ec) : StorageError::Ok;
}

StorageError HostDirectoryStorage::remove_file(std::string_view path) {
    fs::path p;
    if (!host_path(path, &p))
        return StorageError::InvalidPath;
    if (read_only_)
        return StorageError::ReadOnly;
    std::error_code ec;
    const fs::file_status st = fs::symlink_status(p, ec);
    if (st.type() == fs::file_type::not_found)
        return StorageError::NotFound;
    if (st.type() == fs::file_type::directory)
        return StorageError::IsADirectory;
    fs::remove(p, ec);
    dirty_->dir(p.parent_path());
    return ec ? from_error(ec) : StorageError::Ok;
}

StorageError HostDirectoryStorage::remove_directory(std::string_view path, bool recursive) {
    fs::path p;
    if (!host_path(path, &p) || p == root_)
        return StorageError::InvalidPath;
    if (read_only_)
        return StorageError::ReadOnly;
    std::error_code ec;
    const fs::file_status st = fs::symlink_status(p, ec);
    if (st.type() == fs::file_type::not_found)
        return StorageError::NotFound;
    if (st.type() != fs::file_type::directory)
        return StorageError::NotADirectory;
    if (!recursive) {
        if (!fs::is_empty(p, ec))
            return StorageError::NotEmpty;
        fs::remove(p, ec);
    } else {
        fs::remove_all(p, ec);
    }
    dirty_->dir(p.parent_path());
    return ec ? from_error(ec) : StorageError::Ok;
}

StorageError HostDirectoryStorage::rename(std::string_view from, std::string_view to) {
    fs::path a;
    fs::path b;
    if (!host_path(from, &a) || !host_path(to, &b) || a == root_ || b == root_)
        return StorageError::InvalidPath;
    if (read_only_)
        return StorageError::ReadOnly;
    std::error_code ec;
    if (!fs::exists(fs::symlink_status(a, ec)))
        return StorageError::NotFound;
    // fs::rename replaces an existing target on POSIX; Horizon refuses.
    if (fs::exists(fs::symlink_status(b, ec)))
        return StorageError::AlreadyExists;
    if (const StorageError e = check_parent(b); e != StorageError::Ok)
        return e;
    const std::string as = utf8(a);
    const std::string bs = utf8(b);
    if (bs.size() > as.size() && bs.compare(0, as.size(), as) == 0 && bs[as.size()] == '/')
        return StorageError::InvalidPath;  // into itself
    fs::rename(a, b, ec);
    if (!ec) {
        // Written files keep their pending sync under the new name.
        std::lock_guard lock(dirty_->mutex);
        for (auto* set : {&dirty_->files, &dirty_->dirs}) {
            std::vector<fs::path> moved;
            for (auto it = set->begin(); it != set->end();) {
                const fs::path rel = it->lexically_relative(a);
                if (!rel.empty() && *rel.begin() != "..") {
                    moved.push_back(rel == "." ? b : b / rel);
                    it = set->erase(it);
                } else {
                    ++it;
                }
            }
            set->insert(moved.begin(), moved.end());
        }
        dirty_->dirs.insert(a.parent_path());
        dirty_->dirs.insert(b.parent_path());
    }
    return ec ? from_error(ec) : StorageError::Ok;
}

void HostDirectoryStorage::Dirty::file(const fs::path& p) {
    std::lock_guard lock(mutex);
    files.insert(p);
}

void HostDirectoryStorage::Dirty::dir(const fs::path& p) {
    std::lock_guard lock(mutex);
    dirs.insert(p);
}

StorageError HostDirectoryStorage::commit() {
    std::set<fs::path> files;
    std::set<fs::path> dirs;
    {
        std::lock_guard lock(dirty_->mutex);
        files.swap(dirty_->files);
        dirs.swap(dirty_->dirs);
    }
    // Data first, then the directory entries that point at it. A path that
    // is gone (removed after writing) has nothing left to sync.
    StorageError result = StorageError::Ok;
    for (const auto* set : {&files, &dirs}) {
        for (const fs::path& p : *set) {
            const std::error_code ec = sync_host_path(p, set == &dirs);
            if (ec && ec != std::errc::no_such_file_or_directory && result == StorageError::Ok)
                result = from_error(ec);
        }
    }
    if (result != StorageError::Ok) {  // try them again on the next commit
        std::lock_guard lock(dirty_->mutex);
        dirty_->files.insert(files.begin(), files.end());
        dirty_->dirs.insert(dirs.begin(), dirs.end());
    }
    return result;
}

}  // namespace lgp::platform
