// ASTC decoder (gpu/texture) against vectors from ARM's reference codec
// (tests/gpu/texture/astc_vectors.txt, tools/texture/gen_astc_vectors.py),
// plus hand-built blocks with known results.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "lgp/gpu/texture/astc.hpp"

using lgp::gpu::texture::decode_astc_block;
using lgp::gpu::texture::decode_astc_image;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

uint64_t fnv1a64(const uint8_t* p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

bool parse_hex(const std::string& s, uint8_t* out, size_t n) {
    if (s.size() != n * 2)
        return false;
    for (size_t i = 0; i < n; ++i)
        out[i] = uint8_t(std::stoul(s.substr(i * 2, 2), nullptr, 16));
    return true;
}

// Void extent with an RGBA16 colour: every texel is its top byte.
void test_void_extent() {
    uint8_t blk[16] = {0xFC, 0xFD, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    const uint16_t rgba[4] = {0x1234, 0xABCD, 0x00FF, 0xFF00};
    std::memcpy(blk + 8, rgba, 8);
    uint8_t out[6 * 6 * 4];
    check(decode_astc_block(blk, 6, 6, false, out, 6 * 4), "void extent decodes");
    check(out[0] == 0x12 && out[1] == 0xAB && out[2] == 0x00 && out[3] == 0xFF, "void extent colour");
    check(std::memcmp(out, out + (6 * 6 - 1) * 4, 4) == 0, "void extent is constant");
    blk[1] |= 0x02;  // HDR flag: not in the LDR profile
    check(!decode_astc_block(blk, 6, 6, false, out, 6 * 4), "HDR void extent is an error");
    check(out[0] == 0xFF && out[1] == 0x00 && out[2] == 0xFF && out[3] == 0xFF, "error colour is magenta");
}

// A reserved block mode (low four bits 0) gives the error colour.
void test_reserved_mode() {
    uint8_t blk[16] = {};
    uint8_t out[4 * 4 * 4];
    check(!decode_astc_block(blk, 4, 4, false, out, 16), "all-zero block is reserved");
    check(out[0] == 0xFF && out[1] == 0x00 && out[2] == 0xFF, "reserved block is magenta");
}

// Image path: partial edge blocks are cropped, error blocks counted.
void test_image_crop() {
    std::vector<uint8_t> blocks(2 * 2 * 16, 0);  // 2x2 blocks of 5x5, all reserved
    uint8_t ve[16] = {0xFC, 0xFD, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0x80, 0, 0x40, 0, 0x20, 0, 0x10};
    std::memcpy(blocks.data(), ve, 16);  // block (0, 0) valid
    std::vector<uint8_t> img(7 * 9 * 4, 0xAA);
    const size_t errors = decode_astc_image(blocks.data(), 7, 9, 5, 5, false, img.data());
    check(errors == 3, "three error blocks");
    check(img[0] == 0x80 && img[1] == 0x40 && img[2] == 0x20 && img[3] == 0x10, "texel (0,0) from void extent");
    const uint8_t* last = &img[(8 * 7 + 6) * 4];
    check(last[0] == 0xFF && last[1] == 0x00, "texel (6,8) from an error block");
}

int run_vectors(const char* path) {
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", path);
        return 1;
    }
    std::string line;
    size_t total = 0, failed = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream ls(line);
        unsigned bw, bh, srgb;
        std::string block_hex, hash_hex;
        ls >> bw >> bh >> srgb >> block_hex >> hash_hex;
        uint8_t blk[16];
        if (!ls || !parse_hex(block_hex, blk, 16)) {
            std::fprintf(stderr, "bad line: %s\n", line.c_str());
            return 1;
        }
        std::vector<uint8_t> out(size_t(bw) * bh * 4);
        decode_astc_block(blk, bw, bh, srgb != 0, out.data(), bw * 4);
        const unsigned long long want = std::stoull(hash_hex, nullptr, 16);
        ++total;
        if (fnv1a64(out.data(), out.size()) != want) {
            if (++failed <= 20)
                std::fprintf(stderr, "mismatch %ux%u srgb=%u block %s\n", bw, bh, srgb, block_hex.c_str());
        }
    }
    std::printf("astc vectors: %zu/%zu match\n", total - failed, total);
    if (total < 1000) {
        std::fprintf(stderr, "too few vectors (%zu)\n", total);
        return 1;
    }
    return failed == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    test_void_extent();
    test_reserved_mode();
    test_image_crop();
    int rc = g_failures ? 1 : 0;
    if (argc > 1)
        rc |= run_vectors(argv[1]);
    if (g_failures)
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
    return rc;
}
