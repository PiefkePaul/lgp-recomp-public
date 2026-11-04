// Homebrew test fixture "threads" (own code, no Nintendo code besides libnx/newlib).
// Multi-threading the way libnx does it: threadCreate (heap stack mapped with
// svcMapMemory), mutexes and condition variables over the kernel's arbiter
// (svcArbitrateLock/Unlock, svcWaitProcessWideKeyAtomic/SignalProcessWideKey),
// thread-local variables through TPIDRRO_EL0, malloc from the newlib heap
// (svcSetHeapSize), thread priorities and svcSleepThread(0) as yield.
//
// All threads run on the default core, so the scheduler only switches at
// SVCs and the run is deterministic (ADR-0002 core baton). Results go to
// svcOutputDebugString.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

// No services (see basic/main.c).
void __appInit(void) {}
void __appExit(void) {}

static void debug_hex(const char* prefix, uint32_t v) {
    char buf[40];
    int n = 0;
    while (prefix[n] != '\0' && n < 24) {
        buf[n] = prefix[n];
        ++n;
    }
    for (int i = 7; i >= 0; --i)
        buf[n++] = "0123456789abcdef"[(v >> (i * 4)) & 0xF];
    svcOutputDebugString(buf, (u64)n);
}

// --- bounded queue: two producers, two consumers -----------------------------

enum { kQueueSize = 8, kItemsPerProducer = 300, kProducers = 2, kConsumers = 2 };

static Mutex g_lock;
static CondVar g_not_empty;
static CondVar g_not_full;
static uint32_t g_queue[kQueueSize];
static uint32_t g_head, g_count;
static uint32_t g_producers_left = kProducers;
static uint32_t g_consumed_sum, g_consumed_count;
static uint32_t g_order_hash = 2166136261u;  // FNV-1a over the consumption order

static __thread uint32_t t_local_sum;  // per-thread, through the TLS block

static void producer(void* arg) {
    const uint32_t base = (uint32_t)(uintptr_t)arg;
    for (uint32_t i = 1; i <= kItemsPerProducer; ++i) {
        mutexLock(&g_lock);
        while (g_count == kQueueSize)
            condvarWait(&g_not_full, &g_lock);
        g_queue[(g_head + g_count) % kQueueSize] = base + i;
        ++g_count;
        condvarWakeOne(&g_not_empty);
        mutexUnlock(&g_lock);
        if ((i & 63) == 0)
            svcSleepThread(0);  // yield to the other threads of the same priority
    }
    mutexLock(&g_lock);
    --g_producers_left;
    condvarWakeAll(&g_not_empty);
    mutexUnlock(&g_lock);
}

static void consumer(void* arg) {
    uint32_t* out = (uint32_t*)arg;  // malloc'ed by main
    t_local_sum = 0;
    for (;;) {
        mutexLock(&g_lock);
        while (g_count == 0 && g_producers_left != 0)
            condvarWait(&g_not_empty, &g_lock);
        if (g_count == 0) {
            mutexUnlock(&g_lock);
            break;
        }
        const uint32_t v = g_queue[g_head];
        g_head = (g_head + 1) % kQueueSize;
        --g_count;
        g_consumed_sum += v;
        ++g_consumed_count;
        g_order_hash = (g_order_hash ^ v) * 16777619u;
        condvarWakeOne(&g_not_full);
        mutexUnlock(&g_lock);
        t_local_sum += v;
    }
    *out = t_local_sum;
}

// --- priorities: a higher-priority thread preempts at threadStart ------------

static volatile uint32_t g_prio_log[4];
static volatile uint32_t g_prio_n;

static void prio_worker(void* arg) { g_prio_log[g_prio_n++] = (uint32_t)(uintptr_t)arg; }

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    mutexInit(&g_lock);
    condvarInit(&g_not_empty);
    condvarInit(&g_not_full);

    Thread threads[kProducers + kConsumers];
    uint32_t* sums = (uint32_t*)malloc(kConsumers * sizeof(uint32_t));
    memset(sums, 0, kConsumers * sizeof(uint32_t));
    int n = 0;
    for (int i = 0; i < kProducers; ++i, ++n)
        if (R_FAILED(threadCreate(&threads[n], producer, (void*)(uintptr_t)(i * 1000), NULL, 0x10000, 0x2C, -2)))
            debug_hex("threadCreate failed ", (uint32_t)n);
    for (int i = 0; i < kConsumers; ++i, ++n)
        if (R_FAILED(threadCreate(&threads[n], consumer, &sums[i], NULL, 0x10000, 0x2C, -2)))
            debug_hex("threadCreate failed ", (uint32_t)n);
    for (int i = 0; i < n; ++i)
        threadStart(&threads[i]);
    for (int i = 0; i < n; ++i) {
        threadWaitForExit(&threads[i]);
        threadClose(&threads[i]);
    }
    debug_hex("consumed ", g_consumed_count);
    debug_hex("sum ", g_consumed_sum);
    debug_hex("order ", g_order_hash);
    debug_hex("consumer0 ", sums[0]);
    debug_hex("consumer1 ", sums[1]);

    // Main runs at 0x2C. A worker at 0x20 (higher) runs at once, one at 0x30
    // (lower) only when main blocks.
    Thread hi, lo;
    threadCreate(&lo, prio_worker, (void*)(uintptr_t)0x30, NULL, 0x4000, 0x30, -2);
    threadCreate(&hi, prio_worker, (void*)(uintptr_t)0x20, NULL, 0x4000, 0x20, -2);
    threadStart(&lo);
    g_prio_log[g_prio_n++] = 1;
    threadStart(&hi);
    g_prio_log[g_prio_n++] = 2;
    threadWaitForExit(&lo);
    threadWaitForExit(&hi);
    threadClose(&lo);
    threadClose(&hi);
    const uint32_t prio = (g_prio_log[0] << 24) | (g_prio_log[1] << 16) | (g_prio_log[2] << 8) | g_prio_log[3];
    debug_hex("prio ", prio);

    // Expected by construction (independent of the interleaving): every item
    // consumed once, sum of 1..300 and 1001..1300, and the priority order above.
    const uint32_t expected_sum = 2 * (300 * 301 / 2) + 300 * 1000;
    const int ok = g_consumed_count == kProducers * kItemsPerProducer && g_consumed_sum == expected_sum &&
                   sums[0] + sums[1] == expected_sum && prio == 0x01200230u;
    svcOutputDebugString(ok ? "checks passed" : "checks FAILED", 13);

    free(sums);
    svcOutputDebugString("lgp fixture done", 16);
    return 0;
}
