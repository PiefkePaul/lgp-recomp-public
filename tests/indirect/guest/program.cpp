// P2.5 test program: indirect control flow of the kinds ADR-0004 handles.
// Built freestanding for AArch64 with the host clang (tests/indirect), run
// once under Unicorn and once recompiled; both runs must end identically.
//
//   mode 0: function pointers, virtual calls, switch jump tables, indirect and
//           direct tail calls (deep chain), setjmp/longjmp
//   mode 1: call into the middle of a function (unresolved without config)
#include <stdint.h>

#define NOINLINE __attribute__((noinline))

extern "C" {
int setjmp(uint64_t* env);
[[noreturn]] void longjmp(uint64_t* env, int value);
int two_entries(int x);
void __cxa_pure_virtual() { __builtin_trap(); }
uint64_t guest_main(uint64_t mode);

uint64_t results[64];
uint32_t result_count;
}

// Keeps the compiler from folding values it would otherwise know.
static volatile int g_opaque[4] = {0, 1, 2, 3};

static void put(uint64_t v) { results[result_count++ & 63] = v; }

// ---- function pointers (BLR through a relocated table in .data.rel.ro) ------
typedef int (*BinOp)(int, int);
NOINLINE int op_add(int a, int b) { return a + b; }
NOINLINE int op_sub(int a, int b) { return a - b; }
NOINLINE int op_mul(int a, int b) { return a * b; }
NOINLINE int op_xor(int a, int b) { return a ^ (b << 3); }
NOINLINE int op_mix(int a, int b) { return (a << 2) - (b >> 1) + 17; }
static BinOp const kOps[] = {op_add, op_sub, op_mul, op_xor, op_mix};

NOINLINE int fold(const BinOp* ops, int n, int seed) {
    int acc = seed;
    for (int i = 0; i < 3 * n; ++i)
        acc = ops[(i * 7 + g_opaque[i & 3]) % n](acc, i + 1);
    return acc;
}

// BR to a loaded pointer in tail position: an indirect tail call.
NOINLINE int apply(BinOp f, int a, int b) { return f(a, b); }

// ---- virtual calls --------------------------------------------------------------
struct Shape {
    virtual int area() const = 0;
    virtual int scaled(int k) const { return area() * k; }
    virtual int kind() const { return 0; }
};
struct Rect : Shape {
    int w, h;
    Rect(int w_, int h_) : w(w_), h(h_) {}
    int area() const override { return w * h; }
    int kind() const override { return 1; }
};
struct Square : Rect {
    explicit Square(int s) : Rect(s, s) {}
    int scaled(int k) const override { return Rect::scaled(k) + 5; }
    int kind() const override { return 2; }
};
struct Circle : Shape {
    int r;
    explicit Circle(int r_) : r(r_) {}
    int area() const override { return 3 * r * r; }
    int scaled(int k) const override { return area() * k * 2 - 1; }
};

NOINLINE int total(Shape* const* shapes, int n, int k) {
    int sum = 0;
    for (int i = 0; i < n; ++i)
        sum = sum * 3 + shapes[i]->scaled(k) + shapes[i]->kind() * 100;
    return sum;
}

// ---- switch (jump table) ------------------------------------------------------
NOINLINE int helper(int x) { return x * 5 + 3; }

NOINLINE int dispatch(int sel, int y) {
    switch (sel) {
    case 0: return helper(y + 1);
    case 1: return y * 3 + 2;
    case 2: return helper(y - 7) * 4;
    case 3: return y ^ 0x55;
    case 4: return helper(y << 2);
    case 5: return y / 3;
    case 6: return helper(100);
    case 7: return y | 8;
    case 8: return helper(y) - 9;
    case 9: return 77;
    case 10: return helper(y ^ 3);
    case 11: return y * y;
    case 12: return helper(y + 12) + 1;
    case 13: return y - 13;
    case 14: return helper(-y);
    case 15: return y & 0xF0;
    case 16: return helper(y * 7);
    case 17: return y >> 2;
    case 18: return helper(y) ^ 18;
    case 19: return 19 - y;
    default: return -1;
    }
}

