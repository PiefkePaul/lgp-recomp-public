// Benchmark kernels for the lifted-code performance baseline (P6). Plain C++
// without a C library, compiled twice: for AArch64 (freestanding, recompiled
// with lgp_lift, the code under test) and for the host (the native reference
// for speed and for the expected result). Patterns typical of game code:
// table lookups, sorting, pointer chasing, a switch interpreter, virtual
// calls, recursion, float vector math, memory copies and string hashing.
//
// Entry: bench_main(kernel, iterations) -> checksum. Every kernel is its own
// function (noinline), as in a game: one huge function with all kernels inlined
// distorts register allocation, above all in WebAssembly engines. All FP code
// is built with -ffp-contract=off on both sides, so the checksums agree bit for
// bit.
#include <stddef.h>
#include <stdint.h>

#ifdef LGP_BENCH_GUEST
// The freestanding guest has no C library; the compiler may still emit calls.
extern "C" void* memcpy(void* dst, const void* src, size_t n) {
    auto* d = static_cast<unsigned char*>(dst);
    const auto* s = static_cast<const unsigned char*>(src);
    while (n--)
        *d++ = *s++;
    return dst;
}
extern "C" void* memset(void* dst, int c, size_t n) {
    auto* d = static_cast<unsigned char*>(dst);
    while (n--)
        *d++ = static_cast<unsigned char>(c);
    return dst;
}
#define BENCH_SQRTF(x) __builtin_sqrtf(x)
#define BENCH_NAMESPACE_BEGIN
#define BENCH_NAMESPACE_END
#else
#include <cmath>
#include <cstring>
#define BENCH_SQRTF(x) std::sqrt(x)
#define BENCH_NAMESPACE_BEGIN namespace lgp_bench_native {
#define BENCH_NAMESPACE_END }
#endif

BENCH_NAMESPACE_BEGIN

