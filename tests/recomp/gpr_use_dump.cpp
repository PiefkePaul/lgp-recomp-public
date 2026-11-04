// Reads instruction words (hex, whitespace separated) and prints per word:
// "<word> <valid 0|1> <reads hex> <writes hex>" (lgp::recomp::gpr_use).
// With --rename: reads "<word> <to>" pairs and prints "<word> <ok 0|1> <new word>"
// (lgp::recomp::rename_gpr from x18 to x<to>).
// Driver for test_gpr_use_vs_llvm.py and test_rename_vs_llvm.py.
#include <cstdio>
#include <cstring>

#include "lgp/recomp/a64.hpp"
#include "lgp/recomp/native_census.hpp"
#include "lgp/recomp/native_rewrite.hpp"

int main(int argc, char** argv) {
    unsigned word = 0, to = 0;
    if (argc > 1 && std::strcmp(argv[1], "--rename") == 0) {
        while (std::scanf("%x %u", &word, &to) == 2) {
            const auto nw = lgp::recomp::rename_gpr(word, 0x100000, 18, to);
            std::printf("%08x %d %08x\n", word, nw ? 1 : 0, nw ? *nw : 0u);
        }
        return 0;
    }
    while (std::scanf("%x", &word) == 1) {
        const auto inst = lgp::recomp::a64::decode(word, 0x100000);
        const auto use = lgp::recomp::gpr_use(inst);
        std::printf("%08x %d %08x %08x\n", word, inst.op == lgp::recomp::a64::Op::Invalid ? 0 : 1, use.reads, use.writes);
    }
    return 0;
}
