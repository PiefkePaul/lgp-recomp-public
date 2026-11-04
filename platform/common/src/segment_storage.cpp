// Segmented files and AddedFilesStorage (segment_storage.hpp).
#include "lgp/platform/segment_storage.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <filesystem>
#include <mutex>

namespace lgp::platform {

namespace {

bool parse_u64(std::string_view s, uint64_t* out) {
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), *out);
    return !s.empty() && ec == std::errc() && end == s.data() + s.size();
}

std::FILE* open_read(const std::string& path) {
#if defined(_WIN32)
    return _wfopen(std::filesystem::path(std::u8string(path.begin(), path.end())).c_str(), L"rb");  // UTF-8
#else
    return std::fopen(path.c_str(), "rb");
#endif
}

int seek(std::FILE* f, uint64_t offset) {
#if defined(_WIN32)
    return _fseeki64(f, static_cast<long long>(offset), SEEK_SET);
#else
    return fseeko(f, static_cast<off_t>(offset), SEEK_SET);
#endif
}

uint64_t file_length(std::FILE* f) {
#if defined(_WIN32)
    if (_fseeki64(f, 0, SEEK_END) != 0)
        return 0;
    return static_cast<uint64_t>(_ftelli64(f));
#else
    if (fseeko(f, 0, SEEK_END) != 0)
        return 0;
    return static_cast<uint64_t>(ftello(f));
#endif
}

class SegmentedFile final : public IFile {
public:
    struct Part {
        std::FILE* file;
        uint64_t offset, length, start;  // start: position in this file
    };
    explicit SegmentedFile(std::vector<Part> parts) : parts_(std::move(parts)) {
        for (const Part& p : parts_)
            size_ += p.length;
    }
    ~SegmentedFile() override {
        for (Part& p : parts_)
            std::fclose(p.file);
    }
    uint64_t size() const override { return size_; }

    StorageError read(uint64_t offset, void* dst, uint64_t len, uint64_t* got) override {
        *got = 0;
        if (offset >= size_ || len == 0)
            return StorageError::Ok;
        len = std::min(len, size_ - offset);
        // The part holding `offset`: the last one starting at or before it.
        auto it = std::upper_bound(parts_.begin(), parts_.end(), offset,
                                   [](uint64_t o, const Part& p) { return o < p.start; });
        auto* out = static_cast<uint8_t*>(dst);
        std::lock_guard lock(mutex_);
        for (--it; len > 0 && it != parts_.end(); ++it) {
            const uint64_t within = offset - it->start;
            const uint64_t n = std::min(len, it->length - within);
            if (seek(it->file, it->offset + within) != 0)
                return StorageError::Io;
            const size_t r = std::fread(out, 1, static_cast<size_t>(n), it->file);
            *got += r;
            if (r != n)
                return StorageError::Io;  // the part was checked to be long enough
            out += n;
            offset += n;
            len -= n;
        }
        return StorageError::Ok;
    }
    StorageError write(uint64_t, const void*, uint64_t) override { return StorageError::ReadOnly; }
    StorageError set_size(uint64_t) override { return StorageError::ReadOnly; }
    StorageError flush() override { return StorageError::Ok; }

private:
    std::vector<Part> parts_;
    uint64_t size_ = 0;
    std::mutex mutex_;  // one FILE position per part
};

}  // namespace

bool parse_file_segments(std::string_view spec, std::vector<FileSegment>* out, std::string* error) {
    out->clear();
    size_t pos = 0;
    while (pos <= spec.size()) {
        size_t end = spec.find(',', pos);
        if (end == std::string_view::npos)
            end = spec.size();
        const std::string_view item = spec.substr(pos, end - pos);
        FileSegment s;
        const size_t at = item.rfind('@');
        if (at == std::string_view::npos) {
            s.path = std::string(item);
            s.whole_file = true;
        } else {
            const std::string_view range = item.substr(at + 1);
            const size_t plus = range.find('+');
            if (plus == std::string_view::npos || !parse_u64(range.substr(0, plus), &s.offset) ||
                !parse_u64(range.substr(plus + 1), &s.length) || s.length == 0) {
                *error = "bad range in \"" + std::string(item) + "\" (FILE@OFFSET+LENGTH)";
                return false;
            }
            s.path = std::string(item.substr(0, at));
        }
        if (s.path.empty()) {
            *error = "empty file name in \"" + std::string(spec) + "\"";
            return false;
        }
        out->push_back(std::move(s));
        if (end == spec.size())
            break;
        pos = end + 1;
    }
    if (out->empty()) {
        *error = "no file segments";
        return false;
    }
    return true;
}