namespace {

struct Lcg {
    uint32_t s;
    uint32_t next() {
        s = s * 1664525u + 1013904223u;
        return s;
    }
};

// 1. CRC-32 with a 256-entry table over a 16 KiB buffer.
uint32_t g_crc_table[256];
uint8_t g_buf[16384];

__attribute__((noinline)) uint64_t k_crc(uint64_t iters) {
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        g_crc_table[i] = c;
    }
    Lcg r{1};
    for (auto& b : g_buf)
        b = static_cast<uint8_t>(r.next() >> 24);
    uint32_t crc = 0xFFFFFFFFu;
    for (uint64_t it = 0; it < iters; ++it)
        for (uint8_t b : g_buf)
            crc = g_crc_table[(crc ^ b) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

// 2. Recursive quicksort of 4096 ints.
int32_t g_sort[4096];

void quicksort(int32_t* a, int lo, int hi) {
    while (lo < hi) {
        const int32_t pivot = a[(lo + hi) / 2];
        int i = lo, j = hi;
        while (i <= j) {
            while (a[i] < pivot) ++i;
            while (a[j] > pivot) --j;
            if (i <= j) {
                const int32_t t = a[i];
                a[i] = a[j];
                a[j] = t;
                ++i;
                --j;
            }
        }
        if (j - lo < hi - i) {
            quicksort(a, lo, j);
            lo = i;
        } else {
            quicksort(a, i, hi);
            hi = j;
        }
    }
}

__attribute__((noinline)) uint64_t k_sort(uint64_t iters) {
    uint64_t sum = 0;
    Lcg r{7};
    for (uint64_t it = 0; it < iters; ++it) {
        for (auto& v : g_sort)
            v = static_cast<int32_t>(r.next());
        quicksort(g_sort, 0, 4095);
        sum += static_cast<uint32_t>(g_sort[it % 4096]) ^ static_cast<uint32_t>(g_sort[100]);
    }
    return sum;
}

// 3. Pointer chasing through a shuffled linked list of game-object-like nodes.
struct Node {
    Node* next;
    int32_t hp;
    int16_t x, y;
    uint8_t flags, kind;
};
Node g_nodes[8192];

__attribute__((noinline)) uint64_t k_list(uint64_t iters) {
    Lcg r{3};
    uint32_t perm[8192];
    for (uint32_t i = 0; i < 8192; ++i)
        perm[i] = i;
    for (uint32_t i = 8191; i > 0; --i) {
        const uint32_t j = r.next() % (i + 1);
        const uint32_t t = perm[i];
        perm[i] = perm[j];
        perm[j] = t;
    }
    for (uint32_t i = 0; i < 8192; ++i) {
        Node& n = g_nodes[perm[i]];
        n.next = i + 1 < 8192 ? &g_nodes[perm[i + 1]] : nullptr;
        n.hp = static_cast<int32_t>(r.next() % 1000);
        n.x = static_cast<int16_t>(r.next());
        n.y = static_cast<int16_t>(r.next());
        n.flags = static_cast<uint8_t>(r.next());
        n.kind = static_cast<uint8_t>(r.next() % 5);
    }
    uint64_t sum = 0;
    for (uint64_t it = 0; it < iters; ++it) {
        for (Node* n = &g_nodes[perm[0]]; n; n = n->next) {
            if (n->flags & 1) n->hp -= n->kind;
            if (n->hp < 0) n->hp += 1000;
            sum += static_cast<uint64_t>(n->hp) * static_cast<uint64_t>(n->x - n->y);
        }
    }
    return sum;
}

// 4. Stack-machine interpreter with a switch (jump table).
enum Op : uint8_t { kPush, kAdd, kSub, kMul, kDup, kSwap, kJnz, kLoad, kStore, kXor, kShr, kHalt };
const uint8_t kProgram[] = {
    kPush, 200, kStore, 0,                          // counter = 200
    kLoad, 2, kStore, 1,                            // acc = iteration
    kLoad, 1, kPush, 3, kMul, kPush, 7, kAdd,       // 8: acc = acc * 3 + 7
    kDup, kPush, 5, kShr, kXor, kStore, 1,          // acc ^= acc >> 5
    kLoad, 0, kPush, 1, kSub, kDup, kStore, 0,      // --counter
    kJnz, 8,                                        // loop
    kLoad, 1, kHalt};

__attribute__((noinline)) uint64_t k_interp(uint64_t iters) {
    uint64_t sum = 0;
    for (uint64_t it = 0; it < iters; ++it) {
        uint32_t stack[16];
        uint32_t regs[4] = {0, 0, static_cast<uint32_t>(it), 0};
        int sp = 0;
        uint32_t pc = 0;
        for (;;) {
            const uint8_t op = kProgram[pc++];
            switch (op) {
            case kPush: stack[sp++] = kProgram[pc++]; break;
            case kAdd: --sp; stack[sp - 1] += stack[sp]; break;
            case kSub: --sp; stack[sp - 1] -= stack[sp]; break;
            case kMul: --sp; stack[sp - 1] *= stack[sp]; break;
            case kDup: stack[sp] = stack[sp - 1]; ++sp; break;
            case kSwap: { const uint32_t t = stack[sp - 1]; stack[sp - 1] = stack[sp - 2]; stack[sp - 2] = t; break; }
            case kJnz: { const uint8_t target = kProgram[pc++]; if (stack[--sp] != 0) pc = target; break; }
            case kLoad: stack[sp++] = regs[kProgram[pc++] & 3]; break;
            case kStore: regs[kProgram[pc++] & 3] = stack[--sp]; break;
            case kXor: --sp; stack[sp - 1] ^= stack[sp]; break;
            case kShr: --sp; stack[sp - 1] >>= (stack[sp] & 31); break;
            default: goto done;
            }
            if (sp < 0 || sp > 15) goto done;
        }
    done:
        sum += sp > 0 ? stack[sp - 1] : 0;
    }
    return sum;
}

// 5. Virtual calls over heterogeneous objects.
struct Entity {
    float x = 0.0f, y = 0.0f, speed = 0.0f;
    virtual uint32_t update(uint32_t t) = 0;
};
struct Walker : Entity {
    uint32_t update(uint32_t t) override {
        x += speed;
        return static_cast<uint32_t>(x) + t;
    }
};
struct Flyer : Entity {
    uint32_t height = 3;
    uint32_t update(uint32_t t) override {
        y += speed * 0.5f;
        height ^= t;
        return height + static_cast<uint32_t>(y);
    }
};
struct Idle : Entity {
    uint32_t update(uint32_t t) override { return t & 7; }
};
struct Spinner : Entity {
    uint32_t angle = 0;
    uint32_t update(uint32_t t) override {
        angle = (angle + t * 13) % 360;
        return angle;
    }
};
Walker g_walkers[64];
Flyer g_flyers[64];
Idle g_idle[64];
Spinner g_spinners[64];
Entity* g_entities[256];

__attribute__((noinline)) uint64_t k_vcall(uint64_t iters) {
    Lcg r{11};
    for (int i = 0; i < 64; ++i) {
        g_walkers[i] = Walker{};
        g_flyers[i] = Flyer{};
        g_idle[i] = Idle{};
        g_spinners[i] = Spinner{};
    }
    for (int i = 0; i < 256; ++i) {
        Entity* e = nullptr;
        switch (r.next() % 4) {
        case 0: e = &g_walkers[i / 4]; break;
        case 1: e = &g_flyers[i / 4]; break;
        case 2: e = &g_idle[i / 4]; break;
        default: e = &g_spinners[i / 4]; break;
        }
        e->x = static_cast<float>(i);
        e->y = 0.0f;
        e->speed = static_cast<float>(r.next() % 100) * 0.01f;
        g_entities[i] = e;
    }
    uint64_t sum = 0;
    for (uint64_t it = 0; it < iters; ++it)
        for (Entity* e : g_entities)
            sum += e->update(static_cast<uint32_t>(it));
    return sum;
}

// 6. Recursion: many small calls.
__attribute__((noinline)) uint64_t fib(uint32_t n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

__attribute__((noinline)) uint64_t k_fib(uint64_t iters) {
    uint64_t sum = 0;
    for (uint64_t it = 0; it < iters; ++it)
        sum += fib(20 + static_cast<uint32_t>(it & 1));
    return sum;
}

// 7. 4x4 float matrix chains (scalar and NEON FP).
struct Mat4 {
    float m[16];
};
Mat4 g_mats[64];

void mul(const Mat4& a, const Mat4& b, Mat4& out) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k)
                s += a.m[r * 4 + k] * b.m[k * 4 + c];
            out.m[r * 4 + c] = s;
        }
}

