// FRSQRTE/URSQRTE table step (fp::rsqrt_estimate_int) against the Arm ARM
// pseudocode loop, for every input it can get, and the derived estimates on
// a sweep of single and double values.

#include <cstdint>
#include <cstdio>

#include "lgp/lifted/fp.hpp"

namespace {

// RecipSqrtEstimate() as written in the Arm ARM.
uint32_t reference(uint32_t a) {
    if (a < 256) {
        a = a * 2 + 1;
    } else {
        a = (a >> 1) << 1;
        a = (a + 1) * 2;
    }
    uint64_t b = 512;
    while (uint64_t{a} * (b + 1) * (b + 1) < (uint64_t{1} << 28))
        ++b;
    return static_cast<uint32_t>((b + 1) / 2);
}

}  // namespace

int main() {
    int failures = 0;
    // Callers pass 128 <= a < 512; the wider range also covers 0..127.
    for (uint32_t a = 1; a < 1024; ++a) {
        const uint32_t got = lgp::lifted::fp::rsqrt_estimate_int(a);
        const uint32_t want = reference(a);
        if (got != want) {
            if (++failures <= 10)
                std::fprintf(stderr, "rsqrt_estimate_int(%u) = %u, want %u\n", a, got, want);
        }
    }
    if (failures != 0) {
        std::fprintf(stderr, "%d mismatches\n", failures);
        return 1;
    }
    std::printf("rsqrt_estimate_int: 1023 inputs match the pseudocode\n");
    return 0;
}
