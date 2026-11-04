#include "lgp/runtime/system.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "lgp/runtime/clock.hpp"
#include "lgp/runtime/kernel.hpp"
#include "lgp/runtime/log.hpp"

namespace lgp::rt {
namespace {

constexpr uint32_t sysreg(uint32_t op0, uint32_t op1, uint32_t crn, uint32_t crm, uint32_t op2) {
    return (op0 << 14) | (op1 << 11) | (crn << 7) | (crm << 3) | op2;
}
constexpr uint32_t kCntfrqEl0 = sysreg(3, 3, 14, 0, 0);
constexpr uint32_t kCntpctEl0 = sysreg(3, 3, 14, 0, 1);
constexpr uint32_t kCntvctEl0 = sysreg(3, 3, 14, 0, 2);
constexpr uint32_t kCtrEl0 = sysreg(3, 3, 0, 0, 1);
constexpr uint32_t kDczidEl0 = sysreg(3, 3, 0, 0, 7);

// Cortex-A57 values: 64-byte cache lines, DC ZVA zeroes 64 bytes.
constexpr uint64_t kCtrValue = 0x8444C004;
constexpr uint64_t kDczidValue = 4;

Kernel* g_kernel = nullptr;
std::atomic<IndirectResolver> g_resolver{nullptr};

}  // namespace

void set_active_kernel(Kernel* kernel) { g_kernel = kernel; }

Kernel& active_kernel() {
    if (g_kernel == nullptr) {
        std::fprintf(stderr, "lgp runtime: svc without an active kernel\n");
        std::abort();
    }
    return *g_kernel;
}

uint64_t system_tick() {
    const Kernel* k = g_kernel;
    const uint64_t n = k != nullptr && k->clock().is_virtual() ? k->clock().now_ns() : host_clock_ns();
    // 19.2 MHz = 12/625 ticks per ns; split to avoid overflow for long runs.
    return (n / 625) * 12 + (n % 625) * 12 / 625;
}

uint64_t guest_counter_read() {
    Kernel* k = g_kernel;
    if (k != nullptr && k->clock().is_virtual() && Scheduler::current() != nullptr) {
        k->clock().charge(kVirtualCounterReadCostNs);
        k->scheduler().on_clock_advanced();
    }
    return system_tick();
}

uint64_t read_sysreg(uint32_t encoding) {
    switch (encoding) {
    case kCntfrqEl0: return kCounterFrequency;
    case kCntpctEl0:
    case kCntvctEl0: return guest_counter_read();
    case kCtrEl0: return kCtrValue;
    case kDczidEl0: return kDczidValue;
    default:
        log(LogLevel::Error, "MRS of unsupported system register 0x%04x", encoding);
        std::abort();
    }
}

void guest_yield() {
    if (KThread* self = Scheduler::current())
        active_kernel().scheduler().yield_hint(*self);
    else
        std::this_thread::yield();
}

namespace {
std::atomic<InterruptListener> g_interrupt_listener{nullptr};
}  // namespace

void set_interrupt_listener(InterruptListener listener) {
    g_interrupt_listener.store(listener, std::memory_order_release);
}

namespace detail {
void notify_interrupt_listener(GuestInterrupt what) {
    if (const InterruptListener l = g_interrupt_listener.load(std::memory_order_acquire))
        l(what);
}
}  // namespace detail

void set_indirect_resolver(IndirectResolver resolver) { g_resolver.store(resolver, std::memory_order_release); }

IndirectResolver indirect_resolver() { return g_resolver.load(std::memory_order_acquire); }

}  // namespace lgp::rt
