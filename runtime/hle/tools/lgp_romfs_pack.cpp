// Packs an extracted RomFS directory into a RomFS image file (romfs.bin),
// the same image fsp-srv builds virtually (RomFsImage). Useful where a whole
// tree is awkward to move, e.g. the browser page, which takes one romfs.bin
// instead of tens of thousands of files.
//
//   lgp_romfs_pack [--compress[=LEVEL]] [--block-size KIB] [--threads N] <romfs dir | romfs.bin> <out romfs.bin>
//
// --compress writes a compressed image (zstd per block with a block index,
// romfs.hpp), which fsp-srv reads like a plain one; LEVEL is the zstd level
// (default 9). The input may also be an image file: a plain one is then
// compressed, a compressed one without --compress unpacked again.
//
// Input and output are the user's own game data: keep them in
// $LGP_PRIVATE_DIR.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "lgp/hle/romfs.hpp"
#include "lgp/platform/host_directory_storage.hpp"

namespace {

int usage(const char* argv0) {
    std::fprintf(stderr,
                 "usage: %s [--compress[=LEVEL]] [--block-size KIB] [--threads N] <romfs dir | romfs.bin> "
                 "<out romfs.bin>\n",
                 argv0);
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    bool compress = false;
    lgp::hle::CompressOptions options;
    std::vector<std::string> paths;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--compress") {
            compress = true;
        } else if (a.rfind("--compress=", 0) == 0) {
            compress = true;
            options.level = std::atoi(a.c_str() + 11);
            if (options.level < 1 || options.level > 22) {
                std::fprintf(stderr, "lgp_romfs_pack: level %s out of 1..22\n", a.c_str() + 11);
                return 2;
            }
        } else if (a == "--block-size" && i + 1 < argc) {
            const long kib = std::strtol(argv[++i], nullptr, 10);
            if (kib < 4 || kib > 16384 || (kib & (kib - 1)) != 0) {
                std::fprintf(stderr, "lgp_romfs_pack: block size %s KiB is not a power of two in 4..16384\n",
                             argv[i]);
                return 2;
            }
            options.block_size = uint32_t(kib) << 10;
        } else if (a == "--threads" && i + 1 < argc) {
            options.threads = unsigned(std::max(1L, std::strtol(argv[++i], nullptr, 10)));
        } else if (!a.empty() && a[0] == '-') {
            return usage(argv[0]);
        } else {
            paths.push_back(a);
        }
    }
    if (paths.size() != 2)
        return usage(argv[0]);
    const std::filesystem::path in_path = paths[0], out_path = paths[1];
    std::error_code ec;
    const bool from_image = std::filesystem::is_regular_file(in_path, ec);

    // The output must not lie inside the tree being packed (a rerun would
    // pack the previous image into the new one), nor be the input image.
    {
        std::error_code ec_root, ec_dest;
        auto root = std::filesystem::weakly_canonical(in_path, ec_root);
        const auto dest = std::filesystem::weakly_canonical(out_path, ec_dest);
        if (root.filename().empty()) root = root.parent_path();  // "dir/"
        const auto r = std::mismatch(root.begin(), root.end(), dest.begin(), dest.end()).first;
        if (!ec_root && !ec_dest && r == root.end()) {
            std::fprintf(stderr, "lgp_romfs_pack: the output %s is %s %s\n", out_path.string().c_str(),
                         from_image ? "the input" : "inside", in_path.string().c_str());
            return 2;
        }
    }

    // The same file under another name (a hard link, or other letter case on
    // Windows/macOS) passes the path check; truncating it would empty the input.
    if (from_image && std::filesystem::exists(out_path, ec) && std::filesystem::equivalent(in_path, out_path, ec)) {
        std::fprintf(stderr, "lgp_romfs_pack: the output %s is the input %s\n", out_path.string().c_str(),
                     in_path.string().c_str());
        return 2;
    }

    std::unique_ptr<lgp::platform::HostDirectoryStorage> fs;
    std::shared_ptr<lgp::hle::DataSource> image;
    size_t files = 0;
    if (from_image) {
        const auto parent = std::filesystem::absolute(in_path, ec).parent_path();
        fs = lgp::platform::HostDirectoryStorage::open_root(parent, true);
        std::unique_ptr<lgp::platform::IFile> f;
        if (!fs || fs->open("/" + in_path.filename().string(), lgp::platform::kOpenRead, &f) !=
                       lgp::platform::StorageError::Ok) {
            std::fprintf(stderr, "lgp_romfs_pack: cannot open %s\n", in_path.string().c_str());
            return 1;
        }
        std::string why;
        image = lgp::hle::open_image_source(std::move(f), &why);
        if (!image) {
            std::fprintf(stderr, "lgp_romfs_pack: %s: %s\n", in_path.string().c_str(), why.c_str());
            return 1;
        }
    } else {
        fs = lgp::platform::HostDirectoryStorage::open_root(in_path, true);
        if (!fs) {
            std::fprintf(stderr, "lgp_romfs_pack: cannot open %s\n", in_path.string().c_str());
            return 1;
        }
        lgp::platform::StorageError error{};
        auto built = lgp::hle::RomFsImage::build(*fs, "", &error);
        if (!built) {
            std::fprintf(stderr, "lgp_romfs_pack: cannot list %s (error %d)\n", in_path.string().c_str(), int(error));
            return 1;
        }
        files = built->file_count();
        image = std::move(built);
    }
    if (compress && !lgp::hle::compressed_images_supported()) {
        std::fprintf(stderr, "lgp_romfs_pack: --compress needs a build with zstd (LGP_ENABLE_ZSTD)\n");
        return 1;
    }

    std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::fprintf(stderr, "lgp_romfs_pack: cannot write %s\n", out_path.string().c_str());
        return 1;
    }
    uint64_t written = 0;
    auto write = [&](const void* data, size_t len) {
        written += len;
        return bool(out.write(static_cast<const char*>(data), std::streamsize(len)));
    };
    const auto t0 = std::chrono::steady_clock::now();
    if (compress) {
        std::string why;
        uint64_t shown = 0;
        auto progress = [&](uint64_t done) {
            if (done - shown >= (256ull << 20)) {
                std::fprintf(stderr, "%llu / %llu MiB\n", (unsigned long long)(done >> 20),
                             (unsigned long long)(image->size() >> 20));
                shown = done;
            }
        };
        if (!lgp::hle::write_compressed_image(*image, options, write, &why, progress)) {
            std::fprintf(stderr, "lgp_romfs_pack: %s\n", why.c_str());
            return 1;
        }
    } else {
        constexpr uint64_t kChunk = 8u << 20;
        std::vector<char> buf(kChunk);
        for (uint64_t at = 0; at < image->size(); at += kChunk) {
            const uint64_t n = std::min(kChunk, image->size() - at);
            if (!image->read(at, buf.data(), n) || !write(buf.data(), size_t(n))) {
                std::fprintf(stderr, "lgp_romfs_pack: read/write failed at 0x%llx\n", (unsigned long long)at);
                return 1;
            }
        }
    }
    out.close();
    if (!out) {
        std::fprintf(stderr, "lgp_romfs_pack: cannot finish writing %s\n", out_path.string().c_str());
        return 1;
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (!from_image)
        std::printf("%zu files, ", files);
    std::printf("%llu bytes", (unsigned long long)image->size());
    if (compress)
        std::printf(", compressed %llu bytes (%.1f %%, level %d, blocks of %u KiB)", (unsigned long long)written,
                    image->size() != 0 ? 100.0 * double(written) / double(image->size()) : 100.0, options.level,
                    options.block_size >> 10);
    std::printf(" in %.1f s\n", secs);
    return 0;
}
