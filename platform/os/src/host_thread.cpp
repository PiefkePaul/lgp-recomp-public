#include "lgp/platform/host_thread.hpp"

#include "lgp/platform/cpu_affinity.hpp"

#include <cstring>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <process.h>
#include <windows.h>
#else
#include <pthread.h>
#include <signal.h>

#include <cstdlib>
#endif
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace lgp::platform {

struct HostThread::Impl {
    std::function<void()> fn;
    bool joined = false;
#if defined(_WIN32)
    HANDLE handle = nullptr;
#else
    pthread_t thread{};
#endif
};

namespace {

#if defined(_WIN32)
unsigned __stdcall trampoline(void* arg) {
    static_cast<HostThread::Impl*>(arg)->fn();
    return 0;
}
#else
void* trampoline(void* arg) {
    apply_thread_cpu_policy();
    static_cast<HostThread::Impl*>(arg)->fn();
    forget_thread_cpu_policy();
    return nullptr;
}
#endif

}  // namespace

std::unique_ptr<HostThread> HostThread::start(uint64_t stack_size, std::function<void()> fn) {
    std::unique_ptr<HostThread> t(new HostThread());
    t->impl_ = std::make_unique<Impl>();
    t->impl_->fn = std::move(fn);
#if defined(_WIN32)
    const uintptr_t h = _beginthreadex(nullptr, static_cast<unsigned>(stack_size), &trampoline, t->impl_.get(),
                                       STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (h == 0)
        return nullptr;
    t->impl_->handle = reinterpret_cast<HANDLE>(h);
#else
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0)
        return nullptr;
    pthread_attr_setstacksize(&attr, static_cast<size_t>(stack_size));
    const int rc = pthread_create(&t->impl_->thread, &attr, &trampoline, t->impl_.get());
    pthread_attr_destroy(&attr);
    if (rc != 0)
        return nullptr;
#endif
    return t;
}

void HostThread::join() {
    if (!impl_ || impl_->joined)
        return;
    impl_->joined = true;
#if defined(_WIN32)
    WaitForSingleObject(impl_->handle, INFINITE);
    CloseHandle(impl_->handle);
#else
    pthread_join(impl_->thread, nullptr);
#endif
}

HostThread::~HostThread() { join(); }

void set_current_thread_name(const char* name) {
#if defined(__APPLE__)
    pthread_setname_np(name);
#elif defined(__linux__) || defined(__ANDROID__)
    char shortened[16] = {};
    std::strncpy(shortened, name, sizeof(shortened) - 1);
    pthread_setname_np(pthread_self(), shortened);
#else
    (void)name;
#endif
}

void ensure_signal_stack() {
    // tvOS has no sigaltstack; nothing there needs it (only the native start
    // runs its signal handlers on it, and that start is not built for tvOS).
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__) && !(defined(__APPLE__) && (TARGET_OS_TV || TARGET_OS_WATCH))
    // Freed with the thread: a thread_local owner releases it at thread exit,
    // after switching the alternate stack off again.
    struct AltStack {
        void* mem = nullptr;
        ~AltStack() {
            if (mem == nullptr)
                return;
            stack_t off{};
            off.ss_flags = SS_DISABLE;
            sigaltstack(&off, nullptr);
            std::free(mem);
        }
    };
    static thread_local AltStack alt;
    if (alt.mem != nullptr)
        return;
    constexpr size_t kSize = 64 * 1024;
    stack_t ss{};
    ss.ss_sp = std::malloc(kSize);
    if (ss.ss_sp == nullptr)
        return;
    ss.ss_size = kSize;
    if (sigaltstack(&ss, nullptr) != 0) {
        std::free(ss.ss_sp);
        return;
    }
    alt.mem = ss.ss_sp;
#endif
}

}  // namespace lgp::platform
