#include "lgp/platform/memory_storage.hpp"

#include <algorithm>
#include <cstring>
#include <new>

namespace lgp::platform {
namespace {

std::string parent_of(const std::string& path) {
    const size_t slash = path.rfind('/');
    return slash == 0 ? std::string("/") : path.substr(0, slash);
}

std::string name_of(const std::string& path) { return path.substr(path.rfind('/') + 1); }

// True if `path` lies below directory `dir` (not equal to it).
bool is_below(const std::string& path, const std::string& dir) {
    if (dir == "/")
        return path != "/";
    return path.size() > dir.size() && path.compare(0, dir.size(), dir) == 0 && path[dir.size()] == '/';
}

}  // namespace

class MemoryStorage::File final : public IFile {
public:
    File(std::shared_ptr<Blob> blob, bool writable) : blob_(std::move(blob)), writable_(writable) {}

    uint64_t size() const override {
        std::lock_guard lock(blob_->mutex);
        return blob_->data.size();
    }

    StorageError read(uint64_t offset, void* dst, uint64_t len, uint64_t* got) override {
        std::lock_guard lock(blob_->mutex);
        const uint64_t size = blob_->data.size();
        const uint64_t n = offset >= size ? 0 : std::min(len, size - offset);
        if (n != 0)
            std::memcpy(dst, blob_->data.data() + offset, n);
        *got = n;
        return StorageError::Ok;
    }

    StorageError write(uint64_t offset, const void* src, uint64_t len) override {
        if (!writable_)
            return StorageError::ReadOnly;
        if (offset > kMaxFileSize || len > kMaxFileSize - offset)
            return StorageError::OutOfRange;
        std::lock_guard lock(blob_->mutex);
        try {
            if (offset + len > blob_->data.size())
                blob_->data.resize(offset + len);
        } catch (const std::bad_alloc&) {
            return StorageError::Io;
        }
        if (len != 0)
            std::memcpy(blob_->data.data() + offset, src, len);
        return StorageError::Ok;
    }

    StorageError set_size(uint64_t size) override {
        if (!writable_)
            return StorageError::ReadOnly;
        if (size > kMaxFileSize)
            return StorageError::OutOfRange;
        std::lock_guard lock(blob_->mutex);
        try {
            blob_->data.resize(size);
        } catch (const std::bad_alloc&) {
            return StorageError::Io;
        }
        return StorageError::Ok;
    }

