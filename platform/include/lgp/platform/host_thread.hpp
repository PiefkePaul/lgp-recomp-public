// A host thread with a caller-chosen stack size. Lifted code keeps the guest
// call depth on the host stack (native calls and returns, ADR-0004), so a
// guest thread's host stack must be at least as large as its guest stack
// (ADR-0002). std::thread cannot set a stack size.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>

namespace lgp::platform {

class HostThread {
public:
    // Starts fn on a new thread. Returns nullptr if the host refuses.
    static std::unique_ptr<HostThread> start(uint64_t stack_size, std::function<void()> fn);
    ~HostThread();  // joins
    void join();

    struct Impl;

private:
    HostThread() = default;
    std::unique_ptr<Impl> impl_;
};

// Names the calling thread for top -H, debuggers and crash reports (Linux and
// Android keep 15 characters). No-op where the host has no such call.
void set_current_thread_name(const char* name);

// Gives the calling thread its own signal stack (POSIX sigaltstack, 64 KiB,
// kept until the thread ends; once per thread). Native guest code keeps
// values below its SP (docs/analysis/56 §8), which a signal handler running
// on the guest stack would overwrite. Handlers only use it when installed
// with SA_ONSTACK. No-op where the host has no such call.
void ensure_signal_stack();

}  // namespace lgp::platform
