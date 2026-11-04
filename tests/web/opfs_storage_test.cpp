// OPFS storage in the browser (P6.1): run by browser.opfs_storage through
// the page of lgp_add_web_page(), after the test's file picker copied
// test_data.mjs's dump folder into OPFS.
//
//   --phase import    the game data is complete, readable through IStorage,
//                     read-only, without the files the page must leave out;
//                     writes a save file; a journaled save (P5.3,
//                     runtime/hle/src/save_data.hpp) commits a renamed
//                     directory (OPFS cannot rename directories) and keeps
//                     one change uncommitted
//   --phase persist   after a reload: game data and save are still there,
//                     the journaled save holds exactly the committed state
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "../../runtime/hle/src/save_data.hpp"
#include "lgp/platform/web/opfs.hpp"

namespace {

using namespace lgp::platform;

// Keep in sync with FILES in test_data.mjs.
struct ExpectedFile {
    const char* path;
    uint64_t size;
};
constexpr ExpectedFile kFiles[] = {
    {"/exefs/main", 65536},
    {"/exefs/main.npdm", 1024},
    {"/exefs/sdk", 100003},
    {"/romfs/bin/large.dat", 9 * 1048576 + 7},
    {"/romfs/deep/a/b/c.txt", 100},
    {"/romfs/empty.bin", 0},
    {"/systemdata/0100000000000811", 2048},
};
constexpr const char* kSaveText = "saved in OPFS\n";
constexpr const char* kCommittedText = "committed in OPFS\n";

int g_failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("FAILED: %s\n", what.c_str());
        ++g_failures;
    }
}

uint32_t fnv1a32(const char* s) {
    uint32_t h = 0x811c9dc5u;
    for (; *s != '\0'; ++s)
        h = (h ^ static_cast<uint8_t>(*s)) * 0x01000193u;
    return h;
}

uint8_t expected_byte(uint32_t seed, uint64_t i) {
    return static_cast<uint8_t>((static_cast<uint32_t>(i) * 0x9E3779B1u + seed) >> 24);
}

// Reads the whole file in odd-sized pieces and compares it with the formula.
void verify_file(IStorage& game, const ExpectedFile& f) {
    DirEntry st;
    if (game.stat(f.path, &st) != StorageError::Ok || st.is_directory || st.size != f.size) {
        check(false, std::string("stat ") + f.path);
        return;
    }
    std::unique_ptr<IFile> file;
    if (game.open(f.path, kOpenRead, &file) != StorageError::Ok) {
        check(false, std::string("open ") + f.path);
        return;
    }
    check(file->size() == f.size, std::string("size ") + f.path);
    const uint32_t seed = fnv1a32(f.path + 1);  // without the leading '/'
    std::vector<uint8_t> buf((1u << 20) + 13);
    uint64_t offset = 0;
    while (offset < f.size) {
        uint64_t got = 0;
        if (file->read(offset, buf.data(), buf.size(), &got) != StorageError::Ok || got == 0) {
            check(false, std::string("read ") + f.path);
            return;
        }
        for (uint64_t i = 0; i < got; ++i) {
            if (buf[i] != expected_byte(seed, offset + i)) {
                check(false, std::string("content ") + f.path + " at " + std::to_string(offset + i));
                return;
            }
        }
        offset += got;
    }
    uint64_t got = 1;
    check(file->read(f.size, buf.data(), 16, &got) == StorageError::Ok && got == 0, std::string("eof ") + f.path);
}

void verify_game_data(IStorage& game) {
    check(game.read_only(), "game storage is read-only");
    std::vector<DirEntry> root;
    check(game.list("/", &root) == StorageError::Ok, "list /");
    std::string names;
    for (const DirEntry& e : root)
        names += e.name + (e.is_directory ? "/ " : " ");
    // Only the known parts of the dump: no Game.nsp, prod.keys or notes/.
    check(names == "exefs/ romfs/ systemdata/ ", "root holds exefs/ romfs/ systemdata/, got: " + names);
    for (const ExpectedFile& f : kFiles)
        verify_file(game, f);
    std::unique_ptr<IFile> file;
    check(game.open("/exefs/main", kOpenRead | kOpenWrite, &file) == StorageError::ReadOnly, "write open refused");
    check(game.create_file("/new", 1) == StorageError::ReadOnly, "create refused");
    DirEntry st;
    check(game.stat("/../lgp/save", &st) == StorageError::InvalidPath, "path outside the root refused");
}

