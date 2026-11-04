// CPU BCn decoding speed (gpu/texture/bc.hpp): reference against fast decoder
// on random blocks (every BC7 mode equally often), RGBA8 texels per second.
// Only meaningful in a Release build; --smoke (ctest) runs a small image and
// checks that both decoders write the same bytes.
//
//   lgp_bc_bench [--size N] [--seconds S] [--smoke]
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "lgp/gpu/texture/bc.hpp"

using namespace lgp::gpu::texture;

namespace {

struct Format {
    const char* name;
    BcFormat format;
};
constexpr Format kFormats[] = {{"BC1", BcFormat::BC1}, {"BC2", BcFormat::BC2}, {"BC3", BcFormat::BC3},
                               {"BC4", BcFormat::BC4}, {"BC5", BcFormat::BC5}, {"BC7", BcFormat::BC7}};

std::vector<uint8_t> random_blocks(BcFormat f, size_t blocks) {
    uint64_t s = 0x2545f4914f6cdd1dull;
    std::vector<uint8_t> data(blocks * bc_block_bytes(f));
    for (size_t i = 0; i < data.size(); i += 8) {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        std::memcpy(data.data() + i, &s, 8);
    }
    if (f == BcFormat::BC7)
        for (size_t b = 0; b < blocks; ++b) {
            uint8_t& first = data[b * 16];
            const uint32_t mode = uint32_t(b % 8);
            first = uint8_t((first & ~((2u << mode) - 1)) | 1u << mode);
        }
    return data;
}

// Texels per second of decode_bc_image_with over at least `seconds`.
double rate(BcDecoder d, BcFormat f, const std::vector<uint8_t>& data, uint32_t size, double seconds,
            std::vector<uint8_t>* out) {
    using clock = std::chrono::steady_clock;
    uint64_t texels = 0;
    const auto t0 = clock::now();
    double elapsed = 0;
    do {
        decode_bc_image_with(d, f, data.data(), size, size, out->data());
        texels += uint64_t(size) * size;
        elapsed = std::chrono::duration<double>(clock::now() - t0).count();
    } while (elapsed < seconds);
    return double(texels) / elapsed;
}

}  // namespace

int main(int argc, char** argv) {
    uint32_t size = 1024;
    double seconds = 1.0;
    bool smoke = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--size" && i + 1 < argc)
            size = uint32_t(std::atoi(argv[++i]));
        else if (a == "--seconds" && i + 1 < argc)
            seconds = std::atof(argv[++i]);
        else if (a == "--smoke")
            smoke = true;
        else {
            std::fprintf(stderr, "usage: %s [--size N] [--seconds S] [--smoke]\n", argv[0]);
            return 2;
        }
    }
    if (smoke) {
        size = 64;
        seconds = 0.01;
    }
    size = (size + 3) / 4 * 4;
    int failures = 0;
    std::printf("%-5s %14s %14s %8s   (%ux%u, Mtexel/s)\n", "", "reference", "fast", "speedup", size, size);
    for (const Format& fmt : kFormats) {
        const auto data = random_blocks(fmt.format, size_t(size / 4) * (size / 4));
        std::vector<uint8_t> a(size_t(size) * size * 4), b(a.size());
        const double r = rate(BcDecoder::Reference, fmt.format, data, size, seconds, &a);
        const double q = rate(BcDecoder::Fast, fmt.format, data, size, seconds, &b);
        const bool same = a == b;
        failures += same ? 0 : 1;
        std::printf("%-5s %14.1f %14.1f %7.2fx%s\n", fmt.name, r / 1e6, q / 1e6, q / r, same ? "" : "  DIFFERENT BYTES");
    }
    return failures == 0 ? 0 : 1;
}
