// P3.4: fsp-srv through sm: and the IPC svcs, over MemoryStorage. The RomFS
// image builder is checked with a separate parser written from the format
// description (switchbrew/3dbrew "RomFS", libnx romfs_dev), not with the
// builder's own code.
#include <algorithm>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "ipc_client.hpp"
#include "lgp/hle/fs.hpp"
#include "lgp/hle/pl.hpp"
#include "lgp/hle/romfs.hpp"
#include "lgp/platform/memory_storage.hpp"

using namespace lgp_test;
using lgp::platform::MemoryStorage;
using lgp::platform::StorageError;
namespace fr = fs_result;

namespace {

constexpr uint64_t kProgram = 0x010003F003A34000ull;
constexpr uint64_t kUid0 = 0x1111222233334444ull, kUid1 = 0x5555666677778888ull;

std::vector<uint8_t> bytes(const std::string& s) { return {s.begin(), s.end()}; }

std::vector<uint8_t> pattern(size_t n, uint8_t seed) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i)
        v[i] = static_cast<uint8_t>(seed + i * 7 + (i >> 8));
    return v;
}

// --- independent RomFS parser --------------------------------------------------------

struct Parsed {
    std::map<std::string, std::vector<uint8_t>> files;
    std::vector<std::string> dirs;
    bool ok = true;
};

uint32_t u32_at(const std::vector<uint8_t>& img, uint64_t at) {
    uint32_t v = 0;
    if (at + 4 <= img.size())
        std::memcpy(&v, img.data() + at, 4);
    return v;
}
uint64_t u64_at(const std::vector<uint8_t>& img, uint64_t at) {
    uint64_t v = 0;
    if (at + 8 <= img.size())
        std::memcpy(&v, img.data() + at, 8);
    return v;
}

uint32_t ref_hash(uint32_t parent, const std::string& name) {
    uint32_t h = parent ^ 123456789u;
    for (unsigned char c : name) {
        h = (h >> 5) | (h << 27);
        h ^= c;
    }
    return h;
}

struct Header {
    uint64_t dir_hash, dir_hash_size, dir_table, dir_table_size, file_hash, file_hash_size, file_table,
        file_table_size, data;
};

Header header(const std::vector<uint8_t>& img) {
    CHECK_EQ(u64_at(img, 0), 0x50u);
    return {u64_at(img, 0x08), u64_at(img, 0x10), u64_at(img, 0x18), u64_at(img, 0x20), u64_at(img, 0x28),
            u64_at(img, 0x30), u64_at(img, 0x38), u64_at(img, 0x40), u64_at(img, 0x48)};
}

std::string name_at(const std::vector<uint8_t>& img, uint64_t at, uint32_t len) {
    if (at + len > img.size())
        return "<out of range>";
    return std::string(reinterpret_cast<const char*>(img.data() + at), len);
}

void walk(const std::vector<uint8_t>& img, const Header& h, uint32_t dir, const std::string& path, Parsed* out,
          int depth) {
    if (depth > 16) {
        out->ok = false;
        return;
    }
    const uint64_t d = h.dir_table + dir;
    for (uint32_t f = u32_at(img, d + 0x0C); f != 0xFFFFFFFF; f = u32_at(img, h.file_table + f + 0x04)) {
        const uint64_t e = h.file_table + f;
        CHECK_EQ(u32_at(img, e + 0x00), dir);  // parent link
        const std::string name = name_at(img, e + 0x20, u32_at(img, e + 0x1C));
        const uint64_t off = h.data + u64_at(img, e + 0x08);
        const uint64_t size = u64_at(img, e + 0x10);
        CHECK(off + size <= img.size());
        CHECK_EQ(u64_at(img, e + 0x08) % 0x10, 0u);
        if (off + size <= img.size())
            out->files[path + "/" + name] = std::vector<uint8_t>(img.begin() + off, img.begin() + off + size);
    }
    for (uint32_t c = u32_at(img, d + 0x08); c != 0xFFFFFFFF; c = u32_at(img, h.dir_table + c + 0x04)) {
        const uint64_t e = h.dir_table + c;
        CHECK_EQ(u32_at(img, e + 0x00), dir);
        const std::string name = name_at(img, e + 0x18, u32_at(img, e + 0x14));
        out->dirs.push_back(path + "/" + name);
        walk(img, h, c, path + "/" + name, out, depth + 1);
    }
}

Parsed parse(const std::vector<uint8_t>& img) {
    Parsed p;
    const Header h = header(img);
    CHECK_EQ(u32_at(img, h.dir_table + 0), 0u);  // the root is its own parent
    CHECK_EQ(u32_at(img, h.dir_table + 0x14), 0u);
    walk(img, h, 0, "", &p, 0);
    return p;
}

// Looks a path up through the hash tables, as a game's RomFS driver does.
bool lookup(const std::vector<uint8_t>& img, const std::string& path, std::vector<uint8_t>* data) {
    const Header h = header(img);
    uint32_t dir = 0;
    size_t pos = 1;
    while (true) {
        const size_t slash = path.find('/', pos);
        const std::string name = path.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
        const bool last = slash == std::string::npos;
        const uint64_t table = last ? h.file_table : h.dir_table;
        const uint64_t hash_table = last ? h.file_hash : h.dir_hash;
        const uint64_t buckets = (last ? h.file_hash_size : h.dir_hash_size) / 4;
        const uint32_t name_len_at = last ? 0x1C : 0x14;
        const uint32_t name_at_off = last ? 0x20 : 0x18;
        const uint32_t next_at = last ? 0x18 : 0x10;
        uint32_t e = u32_at(img, hash_table + 4 * (ref_hash(dir, name) % buckets));
        int guard = 0;
        while (e != 0xFFFFFFFF && guard++ < 1000) {
            const uint64_t at = table + e;
            if (u32_at(img, at) == dir && name_at(img, at + name_at_off, u32_at(img, at + name_len_at)) == name)
                break;
            e = u32_at(img, at + next_at);
        }
        if (e == 0xFFFFFFFF || guard >= 1000)
            return false;
        if (last) {
            const uint64_t off = h.data + u64_at(img, table + e + 0x08);
            const uint64_t size = u64_at(img, table + e + 0x10);
            *data = std::vector<uint8_t>(img.begin() + off, img.begin() + off + size);
            return true;
        }
        dir = e;
        pos = slash + 1;
    }
}

std::map<std::string, std::vector<uint8_t>> sample_tree() {
    std::map<std::string, std::vector<uint8_t>> t;
    t["/a.txt"] = bytes("hello romfs");
    t["/empty.dat"] = {};
    t["/bin/data/large.bin"] = pattern(70000, 3);
    t["/bin/data/odd.bin"] = pattern(13, 9);
    t["/bin/z"] = bytes("z");
    t["/\xc3\xa4nderung/\xc3\xbc.txt"] = bytes("utf-8");  // multi-byte names
    for (int i = 0; i < 25; ++i)  // > 19 files in one table: the prime-ish bucket path
        t["/many/f" + std::to_string(i)] = pattern(static_cast<size_t>(i * 3), static_cast<uint8_t>(i));
    return t;
}

void test_romfs_buckets() {
    CHECK_EQ(RomFsImage::bucket_count(0), 3u);
    CHECK_EQ(RomFsImage::bucket_count(2), 3u);
    CHECK_EQ(RomFsImage::bucket_count(3), 3u);
    CHECK_EQ(RomFsImage::bucket_count(4), 5u);
    CHECK_EQ(RomFsImage::bucket_count(18), 19u);
    CHECK_EQ(RomFsImage::bucket_count(19), 19u);
    CHECK_EQ(RomFsImage::bucket_count(20), 23u);
    CHECK_EQ(RomFsImage::bucket_count(290), 293u);
    CHECK_EQ(RomFsImage::hash(0, ""), 123456789u);
}

