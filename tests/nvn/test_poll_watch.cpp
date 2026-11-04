// PollWatch (lgp/gpu/webgpu/poll_watch.hpp): when a device poll loop that
// keeps waiting is reported. The schedule, and that the watchdog thread
// reports a wait whose owner never calls check() (blocked inside a poll).
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>

#include "lgp/gpu/webgpu/poll_watch.hpp"

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}
}  // namespace

int main() {
    using lgp::gpu::webgpu::PollWatch;
    {
        uint64_t next = PollWatch::kFirstWarnMs;
        check(!PollWatch::due(&next, 0), "nothing at 0 ms");
        check(!PollWatch::due(&next, 4999), "nothing before 5 s");
        check(PollWatch::due(&next, 5000), "warning at 5 s");
        check(!PollWatch::due(&next, 5001), "once at 5 s");
        check(!PollWatch::due(&next, 9999), "nothing before 10 s");
        check(PollWatch::due(&next, 10000), "warning at 10 s");
        check(!PollWatch::due(&next, 19999), "nothing before 20 s");
        check(PollWatch::due(&next, 20000), "warning at 20 s");
    }
    {
        // A wait seen late warns once, then at the next doubling.
        uint64_t next = PollWatch::kFirstWarnMs;
        check(PollWatch::due(&next, 25000), "warning at 25 s");
        check(!PollWatch::due(&next, 39999), "nothing before 40 s");
        check(PollWatch::due(&next, 40000), "warning at 40 s");
    }
    {
        // A short wait: no warning, no line at the end.
        PollWatch w("short test wait");
        w.check();
        check(w.warnings() == 0, "no warning for a short wait");
    }
    {
        // The owner sleeps (as if blocked in wgpuDevicePoll) and never calls
        // check(): the watchdog warns at 100 ms and again at 200 ms.
        PollWatch w("blocked test wait", 100);
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        check(w.warnings() >= 2, "watchdog warns while the owner is blocked");
    }
    if (failures == 0) std::printf("poll watch: ok\n");
    return failures == 0 ? 0 : 1;
}
