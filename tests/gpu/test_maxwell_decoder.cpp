// Compares lgp::gpu::maxwell::disassemble() with envydis output for the
// vectors in tests/gpu/maxwell_vectors.txt (tools/shader/gen_maxwell_vectors.py).
// Agreement on every printed field shows the decoder reads the same bits as
// envytools' independent gm107 tables.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include "lgp/gpu/shader/maxwell.h"

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: test_maxwell_decoder <maxwell_vectors.txt>\n");
        return 2;
    }
    std::ifstream in(argv[1]);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 1;
    }
    std::string line;
    int total = 0, failures = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ls(line);
        std::string word_hex, addr_hex;
        ls >> word_hex >> addr_hex;
        std::string want;
        std::getline(ls >> std::ws, want);
        const uint64_t word = std::strtoull(word_hex.c_str(), nullptr, 16);
        const auto addr = static_cast<uint32_t>(std::strtoul(addr_hex.c_str(), nullptr, 16));
        const auto decoded = lgp::gpu::maxwell::decode(word);
        const std::string got = decoded ? lgp::gpu::maxwell::disassemble(*decoded, addr) : "<not decoded>";
        ++total;
        if (got != want) {
            if (++failures <= 20)
                std::printf("MISMATCH %s @%s\n  envydis: %s\n  ours:    %s\n", word_hex.c_str(), addr_hex.c_str(),
                            want.c_str(), got.c_str());
        }
    }
    std::printf("%d vectors, %d mismatches\n", total, failures);
    return (failures == 0 && total > 0) ? 0 : 1;
}