void test_romfs_image() {
    MemoryStorage game(true);
    const auto tree = sample_tree();
    for (const auto& [path, data] : tree)
        game.add_file("/romfs" + path, data);

    StorageError err = StorageError::Io;
    auto img = RomFsImage::build(game, "/romfs", &err);
    CHECK(err == StorageError::Ok);
    CHECK(img != nullptr);
    if (!img)
        return;
    CHECK_EQ(img->file_count(), tree.size());
    CHECK_EQ(img->directory_count(), 5u);           // root, bin, bin/data, änderung, many

    std::vector<uint8_t> whole(img->size());
    CHECK(img->read(0, whole.data(), whole.size()));
    const Parsed p = parse(whole);
    CHECK(p.ok);
    CHECK_EQ(p.files.size(), tree.size());
    for (const auto& [path, data] : tree) {
        const auto it = p.files.find(path);
        CHECK(it != p.files.end());
        CHECK(it != p.files.end() && it->second == data);
        std::vector<uint8_t> via_hash;
        CHECK(lookup(whole, path, &via_hash));
        CHECK(via_hash == data);
    }
    // romfs_find (the movie HLE's path lookup) over the image itself.
    for (const auto& [path, data] : tree) {
        uint64_t offset = 0, size = 1;
        CHECK(lgp::hle::romfs_find(*img, path, &offset, &size));
        CHECK_EQ(size, data.size());
        std::vector<uint8_t> got(static_cast<size_t>(size));
        CHECK(size == 0 || img->read(offset, got.data(), got.size()));
        CHECK(got == data);
    }
    {
        uint64_t offset = 0, size = 0;
        CHECK(lgp::hle::romfs_find(*img, "bin//data/odd.bin", &offset, &size));  // relative, doubled slash
        CHECK_EQ(size, 13u);
        CHECK(!lgp::hle::romfs_find(*img, "/bin/missing", &offset, &size));
        CHECK(!lgp::hle::romfs_find(*img, "/nodir/a.txt", &offset, &size));
        CHECK(!lgp::hle::romfs_find(*img, "/bin/data", &offset, &size));  // a directory
        CHECK(!lgp::hle::romfs_find(*img, "/", &offset, &size));
    }
    // The read log (LGP_ROMFS_READ_LOG) knows every non-empty file and reads through.
    {
        auto logged = lgp::hle::RomFsReadLog::wrap(img);
        CHECK(logged != nullptr);
        size_t non_empty = 0;
        for (const auto& [path, data] : tree)
            non_empty += !data.empty();
        CHECK(logged && logged->file_count() == non_empty);
        std::vector<uint8_t> again(whole.size());
        CHECK(logged && logged->read(0, again.data(), again.size()) && again == whole);
        CHECK(logged && logged->read(0, again.data(), again.size()) && again == whole);  // logged once, read twice
        // With a path filter (LGP_ROMFS_READ_LOG=odd.bin) reads of that file are counted too, still read through.
        auto each = lgp::hle::RomFsReadLog::wrap(img, "odd.bin");
        uint64_t odd_at = 0, odd_size = 0;
        CHECK(lgp::hle::romfs_find(*img, "/bin/data/odd.bin", &odd_at, &odd_size));
        std::vector<uint8_t> odd(static_cast<size_t>(odd_size)), expect(static_cast<size_t>(odd_size));
        CHECK(img->read(odd_at, expect.data(), expect.size()));
        for (int i = 0; i < 3; ++i)
            CHECK(each && each->read(odd_at, odd.data(), odd.size()) && odd == expect);
        struct Junk final : lgp::hle::DataSource {
            uint64_t size() const override { return 128; }
            bool read(uint64_t offset, void* dst, uint64_t len) override {
                if (offset + len > 128)
                    return false;
                std::memset(dst, 0x5A, len);
                return true;
            }
        };
        CHECK(lgp::hle::RomFsReadLog::wrap(std::make_shared<Junk>()) == nullptr);
    }
    std::vector<uint8_t> dummy;
    CHECK(!lookup(whole, "/bin/missing", &dummy));
    CHECK(!lookup(whole, "/nodir/a.txt", &dummy));

    // Reads in odd pieces return the same bytes as one read.
    std::vector<uint8_t> pieces(whole.size());
    for (uint64_t at = 0; at < pieces.size();) {
        const uint64_t n = std::min<uint64_t>(pieces.size() - at, 997);
        CHECK(img->read(at, pieces.data() + at, n));
        at += n;
    }
    CHECK(pieces == whole);
    CHECK(!img->read(img->size() - 4, pieces.data(), 8));
    CHECK(img->read(img->size(), pieces.data(), 0));

    // A file that is not there makes the build fail cleanly.
    CHECK(RomFsImage::build(game, "/nothing", &err) == nullptr);
    CHECK(err == StorageError::NotFound);
}

// --- fsp-srv over IPC ----------------------------------------------------------------

struct FsEnv : Env {
    MemoryStorage game{true};
    MemoryStorage save;
    int commits = 0;  // FsConfig::on_commit calls
    // save_storage: another save storage than `save` (kept across envs).
    explicit FsEnv(bool with_save = true, lgp::platform::IStorage* save_storage = nullptr,
                   std::map<std::string, std::vector<uint8_t>> romfs_replace = {},
                   std::function<bool(const std::string&)> romfs_keep = nullptr,
                   FsConfig::RomFsChanges romfs_changes = {})
        : Env([](ServiceManager&) {}) {
        FsConfig c;
        c.romfs_replace = std::move(romfs_replace);
        c.romfs_keep = std::move(romfs_keep);
        c.romfs_changes = std::move(romfs_changes);
        c.game = &game;
        c.save = save_storage != nullptr ? save_storage : with_save ? &save : nullptr;
        c.program_id = kProgram;
        c.save_space_size = 0x10000;
        c.on_commit = [this] { ++commits; };
        register_fs(sm, std::move(c));
    }

    Reply call(Handle h, uint32_t command, Msg m = {}) {
        m.command = command;
        const std::optional<Reply> r = send(h, m);
        return r ? *r : Reply{};
    }
    Handle object(const Reply& r) {
        CHECK_EQ(r.result, 0u);
        CHECK_EQ(r.move.size(), 1u);
        return r.move.empty() ? kInvalidHandle : r.move[0];
    }

    // X buffer slot (0 or 1) holding a NUL-terminated path.
    Desc path(const std::string& p, uint32_t slot = 0) {
        std::vector<uint8_t> buf(0x301, 0);
        std::memcpy(buf.data(), p.data(), std::min<size_t>(p.size(), 0x301));
        const uint64_t addr = kBufA + 0x4000 + slot * 0x400;
        CHECK(k.memory().copy_to_user(addr, buf.data(), buf.size()));
        return {addr, 0x301, slot};
    }
    Msg path_msg(const std::string& p) {
        Msg m;
        m.x = {path(p)};
        return m;
    }

    Handle open_save(uint64_t program = 0, uint8_t type = 1) {
        const Handle fsp = open_service("fsp-srv");
        Msg m;
        m.arg<uint8_t>(1);  // SaveDataSpaceId User
        uint8_t attr[0x40] = {};
        std::memcpy(attr + 0x00, &program, 8);
        std::memcpy(attr + 0x08, &kUid0, 8);
        std::memcpy(attr + 0x10, &kUid1, 8);
        attr[0x20] = type;
        for (size_t i = 0; i < sizeof(attr); i += 8) {
            uint64_t w = 0;
            std::memcpy(&w, attr + i, 8);
            m.arg<uint64_t>(w);
        }
        return object(call(fsp, 51, m));
    }

    Reply storage_read(Handle h, int64_t offset, int64_t size, uint64_t buf_size) {
        Msg m;
        m.arg<int64_t>(offset).arg<int64_t>(size);
        m.b = {{kBufB, buf_size, 0}};
        return call(h, 0, m);
    }
    std::vector<uint8_t> guest(uint64_t addr, size_t n) {
        std::vector<uint8_t> v(n);
        CHECK(k.memory().copy_from_user(v.data(), addr, n));
        return v;
    }

    void close(Handle h) { CHECK_EQ(svc(kSvcCloseHandle, {h})[0], kSuccess.raw); }

    // Opens path with mode; the IFile handle or kInvalidHandle (*result set).
    Handle open_file(Handle fs, const std::string& p, uint32_t mode, uint32_t* result = nullptr) {
        Msg m = path_msg(p);
        m.arg<uint32_t>(mode);
        const Reply r = call(fs, 8, m);
        if (result != nullptr)
            *result = r.result;
        return r.result == 0 && !r.move.empty() ? r.move[0] : kInvalidHandle;
    }
    // CreateFile + OpenFile(Write | Append) + Write; the file stays open
    // when keep_open (returned), else it is closed.
    Handle write_file(Handle fs, const std::string& p, const std::vector<uint8_t>& data, bool keep_open = false) {
        Msg c = path_msg(p);
        c.arg<uint32_t>(0).arg<int64_t>(0);
        const uint32_t created = call(fs, 0, c).result;
        CHECK(created == 0 || created == fr::kPathAlreadyExists.raw);
        const Handle f = open_file(fs, p, 2 | 4);
        Msg set;
        set.arg<int64_t>(0);
        CHECK_EQ(call(f, 3, set).result, 0u);
        CHECK(k.memory().copy_to_user(kBufA, data.data(), data.size()));
        Msg w;
        w.arg<uint32_t>(0).arg<int64_t>(0).arg<int64_t>(static_cast<int64_t>(data.size()));
        w.a = {{kBufA, data.size(), 0}};
        CHECK_EQ(call(f, 1, w).result, 0u);
        if (keep_open)
            return f;
        close(f);
        return kInvalidHandle;
    }
    // Whole file, or nullopt if it cannot be opened.
    std::optional<std::vector<uint8_t>> read_file(Handle fs, const std::string& p) {
        uint32_t rc = 0;
        const Handle f = open_file(fs, p, 1, &rc);
        if (rc != 0)
            return std::nullopt;
        const auto size = static_cast<uint64_t>(call(f, 4).get<int64_t>(0));
        Msg r;
        r.arg<uint32_t>(0).arg<int64_t>(0).arg<int64_t>(static_cast<int64_t>(size));
        r.b = {{kBufB, size, 0}};
        CHECK_EQ(call(f, 0, r).result, 0u);
        close(f);
        return guest(kBufB, static_cast<size_t>(size));
    }
};

