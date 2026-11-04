// Self-test program for the function-level differential test (ADR-0006
// stage 3): own leaf and near-leaf functions over integers, memory, FP and
// NEON, built freestanding for AArch64 (ARMv8.0 + CRC, like the A57).
// lgp_fn_select picks them up from the NRO like functions of a game module;
// the runner calls each one with random register and memory states.
#include <arm_acle.h>
#include <arm_neon.h>
#include <stddef.h>
#include <stdint.h>

#define FN __attribute__((noinline, used, visibility("default")))

// Keeps array lengths sane for random inputs.
static inline size_t cap(size_t n, size_t max) { return n > max ? max : n; }

FN uint64_t mix64(uint64_t a, uint64_t b, unsigned r) {
    uint64_t x = a ^ (b * 0x9E3779B97F4A7C15ull);
    x = (x << (r & 63)) | (x >> ((64 - r) & 63));
    return x * 0xBF58476D1CE4E5B9ull + (b >> 7);
}

FN uint32_t fnv1a(const uint8_t* p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < cap(n, 256); ++i) h = (h ^ p[i]) * 16777619u;
    return h;
}

FN uint32_t crc32_bitwise(const uint8_t* p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < cap(n, 64); ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

FN uint32_t crc32_hw(const uint8_t* p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    size_t i = 0;
    n = cap(n, 512);
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        __builtin_memcpy(&w, p + i, 8);
        c = __crc32cd(c, w);
    }
    for (; i < n; ++i) c = __crc32b(c, p[i]);
    return ~c;
}

FN void insertion_sort(int32_t* a, size_t n) {
    n = cap(n, 48);
    for (size_t i = 1; i < n; ++i) {
        int32_t v = a[i];
        size_t j = i;
        while (j > 0 && a[j - 1] > v) {
            a[j] = a[j - 1];
            --j;
        }
        a[j] = v;
    }
}

FN long binary_search(const int32_t* a, size_t n, int32_t key) {
    size_t lo = 0, hi = cap(n, 4096);
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (a[mid] < key) lo = mid + 1;
        else hi = mid;
    }
    return lo < cap(n, 4096) && a[lo] == key ? (long)lo : -1;
}

FN int64_t divisions(int64_t a, int64_t b, uint32_t c, uint32_t d) {
    // Division by zero gives 0 on AArch64; INT64_MIN / -1 wraps.
    return a / b + (int64_t)(c / d) - a % (b | 1) + (int64_t)(c % (d | 1));
}

FN uint64_t mulhi_and_carry(uint64_t a, uint64_t b, uint64_t c) {
    unsigned __int128 p = (unsigned __int128)a * b;
    unsigned __int128 s = p + c;
    __int128 sp = (__int128)(int64_t)a * (int64_t)b;
    return (uint64_t)(s >> 64) ^ (uint64_t)s ^ (uint64_t)(sp >> 64);
}

FN uint64_t bit_tricks(uint64_t x, uint32_t y) {
    uint64_t r = (uint64_t)__builtin_clzll(x | 1) + (uint64_t)__builtin_ctzll(x | (1ull << 63));
    r += (uint64_t)__builtin_popcountll(x) + __builtin_bswap64(x) + __builtin_bswap32(y) + __builtin_bitreverse32(y);
    r ^= (uint64_t)__builtin_clrsbll((int64_t)x) << 32;
    return r + ((x >> 3) & 0x1F) * ((y << 5) | 3);
}

FN int clamp_and_select(int a, int b, int c, int d) {
    int r = a < b ? (c > d ? c : d) : (a == b ? -c : b);
    if (a > 0 && b > 0 && (c < 0 || d < 0)) r += 7;
    return r < -1000 ? -1000 : (r > 1000 ? 1000 : r);
}

FN int jump_table(unsigned op, int a, int b) {
    switch (op & 15) {
    case 0: return a + b;
    case 1: return a - b;
    case 2: return a * b;
    case 3: return a & b;
    case 4: return a | b;
    case 5: return a ^ b;
    case 6: return a << (b & 31);
    case 7: return a >> (b & 31);
    case 8: return (int)((unsigned)a >> (b & 31));
    case 9: return a < b;
    case 10: return -a;
    case 11: return ~b;
    default: return 42 + (int)op;
    }
}

FN double fp_scalar(double a, double b, float c, float d) {
    double r = a * b + (double)(c / d);
    r += __builtin_sqrt(__builtin_fabs(a)) + __builtin_fma(a, b, (double)c);
    r += __builtin_round(b) - __builtin_floor(a) + __builtin_ceil((double)d) + __builtin_trunc(a * 0.5);
    r += __builtin_fmin(a, b) - __builtin_fmax((double)c, (double)d);
    return r;
}

