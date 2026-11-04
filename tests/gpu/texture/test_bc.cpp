// BCn decoder (gpu/texture/bc.hpp) against vectors from Pillow's decoder
// (tests/gpu/texture/bc_vectors.txt, tools/texture/gen_bc_vectors.py), plus
// hand-built snorm and image cases, each with both decoders; the fast decoder
// against the reference on random blocks and images (same bytes).
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <random>
#include <utility>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "lgp/gpu/texture/bc.hpp"
#include "lgp/gpu/texture/parallel.hpp"

using namespace lgp::gpu::texture;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

bool parse_hex(const std::string& s, std::vector<uint8_t>* out) {
    if (s.size() % 2 != 0)
        return false;
    out->resize(s.size() / 2);
    for (size_t i = 0; i < out->size(); ++i)
        (*out)[i] = uint8_t(std::stoul(s.substr(2 * i, 2), nullptr, 16));
    return true;
}

bool format_by_name(const std::string& name, BcFormat* f) {
    static const struct {
        const char* name;
        BcFormat format;
    } kNames[] = {{"BC1", BcFormat::BC1}, {"BC2", BcFormat::BC2}, {"BC3", BcFormat::BC3},
                  {"BC4", BcFormat::BC4}, {"BC5", BcFormat::BC5}, {"BC7", BcFormat::BC7}};
    for (const auto& n : kNames)
        if (name == n.name) {
            *f = n.format;
            return true;
        }
    return false;
}

void test_vectors(const char* path) {
    std::ifstream in(path);
    check(bool(in), "vector file opens");
    std::string line;
    int count = 0, wrong = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream ls(line);
        std::string name, block_hex, texels_hex;
        ls >> name >> block_hex >> texels_hex;
        BcFormat f{};
        std::vector<uint8_t> block, want;
        if (!format_by_name(name, &f) || !parse_hex(block_hex, &block) || !parse_hex(texels_hex, &want) ||
            block.size() != bc_block_bytes(f) || want.size() != 64) {
            check(false, "vector line parses");
            continue;
        }
        uint8_t got[64];
        decode_bc_block(f, block.data(), got, 16);
        ++count;
        if (std::memcmp(got, want.data(), 64) != 0) {
            if (++wrong <= 10)
                std::fprintf(stderr, "mismatch: %s %s\n", name.c_str(), block_hex.c_str());
        }
    }
    std::printf("bc vectors: %d blocks, %d wrong\n", count, wrong);
    check(count > 1000, "enough vectors");
    check(wrong == 0, "all vectors decode as the reference");
}

void test_snorm() {
    // BC4 snorm: endpoints 127 and -128 (read as -127), all texels index 1
    // (= second endpoint), except texel 0 with index 0.
    uint8_t block[8] = {0x7f, 0x80, 0, 0, 0, 0, 0, 0};
    uint64_t idx = 0;
    for (int i = 1; i < 16; ++i)
        idx |= uint64_t(1) << (3 * i);
    for (int i = 0; i < 6; ++i)
        block[2 + i] = uint8_t(idx >> (8 * i));
    uint8_t out[64];
    decode_bc_block(BcFormat::BC4Snorm, block, out, 16);
    check(int8_t(out[0]) == 127 && int8_t(out[4]) == -127, "BC4 snorm endpoints");
    check(out[1] == 0 && out[3] == 127, "BC4 snorm fills g = 0, a = 127");
    // Six-value mode (a0 <= a1): index 6 and 7 are -127 and 127.
    uint8_t six[16] = {0x00, 0x10};  // red half; the green half stays zero
    idx = uint64_t(6) | uint64_t(7) << 3;
    for (int i = 0; i < 6; ++i)
        six[2 + i] = uint8_t(idx >> (8 * i));
    decode_bc_block(BcFormat::BC5Snorm, six, out, 16);
    check(int8_t(out[0]) == -127 && int8_t(out[4]) == 127, "BC5 snorm six-value extremes");
    check(out[1] == 0 && out[2] == 0 && out[3] == 127, "BC5 snorm green 0, blue 0, alpha 127");
}

void test_image_edges() {
    // A 6x5 image (2x2 blocks, partial right/bottom blocks) of solid BC1
    // blocks with different colours: every texel takes its block's colour.
    const uint16_t colours[4] = {0xf800, 0x07e0, 0x001f, 0xffff};
    std::vector<uint8_t> data;
    for (uint16_t c : colours) {
        const uint8_t b[8] = {uint8_t(c), uint8_t(c >> 8), uint8_t(c), uint8_t(c >> 8), 0, 0, 0, 0};
        data.insert(data.end(), b, b + 8);
    }
    std::vector<uint8_t> out(6 * 5 * 4);
    decode_bc_image(BcFormat::BC1, data.data(), 6, 5, out.data());
    auto texel = [&](int x, int y) { return out.data() + (y * 6 + x) * 4; };
    check(texel(0, 0)[0] == 255 && texel(0, 0)[1] == 0, "block (0,0) red");
    check(texel(5, 0)[1] == 255 && texel(5, 0)[0] == 0, "block (1,0) green");
    check(texel(3, 4)[2] == 255 && texel(3, 4)[0] == 0, "block (0,1) blue");
    check(texel(5, 4)[0] == 255 && texel(5, 4)[1] == 255, "block (1,1) white");
}

// xorshift64: the same blocks on every run.
struct Rng {
    uint64_t s = 0x9e3779b97f4a7c15ull;
    uint64_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
};