void test_fs_romfs_image_file() {
    FsEnv e;
    const auto image = pattern(0x6000, 1);
    e.game.add_file("/romfs.bin", image);
    const Handle fsp = e.open_service("fsp-srv");
    CHECK_EQ(e.call(fsp, 1).result, 0u);  // SetCurrentProcess
    const Reply mode = e.call(fsp, 1005);  // GetGlobalAccessLogMode
    CHECK_EQ(mode.result, 0u);
    CHECK_EQ(mode.get<uint32_t>(0), 0u);

    const Handle st = e.object(e.call(fsp, 200));
    const Reply size = e.call(st, 4);
    CHECK_EQ(size.get<int64_t>(0), 0x6000);
    CHECK_EQ(e.storage_read(st, 0x1234, 0x800, 0x800).result, 0u);
    CHECK(e.guest(kBufB, 0x800) == std::vector<uint8_t>(image.begin() + 0x1234, image.begin() + 0x1A34));
    CHECK_EQ(e.storage_read(st, 0x5C00, 0x800, 0x800).result, fr::kOutOfRange.raw);
    CHECK_EQ(e.storage_read(st, -1, 0x10, 0x10).result, fr::kInvalidOffset.raw);
    CHECK_EQ(e.storage_read(st, 0, 0x100, 0x10).result, fr::kInvalidSize.raw);  // larger than the buffer
    CHECK_EQ(e.call(st, 1).result, fr::kUnsupportedOperation.raw);              // Write
    CHECK_EQ(e.call(st, 2).result, 0u);                                          // Flush
}

// Data mods: files of an image replaced, bigger, smaller and empty; the rest unchanged.
void test_romfs_overlay() {
    MemoryStorage game(true);
    auto tree = sample_tree();
    for (const auto& [path, data] : tree)
        game.add_file("/romfs" + path, data);
    StorageError err = StorageError::Io;
    std::shared_ptr<DataSource> img = RomFsImage::build(game, "/romfs", &err);
    CHECK(img != nullptr);
    if (!img)
        return;

    std::map<std::string, std::vector<uint8_t>> replace;
    replace["/bin/data/odd.bin"] = pattern(5000, 77);  // bigger
    replace["/a.txt"] = bytes("hi");                   // smaller
    replace["/many/f3"] = {};                          // empty
    std::string error;
    auto overlay = RomFsOverlay::build(img, replace, &error);
    CHECK(overlay != nullptr);
    if (!overlay)
        return;
    for (const auto& [path, data] : replace)
        tree[path] = data;

    std::vector<uint8_t> whole(overlay->size());
    CHECK(overlay->read(0, whole.data(), whole.size()));
    const Parsed p = parse(whole);
    CHECK(p.ok);
    CHECK(p.files == tree);
    // Pieces across the table and the end of the base read the same.
    std::vector<uint8_t> pieces(whole.size());
    for (uint64_t at = 0; at < pieces.size(); at += 777)
        CHECK(overlay->read(at, pieces.data() + at, std::min<uint64_t>(777, pieces.size() - at)));
    CHECK(pieces == whole);
    CHECK(!overlay->read(whole.size() - 1, pieces.data(), 2));
    uint64_t offset = 0, size = 0;
    CHECK(romfs_find(*overlay, "/bin/data/odd.bin", &offset, &size));
    CHECK_EQ(size, 5000u);

    CHECK_EQ(overlay->added(), 0u);

    // New files in directories the image has: found through the hash tables
    // and the directory's file list, the rest unchanged.
    std::map<std::string, std::vector<uint8_t>> add = replace;
    add["/bin/data/new.bin"] = pattern(300, 5);
    add["/many/f99"] = bytes("ninety-nine");
    add["/root.new"] = bytes("at the root");
    auto grown = RomFsOverlay::build(img, add, &error);
    CHECK(grown != nullptr);
    if (!grown)
        return;
    CHECK_EQ(grown->added(), 3u);
    for (const auto& [path, data] : add)
        tree[path] = data;
    std::vector<uint8_t> all(grown->size());
    CHECK(grown->read(0, all.data(), all.size()));
    const Parsed q = parse(all);
    CHECK(q.ok);
    CHECK(q.files == tree);
    for (const auto& [path, data] : tree) {
        std::vector<uint8_t> got;
        CHECK(lookup(all, path, &got));
        CHECK(got == data);
    }
    std::vector<uint8_t> parts(all.size());
    for (uint64_t at = 0; at < parts.size(); at += 501)
        CHECK(grown->read(at, parts.data() + at, std::min<uint64_t>(501, parts.size() - at)));
    CHECK(parts == all);
    CHECK(romfs_find(*grown, "/many/f99", &offset, &size));
    CHECK_EQ(size, 11u);

    CHECK(RomFsOverlay::build(img, {{"/no/such.bin", bytes("x")}}, &error) == nullptr);  // no such directory
    CHECK(error.find("/no/such.bin") != std::string::npos);
}

// fsp-srv serves the replaced files from romfs.bin; a path the image lacks
// leaves the image as it is.
void test_fs_romfs_replace() {
    MemoryStorage tree_storage(true);
    for (const auto& [path, data] : sample_tree())
        tree_storage.add_file("/romfs" + path, data);
    StorageError err = StorageError::Io;
    auto img = RomFsImage::build(tree_storage, "/romfs", &err);
    CHECK(img != nullptr);
    if (!img)
        return;
    std::vector<uint8_t> image(img->size());
    CHECK(img->read(0, image.data(), image.size()));

    // 0: replaced, 1: a file whose directory the RomFS lacks (all left out), 2: left out by romfs_keep
    for (const int mode : {0, 1, 2}) {
        const bool good = mode == 0;
        std::map<std::string, std::vector<uint8_t>> replace;
        replace[mode == 1 ? "/nodir/missing" : "/bin/z"] = bytes("replaced by a mod");
        int asked = 0;
        FsEnv e(true, nullptr, replace, [&](const std::string& path) {
            ++asked;
            return mode != 2 || path != "/bin/z";
        });
        e.game.add_file("/romfs.bin", image);
        const Handle fsp = e.open_service("fsp-srv");
        const Handle st = e.object(e.call(fsp, 200));
        const Reply size = e.call(st, 4);
        const auto n = static_cast<uint64_t>(size.get<int64_t>(0));
        CHECK(good ? n > image.size() : n == image.size());
        std::vector<uint8_t> got;
        for (uint64_t at = 0; at < n; at += 0x800) {
            const uint64_t len = std::min<uint64_t>(0x800, n - at);
            CHECK_EQ(e.storage_read(st, static_cast<int64_t>(at), len, len).result, 0u);
            const auto piece = e.guest(kBufB, len);
            got.insert(got.end(), piece.begin(), piece.end());
        }
        const Parsed p = parse(got);
        CHECK(p.ok);
        CHECK(p.files.at("/bin/z") == (good ? bytes("replaced by a mod") : bytes("z")));
        CHECK(p.files.at("/a.txt") == bytes("hello romfs"));
        CHECK_EQ(asked, 1);  // once, at the first open
    }
}

