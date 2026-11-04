// Present pacing of lgp_boot (lgp/boot/frame_pacer.hpp): waits for a 60 Hz
// display with the game's present interval, catches up short hiccups and
// starts afresh after a long stall.
#include <cstdio>

#include "lgp/boot/frame_pacer.hpp"

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        ++g_failures;
    }
}

constexpr int64_t kMs = 1'000'000;
constexpr int64_t kPeriod1 = 16'666'666;  // 1/60 s
constexpr int64_t kPeriod2 = 33'333'333;  // interval 2: 30 fps

}  // namespace

int main() {
    using lgp::boot::FramePacer;
    {
        FramePacer p;
        check(p.after_present(0, 2) == 0, "first frame starts the schedule");
        check(p.after_present(5 * kMs, 2) == kPeriod2 - 5 * kMs, "fast frame waits for its slot");
        // The guest slept until 33.3 ms; the next frame is due at 66.7 ms.
        check(p.after_present(kPeriod2 + 10 * kMs, 2) == kPeriod2 - 10 * kMs, "due one period after the last slot");
        // 30 fps over a second: 30 frames, whatever the render time.
        FramePacer q;
        int64_t now = 0;
        for (int i = 0; i < 31; ++i) now += 3 * kMs + q.after_present(now, 2);
        check(now >= 1000 * kMs && now < 1000 * kMs + kPeriod2, "30 frames per second at interval 2");
    }
    {
        FramePacer p;
        p.after_present(0, 1);
        check(p.after_present(20 * kMs, 1) == 0, "late frame (20 ms) does not wait");
        check(p.after_present(25 * kMs, 1) == 2 * kPeriod1 - 25 * kMs, "and the next one catches up");
        check(p.after_present(500 * kMs, 1) == 0, "long stall");
        check(p.after_present(501 * kMs, 1) == kPeriod1 - kMs, "schedule restarts after the stall");
    }
    {
        FramePacer p;
        check(p.after_present(0, 0) == 0 && p.after_present(1, 0) == 0, "interval 0: unpaced");
        check(p.after_present(2, -1) == 0, "no window yet: unpaced");
        FramePacer off(0);
        off.after_present(0, 2);
        check(off.after_present(1, 2) == 0, "refresh 0: unpaced");
    }
    if (g_failures != 0) {
        std::fprintf(stderr, "%d checks failed\n", g_failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
