// Memory probe as a command-line program (desktop, Android via adb shell):
//   lgp_memprobe [--guest MIB] [--chunk MIB] [--max MIB] [--zero] [--pause MS] [--log FILE]
//                [--file DIR [--file-sync]]   (guest block backed by a file in DIR)
//                [--touch MIB]                (first-touch timing, memprobe.hpp 5.)
//                [--native [--native-gib GIB]] (code at a fixed guest address, memprobe.hpp 6.)
// Android: adb push lgp_memprobe /data/local/tmp/ && adb shell /data/local/tmp/lgp_memprobe --log /data/local/tmp/memprobe.log
// (a shell process, not an app: the low-memory killer may treat apps
// differently; the APK probe of P7.3b answers that).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#include "memprobe.hpp"

int main(int argc, char** argv) {
    lgp::memprobe::Options o;
    std::string log_path;
    bool native = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto num = [&]() -> uint64_t {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", a.c_str());
                std::exit(2);
            }
            return std::strtoull(argv[++i], nullptr, 10);
        };
        if (a == "--guest") o.guest_mib = num();
        else if (a == "--chunk") o.chunk_mib = num();
        else if (a == "--max") o.max_mib = num();
        else if (a == "--pause") o.pause_ms = unsigned(num());
        else if (a == "--zero") o.zero_fill = true;
        else if (a == "--file" && i + 1 < argc) o.file_dir = argv[++i];
        else if (a == "--file-sync") o.file_sync = true;
        else if (a == "--touch") o.touch_mib = num();
        else if (a == "--native") native = true;
        else if (a == "--native-gib") o.native_gib = num();
        else if (a == "--log" && i + 1 < argc) log_path = argv[++i];
        else {
            std::fprintf(stderr,
                         "usage: lgp_memprobe [--guest MIB] [--chunk MIB] [--max MIB] [--zero] [--pause MS] [--log FILE]"
                         " [--file DIR [--file-sync]] [--touch MIB] [--native [--native-gib GIB]]\n");
            return 2;
        }
    }
    if (o.chunk_mib == 0 || o.guest_mib == 0) {
        std::fprintf(stderr, "lgp_memprobe: --guest and --chunk must be at least 1\n");
        return 2;
    }
    std::ofstream log;
    if (!log_path.empty())
        log.open(log_path, std::ios::trunc);
    if (!log_path.empty() && !log.is_open()) {
        std::fprintf(stderr, "lgp_memprobe: cannot write %s\n", log_path.c_str());
        return 2;
    }
    const auto out = [&](const std::string& line) {
        std::printf("%s\n", line.c_str());
        std::fflush(stdout);
        if (log.is_open())
            log << line << std::endl;  // flushed: the last line survives a kill
    };
    if (native)
        lgp::memprobe::run_native_probe(o, out);
    else if (o.touch_mib > 0)
        lgp::memprobe::run_touch(o, out);
    else
        lgp::memprobe::run(o, out);
    return 0;
}