// A mod's changes (FsConfig::romfs_changes): one file grows at mount, others
// change as they are read, without changing size.
void test_fs_romfs_changes() {
    MemoryStorage tree_storage(true);
    const auto tree = sample_tree();
    for (const auto& [path, data] : tree)
        tree_storage.add_file("/romfs" + path, data);
    StorageError err = StorageError::Io;
    auto img = RomFsImage::build(tree_storage, "/romfs", &err);
    CHECK(img != nullptr);
    if (!img)
        return;
    std::vector<uint8_t> image(img->size());
    CHECK(img->read(0, image.data(), image.size()));
    // large.bin: two bytes far apart flipped; odd.bin: asked, size changed (refused); the rest unasked.
    std::vector<std::string> asked;
    auto change = [](const std::string& path, std::vector<uint8_t>& b) {
        if (path == "/bin/z") {
            b = bytes("grown at mount");
            return true;
        }
        if (path == "/bin/data/odd.bin") {
            b.push_back(0);
            return true;
        }
        if (path != "/bin/data/large.bin")
            return false;
        b[5] ^= 0xFF;
        b[60000] ^= 0xFF;
        return true;
    };
    auto wants = [&](const std::string& path, std::span<const uint8_t> head) {
        asked.push_back(path);
        CHECK(head.size() == std::min<size_t>(16, tree.at(path).size()));
        return path.rfind("/bin/data/", 0) == 0;
    };

    // The rewriter alone, read in odd pieces.
    {
        auto rw = lgp::hle::RomFsRewriter::wrap(img, wants, change);
        CHECK(rw != nullptr);
        if (!rw)
            return;
        std::vector<uint8_t> got(image.size());
        for (uint64_t at = 0; at < got.size();) {
            const uint64_t n = std::min<uint64_t>(got.size() - at, 997);
            CHECK(rw->read(at, got.data() + at, n));
            at += n;
        }
        const Parsed p = parse(got);
        CHECK(p.ok);
        std::vector<uint8_t> large = tree.at("/bin/data/large.bin");
        large[5] ^= 0xFF;
        large[60000] ^= 0xFF;
        CHECK(p.files.at("/bin/data/large.bin") == large);
        CHECK(p.files.at("/bin/data/odd.bin") == tree.at("/bin/data/odd.bin"));  // size change refused
        CHECK(p.files.at("/a.txt") == tree.at("/a.txt"));
        CHECK_EQ(rw->changed(), 1u);
        const size_t first = asked.size();
        CHECK(rw->read(0, got.data(), got.size()));  // asked once per file
        CHECK_EQ(asked.size(), first);
        struct Junk final : lgp::hle::DataSource {
            uint64_t size() const override { return 128; }
            bool read(uint64_t offset, void* dst, uint64_t len) override {
                if (offset + len > 128)
                    return false;
                std::memset(dst, 0x5A, len);
                return true;
            }
        };
        CHECK(lgp::hle::RomFsRewriter::wrap(std::make_shared<Junk>(), wants, change) == nullptr);
    }

    // With an epoch: large.bin is followed and changed anew once its
    // generation moves; the other files keep what they were decided as.
    {
        uint64_t generation = 1;
        int changes = 0;
        auto counting = [&](const std::string& path, std::vector<uint8_t>& b) {
            ++changes;
            return change(path, b);
        };
        auto epoch = [&](const std::string& path) -> uint64_t {
            return path == "/bin/data/large.bin" ? generation : 0;
        };
        auto rw = lgp::hle::RomFsRewriter::wrap(img, wants, counting, epoch);
        CHECK(rw != nullptr);
        if (!rw)
            return;
        std::vector<uint8_t> got(image.size());
        CHECK(rw->read(0, got.data(), got.size()));
        const int first = changes;
        CHECK(rw->read(0, got.data(), got.size()));
        CHECK_EQ(changes, first);  // the same generation: not again
        generation = 2;
        CHECK(rw->read(0, got.data(), got.size()));
        CHECK_EQ(changes, first + 1);  // large.bin alone, anew
        CHECK_EQ(rw->changed(), 1u);
        const Parsed p = parse(got);
        CHECK(p.ok);
        std::vector<uint8_t> large = tree.at("/bin/data/large.bin");
        large[5] ^= 0xFF;
        large[60000] ^= 0xFF;
        CHECK(p.ok && p.files.at("/bin/data/large.bin") == large);
    }

    // Through fsp-srv: /bin/z changed at mount (and not asked again), large.bin as read.
    asked.clear();
    int begun = 0;
    FsConfig::RomFsChanges changes;
    changes.begin = [&](std::vector<std::string>* at_mount) {
        ++begun;
        at_mount->push_back("/bin/z");
        at_mount->push_back("/bin/missing");  // not in the RomFS: skipped
        return true;
    };
    changes.wants = wants;
    changes.change = change;
    FsEnv e(true, nullptr, {}, nullptr, changes);
    e.game.add_file("/romfs.bin", image);
    const Handle fsp = e.open_service("fsp-srv");
    const Handle st = e.object(e.call(fsp, 200));
    const auto n = static_cast<uint64_t>(e.call(st, 4).get<int64_t>(0));
    CHECK(n > image.size());
    std::vector<uint8_t> got;
    for (uint64_t at = 0; at < n; at += 0x800) {
        const uint64_t len = std::min<uint64_t>(0x800, n - at);
        CHECK_EQ(e.storage_read(st, static_cast<int64_t>(at), len, len).result, 0u);
        const auto piece = e.guest(kBufB, len);
        got.insert(got.end(), piece.begin(), piece.end());
    }
    const Parsed p = parse(got);
    CHECK(p.ok);
    CHECK(p.files.at("/bin/z") == bytes("grown at mount"));
    CHECK(p.files.at("/bin/data/large.bin")[5] == static_cast<uint8_t>(tree.at("/bin/data/large.bin")[5] ^ 0xFF));
    CHECK(std::find(asked.begin(), asked.end(), "/bin/z") == asked.end());
    e.object(e.call(fsp, 200));  // a second open: begin is not asked again
    CHECK_EQ(begun, 1);
}

void test_fs_romfs_tree() {
    FsEnv e;
    const auto tree = sample_tree();
    for (const auto& [path, data] : tree)
        e.game.add_file("/romfs" + path, data);
    const Handle fsp = e.open_service("fsp-srv");
    const Handle st = e.object(e.call(fsp, 200));
    const auto img_size = static_cast<uint64_t>(e.call(st, 4).get<int64_t>(0));
    CHECK(img_size > 70000);
    // Read the image through IPC in buffer-sized pieces and parse it.
    std::vector<uint8_t> img;
    for (uint64_t at = 0; at < img_size;) {
        const uint64_t n = std::min<uint64_t>(img_size - at, kBufSize);
        CHECK_EQ(e.storage_read(st, static_cast<int64_t>(at), static_cast<int64_t>(n), n).result, 0u);
        const auto piece = e.guest(kBufB, n);
        img.insert(img.end(), piece.begin(), piece.end());
        at += n;
    }
    const Parsed p = parse(img);
    CHECK_EQ(p.files.size(), tree.size());
    CHECK(p.files.count("/bin/data/large.bin") == 1 && p.files.at("/bin/data/large.bin") == tree.at("/bin/data/large.bin"));

    // A second open shares the built image.
    const Handle st2 = e.object(e.call(fsp, 200));
    CHECK_EQ(static_cast<uint64_t>(e.call(st2, 4).get<int64_t>(0)), img_size);
}

// --- compressed images ------------------------------------------------------------------

// The sample tree as a plain image, with a block of noise (stored raw, zstd
// cannot shrink it) next to the patterned files (compressed).
std::vector<uint8_t> sample_image() {
    MemoryStorage game(true);
    auto tree = sample_tree();
    std::vector<uint8_t> noise(40000);
    uint32_t x = 0x12345678;
    for (uint8_t& b : noise) {
        x ^= x << 13, x ^= x >> 17, x ^= x << 5;
        b = static_cast<uint8_t>(x);
    }
    tree["/noise.bin"] = noise;
    for (const auto& [path, data] : tree)
        game.add_file("/romfs" + path, data);
    StorageError err = StorageError::Io;
    auto img = RomFsImage::build(game, "/romfs", &err);
    std::vector<uint8_t> whole(img ? img->size() : 0);
    if (img)
        CHECK(img->read(0, whole.data(), whole.size()));
    return whole;
}

class VectorSource final : public lgp::hle::DataSource {
public:
    explicit VectorSource(const std::vector<uint8_t>& v) : v_(v) {}
    uint64_t size() const override { return v_.size(); }
    bool read(uint64_t offset, void* dst, uint64_t len) override {
        if (offset > v_.size() || len > v_.size() - offset)
            return false;
        std::memcpy(dst, v_.data() + offset, len);
        return true;
    }

private:
    const std::vector<uint8_t>& v_;
};

std::vector<uint8_t> compress(const std::vector<uint8_t>& plain, uint32_t block_size, std::string* error = nullptr) {
    VectorSource src(plain);
    lgp::hle::CompressOptions o;
    o.block_size = block_size;
    o.level = 3;
    o.threads = 3;
    std::vector<uint8_t> out;
    const bool ok = lgp::hle::write_compressed_image(
        src, o,
        [&](const void* d, size_t n) {
            out.insert(out.end(), static_cast<const uint8_t*>(d), static_cast<const uint8_t*>(d) + n);
            return true;
        },
        error);
    return ok ? out : std::vector<uint8_t>{};
}

std::shared_ptr<lgp::hle::DataSource> open_bytes(const std::vector<uint8_t>& file, std::string* why) {
    MemoryStorage m(true);
    m.add_file("/romfs.bin", file);
    std::unique_ptr<lgp::platform::IFile> f;
    CHECK(m.open("/romfs.bin", lgp::platform::kOpenRead, &f) == StorageError::Ok);
    return lgp::hle::open_image_source(std::move(f), why);
}