uint32_t float_bits(float f) {
    uint32_t b;
    __builtin_memcpy(&b, &f, 4);
    return b;
}

__attribute__((noinline)) uint64_t k_mat4(uint64_t iters) {
    Lcg r{5};
    for (auto& m : g_mats)
        for (float& v : m.m)
            v = static_cast<float>(static_cast<int32_t>(r.next() % 2001) - 1000) * 0.001f;
    uint64_t sum = 0;
    Mat4 acc = g_mats[0], tmp;
    for (uint64_t it = 0; it < iters; ++it) {
        for (int i = 1; i < 64; ++i) {
            mul(acc, g_mats[i], tmp);
            // Renormalise so values stay finite.
            float n = 0.0f;
            for (float v : tmp.m)
                n += v * v;
            const float inv = 1.0f / (BENCH_SQRTF(n) + 1e-6f);
            for (int k = 0; k < 16; ++k)
                acc.m[k] = tmp.m[k] * inv;
        }
        sum += float_bits(acc.m[it % 16]);
    }
    return sum;
}

// 8. Particle integration with bounces (vectorisable loops).
constexpr int kParticles = 4096;
float g_px[kParticles], g_py[kParticles], g_vx[kParticles], g_vy[kParticles];

__attribute__((noinline)) uint64_t k_particles(uint64_t iters) {
    Lcg r{9};
    for (int i = 0; i < kParticles; ++i) {
        g_px[i] = static_cast<float>(r.next() % 1000);
        g_py[i] = static_cast<float>(r.next() % 1000);
        g_vx[i] = static_cast<float>(static_cast<int32_t>(r.next() % 200) - 100) * 0.1f;
        g_vy[i] = static_cast<float>(static_cast<int32_t>(r.next() % 200) - 100) * 0.1f;
    }
    const float dt = 0.016f, g = -9.81f, damping = 0.999f;
    for (uint64_t it = 0; it < iters; ++it) {
        for (int i = 0; i < kParticles; ++i) {
            g_vy[i] += g * dt;
            g_vx[i] *= damping;
            g_vy[i] *= damping;
            g_px[i] += g_vx[i] * dt;
            g_py[i] += g_vy[i] * dt;
        }
        for (int i = 0; i < kParticles; ++i) {
            if (g_py[i] < 0.0f) {
                g_py[i] = -g_py[i];
                g_vy[i] = -g_vy[i] * 0.8f;
            }
            if (g_px[i] < 0.0f || g_px[i] > 1000.0f) g_vx[i] = -g_vx[i];
        }
    }
    uint64_t sum = 0;
    for (int i = 0; i < kParticles; ++i)
        sum += float_bits(g_px[i]) ^ float_bits(g_py[i]);
    return sum;
}