    StorageError flush() override { return StorageError::Ok; }

private:
    std::shared_ptr<Blob> blob_;
    bool writable_;
};

void MemoryStorage::add_file(std::string_view path, std::vector<uint8_t> data) {
    const auto p = normalize_path(path);
    if (!p || *p == "/")
        return;
    std::lock_guard lock(mutex_);
    for (std::string dir = parent_of(*p); dirs_.insert(dir).second && dir != "/";)
        dir = parent_of(dir);
    auto blob = std::make_shared<Blob>();
    blob->data = std::move(data);
    files_[*p] = std::move(blob);
}

StorageError MemoryStorage::check_parent(const std::string& path) const {
    const std::string parent = parent_of(path);
    if (dirs_.count(parent) != 0)
        return StorageError::Ok;
    return files_.count(parent) != 0 ? StorageError::NotADirectory : StorageError::NotFound;
}

bool MemoryStorage::has_children(const std::string& dir) const {
    const auto below = [&](const std::string& p) { return is_below(p, dir); };
    return std::any_of(files_.begin(), files_.end(), [&](const auto& f) { return below(f.first); }) ||
           std::any_of(dirs_.begin(), dirs_.end(), below);
}

StorageError MemoryStorage::open(std::string_view path, uint32_t mode, std::unique_ptr<IFile>* out) {
    const auto p = normalize_path(path);
    if (!p)
        return StorageError::InvalidPath;
    const bool writable = (mode & kOpenWrite) != 0;
    if (writable && read_only_)
        return StorageError::ReadOnly;
    std::lock_guard lock(mutex_);
    const auto it = files_.find(*p);
    if (it == files_.end())
        return dirs_.count(*p) != 0 ? StorageError::IsADirectory : StorageError::NotFound;
    *out = std::make_unique<File>(it->second, writable);
    return StorageError::Ok;
}

StorageError MemoryStorage::stat(std::string_view path, DirEntry* out) {
    const auto p = normalize_path(path);
    if (!p)
        return StorageError::InvalidPath;
    std::lock_guard lock(mutex_);
    if (const auto it = files_.find(*p); it != files_.end()) {
        std::lock_guard blob_lock(it->second->mutex);
        *out = {name_of(*p), false, it->second->data.size()};
        return StorageError::Ok;
    }
    if (dirs_.count(*p) != 0) {
        *out = {*p == "/" ? std::string() : name_of(*p), true, 0};
        return StorageError::Ok;
    }
    return StorageError::NotFound;
}

StorageError MemoryStorage::list(std::string_view path, std::vector<DirEntry>* out) {
    const auto p = normalize_path(path);
    if (!p)
        return StorageError::InvalidPath;
    std::lock_guard lock(mutex_);
    if (dirs_.count(*p) == 0)
        return files_.count(*p) != 0 ? StorageError::NotADirectory : StorageError::NotFound;
    out->clear();
    for (const std::string& d : dirs_) {
        if (d != "/" && parent_of(d) == *p)
            out->push_back({name_of(d), true, 0});
    }
    for (const auto& [f, blob] : files_) {
        if (parent_of(f) == *p) {
            std::lock_guard blob_lock(blob->mutex);
            out->push_back({name_of(f), false, blob->data.size()});
        }
    }
    std::sort(out->begin(), out->end(), [](const DirEntry& a, const DirEntry& b) { return a.name < b.name; });
    return StorageError::Ok;
}

StorageError MemoryStorage::create_file(std::string_view path, uint64_t size) {
    const auto p = normalize_path(path);
    if (!p || *p == "/")
        return StorageError::InvalidPath;
    if (read_only_)
        return StorageError::ReadOnly;
    if (size > kMaxFileSize)
        return StorageError::OutOfRange;
    std::lock_guard lock(mutex_);
    if (files_.count(*p) != 0 || dirs_.count(*p) != 0)
        return StorageError::AlreadyExists;
    if (const StorageError e = check_parent(*p); e != StorageError::Ok)
        return e;
    auto blob = std::make_shared<Blob>();
    try {
        blob->data.resize(size);
    } catch (const std::bad_alloc&) {
        return StorageError::Io;
    }
    files_[*p] = std::move(blob);
    return StorageError::Ok;
}

StorageError MemoryStorage::create_directory(std::string_view path) {
    const auto p = normalize_path(path);
    if (!p)
        return StorageError::InvalidPath;
    if (read_only_)
        return StorageError::ReadOnly;
    std::lock_guard lock(mutex_);
    if (files_.count(*p) != 0 || dirs_.count(*p) != 0)
        return StorageError::AlreadyExists;
    if (const StorageError e = check_parent(*p); e != StorageError::Ok)
        return e;
    dirs_.insert(*p);
    return StorageError::Ok;
}

StorageError MemoryStorage::remove_file(std::string_view path) {
    const auto p = normalize_path(path);
    if (!p)
        return StorageError::InvalidPath;
    if (read_only_)
        return StorageError::ReadOnly;
    std::lock_guard lock(mutex_);
    if (files_.erase(*p) != 0)
        return StorageError::Ok;
    return dirs_.count(*p) != 0 ? StorageError::IsADirectory : StorageError::NotFound;
}

StorageError MemoryStorage::remove_directory(std::string_view path, bool recursive) {
    const auto p = normalize_path(path);
    if (!p || *p == "/")
        return StorageError::InvalidPath;
    if (read_only_)
        return StorageError::ReadOnly;
    std::lock_guard lock(mutex_);
    if (dirs_.count(*p) == 0)
        return files_.count(*p) != 0 ? StorageError::NotADirectory : StorageError::NotFound;
    if (has_children(*p)) {
        if (!recursive)
            return StorageError::NotEmpty;
        std::erase_if(files_, [&](const auto& f) { return is_below(f.first, *p); });
        std::erase_if(dirs_, [&](const std::string& d) { return is_below(d, *p); });
    }
    dirs_.erase(*p);
    return StorageError::Ok;
}

StorageError MemoryStorage::rename(std::string_view from, std::string_view to) {
    const auto a = normalize_path(from);
    const auto b = normalize_path(to);
    if (!a || !b || *a == "/" || *b == "/")
        return StorageError::InvalidPath;
    if (read_only_)
        return StorageError::ReadOnly;
    std::lock_guard lock(mutex_);
    if (files_.count(*b) != 0 || dirs_.count(*b) != 0)
        return StorageError::AlreadyExists;
    if (const StorageError e = check_parent(*b); e != StorageError::Ok)
        return e;
    if (const auto it = files_.find(*a); it != files_.end()) {
        auto blob = it->second;
        files_.erase(it);
        files_[*b] = std::move(blob);
        return StorageError::Ok;
    }
    if (dirs_.count(*a) == 0)
        return StorageError::NotFound;
    if (is_below(*b, *a))
        return StorageError::InvalidPath;  // into itself
    const auto move = [&](const std::string& p) { return *b + p.substr(a->size()); };
    std::map<std::string, std::shared_ptr<Blob>> files;
    for (auto& [f, blob] : files_)
        files[is_below(f, *a) ? move(f) : f] = blob;
    std::set<std::string> dirs;
    for (const std::string& d : dirs_)
        dirs.insert(d == *a || is_below(d, *a) ? move(d) : d);
    files_.swap(files);
    dirs_.swap(dirs);
    return StorageError::Ok;
}

}  // namespace lgp::platform