void test_romfs_compressed() {
    const auto plain = sample_image();
    CHECK(plain.size() > 100000);
    // Plain images pass through unchanged.
    std::string why;
    auto passthrough = open_bytes(plain, &why);
    CHECK(passthrough != nullptr && passthrough->size() == plain.size());

    if (!lgp::hle::compressed_images_supported()) {
        std::string err;
        CHECK(compress(plain, 4096, &err).empty() && !err.empty());
        std::vector<uint8_t> fake(64, 0);
        std::memcpy(fake.data(), lgp::hle::CompressedImageFormat::kMagic, 8);
        CHECK(open_bytes(fake, &why) == nullptr && why.find("zstd") != std::string::npos);
        return;
    }
    const auto packed = compress(plain, 4096);
    CHECK(!packed.empty() && packed.size() < plain.size());
    {  // both kinds of block: compressed (patterns) and stored raw (noise)
        uint64_t index_at = 0;
        std::memcpy(&index_at, packed.data() + packed.size() - 16, 8);
        int raw = 0, small = 0;
        const uint64_t blocks = (plain.size() + 4095) / 4096;
        for (uint64_t i = 0; i + 1 < blocks; ++i) {  // all but the shorter last one
            uint64_t a = 0, z = 0;
            std::memcpy(&a, packed.data() + index_at + i * 8, 8);
            std::memcpy(&z, packed.data() + index_at + i * 8 + 8, 8);
            (z - a == 4096 ? raw : small) += 1;
        }
        CHECK(raw >= 5);
        CHECK(small >= 5);
    }
    auto src = open_bytes(packed, &why);
    CHECK(src != nullptr);
    if (!src)
        return;
    CHECK_EQ(src->size(), plain.size());
    std::vector<uint8_t> whole(plain.size());
    CHECK(src->read(0, whole.data(), whole.size()));  // whole blocks, past the cache
    CHECK(whole == plain);
    // Ranges inside one block, across block ends, single bytes, the last byte;
    // twice each so the second read comes from the cache.
    const uint64_t ranges[][2] = {{0, 16}, {4090, 12}, {4096, 4096}, {5000, 9000}, {123, 1},
                                  {plain.size() - 1, 1}, {plain.size() - 5000, 5000}, {8191, 2}};
    for (int pass = 0; pass < 2; ++pass)
        for (const auto& r : ranges) {
            std::vector<uint8_t> got(r[1]);
            CHECK(src->read(r[0], got.data(), r[1]));
            CHECK(std::equal(got.begin(), got.end(), plain.begin() + static_cast<ptrdiff_t>(r[0])));
        }
    // More blocks than the cache holds, in a scattered order.
    for (uint64_t i = 0; i < 200; ++i) {
        const uint64_t at = (i * 7919 * 13) % (plain.size() - 300);
        std::vector<uint8_t> got(300);
        CHECK(src->read(at, got.data(), got.size()));
        CHECK(std::equal(got.begin(), got.end(), plain.begin() + static_cast<ptrdiff_t>(at)));
    }
    uint8_t b = 0;
    CHECK(!src->read(plain.size(), &b, 1));
    CHECK(!src->read(plain.size() - 1, &b, 2));
    CHECK(src->read(plain.size(), &b, 0));
    // The RomFS inside is the same: a file found through the tables.
    uint64_t off = 0, size = 0;
    CHECK(lgp::hle::romfs_find(*src, "/bin/data/large.bin", &off, &size));
    CHECK_EQ(size, 70000u);

    // Other block sizes, and an empty image.
    for (uint32_t bs : {8192u, 1u << 20}) {
        auto other = open_bytes(compress(plain, bs), &why);
        std::vector<uint8_t> again(plain.size());
        CHECK(other != nullptr && other->read(0, again.data(), again.size()) && again == plain);
    }
    auto empty = open_bytes(compress({}, 4096), &why);
    CHECK(empty != nullptr && empty->size() == 0);
    std::string err;
    CHECK(compress(plain, 1000, &err).empty() && !err.empty());  // block size below the minimum

    // Damaged files are refused when opened, or fail the read of their block.
    auto refused = [&](std::vector<uint8_t> bad) { return open_bytes(bad, &why) == nullptr && !why.empty(); };
    CHECK(refused(std::vector<uint8_t>(packed.begin(), packed.end() - 1)));  // truncated
    CHECK(refused(std::vector<uint8_t>(packed.begin(), packed.begin() + 40)));
    auto with = [&](size_t at, uint8_t v) {
        auto c = packed;
        c[at] = v;
        return c;
    };
    CHECK(refused(with(8, 2)));                   // version
    CHECK(refused(with(12, 7)));                  // codec
    CHECK(refused(with(20, uint8_t(packed[20] + 1))));  // block count
    CHECK(refused(with(packed.size() - 1, 'x')));  // end marker
    {
        // No blocks but an image size near 2^64: rounding the size up must not wrap to zero blocks.
        auto huge = compress({}, 4096);
        std::memset(huge.data() + 24, 0xFF, 8);
        CHECK(refused(huge));
    }
    uint64_t index_at = 0;
    std::memcpy(&index_at, packed.data() + packed.size() - 16, 8);
    CHECK(refused(with(index_at + 8 + 7, 0x80)));  // second block offset far beyond the file
    // Garbage inside the first (compressed) block: open works, its reads fail.
    uint64_t second = 0;
    std::memcpy(&second, packed.data() + index_at + 8, 8);
    CHECK(second - 32 < 4096);  // the first block is compressed
    auto garbled = packed;
    for (uint64_t i = 32; i < second; ++i)
        garbled[i] ^= 0x5A;
    auto g = open_bytes(garbled, &why);
    CHECK(g != nullptr);
    if (g) {
        CHECK(!g->read(0, &b, 1));
        std::vector<uint8_t> later(100);
        CHECK(g->read(5000, later.data(), later.size()));  // other blocks still read
    }
}

void test_fs_romfs_compressed() {
    if (!lgp::hle::compressed_images_supported())
        return;
    FsEnv e;
    const auto plain = sample_image();
    e.game.add_file("/romfs.bin", compress(plain, 4096));
    const Handle fsp = e.open_service("fsp-srv");
    const Handle st = e.object(e.call(fsp, 200));
    CHECK_EQ(static_cast<uint64_t>(e.call(st, 4).get<int64_t>(0)), plain.size());
    std::vector<uint8_t> img;
    for (uint64_t at = 0; at < plain.size();) {
        const uint64_t n = std::min<uint64_t>(plain.size() - at, kBufSize);
        CHECK_EQ(e.storage_read(st, static_cast<int64_t>(at), static_cast<int64_t>(n), n).result, 0u);
        const auto piece = e.guest(kBufB, n);
        img.insert(img.end(), piece.begin(), piece.end());
        at += n;
    }
    CHECK(img == plain);
    const Parsed p = parse(img);
    CHECK(p.ok);
    CHECK(p.files.count("/noise.bin") == 1);
    CHECK_EQ(e.storage_read(st, static_cast<int64_t>(plain.size()) - 4, 0x10, 0x10).result, fr::kOutOfRange.raw);
}

void test_fs_data_id() {
    FsEnv e;
    e.game.add_file("/systemdata/0100000000000810.bin", bytes("archive"));
    e.game.add_file("/systemdata/010000000000080e/x.txt", bytes("tree"));
    const Handle fsp = e.open_service("fsp-srv");
    Msg m;
    m.arg<uint8_t>(0).arg<uint64_t>(0x0100000000000810ull);
    const Handle st = e.object(e.call(fsp, 202, m));
    CHECK_EQ(e.call(st, 4).get<int64_t>(0), 7);
    Msg t;
    t.arg<uint8_t>(0).arg<uint64_t>(0x010000000000080Eull);
    const Handle tree = e.object(e.call(fsp, 202, t));
    CHECK(e.call(tree, 4).get<int64_t>(0) > 0x200);
    Msg missing;
    missing.arg<uint8_t>(0).arg<uint64_t>(0x0100000000000811ull);
    CHECK_EQ(e.call(fsp, 202, missing).result, fr::kTargetNotFound.raw);
    // No RomFS at all.
    CHECK_EQ(e.call(fsp, 200).result, fr::kTargetNotFound.raw);
}

// --- save data journal (P5.3) ----------------------------------------------------------

constexpr const char* kSaveRoot = "/010003f003a34000/account/11112222333344445555666677778888";

std::string read_host(lgp::platform::IStorage& s, const std::string& path) {
    std::unique_ptr<lgp::platform::IFile> f;
    if (s.open(path, lgp::platform::kOpenRead, &f) != StorageError::Ok)
        return "<missing>";
    std::string text(static_cast<size_t>(f->size()), '\0');
    uint64_t got = 0;
    f->read(0, text.data(), text.size(), &got);
    return text;
}

// Host directory of the committed state of the save at root.
std::string committed_slot(lgp::platform::IStorage& s, const std::string& root) {
    const std::string j = read_host(s, root + "/.lgp-journal");
    return root + (j.rfind("1", 0) == 0 ? "/.lgp-1" : "/.lgp-0");
}

std::vector<std::string> names(lgp::platform::IStorage& s, const std::string& dir) {
    std::vector<lgp::platform::DirEntry> list;
    s.list(dir, &list);
    std::vector<std::string> out;
    for (const auto& e : list)
        out.push_back(e.name);
    return out;
}

// Forwards to another storage. Subclasses change single calls.
class ForwardingStorage : public lgp::platform::IStorage {
public:
    explicit ForwardingStorage(lgp::platform::IStorage& base) : base_(base) {}
    bool read_only() const override { return base_.read_only(); }
    StorageError open(std::string_view p, uint32_t mode, std::unique_ptr<lgp::platform::IFile>* out) override {
        return base_.open(p, mode, out);
    }
    StorageError stat(std::string_view p, lgp::platform::DirEntry* out) override { return base_.stat(p, out); }
    StorageError list(std::string_view p, std::vector<lgp::platform::DirEntry>* out) override {
        return base_.list(p, out);
    }
    StorageError create_file(std::string_view p, uint64_t size) override { return base_.create_file(p, size); }
    StorageError create_directory(std::string_view p) override { return base_.create_directory(p); }
    StorageError remove_file(std::string_view p) override { return base_.remove_file(p); }
    StorageError remove_directory(std::string_view p, bool recursive) override {
        return base_.remove_directory(p, recursive);
    }
    StorageError rename(std::string_view from, std::string_view to) override { return base_.rename(from, to); }
    StorageError commit() override { return base_.commit(); }

protected:
    lgp::platform::IStorage& base_;
};

// Like WasmFS on OPFS: renaming a directory fails with EBUSY (Io).
class NoDirectoryRename final : public ForwardingStorage {
public:
    using ForwardingStorage::ForwardingStorage;
    int refused = 0;
    StorageError rename(std::string_view from, std::string_view to) override {
        lgp::platform::DirEntry e;
        if (base_.stat(from, &e) == StorageError::Ok && e.is_directory) {
            ++refused;
            return StorageError::Io;
        }
        return base_.rename(from, to);
    }
};

// A crash after `budget` changes: every later change fails with Io and
// leaves the storage as it was at that point.
class CrashingStorage final : public ForwardingStorage {
public:
    using ForwardingStorage::ForwardingStorage;
    int budget = 1 << 30;

