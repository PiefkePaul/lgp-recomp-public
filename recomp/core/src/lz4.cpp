// Raw LZ4 block decoder (NSO segments are LZ4 blocks without a frame header).
#include <cstring>

#include "lgp/recomp/loader.hpp"

namespace lgp::recomp {

std::vector<uint8_t> lz4_block_decompress(std::span<const uint8_t> src, size_t out_size) {
    std::vector<uint8_t> dst;
    dst.reserve(out_size);
    size_t i = 0, n = src.size();
    auto need = [&](size_t k) {
        if (i + k > n) throw FormatError("lz4: truncated input");
    };
    while (i < n) {
        need(1);
        uint8_t token = src[i++];
        size_t lit = token >> 4;
        if (lit == 15) {
            for (;;) {
                need(1);
                uint8_t b = src[i++];
                lit += b;
                if (b != 255) break;
            }
        }
        need(lit);
        if (dst.size() + lit > out_size) throw FormatError("lz4: output exceeds expected size");
        dst.insert(dst.end(), src.begin() + static_cast<std::ptrdiff_t>(i),
                   src.begin() + static_cast<std::ptrdiff_t>(i + lit));
        i += lit;
        if (i >= n) break;  // last sequence: literals only
        need(2);
        size_t off = src[i] | (static_cast<size_t>(src[i + 1]) << 8);
        i += 2;
        if (off == 0 || off > dst.size()) throw FormatError("lz4: bad match offset");
        size_t mlen = token & 15;
        if (mlen == 15) {
            for (;;) {
                need(1);
                uint8_t b = src[i++];
                mlen += b;
                if (b != 255) break;
            }
        }
        mlen += 4;
        if (dst.size() + mlen > out_size) throw FormatError("lz4: output exceeds expected size");
        size_t start = dst.size() - off;
        for (size_t k = 0; k < mlen; ++k) dst.push_back(dst[start + k]);  // handles overlap
    }
    if (dst.size() != out_size) throw FormatError("lz4: decoded size mismatch");
    return dst;
}

}  // namespace lgp::recomp