FN int64_t fp_convert(double a, float b, int64_t c, uint32_t d) {
    int64_t r = (int64_t)a + (int64_t)(int32_t)b + (int64_t)(uint32_t)(b * 3.0f);
    r += (int64_t)((double)c * 0.25) + (int64_t)(float)d;
    r += (int64_t)__builtin_lround(a) + (int64_t)__builtin_lrintf(b);
    return r ^ (int64_t)(uint64_t)(a * 1e10);
}

FN float dot(const float* a, const float* b, size_t n) {
    float s = 0;
    for (size_t i = 0; i < cap(n, 64); ++i) s += a[i] * b[i];
    return s;
}

FN void mat4_mul(float* out, const float* a, const float* b) {
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a[i * 4 + k] * b[k * 4 + j];
            out[i * 4 + j] = s;
        }
}

typedef struct {
    float x, y, z;
} Vec3;

FN float vec3_normalize(Vec3* v) {
    float len = __builtin_sqrtf(v->x * v->x + v->y * v->y + v->z * v->z);
    if (len > 1e-6f) {
        v->x /= len;
        v->y /= len;
        v->z /= len;
    }
    return len;
}

FN void neon_saturate(int16_t* out, const int16_t* a, const int16_t* b) {
    int16x8_t va = vld1q_s16(a), vb = vld1q_s16(b);
    int16x8_t s = vqaddq_s16(va, vb);
    int16x8_t d = vqsubq_s16(va, vb);
    int32x4_t m = vmull_s16(vget_low_s16(va), vget_high_s16(vb));
    vst1q_s16(out, vzip1q_s16(s, d));
    vst1q_s32((int32_t*)(out + 8), m);
    vst1q_s16(out + 16, vrshrq_n_s16(vabdq_s16(va, vb), 3));
}

FN uint32_t neon_bytes(const uint8_t* p) {
    uint8x16_t v = vld1q_u8(p);
    uint8x16_t t = vqtbl1q_u8(v, vandq_u8(vrev64q_u8(v), vdupq_n_u8(15)));
    uint8x16_t c = vcntq_u8(veorq_u8(v, t));
    uint16x8_t w = vpaddlq_u8(c);
    return vaddvq_u16(w) + vmaxvq_u8(t) + (vminvq_u8(v) << 8);
}

FN float neon_float(const float* p) {
    float32x4_t a = vld1q_f32(p), b = vld1q_f32(p + 4);
    float32x4_t r = vfmaq_f32(vmulq_f32(a, b), a, vrsqrteq_f32(vabsq_f32(b)));
    r = vbslq_f32(vcgtq_f32(a, b), r, vnegq_f32(r));
    int32x4_t i = vcvtq_s32_f32(r);
    return vaddvq_f32(r) + (float)vaddvq_s32(i) + vmaxnmvq_f32(a);
}

FN int bounded_strcmp(const char* a, const char* b) {
    for (int i = 0; i < 64; ++i) {
        unsigned char x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (x != y) return x < y ? -1 : 1;
        if (x == 0) return 0;
    }
    return 0;
}

FN void copy_and_fill(uint8_t* dst, const uint8_t* src, size_t n, uint8_t v) {
    n = cap(n, 200);
    for (size_t i = 0; i < n; ++i) dst[i] = src[i] ^ v;
    for (size_t i = n; i < n + 17; ++i) dst[i] = v;
}

FN unsigned fib(unsigned n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

FN unsigned fib_bounded(unsigned n) { return fib(n & 15); }

FN uint64_t atomic_bump(uint64_t* p, uint64_t v) {
    uint64_t old = __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST);
    uint32_t prev = __atomic_exchange_n((uint32_t*)(p + 1), (uint32_t)v, __ATOMIC_ACQ_REL);
    uint64_t expected = old + 1;
    __atomic_compare_exchange_n(p + 2, &expected, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return old + prev + expected;
}

FN uint32_t chained(const uint8_t* p, size_t n, uint64_t k) {
    return fnv1a(p, n) ^ crc32_bitwise(p + 3, n) ^ (uint32_t)mix64(k, n, 13);
}

static uint32_t g_counter;
static uint8_t g_table[64];

FN uint32_t touch_globals(uint32_t v) {
    g_counter += v;
    g_table[v & 63] = (uint8_t)(g_counter >> 3);
    return g_counter ^ g_table[(v >> 6) & 63];
}

FN uint64_t struct_by_value(Vec3 a, Vec3 b, double s) {
    Vec3 c = {a.x * b.y - a.y * b.x, a.y * b.z - a.z * b.y, (float)(s * a.z)};
    uint32_t u[3];
    __builtin_memcpy(u, &c, sizeof(u));
    return ((uint64_t)u[0] << 32 | u[1]) ^ u[2];
}

FN long many_args(long a, long b, long c, long d, long e, long f, long g, long h, long i, long j) {
    return a - b + c * d - e + f * g - h + i * 3 - j;  // i and j come from the stack
}

int guest_main(void) { return 0; }