    StorageError open(std::string_view p, uint32_t mode, std::unique_ptr<lgp::platform::IFile>* out) override {
        std::unique_ptr<lgp::platform::IFile> f;
        const StorageError e = base_.open(p, mode, &f);
        if (e == StorageError::Ok)
            *out = std::make_unique<File>(std::move(f), *this);
        return e;
    }
    StorageError create_file(std::string_view p, uint64_t size) override {
        return step() ? base_.create_file(p, size) : StorageError::Io;
    }
    StorageError create_directory(std::string_view p) override {
        return step() ? base_.create_directory(p) : StorageError::Io;
    }
    StorageError remove_file(std::string_view p) override { return step() ? base_.remove_file(p) : StorageError::Io; }
    StorageError remove_directory(std::string_view p, bool recursive) override {
        return step() ? base_.remove_directory(p, recursive) : StorageError::Io;
    }
    StorageError rename(std::string_view from, std::string_view to) override {
        return step() ? base_.rename(from, to) : StorageError::Io;
    }
    StorageError commit() override { return step() ? base_.commit() : StorageError::Io; }

    bool step() { return budget-- > 0; }

private:
    class File final : public lgp::platform::IFile {
    public:
        File(std::unique_ptr<lgp::platform::IFile> f, CrashingStorage& s) : f_(std::move(f)), s_(s) {}
        uint64_t size() const override { return f_->size(); }
        StorageError read(uint64_t o, void* d, uint64_t n, uint64_t* got) override { return f_->read(o, d, n, got); }
        StorageError write(uint64_t o, const void* src, uint64_t n) override {
            if (s_.step())
                return f_->write(o, src, n);
            if (n > 1)
                f_->write(o, src, n / 2);  // torn: only the first half reaches the storage
            return StorageError::Io;
        }
        StorageError set_size(uint64_t n) override { return s_.step() ? f_->set_size(n) : StorageError::Io; }
        StorageError flush() override { return s_.step() ? f_->flush() : StorageError::Io; }

    private:
        std::unique_ptr<lgp::platform::IFile> f_;
        CrashingStorage& s_;
    };
};

// Without Commit, closing the save drops the changes; Commit needs every
// file opened for writing closed; sessions of one save share its state.
void test_fs_save_commit() {
    FsEnv e;
    const auto v1 = bytes("version one");
    const auto v2 = bytes("version two, longer");
    Handle fs = e.open_save();
    e.write_file(fs, "/slot0.bin", v1);
    CHECK(e.read_file(fs, "/slot0.bin") == v1);  // the guest sees its own writes at once
    e.close(fs);
    fs = e.open_save();
    CHECK(!e.read_file(fs, "/slot0.bin"));  // never committed: gone

    const Handle open = e.write_file(fs, "/slot0.bin", v1, true);
    CHECK_EQ(e.call(fs, 10).result, fr::kWriteModeFileNotClosed.raw);
    CHECK_EQ(e.commits, 0);  // a refused Commit is no commit
    const Handle reader = e.open_file(fs, "/slot0.bin", 1);  // read-only files do not block
    e.close(open);
    CHECK_EQ(e.call(fs, 10).result, 0u);
    e.close(reader);
    CHECK(read_host(e.save, committed_slot(e.save, kSaveRoot) + "/slot0.bin") == "version one");
    CHECK_EQ(e.commits, 1);

    // A second session works on the same copy; after both close, only
    // what was committed is left.
    const Handle fs2 = e.open_save();
    e.write_file(fs2, "/slot0.bin", v2);
    CHECK(e.read_file(fs, "/slot0.bin") == v2);
    e.close(fs2);
    e.close(fs);
    fs = e.open_save();
    CHECK(e.read_file(fs, "/slot0.bin") == v1);
    // The guest cannot see or reach the journal.
    Msg od = e.path_msg("/");
    od.arg<uint32_t>(3);
    CHECK_EQ(e.call(e.object(e.call(fs, 9, od)), 1).get<int64_t>(0), 1);
    std::vector<std::string> host = names(e.save, kSaveRoot);
    CHECK(host == (std::vector<std::string>{".lgp-0", ".lgp-1", ".lgp-journal", ".lgp-work"}) ||
          host == (std::vector<std::string>{".lgp-0", ".lgp-journal", ".lgp-work"}) ||
          host == (std::vector<std::string>{".lgp-1", ".lgp-journal", ".lgp-work"}));

    // Temporary storage is not journaled: writes land in its root, Commit is a no-op.
    const Handle tmp = e.open_save(0, 4);
    e.write_file(tmp, "/t.bin", v1);
    CHECK_EQ(e.call(tmp, 10).result, 0u);
    CHECK(read_host(e.save, "/010003f003a34000/temporary/t.bin") == "version one");
    CHECK_EQ(e.commits, 1);  // temporary storage is not backed up
}

// A crash at any point of a commit mounts to the old or the new state,
// never to a mix, and a finished commit always survives.
void test_fs_save_crash_during_commit() {
    const auto old_data = pattern(0x3000, 1);
    const auto new_data = pattern(0x5000, 2);
    bool finished = false;
    int points = 0;
    for (int budget = 0; !finished && budget < 200; ++budget, ++points) {
        MemoryStorage disk;
        {
            FsEnv setup(true, &disk);
            const Handle fs = setup.open_save();
            CHECK_EQ(setup.call(fs, 2, setup.path_msg("/a")).result, 0u);
            setup.write_file(fs, "/a/data.bin", old_data);
            CHECK_EQ(setup.call(fs, 10).result, 0u);
        }
        bool committed = false;
        {
            CrashingStorage crashing(disk);
            FsEnv run(true, &crashing);
            const Handle fs = run.open_save();
            run.write_file(fs, "/a/data.bin", new_data);
            run.write_file(fs, "/b.bin", bytes("extra"));
            crashing.budget = budget;  // the crash happens inside Commit
            committed = run.call(fs, 10).result == 0;
        }
        FsEnv after(true, &disk);
        const Handle fs = after.open_save();
        const auto a = after.read_file(fs, "/a/data.bin");
        const auto b = after.read_file(fs, "/b.bin");
        const bool is_old = a == old_data && !b;
        const bool is_new = a == new_data && b == bytes("extra");
        CHECK(is_old || is_new);
        if (committed)
            CHECK(is_new);
        finished = committed;
    }
    CHECK(finished);
    CHECK(points > 5);  // the crash points were inside the commit, not before it
}

// Saves from before P5.3 (files directly in the root) and files put there
// by hand or by the browser import are taken over on mount.
void test_fs_save_take_over() {
    FsEnv e;
    e.save.add_file(std::string(kSaveRoot) + "/savedata.bin", bytes("legacy"));
    e.save.add_file(std::string(kSaveRoot) + "/sub/x.bin", bytes("x"));
    Handle fs = e.open_save();
    CHECK(e.read_file(fs, "/savedata.bin") == bytes("legacy"));
    CHECK(e.read_file(fs, "/sub/x.bin") == bytes("x"));
    for (const std::string& n : names(e.save, kSaveRoot))
        CHECK(n.rfind(".lgp-", 0) == 0);
    e.close(fs);

    // Dropped over a committed save: replaces that file, keeps the rest.
    e.save.add_file(std::string(kSaveRoot) + "/savedata.bin", bytes("edited"));
    fs = e.open_save();
    CHECK(e.read_file(fs, "/savedata.bin") == bytes("edited"));
    CHECK(e.read_file(fs, "/sub/x.bin") == bytes("x"));
    e.close(fs);
    fs = e.open_save();
    CHECK(e.read_file(fs, "/savedata.bin") == bytes("edited"));

    // A crash while taking over loses nothing: the next mount retries.
    bool mounted = false;
    for (int budget = 0; !mounted && budget < 100; ++budget) {
        MemoryStorage disk;
        disk.add_file(std::string(kSaveRoot) + "/savedata.bin", bytes("legacy"));
        {
            CrashingStorage crashing(disk);
            crashing.budget = budget;
            FsEnv run(true, &crashing);
            const Handle fsp = run.open_service("fsp-srv");
            Msg m;
            m.arg<uint8_t>(1);
            const uint64_t attr[8] = {0, kUid0, kUid1, 0, 1, 0, 0, 0};
            for (uint64_t w : attr)
                m.arg<uint64_t>(w);
            mounted = run.call(fsp, 51, m).result == 0;
        }
        FsEnv after(true, &disk);
        CHECK(after.read_file(after.open_save(), "/savedata.bin") == bytes("legacy"));
    }
    CHECK(mounted);
}

