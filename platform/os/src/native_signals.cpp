#include "lgp/platform/native_signals.hpp"

#include <atomic>
#include <cstring>

#if defined(__aarch64__) && !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#define LGP_NATIVE_SIGNALS 1
#include <pthread.h>
#include <signal.h>
#if defined(__APPLE__)
// <ucontext.h> needs _XOPEN_SOURCE there (deprecated makecontext API); the
// types and the thread state accessors come with <sys/ucontext.h>.
#include <sys/ucontext.h>
#else
#include <ucontext.h>
#endif
#else
#define LGP_NATIVE_SIGNALS 0
#endif

namespace lgp::platform {

#if LGP_NATIVE_SIGNALS

namespace {

// Rarely used by anything else, and not reset by debuggers (Go uses it for
// the same purpose).
constexpr int kKickSignal = SIGURG;
constexpr int kSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGFPE, kKickSignal};

std::atomic<SignalFilter> g_filter{nullptr};
struct sigaction g_previous[sizeof kSignals / sizeof kSignals[0]];

SignalKind kind_of(int sig) {
    switch (sig) {
    case SIGSEGV: return SignalKind::Segv;
    case SIGBUS: return SignalKind::Bus;
    case SIGILL: return SignalKind::Ill;
    case SIGTRAP: return SignalKind::Trap;
    case SIGFPE: return SignalKind::Fpe;
    default: return SignalKind::Kick;
    }
}

#if defined(__APPLE__)
// The accessor macros work with and without opaque thread state (arm64e).
void read_context(const ucontext_t* uc, SignalContext& c) {
    const auto& ss = uc->uc_mcontext->__ss;
    for (int i = 0; i < 29; ++i) c.x[i] = ss.__x[i];
    c.x[29] = (uint64_t)(__darwin_arm_thread_state64_get_fp(ss));
    c.x[30] = (uint64_t)(__darwin_arm_thread_state64_get_lr_fptr(ss));
    c.sp = (uint64_t)(__darwin_arm_thread_state64_get_sp(ss));
    c.pc = (uint64_t)(__darwin_arm_thread_state64_get_pc_fptr(ss));
    c.pstate = ss.__cpsr;
}

void write_context(ucontext_t* uc, const SignalContext& c) {
    auto& ss = uc->uc_mcontext->__ss;
    for (int i = 0; i < 29; ++i) ss.__x[i] = c.x[i];
    __darwin_arm_thread_state64_set_fp(ss, reinterpret_cast<void*>(c.x[29]));
    __darwin_arm_thread_state64_set_lr_fptr(ss, reinterpret_cast<void*>(c.x[30]));
    __darwin_arm_thread_state64_set_sp(ss, reinterpret_cast<void*>(c.sp));
    __darwin_arm_thread_state64_set_pc_fptr(ss, reinterpret_cast<void*>(c.pc));
    ss.__cpsr = static_cast<uint32_t>(c.pstate);
}
#else
void read_context(const ucontext_t* uc, SignalContext& c) {
    for (int i = 0; i < 31; ++i) c.x[i] = uc->uc_mcontext.regs[i];
    c.sp = uc->uc_mcontext.sp;
    c.pc = uc->uc_mcontext.pc;
    c.pstate = uc->uc_mcontext.pstate;
}

void write_context(ucontext_t* uc, const SignalContext& c) {
    for (int i = 0; i < 31; ++i) uc->uc_mcontext.regs[i] = c.x[i];
    uc->uc_mcontext.sp = c.sp;
    uc->uc_mcontext.pc = c.pc;
    uc->uc_mcontext.pstate = c.pstate;
}
#endif

void on_signal(int sig, siginfo_t* info, void* raw) {
    auto* uc = static_cast<ucontext_t*>(raw);
    if (const SignalFilter filter = g_filter.load(std::memory_order_acquire)) {
        SignalContext c;
        read_context(uc, c);
        if (filter(kind_of(sig), reinterpret_cast<uint64_t>(info->si_addr), c)) {
            write_context(uc, c);
            return;
        }
    }
    if (sig == kKickSignal)
        return;
    // Not ours: hand it to whoever handled it before. A default action is
    // taken by restoring it; the instruction faults again on return.
    for (size_t i = 0; i < sizeof kSignals / sizeof kSignals[0]; ++i) {
        if (kSignals[i] != sig)
            continue;
        const struct sigaction& prev = g_previous[i];
        if ((prev.sa_flags & SA_SIGINFO) != 0 && prev.sa_sigaction != nullptr) {
            prev.sa_sigaction(sig, info, raw);
        } else if (prev.sa_handler != SIG_DFL && prev.sa_handler != SIG_IGN && prev.sa_handler != nullptr) {
            prev.sa_handler(sig);
        } else {
            sigaction(sig, &prev, nullptr);
            if (sig == SIGTRAP || sig == SIGFPE)
                raise(sig);  // a BRK does not trap again by itself after the handler
        }
        return;
    }
}

}  // namespace

bool install_signal_filter(SignalFilter filter) {
    static std::atomic<bool> installed{false};
    g_filter.store(filter, std::memory_order_release);
    if (installed.exchange(true))
        return true;
    for (size_t i = 0; i < sizeof kSignals / sizeof kSignals[0]; ++i) {
        struct sigaction sa;
        std::memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = on_signal;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
        sigemptyset(&sa.sa_mask);
        if (sigaction(kSignals[i], &sa, &g_previous[i]) != 0)
            return false;
    }
    return true;
}

uint64_t current_thread_token() { return reinterpret_cast<uint64_t>(reinterpret_cast<void*>(pthread_self())); }

void kick_thread(uint64_t token) {
    pthread_kill(reinterpret_cast<pthread_t>(reinterpret_cast<void*>(token)), kKickSignal);
}

#else

bool install_signal_filter(SignalFilter) { return false; }

uint64_t current_thread_token() { return 0; }

void kick_thread(uint64_t) {}

#endif

}  // namespace lgp::platform
