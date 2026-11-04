// Homebrew test fixture "basic" (own code, no Nintendo code besides libnx/newlib).
// It exercises the constructs the recompiler has to handle early on: integer
// arithmetic and loops, a dense switch (jump table), indirect calls through a
// function-pointer table, floating point, a hand-written assembly leaf and a
// plain SVC. The result is deterministic; it becomes the exit code and is
// printed with svcOutputDebugString, so a run can be checked from outside.
#include <stdint.h>
#include <switch.h>

// No services: libnx's default __appInit connects to sm, applet, hid, time and
// fs. The fixtures test the recompiler and the kernel part of the runtime
// (P2.8); service-backed fixtures need the HLE services from P3.
void __appInit(void) {}
void __appExit(void) {}

// "<prefix>xxxxxxxx" via svcOutputDebugString, without newlib's printf.
static void debug_hex(const char* prefix, uint32_t v) {
    char buf[32];
    int n = 0;
    while (prefix[n] != '\0' && n < 16) {
        buf[n] = prefix[n];
        ++n;
    }
    for (int i = 7; i >= 0; --i)
        buf[n++] = "0123456789abcdef"[(v >> (i * 4)) & 0xF];
    svcOutputDebugString(buf, (u64)n);
}

uint64_t lgp_asm_mix(uint64_t a, uint64_t b);  // asm_ops.s

// Export a few symbols so the NRO/NSO carries a non-trivial dynamic symbol table.
__attribute__((visibility("default"), noinline)) uint32_t lgp_fib(uint32_t n) {
    uint32_t a = 0, b = 1;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t t = a + b;
        a = b;
        b = t;
    }
    return a;
}

__attribute__((visibility("default"), noinline)) uint32_t lgp_crc32(const uint8_t* p, uint32_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; ++i) {
        crc ^= p[i];
        for (int k = 0; k < 8; ++k)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

// Dense enough that GCC emits a jump table at -O2.
__attribute__((noinline)) int lgp_dispatch(int op, int x) {
    switch (op) {
    case 0: return x + 1;
    case 1: return x * 3;
    case 2: return x - 7;
    case 3: return x ^ 0x55;
    case 4: return x << 2;
    case 5: return x >> 1;
    case 6: return -x;
    case 7: return x & 0xF0;
    default: return 0;
    }
}

static int op_add(int a, int b) { return a + b; }
static int op_mul(int a, int b) { return a * b; }
static int op_sub(int a, int b) { return a - b; }

typedef int (*binop_t)(int, int);
static binop_t const g_ops[] = {op_add, op_mul, op_sub};

__attribute__((noinline)) double lgp_poly(double x) {
    return ((0.5 * x - 1.25) * x + 2.0) * x - 0.75;
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    static const uint8_t msg[] = "lgp-recomp fixture";
    uint32_t acc = lgp_fib(20) ^ lgp_crc32(msg, sizeof(msg) - 1);
    for (int i = 0; i < 8; ++i)
        acc += (uint32_t)lgp_dispatch(i, (int)(acc & 0xFF));
    volatile unsigned sel = 2;  // keep the indirect call indirect
    for (unsigned i = 0; i <= sel; ++i)
        acc += (uint32_t)g_ops[i]((int)(acc & 0xFFF), 3);
    acc += (uint32_t)(int)lgp_poly(3.5);
    acc ^= (uint32_t)lgp_asm_mix(acc, 0x1234);
    debug_hex("result ", acc);
    svcOutputDebugString("lgp fixture done", 16);
    return (int)(acc & 0x7F);
}