// ---- tail-call chain: three states that call each other through a table --------
typedef uint64_t (*State)(uint64_t n, uint64_t acc);
NOINLINE uint64_t state_a(uint64_t n, uint64_t acc);
NOINLINE uint64_t state_b(uint64_t n, uint64_t acc);
NOINLINE uint64_t state_c(uint64_t n, uint64_t acc);
static State const kStates[] = {state_a, state_b, state_c};

uint64_t state_a(uint64_t n, uint64_t acc) {
    if (n == 0) return acc;
    return kStates[(acc >> 7) % 3](n - 1, acc * 3 + 1);
}
uint64_t state_b(uint64_t n, uint64_t acc) {
    if (n == 0) return acc ^ 0xB;
    return kStates[(acc >> 5) % 3](n - 1, acc + 0x9E3779B97F4A7C15ull);
}
uint64_t state_c(uint64_t n, uint64_t acc) {
    if (n == 0) return acc + 0xC;
    return kStates[(acc >> 11) % 3](n - 1, (acc << 7) ^ (acc >> 3));
}

// ---- setjmp/longjmp ----------------------------------------------------------------
NOINLINE void unwind_from(uint64_t* env, int depth, int value) {
    if (depth == 0) longjmp(env, value);
    unwind_from(env, depth - 1, value);
    put(0xDEAD);  // never reached
}

// a, b and c are set before setjmp and not changed afterwards, so they stay
// valid across longjmp; the compiler keeps them in callee-saved registers,
// which the deep recursion of unwind_from overwrites before it jumps back.
NOINLINE int jump_test(int depth, int value) {
    uint64_t env[22];
    volatile int stage = 0;
    const int a = g_opaque[1] * 7 + depth;
    const int b = g_opaque[2] * 11 + value;
    const int c = g_opaque[3] * a + b;
    int r = setjmp(env);
    if (r == 0) {
        stage = 1;
        unwind_from(env, depth, value + a);
        return -1;
    }
    return r * 100 + stage + a * 3 + b * 5 + c * 7;
}

// ---- mode 1: second entry of an assembler function ----------------------------------
NOINLINE int call_second_entry(int x) {
    int (*fp)(int) = reinterpret_cast<int (*)(int)>(reinterpret_cast<uintptr_t>(&two_entries) + 4 +
                                                     static_cast<uintptr_t>(g_opaque[0]));
    return fp(x) + 1;
}

uint64_t guest_main(uint64_t mode) {
    result_count = 0;
    if (mode == 1) {
        put(static_cast<uint64_t>(call_second_entry(5)));
        return results[0];
    }
    put(static_cast<uint64_t>(fold(kOps, 5, 11)));
    for (int i = 0; i < 5; ++i)
        put(static_cast<uint64_t>(apply(kOps[(i + g_opaque[1]) % 5], 1000 + i, 7)));

    Rect r(3, 4);
    Square s(5);
    Circle c(2);
    Shape* shapes[] = {&r, &s, &c, &s, &r};
    put(static_cast<uint64_t>(total(shapes, 5, 3)));

    uint64_t sw = 0;
    for (int sel = -2; sel < 22; ++sel)
        sw = sw * 31 + static_cast<uint32_t>(dispatch(sel, 1234 + sel));
    put(sw);

    put(state_a(LGP_STATE_DEPTH, 1));
    put(static_cast<uint64_t>(jump_test(10, 42)));
    put(static_cast<uint64_t>(jump_test(3, 0)));  // longjmp(env, 0) makes setjmp return 1

    uint64_t sum = 0;
    for (uint32_t i = 0; i < result_count; ++i)
        sum = sum * 1099511628211ull + results[i];
    return sum;
}