// OPFS (WasmFS) cannot rename directories: RenameDirectory copies instead.
void test_fs_rename_directory_by_copy() {
    MemoryStorage disk;
    NoDirectoryRename storage(disk);
    FsEnv e(true, &storage);
    const Handle fs = e.open_save();
    CHECK_EQ(e.call(fs, 2, e.path_msg("/old")).result, 0u);
    CHECK_EQ(e.call(fs, 2, e.path_msg("/old/deep")).result, 0u);
    e.write_file(fs, "/old/deep/f.bin", bytes("payload"));
    CHECK_EQ(e.call(fs, 2, e.path_msg("/taken")).result, 0u);
    Msg ren;
    ren.x = {e.path("/old", 0), e.path("/new", 1)};
    CHECK_EQ(e.call(fs, 6, ren).result, 0u);
    CHECK(storage.refused >= 1);
    CHECK(e.read_file(fs, "/new/deep/f.bin") == bytes("payload"));
    CHECK_EQ(e.call(fs, 7, e.path_msg("/old")).result, fr::kPathNotFound.raw);
    Msg clash;
    clash.x = {e.path("/new", 0), e.path("/taken", 1)};
    CHECK_EQ(e.call(fs, 6, clash).result, fr::kPathAlreadyExists.raw);
    CHECK(e.read_file(fs, "/new/deep/f.bin") == bytes("payload"));
    CHECK_EQ(e.call(fs, 10).result, 0u);  // the commit itself renames no directory
}

// The game's save sequence (docs/analysis/02-services.md): CreateFile,
// OpenFile(Write), SetSize, Write, Flush, Commit.
void test_fs_save_sequence() {
    FsEnv e;
    const Handle fs = e.open_save();
    const std::string root = "/010003f003a34000/account/11112222333344445555666677778888";
    lgp::platform::DirEntry st;
    CHECK(e.save.stat(root, &st) == StorageError::Ok && st.is_directory);

    Msg create = e.path_msg("/savedata.bin");
    create.arg<uint32_t>(0).arg<int64_t>(0);
    CHECK_EQ(e.call(fs, 0, create).result, 0u);
    CHECK_EQ(e.call(fs, 0, create).result, fr::kPathAlreadyExists.raw);

    Msg open = e.path_msg("/savedata.bin");
    open.arg<uint32_t>(2);  // Write
    const Handle file = e.object(e.call(fs, 8, open));
    Msg set;
    set.arg<int64_t>(0x100);
    CHECK_EQ(e.call(file, 3, set).result, 0u);

    const auto data = pattern(0x100, 0x40);
    CHECK(e.k.memory().copy_to_user(kBufA, data.data(), data.size()));
    Msg w;
    w.arg<uint32_t>(0).arg<int64_t>(0).arg<int64_t>(0x100);
    w.a = {{kBufA, 0x100, 0}};
    CHECK_EQ(e.call(file, 1, w).result, 0u);
    // Growing the file needs Append.
    Msg grow;
    grow.arg<uint32_t>(0).arg<int64_t>(0x80).arg<int64_t>(0x100);
    grow.a = {{kBufA, 0x100, 0}};
    CHECK_EQ(e.call(file, 1, grow).result, fr::kFileExtensionWithoutOpenModeAllowAppend.raw);
    Msg read;
    read.arg<uint32_t>(0).arg<int64_t>(0).arg<int64_t>(0x10);
    read.b = {{kBufB, 0x10, 0}};
    CHECK_EQ(e.call(file, 0, read).result, fr::kReadNotPermitted.raw);
    CHECK_EQ(e.call(file, 2).result, 0u);  // Flush
    CHECK_EQ(e.call(file, 4).get<int64_t>(0), 0x100);
    CHECK_EQ(e.svc(kSvcCloseHandle, {file})[0], kSuccess.raw);
    CHECK_EQ(e.call(fs, 10).result, 0u);  // Commit

    std::unique_ptr<lgp::platform::IFile> host;
    CHECK(e.save.open(committed_slot(e.save, root) + "/savedata.bin", lgp::platform::kOpenRead, &host) ==
          StorageError::Ok);
    std::vector<uint8_t> back(0x100);
    uint64_t got = 0;
    if (host)
        CHECK(host->read(0, back.data(), back.size(), &got) == StorageError::Ok);
    CHECK(back == data);

    // Read it back through fs, past the end: only the bytes that exist.
    Msg ro = e.path_msg("/savedata.bin");
    ro.arg<uint32_t>(1 | 4);  // Read | Append
    const Handle rf = e.object(e.call(fs, 8, ro));
    Msg r2;
    r2.arg<uint32_t>(0).arg<int64_t>(0xF0).arg<int64_t>(0x40);
    r2.b = {{kBufB, 0x40, 0}};
    const Reply rr = e.call(rf, 0, r2);
    CHECK_EQ(rr.result, 0u);
    CHECK_EQ(rr.get<int64_t>(0), 0x10);
    CHECK(e.guest(kBufB, 0x10) == std::vector<uint8_t>(data.begin() + 0xF0, data.end()));
    Msg past;
    past.arg<uint32_t>(0).arg<int64_t>(0x101).arg<int64_t>(1);
    past.b = {{kBufB, 1, 0}};
    CHECK_EQ(e.call(rf, 0, past).result, fr::kOutOfRange.raw);
    CHECK_EQ(e.call(rf, 1, w).result, fr::kWriteNotPermitted.raw);

    // Reopening the save sees the same data; the save of another user does not.
    const Handle again = e.open_save(kProgram);
    Msg type = e.path_msg("/savedata.bin");
    const Reply t = e.call(again, 7, type);
    CHECK_EQ(t.result, 0u);
    CHECK_EQ(t.get<uint32_t>(0), 1u);
    const Handle device = e.open_save(0, 3);
    CHECK_EQ(e.call(device, 7, e.path_msg("/savedata.bin")).result, fr::kPathNotFound.raw);
    CHECK(e.save.stat("/010003f003a34000/device", &st) == StorageError::Ok);

    // Space: total from the config, free minus the bytes in use.
    CHECK_EQ(e.call(again, 12, e.path_msg("/")).get<int64_t>(0), 0x10000);
    CHECK_EQ(e.call(again, 11, e.path_msg("/")).get<int64_t>(0), 0x10000 - 0x100);
}

void test_fs_directories() {
    FsEnv e;
    const Handle fs = e.open_save();
    CHECK_EQ(e.call(fs, 2, e.path_msg("/dir")).result, 0u);
    CHECK_EQ(e.call(fs, 2, e.path_msg("/dir/sub")).result, 0u);
    CHECK_EQ(e.call(fs, 2, e.path_msg("/no/parent")).result, fr::kPathNotFound.raw);
    Msg c = e.path_msg("/dir/f.bin");
    c.arg<uint32_t>(0).arg<int64_t>(0x30);
    CHECK_EQ(e.call(fs, 0, c).result, 0u);

    Msg od = e.path_msg("/dir");
    od.arg<uint32_t>(3);  // directories and files
    const Handle dir = e.object(e.call(fs, 9, od));
    CHECK_EQ(e.call(dir, 1).get<int64_t>(0), 2);
    Msg rd;
    rd.b = {{kBufB, 0x310 * 4, 0}};
    const Reply r = e.call(dir, 0, rd);
    CHECK_EQ(r.get<int64_t>(0), 2);
    const auto ents = e.guest(kBufB, 0x310 * 2);
    CHECK(std::string(reinterpret_cast<const char*>(ents.data())) == "f.bin");
    CHECK_EQ(ents[0x304], 1u);
    int64_t size = 0;
    std::memcpy(&size, ents.data() + 0x308, 8);
    CHECK_EQ(size, 0x30);
    CHECK(std::string(reinterpret_cast<const char*>(ents.data() + 0x310)) == "sub");
    CHECK_EQ(ents[0x310 + 0x304], 0u);
    CHECK_EQ(e.call(dir, 0, rd).get<int64_t>(0), 0);  // all read

    Msg files_only = e.path_msg("/dir");
    files_only.arg<uint32_t>(2);
    CHECK_EQ(e.call(e.object(e.call(fs, 9, files_only)), 1).get<int64_t>(0), 1);

    CHECK_EQ(e.call(fs, 3, e.path_msg("/dir")).result, fr::kDirectoryNotEmpty.raw);
    Msg ren;
    ren.x = {e.path("/dir/f.bin", 0), e.path("/dir/g.bin", 1)};
    CHECK_EQ(e.call(fs, 6, ren).result, fr::kPathNotFound.raw);  // RenameDirectory on a file
    CHECK_EQ(e.call(fs, 5, ren).result, 0u);                      // RenameFile
    CHECK_EQ(e.call(fs, 7, e.path_msg("/dir/g.bin")).get<uint32_t>(0), 1u);
    CHECK_EQ(e.call(fs, 7, e.path_msg("/dir/f.bin")).result, fr::kPathNotFound.raw);
    CHECK_EQ(e.call(fs, 1, e.path_msg("/dir/g.bin")).result, 0u);  // DeleteFile
    CHECK_EQ(e.call(fs, 3, e.path_msg("/dir/sub")).result, 0u);
    CHECK_EQ(e.call(fs, 2, e.path_msg("/dir/sub")).result, 0u);
    CHECK_EQ(e.call(fs, 13, e.path_msg("/dir")).result, 0u);  // CleanDirectoryRecursively
    CHECK_EQ(e.call(fs, 7, e.path_msg("/dir")).get<uint32_t>(0), 0u);
    CHECK_EQ(e.call(fs, 7, e.path_msg("/dir/sub")).result, fr::kPathNotFound.raw);
    CHECK_EQ(e.call(fs, 4, e.path_msg("/dir")).result, 0u);  // DeleteDirectoryRecursively
    CHECK_EQ(e.call(fs, 4, e.path_msg("/")).result, fr::kInvalidPath.raw);

    // Paths a guest must not use to leave its save.
    CHECK_EQ(e.call(fs, 7, e.path_msg("../x")).result, fr::kInvalidPath.raw);
    CHECK_EQ(e.call(fs, 7, e.path_msg("/../../x")).result, fr::kInvalidPath.raw);
    CHECK_EQ(e.call(fs, 7, e.path_msg("relative")).result, fr::kInvalidPath.raw);
    std::string unterminated(0x301, 'a');
    unterminated[0] = '/';
    CHECK_EQ(e.call(fs, 7, e.path_msg(unterminated)).result, fr::kInvalidPath.raw);
    Msg bad_mode = e.path_msg("/x");
    bad_mode.arg<uint32_t>(8);
    CHECK_EQ(e.call(fs, 8, bad_mode).result, fr::kInvalidArgument.raw);
}

