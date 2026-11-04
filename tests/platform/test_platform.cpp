// P3.6: platform interfaces and the device-less backends (platform/common).
// The storage tests run the same checks against the in-memory backend and a
// host directory, so both behave like one IStorage.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "../runtime/check.hpp"
#include "lgp/platform/cpu_affinity.hpp"
#include "lgp/platform/host_block.hpp"
#include "lgp/platform/host_env.hpp"
#include "lgp/platform/host_directory_storage.hpp"
#include "lgp/platform/host_env.hpp"
#include "lgp/platform/host_sync.hpp"
#include "lgp/platform/host_thread.hpp"
#include "lgp/platform/memory_storage.hpp"
#include "lgp/platform/null_backends.hpp"
#include "lgp/platform/platform.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#if defined(__linux__)
#include <sched.h>
#endif
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace lgp::platform;
namespace fs = std::filesystem;

namespace {

using E = StorageError;

#define CHECK_SE(expr, expected)                                                                      \
    do {                                                                                              \
        const StorageError e_ = (expr);                                                               \
        if (e_ != (expected)) {                                                                       \
            std::printf("  FAIL %s:%d: %s == %s, expected %s\n", __FILE__, __LINE__, #expr,          \
                        storage_error_name(e_), storage_error_name(expected));                        \
            ++lgp_test::g_failures;                                                                   \
        }                                                                                             \
    } while (0)

void test_normalize_path() {
    CHECK(normalize_path("") == std::string("/"));
    CHECK(normalize_path("/") == std::string("/"));
    CHECK(normalize_path("//a///b/") == std::string("/a/b"));
    CHECK(normalize_path("/save/0/data.bin") == std::string("/save/0/data.bin"));
    CHECK(normalize_path("/ümlaut name") == std::string("/ümlaut name"));
    for (const char* bad : {"a/b", "/a/../b", "/..", "/./a", "/a\\b", "/c:/x", "rom:/a"})
        CHECK(!normalize_path(bad).has_value());
    CHECK(!normalize_path(std::string_view("/a\0b", 4)).has_value());
}

std::string read_all(IStorage& s, const char* path) {
    std::unique_ptr<IFile> f;
    if (s.open(path, kOpenRead, &f) != E::Ok)
        return "<open failed>";
    std::string out(f->size(), '\0');
    uint64_t got = 0;
    if (f->read(0, out.data(), out.size(), &got) != E::Ok || got != out.size())
        return "<read failed>";
    return out;
}

// Common IStorage semantics; `s` starts empty and writable.
void storage_semantics(IStorage& s) {
    CHECK(!s.read_only());
    std::vector<DirEntry> list;
    CHECK_SE(s.list("/", &list), E::Ok);
    CHECK(list.empty());

    CHECK_SE(s.create_directory("/save"), E::Ok);
    CHECK_SE(s.create_directory("/save"), E::AlreadyExists);
    CHECK_SE(s.create_directory("/missing/dir"), E::NotFound);
    CHECK_SE(s.create_file("/save/a.bin", 16), E::Ok);
    CHECK_SE(s.create_file("/save/a.bin", 16), E::AlreadyExists);
    CHECK_SE(s.create_file("/save/a.bin/x", 1), E::NotADirectory);
    CHECK_SE(s.create_file("/../escape", 1), E::InvalidPath);

    DirEntry st;
    CHECK_SE(s.stat("/save/a.bin", &st), E::Ok);
    CHECK(!st.is_directory);
    CHECK_EQ(st.size, 16u);
    CHECK(st.name == "a.bin");
    CHECK_SE(s.stat("/save", &st), E::Ok);
    CHECK(st.is_directory);
    CHECK_SE(s.stat("/nope", &st), E::NotFound);

    // Read/write, growth with a zero-filled gap, truncation.
    std::unique_ptr<IFile> f;
    CHECK_SE(s.open("/save/a.bin", kOpenRead | kOpenWrite, &f), E::Ok);
    if (f) {
        CHECK_SE(f->write(0, "hello", 5), E::Ok);
        CHECK_SE(f->write(20, "!", 1), E::Ok);
        CHECK_EQ(f->size(), 21u);
        char buf[32] = {};
        uint64_t got = 0;
        CHECK_SE(f->read(0, buf, sizeof(buf), &got), E::Ok);
        CHECK_EQ(got, 21u);
        CHECK(std::memcmp(buf, "hello", 5) == 0);
        CHECK_EQ(buf[16], 0);
        CHECK_EQ(buf[20], '!');
        CHECK_SE(f->read(100, buf, 4, &got), E::Ok);
        CHECK_EQ(got, 0u);
        CHECK_SE(f->set_size(3), E::Ok);
        CHECK_SE(f->flush(), E::Ok);
        CHECK_EQ(f->size(), 3u);
    }
    f.reset();
    CHECK(read_all(s, "/save/a.bin") == "hel");
    std::unique_ptr<IFile> ro;
    CHECK_SE(s.open("/save/a.bin", kOpenRead, &ro), E::Ok);
    if (ro)
        CHECK_SE(ro->write(0, "x", 1), E::ReadOnly);
    ro.reset();
    CHECK_SE(s.open("/save", kOpenRead, &f), E::IsADirectory);
    CHECK_SE(s.open("/save/none", kOpenRead, &f), E::NotFound);

    // Listing is sorted, files and directories mixed.
    CHECK_SE(s.create_directory("/save/sub"), E::Ok);
    CHECK_SE(s.create_file("/save/0.bin", 2), E::Ok);
    CHECK_SE(s.list("/save", &list), E::Ok);
    CHECK_EQ(list.size(), 3u);
    if (list.size() == 3) {
        CHECK(list[0].name == "0.bin" && !list[0].is_directory && list[0].size == 2);
        CHECK(list[1].name == "a.bin");
        CHECK(list[2].name == "sub" && list[2].is_directory);
    }
    CHECK_SE(s.list("/save/a.bin", &list), E::NotADirectory);

    // Rename: refuses to overwrite, moves directories with their contents.
    CHECK_SE(s.rename("/save/0.bin", "/save/a.bin"), E::AlreadyExists);
    CHECK_SE(s.rename("/save/0.bin", "/save/sub/b.bin"), E::Ok);
    CHECK_SE(s.rename("/save/sub", "/save/sub/inner"), E::InvalidPath);
    CHECK_SE(s.rename("/save/sub", "/moved"), E::Ok);
    CHECK_SE(s.stat("/moved/b.bin", &st), E::Ok);
    CHECK_SE(s.stat("/save/sub", &st), E::NotFound);

    // Removal.
    CHECK_SE(s.remove_file("/moved"), E::IsADirectory);
    CHECK_SE(s.remove_directory("/moved", false), E::NotEmpty);
    CHECK_SE(s.remove_directory("/moved", true), E::Ok);
    CHECK_SE(s.remove_directory("/save/a.bin", false), E::NotADirectory);
    CHECK_SE(s.remove_file("/save/a.bin"), E::Ok);
    CHECK_SE(s.remove_file("/save/a.bin"), E::NotFound);
    CHECK_SE(s.remove_directory("/", true), E::InvalidPath);
    CHECK_SE(s.commit(), E::Ok);
}

void test_memory_storage() {
    MemoryStorage s;
    storage_semantics(s);

    // Open files survive a rename of their path.
    MemoryStorage m;
    m.add_file("/dir/deep/file", {1, 2, 3});
    std::unique_ptr<IFile> f;
    CHECK_SE(m.open("/dir/deep/file", kOpenRead | kOpenWrite, &f), E::Ok);
    CHECK_SE(m.rename("/dir", "/other"), E::Ok);
    if (f) {
        CHECK_SE(f->write(3, "\x04", 1), E::Ok);
        CHECK_SE(f->set_size(MemoryStorage::kMaxFileSize + 1), E::OutOfRange);
    }
    DirEntry st;
    CHECK_SE(m.stat("/other/deep/file", &st), E::Ok);
    CHECK_EQ(st.size, 4u);

    MemoryStorage ro(true);
    ro.add_file("/romfs/a", {7});
    CHECK(ro.read_only());
    CHECK(read_all(ro, "/romfs/a") == std::string("\x07"));
    CHECK_SE(ro.open("/romfs/a", kOpenWrite, &f), E::ReadOnly);
    CHECK_SE(ro.create_file("/b", 1), E::ReadOnly);
    CHECK_SE(ro.remove_file("/romfs/a"), E::ReadOnly);
}

fs::path make_temp_dir(const char* tag) {
    static std::atomic<int> counter{0};
    const fs::path p = fs::temp_directory_path() /
                       ("lgp_platform_" + std::string(tag) + "_" + std::to_string(counter++) + "_" +
                        std::to_string(reinterpret_cast<uintptr_t>(&counter) & 0xFFFF));
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

void test_host_directory_storage() {
    const fs::path base = make_temp_dir("host");
    const fs::path root = base / "root";
    fs::create_directories(root);
    {
        auto s = HostDirectoryStorage::open_root(root, false);
        CHECK(s != nullptr);
        if (s)
            storage_semantics(*s);
        CHECK(fs::is_directory(root));  // remove_directory("/") must not have touched it
    }

    // commit() syncs written files and changed directories to the disk
    // (P5.3), also after renames and removals of written paths.
    if (auto sp = HostDirectoryStorage::open_root(root, false)) {
        HostDirectoryStorage& s = *sp;
        CHECK_SE(s.create_directory("/d"), E::Ok);
        CHECK_SE(s.create_file("/d/f", 0), E::Ok);
        std::unique_ptr<IFile> f;
        CHECK_SE(s.open("/d/f", kOpenRead | kOpenWrite, &f), E::Ok);
        if (f)
            CHECK_SE(f->write(0, "data", 4), E::Ok);
        CHECK_SE(s.commit(), E::Ok);
        if (f)
            CHECK_SE(f->write(4, "more", 4), E::Ok);  // dirty again after the commit
        f.reset();
        CHECK_SE(s.rename("/d", "/e"), E::Ok);
        CHECK_SE(s.create_file("/gone", 1), E::Ok);
        CHECK_SE(s.remove_file("/gone"), E::Ok);
        CHECK_SE(s.commit(), E::Ok);
        CHECK(read_all(s, "/e/f") == "datamore");
        CHECK_SE(s.remove_directory("/e", true), E::Ok);
        CHECK_SE(s.commit(), E::Ok);
    }
    CHECK(!lgp::platform::sync_host_path(root, true));
    { std::ofstream(root / "synced.bin") << "x"; }
    CHECK(!lgp::platform::sync_host_path(root / "synced.bin", false));
    CHECK(lgp::platform::sync_host_path(root / "missing.bin", false) == std::errc::no_such_file_or_directory);
    fs::remove(root / "synced.bin");
#if defined(_WIN32)
    // sync_host_path skips directories on file systems that cannot flush
    // them; on NTFS (the test's temp dir) the flush itself must work.
    {
        HANDLE h = CreateFileW(root.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        CHECK(h != INVALID_HANDLE_VALUE);
        if (h != INVALID_HANDLE_VALUE) {
            const BOOL ok = FlushFileBuffers(h);
            if (!ok)
                std::printf("  FlushFileBuffers(directory): error %lu\n", GetLastError());
            CHECK(ok != 0);
            CloseHandle(h);
        }
    }
#endif

    // Nothing outside the root is reachable, whatever the guest asks for.
    { std::ofstream(base / "secret.txt") << "outside"; }
    if (auto sp = HostDirectoryStorage::open_root(root, false)) {
        HostDirectoryStorage& s = *sp;
        std::unique_ptr<IFile> f;
        for (const char* p : {"/../secret.txt", "../secret.txt", "/..\\secret.txt", "/a/../../secret.txt"})
            CHECK_SE(s.open(p, kOpenRead, &f), E::InvalidPath);
        CHECK_SE(s.create_file("/../x", 1), E::InvalidPath);
        CHECK_SE(s.remove_directory("/", true), E::InvalidPath);
        CHECK(fs::exists(base / "secret.txt"));
    }

    // Read-only: the user's dump is never modified.
    {
        std::ofstream(root / "rom.bin", std::ios::binary) << "ROM";
        auto sp = HostDirectoryStorage::open_root(root, true);
        CHECK(sp != nullptr);
        if (!sp)
            return;
        HostDirectoryStorage& s = *sp;
        CHECK(s.read_only());
        CHECK(read_all(s, "/rom.bin") == "ROM");
        std::unique_ptr<IFile> f;
        CHECK_SE(s.open("/rom.bin", kOpenRead | kOpenWrite, &f), E::ReadOnly);
        CHECK_SE(s.create_directory("/new"), E::ReadOnly);
        CHECK_SE(s.remove_file("/rom.bin"), E::ReadOnly);
        CHECK(fs::exists(root / "rom.bin"));
    }

    CHECK(HostDirectoryStorage::open_root(base / "does-not-exist", true) == nullptr);
    CHECK(HostDirectoryStorage::open_root(root / "rom.bin", true) == nullptr);  // a file, not a directory
    fs::remove_all(base);
}

void test_null_backends() {
    NullRenderer r({640, 360}, 3);
    CHECK_EQ(r.surface_size().width, 640u);
    CHECK(!r.gpu().valid());
    const uint8_t px[2 * 2 * 4 + 8] = {1, 2, 3, 4, 5, 6, 7, 8, 0xEE, 0xEE, 0xEE, 0xEE, 9, 10, 11, 12, 13, 14, 15, 16};
    r.present_rgba8({px, 2, 2, 12});  // stride with padding
    SurfaceSize size;
    const std::vector<uint8_t> img = r.last_image(&size);
    CHECK_EQ(size.width, 2u);
    CHECK_EQ(img.size(), 16u);
    if (img.size() == 16) {
        CHECK_EQ(img[7], 8u);
        CHECK_EQ(img[8], 9u);  // padding skipped
        CHECK_EQ(img[15], 16u);
    }
    CHECK(r.end_frame());
    CHECK(r.end_frame());
    CHECK(!r.end_frame());  // "window closed" after 3 frames
    CHECK_EQ(r.frames(), 3u);

    NullAudio audio(100);
    CHECK(audio.open_sink({48000, 0}) == nullptr);
    std::unique_ptr<IAudioSink> sink = audio.open_sink({48000, 2});
    CHECK(sink != nullptr);
    if (sink) {
        auto* ns = static_cast<NullAudioSink*>(sink.get());
        std::vector<int16_t> samples(2 * 150);
        for (size_t i = 0; i < samples.size(); ++i)
            samples[i] = static_cast<int16_t>(i);
        CHECK_EQ(sink->write(samples.data(), 150), 100u);  // queue full at 100 frames
        CHECK_EQ(sink->queued_frames(), 100u);
        CHECK_EQ(ns->checksum(), 199 * 200 / 2);
        sink->set_paused(true);
        CHECK_EQ(ns->drain(30), 0u);
        sink->set_paused(false);
        CHECK_EQ(ns->drain(30), 30u);
        CHECK_EQ(sink->queued_frames(), 70u);
        CHECK_EQ(ns->played_frames(), 30u);
    }

    ManualInput in(2);
    CHECK_EQ(in.controller_count(), 2u);
    ControllerState s;
    s.style = ControllerStyle::Handheld;
    s.buttons = kButtonA | kButtonPlus;
    s.left = {32767, -100};
    in.set(0, s);
    CHECK(in.controller(0).style == ControllerStyle::None);  // not sampled yet
    in.poll();
    CHECK(in.controller(0).style == ControllerStyle::Handheld);
    CHECK_EQ(in.controller(0).buttons, kButtonA | kButtonPlus);
    CHECK_EQ(in.controller(0).left.x, 32767);
    CHECK(in.controller(5).style == ControllerStyle::None);
    in.set_vibration(1, 0.5f, 0.25f);
    CHECK(in.vibration(1).first == 0.5f && in.vibration(1).second == 0.25f);

    // Video without a decoder: one black NV12 picture per frame, nothing on flush.
    NullVideo video;
    std::unique_ptr<IVideoDecoder> dec = video.open_decoder({VideoCodec::Vp9, 65, 37, {}});
    CHECK(dec != nullptr);
    if (dec) {
        std::vector<VideoPicture> pics;
        const uint8_t frame[3] = {1, 2, 3};
        CHECK(dec->decode(frame, sizeof(frame), 33000, &pics));
        CHECK(dec->decode(nullptr, 0, 66000, &pics));
        CHECK_EQ(pics.size(), 1u);
        if (!pics.empty()) {
            CHECK_EQ(pics[0].width, 64u);  // even sizes for NV12
            CHECK_EQ(pics[0].height, 36u);
            CHECK_EQ(pics[0].pts_us, 33000);
            CHECK_EQ(pics[0].nv12.size(), 64u * 36 * 3 / 2);
            CHECK_EQ(pics[0].nv12.front(), 16u);  // black: Y 16, Cb/Cr 128
            CHECK_EQ(pics[0].nv12.back(), 128u);
        }
    }

    Platform p{&r, &audio, &in, nullptr, nullptr};
    CHECK(p.renderer->surface_size().height == 360u);
}

void test_cpu_affinity() {
    CHECK(parse_cpu_list("4-7") == (std::vector<int>{4, 5, 6, 7}));
    CHECK(parse_cpu_list("0,2,4-5") == (std::vector<int>{0, 2, 4, 5}));
    CHECK(parse_cpu_list("3,1-2") == (std::vector<int>{1, 2, 3}));
    for (const char* bad : {"", "big", "4-", "7-4", "1,,2", "1;2", "-1", "99999"})
        CHECK(parse_cpu_list(bad).empty());
    CHECK(format_cpu_list(std::vector<int>{0, 1, 2, 3, 5, 7, 8}) == "0-3,5,7-8");
    CHECK(format_cpu_list(std::vector<int>{}) == "-");
    // Snapdragon 855: four little, three big, one prime core.
    const std::vector<uint64_t> sd855{1785600, 1785600, 1785600, 1785600, 2419200, 2419200, 2419200, 2841600};
    CHECK(fastest_cpus(sd855) == (std::vector<int>{4, 5, 6, 7}));
    CHECK(fastest_cpus({1800000, 1800000}).empty());
    CHECK(fastest_cpus({0, 0}).empty());
    CHECK(fastest_cpus({0, 1000, 2000}) == (std::vector<int>{2}));

    // cpu_speeds on a fake sysfs tree.
    const fs::path root = fs::temp_directory_path() / ("lgp_cpu_speeds_" + std::to_string(std::rand()));
    auto put = [&](const std::string& rel, const std::string& text) {
        fs::create_directories((root / rel).parent_path());
        std::ofstream(root / rel) << text << "\n";
    };
    // Policies: 0-3 at 1.8 GHz, 4-6 at 2.4 GHz (cpu5/6 offline: no cpufreq dir of their own), 7 at 2.8 GHz.
    put("cpufreq/policy0/cpuinfo_max_freq", "1785600");
    put("cpufreq/policy0/related_cpus", "0 1 2 3");
    put("cpufreq/policy4/cpuinfo_max_freq", "2419200");
    put("cpufreq/policy4/related_cpus", "4 5 6 ");
    put("cpufreq/policy7/cpuinfo_max_freq", "2841600");
    put("cpufreq/policy7/related_cpus", "7");
    CHECK(fastest_cpus(cpu_speeds(root.string(), 8)) == (std::vector<int>{4, 5, 6, 7}));
    // Same top frequency on both clusters (Helio P60-like): cpu_capacity decides.
    const fs::path root2 = root / "same";
    for (int c = 0; c < 8; ++c) {
        fs::create_directories(root2 / ("cpu" + std::to_string(c)));
        std::ofstream(root2 / ("cpu" + std::to_string(c)) / "cpu_capacity") << (c < 4 ? "512" : "1024") << "\n";
    }
    fs::create_directories(root2 / "cpufreq/policy0");
    std::ofstream(root2 / "cpufreq/policy0/cpuinfo_max_freq") << "2000000\n";
    std::ofstream(root2 / "cpufreq/policy0/related_cpus") << "0-7\n";
    CHECK(fastest_cpus(cpu_speeds(root2.string(), 8)) == (std::vector<int>{4, 5, 6, 7}));
    CHECK(cpu_speeds((root / "missing").string(), 2) == (std::vector<uint64_t>{0, 0}));
    std::error_code ec;
    fs::remove_all(root, ec);

#if defined(__linux__)
    // Runs before any other HostThread: the policy is read once per process.
    cpu_set_t set;
    CPU_ZERO(&set);
    CHECK(sched_getaffinity(0, sizeof(set), &set) == 0);
    int first = -1;
    for (int c = 0; c < CPU_SETSIZE && first < 0; ++c)
        if (CPU_ISSET(c, &set))
            first = c;
    CHECK(first >= 0);
    CHECK(set_env("LGP_CPU_AFFINITY", std::to_string(first)));
    std::atomic<int> count{0}, cpu{-1};
    auto t = HostThread::start(1ull << 20, [&] {
        cpu_set_t mine;
        CPU_ZERO(&mine);
        if (sched_getaffinity(0, sizeof(mine), &mine) == 0) {
            count = CPU_COUNT(&mine);
            cpu = CPU_ISSET(first, &mine) ? first : -2;
        }
    });
    CHECK(t != nullptr);
    if (t)
        t->join();
    CHECK_EQ(count.load(), 1);
    CHECK_EQ(cpu.load(), first);
    CHECK(set_env("LGP_CPU_AFFINITY", ""));
#endif
}

void test_os() {
    // Host threads with an explicit stack size; deep recursion needs it.
    std::atomic<int> ran{0};
    {
        auto t = HostThread::start(4ull << 20, [&] {
            volatile char big[512 * 1024];
            big[0] = 1;
            big[sizeof(big) - 1] = 2;
            ran = big[0] + big[sizeof(big) - 1];
        });
        CHECK(t != nullptr);
        if (t)
            t->join();
    }
    CHECK_EQ(ran.load(), 3);

    // Committed memory reads zero, decommitted memory reads zero again.
    HostBlock block(1 << 20);
    block.commit_zeroed(0x1000, 0x2000);
    CHECK_EQ(block.base()[0x1000], 0u);
    block.base()[0x1234] = 0x5A;
    block.decommit(0x1000, 0x2000);
    block.commit_zeroed(0x1000, 0x2000);
    CHECK_EQ(block.base()[0x1234], 0u);
#if defined(__linux__) || defined(__APPLE__)
    // Residency counts written pages of the block (--time-stats "host memory").
    {
        HostBlock big(64u << 20);
        uint64_t before = 0, after = 0;
        CHECK(big.resident(&before));
        big.commit_zeroed(0, 32u << 20);
        for (uint64_t i = 0; i < (32u << 20); i += 4096)
            big.base()[i] = 1;
        CHECK(big.resident(&after));
        CHECK(after >= before + (32u << 20));
        big.decommit(0, 32u << 20);
        CHECK(big.resident(&after));
        CHECK(after <= before + (1u << 20));
    }
    // File backing (LGP_HOST_BLOCK_FILE): same contract, nothing left behind.
    {
        const std::filesystem::path dir = std::filesystem::temp_directory_path() / "lgp-host-block-test";
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        setenv("LGP_HOST_BLOCK_FILE", dir.c_str(), 1);
        {
            HostBlock fb(16u << 20);
            unsetenv("LGP_HOST_BLOCK_FILE");
            CHECK(fb.file_backed());
            CHECK(std::filesystem::is_empty(dir));  // the file is unlinked at once
            fb.commit_zeroed(0, 8u << 20);
            CHECK_EQ(fb.base()[0x1000], 0u);
            for (uint64_t i = 0; i < (8u << 20); i += 4096)
                fb.base()[i] = uint8_t(i >> 12);
            CHECK_EQ(fb.base()[0x3000], 3u);
            fb.decommit(0x2000, 0x4000);
            fb.commit_zeroed(0x2000, 0x4000);
            CHECK_EQ(fb.base()[0x3000], 0u);
            CHECK_EQ(fb.base()[0x1000], 1u);
            CHECK_EQ(fb.base()[0x6000], 6u);
            // Decommit of whole pages, then commit again: zero. The op
            // counters (--time-stats "host block ops") see both calls.
            using Op = HostBlock::Op;
            const HostBlock::OpTotals commit0 = HostBlock::op_totals(Op::Commit);
            const HostBlock::OpTotals release0 = HostBlock::op_totals(Op::Release);
            const uint64_t window0 = HostBlock::op_totals(Op::Punch).calls + HostBlock::op_totals(Op::Zero).calls;
            fb.decommit(0, 8u << 20);
            fb.commit_zeroed(0, 8u << 20);
            CHECK_EQ(fb.base()[0x1000], 0u);
            CHECK_EQ(fb.base()[(8u << 20) - 4096], 0u);
            CHECK_EQ(HostBlock::op_totals(Op::Commit).calls, commit0.calls + 1);
            CHECK_EQ(HostBlock::op_totals(Op::Commit).bytes, commit0.bytes + (8u << 20));
            CHECK_EQ(HostBlock::op_totals(Op::Release).calls, release0.calls + 1);
            CHECK_EQ(HostBlock::op_totals(Op::Release).bytes, release0.bytes + (8u << 20));
            CHECK(HostBlock::op_totals(Op::Punch).calls + HostBlock::op_totals(Op::Zero).calls > window0);
            CHECK(std::string(HostBlock::op_name(Op::Punch)) == "punch");
        }
        // LGP_HOST_BLOCK_PUNCH=0: released pages are zeroed, never punched,
        // and still read zero.
        setenv("LGP_HOST_BLOCK_FILE", dir.c_str(), 1);
        setenv("LGP_HOST_BLOCK_PUNCH", "0", 1);
        {
            HostBlock fb(16u << 20);
            unsetenv("LGP_HOST_BLOCK_FILE");
            unsetenv("LGP_HOST_BLOCK_PUNCH");
            CHECK(fb.file_backed());
            using Op = HostBlock::Op;
            fb.commit_zeroed(0, 4u << 20);
            for (uint64_t i = 0; i < (4u << 20); i += 4096)
                fb.base()[i] = 0x5A;
            const uint64_t punch0 = HostBlock::op_totals(Op::Punch).calls;
            const HostBlock::OpTotals zero0 = HostBlock::op_totals(Op::Zero);
            fb.decommit(0, 4u << 20);
            CHECK_EQ(HostBlock::op_totals(Op::Punch).calls, punch0);
            CHECK_EQ(HostBlock::op_totals(Op::Zero).calls, zero0.calls + 1);
            CHECK_EQ(HostBlock::op_totals(Op::Zero).bytes, zero0.bytes + (4u << 20));
            fb.commit_zeroed(0, 4u << 20);
            for (uint64_t i = 0; i < (4u << 20); i += 4096)
                CHECK_EQ(fb.base()[i], 0u);
        }
        // LGP_HOST_BLOCK_FILE_RANGE: only the window is file-backed, the
        // contract holds across its edges.
        setenv("LGP_HOST_BLOCK_FILE", dir.c_str(), 1);
        setenv("LGP_HOST_BLOCK_FILE_RANGE", "4:12", 1);
        {
            HostBlock fb(16u << 20);
            unsetenv("LGP_HOST_BLOCK_FILE");
            unsetenv("LGP_HOST_BLOCK_FILE_RANGE");
            CHECK(fb.file_backed());
            CHECK_EQ(fb.file_begin(), uint64_t{4} << 20);
            CHECK_EQ(fb.file_end(), uint64_t{12} << 20);
            fb.commit_zeroed(0, 16u << 20);
            for (uint64_t i = 0; i < (16u << 20); i += 4096)
                fb.base()[i] = uint8_t((i >> 12) | 1);
            std::vector<HostBlock::Region> regions;
            CHECK(fb.region_stats(4u << 20, &regions));
            CHECK_EQ(regions.size(), 4u);
            for (const HostBlock::Region& r : regions)
                CHECK(r.resident >= (2u << 20));
            // Across both edges of the window.
            fb.decommit((2u << 20), (12u << 20));
            CHECK_EQ(fb.base()[(1u << 20)], uint8_t(((1u << 20) >> 12) | 1));
            CHECK_EQ(fb.base()[(15u << 20)], uint8_t(((15u << 20) >> 12) | 1));
            fb.commit_zeroed((2u << 20), (12u << 20));
            for (uint64_t i = (2u << 20); i < (14u << 20); i += 4096)
                CHECK_EQ(fb.base()[i], 0u);
        }
        // extend_file_window (LGP_HOST_BLOCK_FILE_LATE): the range below the
        // window moves into the file with its contents and commit state.
        setenv("LGP_HOST_BLOCK_FILE", dir.c_str(), 1);
        setenv("LGP_HOST_BLOCK_FILE_RANGE", "8:", 1);
        {
            HostBlock fb(16u << 20);
            unsetenv("LGP_HOST_BLOCK_FILE");
            unsetenv("LGP_HOST_BLOCK_FILE_RANGE");
            CHECK_EQ(fb.file_begin(), uint64_t{8} << 20);
            // MiB 2-6 committed: 2-4 written, 4-6 left zero; 0-2 and 6-8 not committed.
            fb.commit_zeroed(2u << 20, 4u << 20);
            for (uint64_t i = (2u << 20); i < (4u << 20); i += 4096)
                fb.base()[i + 7] = uint8_t((i >> 12) | 1);
            fb.commit_zeroed(9u << 20, 1u << 20);
            fb.base()[9u << 20] = 0x42;
            HostBlock::MoveStats moved;
            std::string error;
            CHECK(fb.extend_file_window((1u << 20) + 5, &moved, &error));  // rounded down to MiB 1
            CHECK_EQ(fb.file_begin(), uint64_t{1} << 20);
            CHECK_EQ(moved.committed, uint64_t{4} << 20);
            CHECK_EQ(moved.written, uint64_t{2} << 20);
            for (uint64_t i = (2u << 20); i < (4u << 20); i += 4096)
                CHECK_EQ(fb.base()[i + 7], uint8_t((i >> 12) | 1));
            for (uint64_t i = (4u << 20); i < (6u << 20); i += 4096)
                CHECK_EQ(fb.base()[i], 0u);
            CHECK_EQ(fb.base()[9u << 20], 0x42u);
            // Still writable, and the window's release path takes over.
            fb.base()[5u << 20] = 9;
            CHECK_EQ(fb.base()[5u << 20], 9u);
            fb.decommit(2u << 20, 4u << 20);
            fb.commit_zeroed(2u << 20, 4u << 20);
            for (uint64_t i = (2u << 20); i < (6u << 20); i += 4096)
                CHECK_EQ(fb.base()[i + 7], 0u);
            // Not again over the same range; never without a file.
            CHECK(!fb.extend_file_window(2u << 20, &moved, &error));
            CHECK(!error.empty());
        }
        // Over several 16 MiB chunks.
        setenv("LGP_HOST_BLOCK_FILE", dir.c_str(), 1);
        setenv("LGP_HOST_BLOCK_FILE_RANGE", "40:", 1);
        {
            HostBlock fb(64u << 20);
            unsetenv("LGP_HOST_BLOCK_FILE");
            unsetenv("LGP_HOST_BLOCK_FILE_RANGE");
            fb.commit_zeroed(0, 40u << 20);
            for (uint64_t i = 0; i < (40u << 20); i += (1u << 20))
                fb.base()[i + 3] = uint8_t((i >> 20) | 0x80);
            HostBlock::MoveStats moved;
            std::string error;
            CHECK(fb.extend_file_window(4u << 20, &moved, &error));
            CHECK_EQ(fb.file_begin(), uint64_t{4} << 20);
            CHECK_EQ(moved.committed, uint64_t{36} << 20);
            for (uint64_t i = 0; i < (40u << 20); i += (1u << 20))
                CHECK_EQ(fb.base()[i + 3], uint8_t((i >> 20) | 0x80));
        }
        setenv("LGP_HOST_BLOCK_FILE", "0", 1);
        {
            HostBlock anon(4u << 20);
            HostBlock::MoveStats moved;
            std::string error;
            CHECK(!anon.extend_file_window(0, &moved, &error));
        }
        unsetenv("LGP_HOST_BLOCK_FILE");
        // An open end means the end of the block; an empty window falls back.
        setenv("LGP_HOST_BLOCK_FILE", dir.c_str(), 1);
        setenv("LGP_HOST_BLOCK_FILE_RANGE", "8:", 1);
        {
            HostBlock fb(16u << 20);
            CHECK_EQ(fb.file_begin(), uint64_t{8} << 20);
            CHECK_EQ(fb.file_end(), uint64_t{16} << 20);
        }
        setenv("LGP_HOST_BLOCK_FILE_RANGE", "20:30", 1);
        {
            HostBlock fb(16u << 20);
            CHECK(!fb.file_backed());
        }
        // Unreadable values keep the whole block in the file, rounded up to
        // whole host pages.
        for (const char* bad : {"-1:", "x:4", "4", "4:9x", "99999999999999999999:"}) {
            setenv("LGP_HOST_BLOCK_FILE_RANGE", bad, 1);
            HostBlock fb((16u << 20) + 100);
            CHECK(fb.file_backed());
            CHECK_EQ(fb.file_begin(), uint64_t{0});
            CHECK(fb.file_end() >= (uint64_t{16} << 20) + 100);
        }
        unsetenv("LGP_HOST_BLOCK_FILE");
        unsetenv("LGP_HOST_BLOCK_FILE_RANGE");
        CHECK(std::filesystem::is_empty(dir));
        // "0" turns it off (also where it is on by default: tvOS,
        // LGP_HOST_BLOCK_FILE_DEFAULT); a folder that does not exist falls back.
        setenv("LGP_HOST_BLOCK_FILE", "0", 1);
        HostBlock anon(1u << 20);
        unsetenv("LGP_HOST_BLOCK_FILE");
        CHECK(!anon.file_backed());
        setenv("LGP_HOST_BLOCK_FILE", (dir / "missing").c_str(), 1);
        HostBlock fallback(1u << 20);
        unsetenv("LGP_HOST_BLOCK_FILE");
        CHECK(!fallback.file_backed());
        fallback.commit_zeroed(0, 4096);
        fallback.base()[0] = 7;
        CHECK_EQ(fallback.base()[0], 7u);
        std::filesystem::remove_all(dir);
    }
#endif
    // ADR-0001: LGP aborts below ≈ 3040 MiB; the web value is 3072 MiB.
    CHECK(kDefaultTotalMemorySize >= 0xC000'0000ull);

    // What the OS counts against the process (--time-stats "host memory"):
    // touching 64 MiB shows up in the footprint.
#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32)
    ProcessMemory before;
    CHECK(process_memory(&before));
    CHECK(before.footprint > 0);
    // --time-stats "host cpu": the same touch shows up as page faults.
    ProcessCpu cpu_before;
    CHECK(process_cpu(&cpu_before));
    std::vector<char> touched(64u << 20);
    // Through volatile: an optimised build drops stores to a buffer nobody
    // reads, and calloc'ed pages stay untouched.
    volatile char* page = touched.data();
    for (size_t i = 0; i < touched.size(); i += 4096)
        page[i] = char((i >> 12) | 1);
    ProcessMemory after;
    CHECK(process_memory(&after));
    CHECK(after.footprint >= before.footprint + (48u << 20));
    CHECK(after.peak == 0 || after.peak >= after.footprint);
    ProcessCpu cpu_after;
    CHECK(process_cpu(&cpu_after));
    // 64 MiB: 4096 pages of 16 KiB, more of 4 KiB, but only 32 transparent
    // huge pages of 2 MiB (Linux CI).
    CHECK(cpu_after.faults >= cpu_before.faults + 16);
    CHECK(cpu_after.user_ns + cpu_after.system_ns > 0);
    CHECK(cpu_after.pageins >= cpu_before.pageins);
#endif
#if defined(__linux__) || defined(__APPLE__)
    SystemMemory sys;
    CHECK(system_memory(&sys));
    CHECK(sys.free > 0);
    CHECK(sys.level >= -1 && sys.level <= 100);
#endif
    // Where the footprint sits (Darwin, Linux): the touched 64 MiB are dirty
    // malloc or plain-mmap pages, so the region parts hold at least most of it.
    std::vector<MemoryPart> parts;
#if defined(__APPLE__)
    CHECK(memory_breakdown(&parts));
    CHECK(!parts.empty() && parts[0].name == "footprint" && parts[0].bytes > 0);
    uint64_t regions = 0;
    for (size_t i = 1; i < parts.size(); ++i)
        if (parts[i].name.rfind("ledger ", 0) != 0 && parts[i].name.rfind("file-backed", 0) != 0)
            regions += parts[i].bytes;
    CHECK(regions >= (48u << 20));
#elif defined(__linux__)
    // Linux/Android: PSS by mapping group; the 64 MiB vector is a malloc block
    // (glibc mmaps it unnamed, Bionic's scudo as [anon:scudo:secondary]), and
    // malloc reports it in use.
    CHECK(memory_breakdown(&parts));
    CHECK(!parts.empty() && parts[0].name == "pss" && parts[0].bytes >= (48u << 20));
    uint64_t heapish = 0, in_use = 0;
    for (const MemoryPart& p : parts) {
        if (p.name == "native heap" || p.name == "anon (unnamed)")
            heapish += p.bytes;
        if (p.name == "malloc in use")
            in_use = p.bytes;
    }
    CHECK(heapish >= (48u << 20));
#if defined(__BIONIC__) || (defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33)))
    CHECK(in_use >= (64u << 20));
#else
    (void)in_use;
#endif
#else
    CHECK(!memory_breakdown(&parts));
    CHECK(parts.empty());
#endif
}

}  // namespace

void test_thread_name() {
    // The name is cut to what Linux keeps (15 characters); elsewhere it only must not crash.
    std::string seen;
    auto t = HostThread::start(1 << 20, [&] {
        set_current_thread_name("lgp-name-longer-than-fifteen");
#if defined(__linux__)
        std::ifstream("/proc/thread-self/comm") >> seen;
#endif
    });
    CHECK(t != nullptr);
    t->join();
#if defined(__linux__)
    CHECK(seen == "lgp-name-longer");
#endif
}

// redirect_output: both streams land in the file, in the order written
// (stdout is line-buffered, stderr unbuffered). In a child process, which
// keeps this test's own output.
// closed_fd: descriptor closed before the redirect (-1: none), as in a
// process started without stdout or stderr.
void check_redirect_output(int closed_fd) {
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
    const fs::path file = fs::temp_directory_path() /
                          ("lgp_test_redirect_" + std::to_string(getpid()) + "_" + std::to_string(closed_fd) + ".log");
    std::fflush(stdout);
    std::fflush(stderr);
    const pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        if (closed_fd >= 0)
            close(closed_fd);
        std::string error;
        if (!redirect_output(file, &error))
            _exit(2);
        for (int i = 0; i < 200; ++i) {
            std::printf("out %d\n", i);
            std::fprintf(stderr, "err %d\n", i);
        }
        std::fflush(stdout);
        _exit(0);
    }
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    std::ifstream in(file);
    std::vector<std::string> lines;
    for (std::string line; std::getline(in, line);)
        lines.push_back(line);
#if defined(__linux__)
    // First the line naming what descriptors 1 and 2 were.
    CHECK(!lines.empty() && lines[0].rfind("log: descriptors before: 1 ", 0) == 0);
    if (!lines.empty())
        lines.erase(lines.begin());
#endif
    CHECK(lines.size() == 400);
    for (size_t i = 0; i < lines.size() && i < 400; ++i)
        CHECK(lines[i] == (i % 2 == 0 ? "out " : "err ") + std::to_string(i / 2));
    std::error_code ec;
    fs::remove(file, ec);
#endif
}

void test_redirect_output() {
    check_redirect_output(-1);
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
    check_redirect_output(STDOUT_FILENO);
    check_redirect_output(STDERR_FILENO);
#endif
}

int main() {
    RUN(test_cpu_affinity);
    RUN(test_thread_name);
    RUN(test_normalize_path);
    RUN(test_memory_storage);
    RUN(test_host_directory_storage);
    RUN(test_null_backends);
    RUN(test_os);
    RUN(test_redirect_output);
    return finish();
}