bool write_all(IStorage& s, const char* path, const char* text) {
    std::unique_ptr<IFile> f;
    return s.open(path, kOpenRead | kOpenWrite, &f) == StorageError::Ok &&
           f->write(0, text, std::strlen(text)) == StorageError::Ok && f->flush() == StorageError::Ok;
}

std::string read_all(IStorage& s, const char* path) {
    std::unique_ptr<IFile> f;
    if (s.open(path, kOpenRead, &f) != StorageError::Ok)
        return "<missing>";
    std::string text(f->size(), '\0');
    uint64_t got = 0;
    if (f->read(0, text.data(), text.size(), &got) != StorageError::Ok)
        return "<unreadable>";
    text.resize(got);
    return text;
}

}  // namespace

int main(int argc, char** argv) {
    // Other arguments are ignored: browser_test.mjs adds boot options such as
    // --keyboard to every page it autostarts.
    std::string phase;
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], "--phase") == 0) phase = argv[i + 1];
    if (phase != "import" && phase != "persist") {
        std::fprintf(stderr, "usage: opfs_storage_test --phase import|persist\n");
        return 2;
    }
    std::string error;
    std::unique_ptr<IStorage> game = web::open_opfs_storage(web::kGameDataDir, true, &error);
    std::unique_ptr<IStorage> save = web::open_opfs_storage(web::kSaveDir, false, &error);
    if (!game || !save) {
        std::printf("FAILED: %s\n", error.c_str());
        return 1;
    }
    verify_game_data(*game);
    if (phase == "import") {
        save->remove_directory("/test", true);
        check(save->create_directory("/test") == StorageError::Ok, "save mkdir");
        check(save->create_file("/test/tmp", 0) == StorageError::Ok, "save create");
        check(write_all(*save, "/test/tmp", kSaveText), "save write");
        check(save->rename("/test/tmp", "/test/state") == StorageError::Ok, "save rename");
        check(save->commit() == StorageError::Ok, "save commit");
    }
    check(read_all(*save, "/test/state") == kSaveText, "save content");
    std::vector<DirEntry> entries;
    check(save->list("/test", &entries) == StorageError::Ok && entries.size() == 1 && entries[0].name == "state",
          "save directory holds only state");

    namespace detail = lgp::hle::detail;
    if (phase == "import") {
        save->remove_directory("/journal", true);
        check(save->create_directory("/journal") == StorageError::Ok, "journal mkdir");
    }
    StorageError err = StorageError::Ok;
    auto journal = detail::SaveData::mount(*save, "/journal", true, &err);
    check(journal != nullptr, std::string("journal mount: ") + storage_error_name(err));
    if (journal) {
        const std::string work = journal->work_root();
        if (phase == "import") {
            check(save->create_directory(work + "/dir") == StorageError::Ok, "journal mkdir dir");
            check(save->create_file(work + "/dir/f", 0) == StorageError::Ok, "journal create");
            check(write_all(*save, (work + "/dir/f").c_str(), kCommittedText), "journal write");
            const StorageError direct = save->rename(work + "/dir", work + "/direct");
            std::printf("opfs: directory rename -> %s\n", storage_error_name(direct));
            if (direct == StorageError::Ok)
                save->rename(work + "/direct", work + "/dir");
            check(detail::rename_directory(*save, work + "/dir", work + "/moved") == StorageError::Ok,
                  "journal directory rename");
            check(journal->commit() == StorageError::Ok, "journal commit");
            check(save->create_file(work + "/uncommitted", 1) == StorageError::Ok, "journal uncommitted");
        } else {
            check(read_all(*save, (work + "/moved/f").c_str()) == kCommittedText, "journal committed content");
            DirEntry st;
            check(save->stat(work + "/dir", &st) == StorageError::NotFound, "journal: renamed source gone");
            check(save->stat(work + "/uncommitted", &st) == StorageError::NotFound, "journal: uncommitted dropped");
        }
    }
    std::printf("opfs storage %s: %s\n", phase.c_str(), g_failures == 0 ? "ok" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