constexpr BcFormat kAllFormats[] = {BcFormat::BC1, BcFormat::BC2,     BcFormat::BC3, BcFormat::BC4,
                                    BcFormat::BC4Snorm, BcFormat::BC5, BcFormat::BC5Snorm, BcFormat::BC7};

void random_block(Rng& rng, BcFormat f, uint32_t n, uint8_t* block) {
    for (size_t i = 0; i < bc_block_bytes(f); i += 8) {
        const uint64_t v = rng.next();
        std::memcpy(block + i, &v, 8);
    }
    if (f == BcFormat::BC7) {
        // Every mode equally often (random bytes would be half mode 0), and the
        // invalid mode byte 0 now and then.
        const uint32_t mode = n % 9;
        block[0] = mode == 8 ? 0 : uint8_t((block[0] & ~((2u << mode) - 1)) | 1u << mode);
    } else if (n % 4 == 0 && f != BcFormat::BC2) {
        // Equal endpoints and the other endpoint order also come up.
        const size_t at = f == BcFormat::BC3 ? 8 : 0;
        if (f == BcFormat::BC1 || f == BcFormat::BC3) {
            block[at + 2] = block[at + 0];
            block[at + 3] = block[at + 1];
        } else {
            block[1] = block[0];
        }
    }
}

void test_fast_matches_reference() {
    Rng rng;
    for (BcFormat f : kAllFormats) {
        int wrong = 0;
        for (uint32_t n = 0; n < 200000; ++n) {
            uint8_t block[16], a[64], b[64];
            random_block(rng, f, n, block);
            decode_bc_block_with(BcDecoder::Reference, f, block, a, 16);
            decode_bc_block_with(BcDecoder::Fast, f, block, b, 16);
            if (std::memcmp(a, b, 64) != 0 && ++wrong <= 5)
                std::fprintf(stderr, "fast != reference: format %d block %u\n", int(f), n);
        }
        check(wrong == 0, "fast decoder matches the reference on random blocks");
        // Images with partial edge blocks and wider strides.
        for (const auto& [w, h] : {std::pair{37u, 23u}, std::pair{64u, 64u}, std::pair{3u, 2u}, std::pair{130u, 5u}}) {
            const uint32_t bx = (w + 3) / 4, by = (h + 3) / 4;
            std::vector<uint8_t> data(size_t(bx) * by * bc_block_bytes(f));
            for (size_t i = 0; i < data.size(); i += bc_block_bytes(f))
                random_block(rng, f, uint32_t(i), data.data() + i);
            std::vector<uint8_t> a(size_t(w) * h * 4, 0xcd), b(size_t(w) * h * 4, 0xab);
            decode_bc_image_with(BcDecoder::Reference, f, data.data(), w, h, a.data());
            decode_bc_image_with(BcDecoder::Fast, f, data.data(), w, h, b.data());
            check(a == b, "fast image matches the reference image");
        }
    }
    std::printf("fast decoder: %zu formats x 200000 random blocks and 4 images compared\n", std::size(kAllFormats));
}

// The pool's split (DecodePool, LGP_WEBGPU_DECODE_THREADS) gives the same
// texels as one pass, for random blocks of every format and sizes with
// partial blocks, small images (caller only), many ranges and a narrow,
// tall image (13×40001: a few ranges of whole block rows).
void test_parallel() {
    std::mt19937 rng(1234);
    DecodePool pool(3);
    const BcFormat formats[] = {BcFormat::BC1, BcFormat::BC2, BcFormat::BC3, BcFormat::BC4,
                                BcFormat::BC4Snorm, BcFormat::BC5, BcFormat::BC5Snorm, BcFormat::BC7};
    const uint32_t sizes[][2] = {{6, 5}, {1024, 1023}, {517, 2050}, {4096, 64}, {13, 40001}};
    for (BcFormat f : formats)
        for (const auto& wh : sizes) {
            const uint32_t w = wh[0], h = wh[1];
            std::vector<uint8_t> data(size_t((w + 3) / 4) * ((h + 3) / 4) * bc_block_bytes(f));
            for (uint8_t& b : data) b = uint8_t(rng());
            std::vector<uint8_t> one(size_t(w) * h * 4, 0xcd), split(size_t(w) * h * 4, 0xab);
            decode_bc_image(f, data.data(), w, h, one.data());
            decode_bc_image(f, data.data(), w, h, split.data(), &pool);
            check(one == split, "parallel decode equals one pass");
        }
    // Many small jobs in a row: no job is lost or run twice.
    for (int i = 0; i < 200; ++i) {
        std::vector<int> hits(1000, 0);
        pool.run(hits.size(), 7, [&](size_t b, size_t e) {
            for (size_t k = b; k < e; ++k) ++hits[k];
        });
        bool once = true;
        for (int v : hits) once &= v == 1;
        check(once, "every range runs once");
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s bc_vectors.txt\n", argv[0]);
        return 2;
    }
    for (BcDecoder d : {BcDecoder::Reference, BcDecoder::Fast}) {
        set_bc_decoder(d);
        std::printf("decoder: %s\n", d == BcDecoder::Fast ? "fast" : "reference");
        test_vectors(argv[1]);
        test_snorm();
        test_image_edges();
    }
    set_bc_decoder(BcDecoder::Reference);
    test_fast_matches_reference();
    set_bc_decoder(BcDecoder::Fast);
    test_parallel();
    if (g_failures == 0)
        std::printf("all BCn checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
