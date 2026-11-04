// P7.3: a game file from byte ranges of host files (segment_storage.hpp), as
// the Android app reads romfs.bin from parts inside its split APKs. Dummy
// files only.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../runtime/check.hpp"
#include "lgp/platform/host_directory_storage.hpp"
#include "lgp/platform/segment_storage.hpp"

using namespace lgp::platform;
namespace fs = std::filesystem;

namespace {

fs::path g_dir;

std::string write(const std::string& name, const std::string& data) {
    const fs::path p = g_dir / name;
    std::ofstream(p, std::ios::binary) << data;
    return p.string();
}

std::string read_all(IFile& f, uint64_t offset, uint64_t len) {
    std::string out(len, '\0');
    uint64_t got = 0;
    CHECK(f.read(offset, out.data(), len, &got) == StorageError::Ok);
    out.resize(got);
    return out;
}

void test_parse() {
    std::vector<FileSegment> s;
    std::string error;
    CHECK(parse_file_segments("/proc/self/fd/7@4096+100,/a/b@0+5", &s, &error));
    CHECK_EQ(s.size(), 2u);
    CHECK(s[0].path == "/proc/self/fd/7" && s[0].offset == 4096 && s[0].length == 100 && !s[0].whole_file);
    CHECK(s[1].path == "/a/b" && s[1].offset == 0 && s[1].length == 5);
    CHECK(parse_file_segments("whole.bin", &s, &error) && s.size() == 1 && s[0].whole_file);
    CHECK(!parse_file_segments("", &s, &error));
    CHECK(!parse_file_segments("a@1", &s, &error));       // no length
    CHECK(!parse_file_segments("a@x+1", &s, &error));     // not a number
    CHECK(!parse_file_segments("a@0+0", &s, &error));     // empty range
    CHECK(!parse_file_segments("@0+1", &s, &error));      // no file
    CHECK(!parse_file_segments("a@0+1,", &s, &error));    // empty item
}

void test_segmented_read() {
    // Parts inside larger files (as assets inside an APK), plus a whole file.
    const std::string a = write("a.apk", "HEADER" "0123456789" "TRAILER");
    const std::string b = write("b.apk", "xx" "abcdef" "yy");
    const std::string c = write("c.bin", "XYZ");
    std::vector<FileSegment> s;
    std::string error;
    CHECK(parse_file_segments(a + "@6+10," + b + "@2+6," + c, &s, &error));
    auto f = open_segmented_file(s, &error);
    CHECK(f != nullptr);
    if (!f)
        return;
    CHECK_EQ(f->size(), 19u);
    CHECK(read_all(*f, 0, 19) == "0123456789abcdefXYZ");
    CHECK(read_all(*f, 8, 6) == "89abcd");    // across a boundary
    CHECK(read_all(*f, 10, 6) == "abcdef");   // exactly one part
    CHECK(read_all(*f, 15, 100) == "fXYZ");   // short at the end
    CHECK(read_all(*f, 19, 4).empty());
    uint64_t got = 0;
    char ch;
    CHECK(f->write(0, "z", 1) == StorageError::ReadOnly);
    CHECK(f->read(30, &ch, 1, &got) == StorageError::Ok && got == 0);
    // A range beyond its file fails when opened, not at the guest's read.
    CHECK(parse_file_segments(b + "@5+6", &s, &error));
    CHECK(open_segmented_file(s, &error) == nullptr);
    CHECK(parse_file_segments(g_dir.string() + "/missing@0+1", &s, &error));
    CHECK(open_segmented_file(s, &error) == nullptr);
}

void test_added_files_storage() {
    fs::create_directories(g_dir / "game" / "exefs");
    std::ofstream(g_dir / "game" / "exefs" / "main", std::ios::binary) << "NSO";
    std::ofstream(g_dir / "game" / "romfs.bin", std::ios::binary) << "old";  // hidden by the added file
    const std::string part = write("part.apk", "....ROMFS-DATA....");
    std::vector<FileSegment> s;
    std::string error;
    CHECK(parse_file_segments(part + "@4+10", &s, &error));
    std::vector<std::pair<std::string, std::vector<FileSegment>>> files;
    files.emplace_back("romfs.bin", s);
    auto storage = AddedFilesStorage::create(HostDirectoryStorage::open_root(g_dir / "game", true), files, &error);
    CHECK(storage != nullptr);
    if (!storage)
        return;
    DirEntry e;
    CHECK(storage->stat("/romfs.bin", &e) == StorageError::Ok && !e.is_directory && e.size == 10);
    CHECK(storage->stat("/exefs/main", &e) == StorageError::Ok && e.size == 3);
    std::unique_ptr<IFile> f;
    CHECK(storage->open("/romfs.bin", kOpenRead, &f) == StorageError::Ok && f);
    if (f)
        CHECK(read_all(*f, 0, 10) == "ROMFS-DATA");
    CHECK(storage->open("/romfs.bin", kOpenRead | kOpenWrite, &f) == StorageError::ReadOnly);
    CHECK(storage->open("/exefs/main", kOpenRead, &f) == StorageError::Ok);
    std::vector<DirEntry> list;
    CHECK(storage->list("/", &list) == StorageError::Ok);
    CHECK_EQ(list.size(), 2u);  // exefs, romfs.bin once
    CHECK(list.size() == 2 && list[0].name == "exefs" && list[1].name == "romfs.bin" && list[1].size == 10);
    CHECK(storage->create_file("/new", 1) == StorageError::ReadOnly);
    // Without a directory (only added files).
    auto only = AddedFilesStorage::create(nullptr, files, &error);
    CHECK(only && only->list("/", &list) == StorageError::Ok && list.size() == 1);
    CHECK(only && only->stat("/exefs", &e) == StorageError::NotFound);
    files[0].first = "a/b";
    CHECK(AddedFilesStorage::create(nullptr, files, &error) == nullptr);
}

}  // namespace

int main() {
    g_dir = fs::temp_directory_path() / "lgp-test-segment-storage";
    std::error_code ec;
    fs::remove_all(g_dir, ec);
    fs::create_directories(g_dir);
    RUN(test_parse);
    RUN(test_segmented_read);
    RUN(test_added_files_storage);
    fs::remove_all(g_dir, ec);
    return finish();
}