void test_fs_no_save_storage() {
    FsEnv e(false);
    const Handle fsp = e.open_service("fsp-srv");
    Msg m;
    m.arg<uint8_t>(1);
    for (int i = 0; i < 8; ++i)
        m.arg<uint64_t>(i == 4 ? 1 : 0);  // type Account at 0x20
    CHECK_EQ(e.call(fsp, 51, m).result, fr::kTargetNotFound.raw);
}

}  // namespace

// pl:u reads the BFTTF system fonts from the same game storage as fsp-srv's
// system data and lays them out decrypted in its shared memory.
std::vector<uint8_t> make_bfttf(const std::vector<uint8_t>& ttf) {
    std::vector<uint8_t> out(8 + ttf.size(), 0);
    const uint32_t key = 0x06186249, magic = 0x18029A7F ^ key, size = static_cast<uint32_t>(ttf.size()) ^ key;
    std::memcpy(out.data(), &magic, 4);
    std::memcpy(out.data() + 4, &size, 4);
    for (size_t i = 0; i < ttf.size(); i += 4) {
        uint32_t v;
        std::memcpy(&v, ttf.data() + i, 4);
        v ^= key;
        std::memcpy(out.data() + 8 + i, &v, 4);
    }
    return out;
}

void test_pl_shared_font() {
    FsEnv e;
    register_pl(e.sm, &e.game);
    std::vector<uint8_t> std_ttf(64), ko_ttf(32);
    for (size_t i = 0; i < std_ttf.size(); ++i) std_ttf[i] = static_cast<uint8_t>(i * 3 + 1);
    for (size_t i = 0; i < ko_ttf.size(); ++i) ko_ttf[i] = static_cast<uint8_t>(0xA0 + i);
    e.game.add_file("/systemdata/0100000000000811/nintendo_udsg-r_std_003.bfttf", make_bfttf(std_ttf));
    e.game.add_file("/systemdata/0100000000000812/nintendo_udsg-r_ko_003.bfttf", make_bfttf(ko_ttf));
    CHECK(decrypt_bfttf(make_bfttf(std_ttf)) == std_ttf);
    CHECK(decrypt_bfttf(std_ttf).empty());  // no BFTTF magic

    const Handle pl = e.open_service("pl:u");
    auto type_arg = [](uint32_t t) {
        Msg m;
        m.arg<uint32_t>(t);
        return m;
    };
    CHECK_EQ(e.call(pl, 0, type_arg(0)).result, 0u);                  // RequestLoad
    CHECK_EQ(e.call(pl, 1, type_arg(0)).get<uint32_t>(0), 1u);        // GetLoadState: loaded
    CHECK_EQ(e.call(pl, 2, type_arg(0)).get<uint32_t>(0), 64u);       // GetFontSize
    CHECK_EQ(e.call(pl, 2, type_arg(4)).get<uint32_t>(0), 32u);
    CHECK_EQ(e.call(pl, 2, type_arg(1)).get<uint32_t>(0), 0u);        // missing file: empty
    const uint32_t off_std = e.call(pl, 3, type_arg(0)).get<uint32_t>(0);
    const uint32_t off_ko = e.call(pl, 3, type_arg(4)).get<uint32_t>(0);
    CHECK_EQ(off_std, 8u);
    const Reply shm = e.call(pl, 4);
    CHECK_EQ(shm.copy.size(), 1u);
    if (shm.copy.empty()) return;
    // The test process is too small to map 17 MiB; read the object's backing directly.
    const auto mem = e.k.handles().get_as<KSharedMemory>(shm.copy[0]);
    CHECK(mem != nullptr && mem->size() == kSharedFontMemorySize);
    if (!mem) return;
    const uint8_t* base = mem->data();
    CHECK(std::memcmp(base + off_std, std_ttf.data(), std_ttf.size()) == 0);
    CHECK(std::memcmp(base + off_ko, ko_ttf.data(), ko_ttf.size()) == 0);
    uint32_t hdr[2] = {};
    std::memcpy(hdr, base, 8);
    CHECK_EQ(hdr[0], 0x18029A7Fu ^ 0x49621806u);
    const uint32_t size_be = hdr[1] ^ 0x06186249u;  // byte-swapped key
    CHECK_EQ(((size_be >> 24) | ((size_be >> 8) & 0xFF00u) | ((size_be << 8) & 0xFF0000u) | (size_be << 24)), 64u);
}

// Without the firmware's standard font the stand-in serves types 0-4 at one
// offset; the Nintendo extension still comes from the firmware if present.
void test_pl_stand_in_font() {
    FsEnv e;
    std::vector<uint8_t> stand_in(40), ext_ttf(16);
    for (size_t i = 0; i < stand_in.size(); ++i) stand_in[i] = static_cast<uint8_t>(0x30 + i);
    for (size_t i = 0; i < ext_ttf.size(); ++i) ext_ttf[i] = static_cast<uint8_t>(0xE0 + i);
    // A stray firmware font of another type is not mixed in.
    e.game.add_file("/systemdata/0100000000000812/nintendo_udsg-r_ko_003.bfttf", make_bfttf(std::vector<uint8_t>(8, 1)));
    e.game.add_file("/systemdata/0100000000000810/nintendo_ext_003.bfttf", make_bfttf(ext_ttf));
    register_pl(e.sm, &e.game, stand_in);
    const Handle pl = e.open_service("pl:u");
    auto type_arg = [](uint32_t t) {
        Msg m;
        m.arg<uint32_t>(t);
        return m;
    };
    const uint32_t off0 = e.call(pl, 3, type_arg(0)).get<uint32_t>(0);
    CHECK_EQ(off0, 8u);
    for (uint32_t t = 0; t < 5; ++t) {
        CHECK_EQ(e.call(pl, 2, type_arg(t)).get<uint32_t>(0), 40u);
        CHECK_EQ(e.call(pl, 3, type_arg(t)).get<uint32_t>(0), off0);
    }
    CHECK_EQ(e.call(pl, 2, type_arg(5)).get<uint32_t>(0), 16u);
    const uint32_t off5 = e.call(pl, 3, type_arg(5)).get<uint32_t>(0);
    CHECK_EQ(off5, 8u + 40u + 8u);
    const Reply shm = e.call(pl, 4);
    CHECK_EQ(shm.copy.size(), 1u);
    if (shm.copy.empty()) return;
    const auto mem = e.k.handles().get_as<KSharedMemory>(shm.copy[0]);
    CHECK(mem != nullptr);
    if (!mem) return;
    CHECK(std::memcmp(mem->data() + off0, stand_in.data(), stand_in.size()) == 0);
    CHECK(std::memcmp(mem->data() + off5, ext_ttf.data(), ext_ttf.size()) == 0);

    // With the firmware's standard font the stand-in is not used.
    FsEnv f;
    std::vector<uint8_t> std_ttf(24, 0x55);
    f.game.add_file("/systemdata/0100000000000811/nintendo_udsg-r_std_003.bfttf", make_bfttf(std_ttf));
    register_pl(f.sm, &f.game, stand_in);
    const Handle pl2 = f.open_service("pl:u");
    CHECK_EQ(f.call(pl2, 2, type_arg(0)).get<uint32_t>(0), 24u);
    CHECK_EQ(f.call(pl2, 2, type_arg(1)).get<uint32_t>(0), 0u);

    // The build's own stand-in, if it has one, is a TrueType file.
    const std::span<const uint8_t> built = default_stand_in_font();
    if (!built.empty()) {
        CHECK(built.size() > 1000000 && built.size() < kSharedFontMemorySize - 8);
        CHECK(built[0] == 0x00 && built[1] == 0x01 && built[2] == 0x00 && built[3] == 0x00);
    }
}

int main() {
    RUN(test_romfs_buckets);
    RUN(test_romfs_image);
    RUN(test_romfs_overlay);
    RUN(test_fs_romfs_replace);
    RUN(test_fs_romfs_changes);
    RUN(test_fs_romfs_image_file);
    RUN(test_fs_romfs_tree);
    RUN(test_romfs_compressed);
    RUN(test_fs_romfs_compressed);
    RUN(test_fs_data_id);
    RUN(test_fs_save_sequence);
    RUN(test_fs_directories);
    RUN(test_fs_save_commit);
    RUN(test_fs_save_crash_during_commit);
    RUN(test_fs_save_take_over);
    RUN(test_fs_rename_directory_by_copy);
    RUN(test_fs_no_save_storage);
    RUN(test_pl_shared_font);
    RUN(test_pl_stand_in_font);
    return finish();
}