std::unique_ptr<IFile> open_segmented_file(const std::vector<FileSegment>& segments, std::string* error) {
    std::vector<SegmentedFile::Part> parts;
    auto fail = [&](const std::string& text) -> std::unique_ptr<IFile> {
        for (auto& p : parts)
            std::fclose(p.file);
        *error = text;
        return nullptr;
    };
    uint64_t start = 0;
    for (const FileSegment& s : segments) {
        std::FILE* f = open_read(s.path);
        if (f == nullptr)
            return fail("cannot open " + s.path);
        std::setvbuf(f, nullptr, _IONBF, 0);  // whole ranges in one read, as HostDirectoryStorage
        const uint64_t have = file_length(f);
        const uint64_t length = s.whole_file ? have : s.length;
        parts.push_back({f, s.offset, length, start});
        if (s.offset > have || length > have - s.offset)
            return fail(s.path + " is shorter than its range");
        start += length;
    }
    return std::make_unique<SegmentedFile>(std::move(parts));
}

std::unique_ptr<AddedFilesStorage> AddedFilesStorage::create(
    std::unique_ptr<IStorage> base, std::vector<std::pair<std::string, std::vector<FileSegment>>> files,
    std::string* error) {
    std::vector<Added> added;
    for (auto& [name, segments] : files) {
        if (name.empty() || name.find('/') != std::string::npos) {
            *error = "added file name must be a plain name: " + name;
            return nullptr;
        }
        auto file = open_segmented_file(segments, error);  // checks the parts now, not at the guest's first read
        if (!file)
            return nullptr;
        added.push_back({name, std::move(segments), file->size()});
    }
    return std::unique_ptr<AddedFilesStorage>(new AddedFilesStorage(std::move(base), std::move(added)));
}

const AddedFilesStorage::Added* AddedFilesStorage::find(std::string_view path) const {
    const auto norm = normalize_path(path);
    if (!norm || norm->size() < 2)
        return nullptr;
    for (const Added& a : added_)
        if (std::string_view(*norm).substr(1) == a.name)
            return &a;
    return nullptr;
}

StorageError AddedFilesStorage::open(std::string_view path, uint32_t mode, std::unique_ptr<IFile>* out) {
    if (const Added* a = find(path)) {
        if (mode & kOpenWrite)
            return StorageError::ReadOnly;
        std::string error;
        *out = open_segmented_file(a->segments, &error);
        return *out ? StorageError::Ok : StorageError::Io;
    }
    if (mode & kOpenWrite)
        return StorageError::ReadOnly;
    return base_ ? base_->open(path, mode, out) : StorageError::NotFound;
}

StorageError AddedFilesStorage::stat(std::string_view path, DirEntry* out) {
    if (const Added* a = find(path)) {
        *out = DirEntry{a->name, false, a->size};
        return StorageError::Ok;
    }
    return base_ ? base_->stat(path, out) : StorageError::NotFound;
}

StorageError AddedFilesStorage::list(std::string_view path, std::vector<DirEntry>* out) {
    out->clear();
    const auto norm = normalize_path(path);
    if (!norm)
        return StorageError::InvalidPath;
    StorageError e = base_ ? base_->list(path, out) : (*norm == "/" ? StorageError::Ok : StorageError::NotFound);
    if (e != StorageError::Ok || *norm != "/")
        return e;
    for (const Added& a : added_) {
        out->erase(std::remove_if(out->begin(), out->end(), [&](const DirEntry& d) { return d.name == a.name; }),
                   out->end());
        out->push_back(DirEntry{a.name, false, a.size});
    }
    std::sort(out->begin(), out->end(), [](const DirEntry& x, const DirEntry& y) { return x.name < y.name; });
    return StorageError::Ok;
}

}  // namespace lgp::platform