// 9. Block copies and fills of 64 KiB (LDP/STP, the guest memcpy/memset).
uint64_t g_src[8192], g_dst[8192];

__attribute__((noinline)) uint64_t k_memops(uint64_t iters) {
    Lcg r{13};
    for (auto& v : g_src)
        v = (uint64_t{r.next()} << 32) | r.next();
    uint64_t sum = 0;
    for (uint64_t it = 0; it < iters; ++it) {
        memcpy(g_dst, g_src, sizeof(g_src));
        memset(g_src + (it % 64) * 128, static_cast<int>(it), 1024);
        for (int i = 0; i < 8192; i += 64)
            sum += g_dst[i];
    }
    return sum;
}

// 10. String hashing and comparison (byte loads, branches).
char g_strings[512][24];

uint32_t fnv1a(const char* s) {
    uint32_t h = 2166136261u;
    while (*s)
        h = (h ^ static_cast<uint8_t>(*s++)) * 16777619u;
    return h;
}

int compare(const char* a, const char* b) {
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return static_cast<uint8_t>(*a) - static_cast<uint8_t>(*b);
}

__attribute__((noinline)) uint64_t k_strings(uint64_t iters) {
    Lcg r{17};
    for (auto& s : g_strings) {
        const uint32_t len = 4 + r.next() % 19;
        for (uint32_t i = 0; i < len; ++i)
            s[i] = static_cast<char>('a' + r.next() % 6);
        s[len] = 0;
    }
    uint64_t sum = 0;
    for (uint64_t it = 0; it < iters; ++it)
        for (int i = 0; i < 512; ++i) {
            sum += fnv1a(g_strings[i]);
            sum += static_cast<uint64_t>(compare(g_strings[i], g_strings[(i + it) % 512]) + 256);
        }
    return sum;
}

}  // namespace

#ifdef LGP_BENCH_GUEST
extern "C"
#endif
    uint64_t
    bench_main(uint64_t kernel, uint64_t iters) {
    switch (kernel) {
    case 0: return k_crc(iters);
    case 1: return k_sort(iters);
    case 2: return k_list(iters);
    case 3: return k_interp(iters);
    case 4: return k_vcall(iters);
    case 5: return k_fib(iters);
    case 6: return k_mat4(iters);
    case 7: return k_particles(iters);
    case 8: return k_memops(iters);
    case 9: return k_strings(iters);
    default: return 0;
    }
}

BENCH_NAMESPACE_END
