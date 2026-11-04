#include "lgp/hle/romfs.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstring>
#include <deque>

#include "lgp/runtime/log.hpp"

namespace lgp::hle {
namespace {

using platform::StorageError;

constexpr uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

void put32(std::vector<uint8_t>& buf, uint64_t at, uint32_t v) { std::memcpy(buf.data() + at, &v, 4); }
void put64(std::vector<uint8_t>& buf, uint64_t at, uint64_t v) { std::memcpy(buf.data() + at, &v, 8); }

std::string join(const std::string& dir, const std::string& name) { return dir == "/" ? "/" + name : dir + "/" + name; }

}  // namespace

bool FileSource::read(uint64_t offset, void* dst, uint64_t len) {
    std::lock_guard lock(mutex_);
    if (offset > file_->size() || len > file_->size() - offset)
        return false;
    uint64_t got = 0;
    return file_->read(offset, dst, len, &got) == StorageError::Ok && got == len;
}

uint32_t RomFsImage::hash(uint32_t parent, std::string_view name) {
    uint32_t h = parent ^ 123456789u;
    for (const char c : name) {
        h = (h >> 5) | (h << 27);
        h ^= static_cast<uint8_t>(c);
    }
    return h;
}

uint32_t RomFsImage::bucket_count(uint32_t entries) {
    if (entries < 3)
        return 3;
    if (entries < 19)
        return entries | 1;
    uint32_t n = entries;
    while (n % 2 == 0 || n % 3 == 0 || n % 5 == 0 || n % 7 == 0 || n % 11 == 0 || n % 13 == 0 || n % 17 == 0)
        ++n;
    return n;
}

std::shared_ptr<RomFsImage> RomFsImage::build(platform::IStorage& fs, std::string_view root, StorageError* error) {
    struct Dir {
        std::string path;
        std::string name;
        size_t parent;
        std::vector<size_t> subdirs;
        std::vector<size_t> files;
        uint32_t offset = 0;
    };
    struct Entry {
        std::string path;
        std::string name;
        size_t parent;
        uint64_t size;
        uint32_t offset = 0;
        uint64_t data = 0;  // relative to the data region
    };

    const auto norm = platform::normalize_path(root);
    if (!norm) {
        *error = StorageError::InvalidPath;
        return nullptr;
    }
    std::vector<Dir> dirs{{*norm, "", 0, {}, {}, 0}};
    std::vector<Entry> files;
    // Breadth first: a directory's files and subdirectories (sorted by name)
    // are listed before anything deeper.
    for (size_t i = 0; i < dirs.size(); ++i) {
        std::vector<platform::DirEntry> list;
        const StorageError e = fs.list(dirs[i].path, &list);
        if (e != StorageError::Ok) {
            *error = e;
            return nullptr;
        }
        for (const auto& d : list) {
            if (d.is_directory) {
                dirs[i].subdirs.push_back(dirs.size());
                dirs.push_back({join(dirs[i].path, d.name), d.name, i, {}, {}, 0});
            } else {
                dirs[i].files.push_back(files.size());
                files.push_back({join(dirs[i].path, d.name), d.name, i, d.size, 0, 0});
            }
        }
    }
    // Files in the file table follow the directory order.
    std::vector<size_t> file_order;
    for (const Dir& d : dirs)
        file_order.insert(file_order.end(), d.files.begin(), d.files.end());

    uint64_t dir_table_size = 0;
    for (Dir& d : dirs) {
        d.offset = static_cast<uint32_t>(dir_table_size);
        dir_table_size += 0x18 + align_up(d.name.size(), 4);
    }
    uint64_t file_table_size = 0;
    uint64_t data_size = 0;
    for (size_t f : file_order) {
        Entry& e = files[f];
        e.offset = static_cast<uint32_t>(file_table_size);
        file_table_size += 0x20 + align_up(e.name.size(), 4);
        e.data = align_up(data_size, 0x10);
        data_size = e.data + e.size;
    }
    if (dir_table_size > 0xFFFFFFF0ull || file_table_size > 0xFFFFFFF0ull) {
        *error = StorageError::OutOfRange;
        return nullptr;
    }

    const uint32_t dir_buckets = bucket_count(static_cast<uint32_t>(dirs.size()));
    const uint32_t file_buckets = bucket_count(static_cast<uint32_t>(files.size()));
    std::shared_ptr<RomFsImage> img(new RomFsImage(fs));
    img->tables_offset_ = align_up(kDataOffset + data_size, 4);
    const uint64_t dir_hash_off = 0;
    const uint64_t dir_table_off = dir_hash_off + 4ull * dir_buckets;
    const uint64_t file_hash_off = dir_table_off + dir_table_size;
    const uint64_t file_table_off = file_hash_off + 4ull * file_buckets;
    std::vector<uint8_t>& t = img->tables_;
    t.assign(file_table_off + file_table_size, 0);
    for (uint32_t b = 0; b < dir_buckets; ++b)
        put32(t, dir_hash_off + 4ull * b, kNone);
    for (uint32_t b = 0; b < file_buckets; ++b)
        put32(t, file_hash_off + 4ull * b, kNone);

    for (size_t i = 0; i < dirs.size(); ++i) {
        const Dir& d = dirs[i];
        const uint64_t at = dir_table_off + d.offset;
        const uint32_t parent = dirs[d.parent].offset;  // the root is its own parent
        uint32_t sibling = kNone;
        if (i != 0) {
            const auto& sibs = dirs[d.parent].subdirs;
            const auto it = std::find(sibs.begin(), sibs.end(), i);
            if (it + 1 != sibs.end())
                sibling = dirs[*(it + 1)].offset;
        }
        // Later entries become the bucket head (as Nintendo's builder does).
        const uint64_t bucket = dir_hash_off + 4ull * (hash(parent, d.name) % dir_buckets);
        uint32_t next = 0;
        std::memcpy(&next, t.data() + bucket, 4);
        put32(t, at + 0x00, parent);
        put32(t, at + 0x04, sibling);
        put32(t, at + 0x08, d.subdirs.empty() ? kNone : dirs[d.subdirs.front()].offset);
        put32(t, at + 0x0C, d.files.empty() ? kNone : files[d.files.front()].offset);
        put32(t, at + 0x10, next);
        put32(t, at + 0x14, static_cast<uint32_t>(d.name.size()));
        std::memcpy(t.data() + at + 0x18, d.name.data(), d.name.size());
        put32(t, bucket, d.offset);
    }
    for (const Dir& d : dirs) {
        for (size_t k = 0; k < d.files.size(); ++k) {
            const Entry& e = files[d.files[k]];
            const uint64_t at = file_table_off + e.offset;
            const uint32_t parent = d.offset;
            const uint64_t bucket = file_hash_off + 4ull * (hash(parent, e.name) % file_buckets);
            uint32_t next = 0;
            std::memcpy(&next, t.data() + bucket, 4);
            put32(t, at + 0x00, parent);
            put32(t, at + 0x04, k + 1 < d.files.size() ? files[d.files[k + 1]].offset : kNone);
            put64(t, at + 0x08, e.data);
            put64(t, at + 0x10, e.size);
            put32(t, at + 0x18, next);
            put32(t, at + 0x1C, static_cast<uint32_t>(e.name.size()));
            std::memcpy(t.data() + at + 0x20, e.name.data(), e.name.size());
            put32(t, bucket, e.offset);
        }
    }

    std::vector<uint8_t>& h = img->header_;
    h.assign(kHeaderSize, 0);
    const uint64_t base = img->tables_offset_;
    put64(h, 0x00, kHeaderSize);
    put64(h, 0x08, base + dir_hash_off);
    put64(h, 0x10, 4ull * dir_buckets);
    put64(h, 0x18, base + dir_table_off);
    put64(h, 0x20, dir_table_size);
    put64(h, 0x28, base + file_hash_off);
    put64(h, 0x30, 4ull * file_buckets);
    put64(h, 0x38, base + file_table_off);
    put64(h, 0x40, file_table_size);
    put64(h, 0x48, kDataOffset);

    img->size_ = base + t.size();
    img->directory_count_ = dirs.size();
    img->file_count_ = files.size();
    for (size_t f : file_order)
        if (files[f].size != 0)
            img->files_.push_back({kDataOffset + files[f].data, files[f].size, files[f].path});
    *error = StorageError::Ok;
    return img;
}

bool RomFsImage::read_file(size_t index, uint64_t offset, uint8_t* dst, uint64_t len) {
    // Called with mutex_ held.
    auto it = open_.find(index);
    if (it == open_.end()) {
        if (open_.size() >= 64)
            open_.clear();
        std::unique_ptr<platform::IFile> f;
        const StorageError e = fs_.open(files_[index].path, platform::kOpenRead, &f);
        if (e != StorageError::Ok) {
            rt::log(rt::LogLevel::Error, "romfs: cannot open %s: %s", files_[index].path.c_str(),
                    platform::storage_error_name(e));
            return false;
        }
        it = open_.emplace(index, std::move(f)).first;
    }
    uint64_t got = 0;
    if (it->second->read(offset, dst, len, &got) != StorageError::Ok)
        return false;
    // A file that shrank since the image was built reads as zeros.
    std::memset(dst + got, 0, len - got);
    return true;
}

bool RomFsImage::read(uint64_t offset, void* dst, uint64_t len) {
    if (offset > size_ || len > size_ - offset)
        return false;
    auto* out = static_cast<uint8_t*>(dst);
    std::memset(out, 0, len);
    const uint64_t end = offset + len;
    // Header and tables.
    if (offset < kHeaderSize) {
        const uint64_t n = std::min(end, kHeaderSize) - offset;
        std::memcpy(out, header_.data() + offset, n);
    }
    if (end > tables_offset_) {
        const uint64_t from = std::max(offset, tables_offset_);
        std::memcpy(out + (from - offset), tables_.data() + (from - tables_offset_), end - from);
    }
    // File data: first file that ends after offset.
    std::lock_guard lock(mutex_);
    auto it = std::upper_bound(files_.begin(), files_.end(), offset,
                               [](uint64_t v, const File& f) { return v < f.offset + f.size; });
    for (; it != files_.end() && it->offset < end; ++it) {
        const uint64_t from = std::max(offset, it->offset);
        const uint64_t to = std::min(end, it->offset + it->size);
        if (!read_file(static_cast<size_t>(it - files_.begin()), from - it->offset, out + (from - offset), to - from))
            return false;
    }
    return true;
}

bool romfs_find(DataSource& image, std::string_view path, uint64_t* offset, uint64_t* size, uint32_t* entry) {
    uint64_t h[10];  // header size, then offset/size of dir hash, dir table, file hash, file table, data offset
    if (!image.read(0, h, sizeof(h)))
        return false;
    const uint64_t dirs = h[3], dirs_size = h[4], files = h[7], files_size = h[8], data = h[9];
    auto name_at = [&](uint64_t table, uint64_t table_size, uint64_t entry, uint64_t fixed, std::string* name) {
        uint32_t len;
        if (entry + fixed > table_size || !image.read(table + entry + fixed - 4, &len, 4) ||
            entry + fixed + len > table_size)
            return false;
        name->resize(len);
        return len == 0 || image.read(table + entry + fixed, name->data(), len);
    };
    uint32_t dir = 0;  // root
    size_t pos = path.find_first_not_of('/');
    while (pos != std::string_view::npos) {
        const size_t slash = path.find('/', pos);
        const std::string_view part = path.substr(pos, slash == std::string_view::npos ? slash : slash - pos);
        pos = slash == std::string_view::npos ? slash : path.find_first_not_of('/', slash);
        const bool last = pos == std::string_view::npos;
        uint32_t link[4];  // dir entry: parent, sibling, first child dir, first child file
        if (uint64_t{dir} + 16 > dirs_size || !image.read(dirs + dir, link, sizeof(link)))
            return false;
        std::string name;
        if (last) {
            for (uint32_t f = link[3]; f != RomFsImage::kNone;) {
                uint32_t sibling;
                uint64_t fo[2];
                if (!image.read(files + f + 4, &sibling, 4) || !image.read(files + f + 8, fo, 16) ||
                    !name_at(files, files_size, f, 0x20, &name))
                    return false;
                if (name == part) {
                    *offset = data + fo[0];
                    *size = fo[1];
                    if (entry != nullptr)
                        *entry = f;
                    return true;
                }
                f = sibling;
            }
            return false;
        }
        uint32_t found = RomFsImage::kNone;
        for (uint32_t d = link[2]; d != RomFsImage::kNone && found == RomFsImage::kNone;) {
            uint32_t sibling;
            if (!image.read(dirs + d + 4, &sibling, 4) || !name_at(dirs, dirs_size, d, 0x18, &name))
                return false;
            if (name == part)
                found = d;
            d = sibling;
        }
        if (found == RomFsImage::kNone)
            return false;
        dir = found;
    }
    return false;  // a directory or an empty path
}

namespace {

// The directory entry of `dir` ("/bin/x", "/" the root) in a copy of the
// directory table, or kNone.
uint32_t find_dir(const std::vector<uint8_t>& dirs, std::string_view dir) {
    auto get32 = [&](uint64_t at, uint32_t* v) {
        if (at + 4 > dirs.size())
            return false;
        std::memcpy(v, dirs.data() + at, 4);
        return true;
    };
    uint32_t cur = 0;
    size_t pos = dir.find_first_not_of('/');
    while (pos != std::string_view::npos) {
        const size_t slash = dir.find('/', pos);
        const std::string_view part = dir.substr(pos, slash == std::string_view::npos ? slash : slash - pos);
        pos = slash == std::string_view::npos ? slash : dir.find_first_not_of('/', slash);
        uint32_t d = 0, found = RomFsImage::kNone;
        if (!get32(uint64_t{cur} + 8, &d))
            return RomFsImage::kNone;
        while (d != RomFsImage::kNone && found == RomFsImage::kNone) {
            uint32_t len = 0, sibling = 0;
            if (!get32(uint64_t{d} + 0x14, &len) || !get32(uint64_t{d} + 4, &sibling) ||
                uint64_t{d} + 0x18 + len > dirs.size())
                return RomFsImage::kNone;
            if (std::string_view(reinterpret_cast<const char*>(dirs.data()) + d + 0x18, len) == part)
                found = d;
            d = sibling;
        }
        if (found == RomFsImage::kNone)
            return RomFsImage::kNone;
        cur = found;
    }
    return cur;
}

uint32_t get32(const std::vector<uint8_t>& buf, uint64_t at) {
    uint32_t v = 0;
    std::memcpy(&v, buf.data() + at, 4);
    return v;
}

}  // namespace

std::shared_ptr<RomFsOverlay> RomFsOverlay::build(std::shared_ptr<DataSource> base,
                                                  const std::map<std::string, std::vector<uint8_t>>& files,
                                                  std::string* error) {
    uint64_t h[10];
    if (!base->read(0, h, sizeof(h)) || h[0] != RomFsImage::kHeaderSize || h[8] > (64ull << 20) ||
        h[4] > (64ull << 20) || h[6] > (64ull << 20) || h[7] > base->size() || h[8] > base->size() - h[7] ||
        h[3] > base->size() || h[4] > base->size() - h[3] || h[5] > base->size() || h[6] > base->size() - h[5] ||
        h[6] < 4) {
        *error = "not a RomFS image";
        return nullptr;
    }
    auto o = std::shared_ptr<RomFsOverlay>(new RomFsOverlay);
    o->base_size_ = base->size();
    o->table_offset_ = h[7];
    o->table_.resize(h[8]);
    o->dirs_offset_ = h[3];
    o->dirs_.resize(h[4]);
    o->hash_offset_ = h[5];
    o->hash_.resize(h[6]);
    if (!base->read(h[7], o->table_.data(), h[8]) || !base->read(h[3], o->dirs_.data(), h[4]) ||
        !base->read(h[5], o->hash_.data(), h[6])) {
        *error = "cannot read the tables";
        return nullptr;
    }
    const uint32_t buckets = static_cast<uint32_t>(h[6] / 4);
    // New and replaced contents follow the base, each 16-byte aligned like
    // Nintendo's file data; offsets in the table are relative to the data
    // region (h[9]).
    o->extra_offset_ = align_up(o->base_size_, 0x10);
    for (const auto& [path, data] : files) {
        const uint64_t at = o->extra_offset_ + align_up(o->extra_.size(), 0x10);
        uint64_t offset = 0, size = 0;
        uint32_t entry = 0;
        if (romfs_find(*base, path, &offset, &size, &entry)) {
            o->extra_.resize(at - o->extra_offset_);
            o->extra_.insert(o->extra_.end(), data.begin(), data.end());
            put64(o->table_, entry + 8, at - h[9]);
            put64(o->table_, entry + 16, data.size());
            continue;
        }
        // A new file in a directory the image has: an entry at the end of the
        // file table, the head of its hash bucket, the last of its siblings.
        const size_t slash = path.rfind('/');
        const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
        const std::string dir = slash == std::string::npos || slash == 0 ? "/" : path.substr(0, slash);
        const uint32_t parent = find_dir(o->dirs_, dir);
        if (parent == RomFsImage::kNone || name.empty() || o->table_.size() > 0xFFFFFF00u) {
            *error = path + " is not in the image and its directory is missing";
            return nullptr;
        }
        o->extra_.resize(at - o->extra_offset_);
        o->extra_.insert(o->extra_.end(), data.begin(), data.end());
        const uint32_t e = static_cast<uint32_t>(o->table_.size());
        const uint64_t bucket = 4ull * (RomFsImage::hash(parent, name) % buckets);
        o->table_.resize(e + 0x20 + align_up(name.size(), 4), 0);
        put32(o->table_, e + 0x00, parent);
        put32(o->table_, e + 0x04, RomFsImage::kNone);
        put64(o->table_, e + 0x08, at - h[9]);
        put64(o->table_, e + 0x10, data.size());
        put32(o->table_, e + 0x18, get32(o->hash_, bucket));
        put32(o->table_, e + 0x1C, static_cast<uint32_t>(name.size()));
        std::memcpy(o->table_.data() + e + 0x20, name.data(), name.size());
        put32(o->hash_, bucket, e);
        uint32_t f = get32(o->dirs_, uint64_t{parent} + 0x0C);
        if (f == RomFsImage::kNone) {
            put32(o->dirs_, uint64_t{parent} + 0x0C, e);
        } else {
            for (int guard = 0; get32(o->table_, uint64_t{f} + 4) != RomFsImage::kNone && guard < (1 << 24); ++guard)
                f = get32(o->table_, uint64_t{f} + 4);
            put32(o->table_, uint64_t{f} + 4, e);
        }
        ++o->added_;
    }
    o->size_ = o->extra_offset_ + o->extra_.size();
    if (o->added_ > 0) {  // the grown file table moves behind the contents, the header points there
        o->table_offset_ = align_up(o->size_, 4);
        o->size_ = o->table_offset_ + o->table_.size();
        o->header_.resize(RomFsImage::kHeaderSize);
        if (!base->read(0, o->header_.data(), o->header_.size())) {
            *error = "cannot read the header";
            return nullptr;
        }
        put64(o->header_, 0x38, o->table_offset_);
        put64(o->header_, 0x40, o->table_.size());
    }
    o->base_ = std::move(base);
    return o;
}

bool RomFsOverlay::read(uint64_t offset, void* dst, uint64_t len) {
    if (offset > size_ || len > size_ - offset)
        return false;
    // Regions over the base (or past it), sorted by offset; elsewhere the base,
    // and zeros past its end.
    const std::pair<uint64_t, const std::vector<uint8_t>*> regions[] = {
        {0, &header_}, {dirs_offset_, &dirs_}, {hash_offset_, &hash_}, {table_offset_, &table_}, {extra_offset_, &extra_}};
    auto* out = static_cast<uint8_t*>(dst);
    while (len > 0) {
        uint64_t n = len;
        bool done = false;
        for (const auto& [at, bytes] : regions) {
            if (!bytes->empty() && offset >= at && offset < at + bytes->size()) {
                n = std::min(n, at + bytes->size() - offset);
                std::memcpy(out, bytes->data() + (offset - at), n);
                done = true;
                break;
            }
            if (!bytes->empty() && at > offset)
                n = std::min(n, at - offset);
        }
        if (!done) {
            if (offset < base_size_) {
                n = std::min(n, base_size_ - offset);
                if (!base_->read(offset, out, n))
                    return false;
            } else {
                std::memset(out, 0, n);
            }
        }
        out += n;
        offset += n;
        len -= n;
    }
    return true;
}

bool romfs_list(DataSource& image, std::vector<RomFsFile>* out) {
    uint64_t h[10];
    if (!image.read(0, h, sizeof(h)) || h[0] != RomFsImage::kHeaderSize)
        return false;
    const uint64_t dirs = h[3], dirs_size = h[4], files = h[7], files_size = h[8], data = h[9];
    auto name_at = [&](uint64_t table, uint64_t table_size, uint64_t entry, uint64_t fixed, std::string* name) {
        uint32_t len;
        if (entry + fixed > table_size || !image.read(table + entry + fixed - 4, &len, 4) ||
            entry + fixed + len > table_size)
            return false;
        name->resize(len);
        return len == 0 || image.read(table + entry + fixed, name->data(), len);
    };
    out->clear();
    // Directories to walk: (entry, path); a bound on the count guards against loops.
    std::vector<std::pair<uint32_t, std::string>> todo = {{0, ""}};
    size_t walked = 0;
    while (!todo.empty() && walked++ < 1000000) {
        const auto [dir, path] = todo.back();
        todo.pop_back();
        uint32_t link[4];
        if (uint64_t{dir} + 16 > dirs_size || !image.read(dirs + dir, link, sizeof(link)))
            return false;
        std::string name;
        for (uint32_t f = link[3]; f != RomFsImage::kNone && walked++ < 1000000;) {
            uint32_t sibling;
            uint64_t fo[2];
            if (!image.read(files + f + 4, &sibling, 4) || !image.read(files + f + 8, fo, 16) ||
                !name_at(files, files_size, f, 0x20, &name))
                return false;
            if (fo[1] != 0)
                out->push_back({data + fo[0], fo[1], path + "/" + name});
            f = sibling;
        }
        for (uint32_t d = link[2]; d != RomFsImage::kNone && walked++ < 1000000;) {
            uint32_t sibling;
            if (!image.read(dirs + d + 4, &sibling, 4) || !name_at(dirs, dirs_size, d, 0x18, &name))
                return false;
            todo.emplace_back(d, path + "/" + name);
            d = sibling;
        }
    }
    std::sort(out->begin(), out->end(), [](const RomFsFile& a, const RomFsFile& b) { return a.offset < b.offset; });
    return true;
}

std::shared_ptr<RomFsReadLog> RomFsReadLog::wrap(std::shared_ptr<DataSource> base, std::string each) {
    std::vector<RomFsFile> files;
    if (!base || !romfs_list(*base, &files))
        return nullptr;
    auto out = std::shared_ptr<RomFsReadLog>(new RomFsReadLog);
    out->base_ = std::move(base);
    out->each_ = std::move(each);
    out->files_.reserve(files.size());
    for (RomFsFile& f : files)
        out->files_.push_back({f.offset, f.size, std::move(f.path)});
    return out;
}

bool RomFsReadLog::read(uint64_t offset, void* dst, uint64_t len) {
    {
        std::lock_guard lock(mutex_);
        const uint64_t end = offset + len;
        auto it = std::upper_bound(files_.begin(), files_.end(), offset,
                                   [](uint64_t o, const File& f) { return o < f.offset; });
        if (it != files_.begin())
            --it;
        for (; it != files_.end() && it->offset < end; ++it)
            if (it->offset + it->size > offset) {
                if (!it->seen) {
                    it->seen = true;
                    rt::log(rt::LogLevel::Info, "romfs read: %s (%" PRIu64 " bytes)", it->path.c_str(), it->size);
                }
                if (!each_.empty() && offset <= it->offset && it->path.find(each_) != std::string::npos &&
                    ++it->starts > 1)
                    rt::log(rt::LogLevel::Info, "romfs read again: %s (%u times)", it->path.c_str(), it->starts);
            }
    }
    return base_->read(offset, dst, len);
}

std::shared_ptr<RomFsRewriter> RomFsRewriter::wrap(std::shared_ptr<DataSource> base, Wants wants, Change change,
                                                   Epoch epoch) {
    std::vector<RomFsFile> files;
    if (!base || !wants || !change || !romfs_list(*base, &files))
        return nullptr;
    auto out = std::shared_ptr<RomFsRewriter>(new RomFsRewriter);
    out->base_ = std::move(base);
    out->wants_ = std::move(wants);
    out->change_ = std::move(change);
    out->epoch_ = std::move(epoch);
    out->files_.reserve(files.size());
    for (RomFsFile& f : files)
        out->files_.push_back({f.offset, f.size, std::move(f.path)});
    return out;
}

size_t RomFsRewriter::changed() const {
    std::lock_guard lock(mutex_);
    return changed_;
}

void RomFsRewriter::decide(File& f) {
    f.decided = true;
    f.epoch = epoch_ ? epoch_(f.path) : 0;
    uint8_t head[16];
    const size_t n = static_cast<size_t>(std::min<uint64_t>(f.size, sizeof(head)));
    if (!base_->read(f.offset, head, n) || !wants_(f.path, std::span<const uint8_t>(head, n)))
        return;
    std::vector<uint8_t> old(f.size);
    if (!base_->read(f.offset, old.data(), old.size())) {
        rt::log(rt::LogLevel::Warn, "romfs: cannot read %s to change it", f.path.c_str());
        return;
    }
    std::vector<uint8_t> now = old;
    if (!change_(f.path, now))
        return;
    if (now.size() != old.size()) {
        rt::log(rt::LogLevel::Error, "romfs: the change of %s changed its size (%zu -> %zu), left as it was",
                f.path.c_str(), old.size(), now.size());
        return;
    }
    // Changed runs; gaps of fewer than 16 equal bytes are taken in.
    for (size_t i = 0; i < now.size();) {
        if (now[i] == old[i]) {
            ++i;
            continue;
        }
        size_t end = i + 1, same = 0;
        for (size_t j = end; j < now.size() && same < 16; ++j) {
            if (now[j] != old[j]) {
                end = j + 1;
                same = 0;
            } else {
                ++same;
            }
        }
        f.runs.push_back({i, std::vector<uint8_t>(now.begin() + i, now.begin() + end)});
        i = end;
    }
    if (!f.runs.empty())
        ++changed_;
}

bool RomFsRewriter::read(uint64_t offset, void* dst, uint64_t len) {
    if (!base_->read(offset, dst, len))
        return false;
    std::lock_guard lock(mutex_);
    const uint64_t end = offset + len;
    auto it = std::upper_bound(files_.begin(), files_.end(), offset,
                               [](uint64_t o, const File& f) { return o < f.offset; });
    if (it != files_.begin())
        --it;
    for (; it != files_.end() && it->offset < end; ++it) {
        if (it->offset + it->size <= offset)
            continue;
        if (it->decided && it->epoch != 0 && epoch_(it->path) != it->epoch) {
            if (!it->runs.empty())
                --changed_;
            it->runs.clear();
            it->decided = false;
        }
        if (!it->decided)
            decide(*it);
        for (const Run& r : it->runs) {
            const uint64_t from = std::max(offset, it->offset + r.at);
            const uint64_t to = std::min(end, it->offset + r.at + r.bytes.size());
            if (from < to)
                std::memcpy(static_cast<uint8_t*>(dst) + (from - offset), r.bytes.data() + (from - it->offset - r.at),
                            to - from);
        }
    }
    return true;
}

}  // namespace lgp::hle
