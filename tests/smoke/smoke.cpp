// Toolchain smoke test: verifies that the configured C++20 compiler produces a
// runnable binary. Replaced by real tests as components land.
#include <bit>
#include <cstdint>

int main() {
    // The guest (AArch64) and every planned host (x86-64, arm64, wasm) are
    // little-endian; the runtime will rely on this.
    static_assert(std::endian::native == std::endian::little, "host must be little-endian");
#if defined(__EMSCRIPTEN__)
    // wasm32 build (ADR-0001 default): host pointers are 32 bits wide.
    static_assert(sizeof(void*) == 4, "expected wasm32");
#endif
    constexpr std::uint64_t value = 0x8000000000000001ull;
    return std::popcount(value) == 2 && std::countl_zero(value >> 1) == 1 ? 0 : 1;
}
