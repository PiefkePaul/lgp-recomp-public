// Present pacing for the NVN-HLE (lgp_boot): the HLE presents at once, while
// on the console nvnQueuePresentTexture lets the GPU show a frame only every
// `interval` vertical blanks of the 60 Hz display (interval 2 = 30 fps). Games
// that run their logic once per frame then run too fast without this.
//
// Pure arithmetic on a nanosecond clock, so tests drive it; the caller sleeps
// for what after_present() returns (as svcSleepThread, so other guest threads
// keep running).
#pragma once

#include <cstdint>

namespace lgp::boot {

class FramePacer {
public:
    explicit FramePacer(double refresh_hz = 60) : refresh_hz_(refresh_hz) {}

    // A frame was presented at `now_ns` with this present interval. Returns
    // how long to wait before the guest continues (0 = at once). Interval
    // <= 0 means unpaced. Each frame is due one period after the previous
    // one's due time, so short hiccups are made up; a frame more than one
    // period late starts the schedule afresh instead of letting the following
    // frames run unpaced to catch up.
    int64_t after_present(int64_t now_ns, int32_t interval) {
        if (interval <= 0 || refresh_hz_ <= 0) {
            next_ = -1;
            return 0;
        }
        const auto period = static_cast<int64_t>(double(interval) * 1e9 / refresh_hz_);
        if (next_ < 0 || now_ns - next_ > period) {
            next_ = now_ns + period;
            return 0;
        }
        if (now_ns >= next_) {
            next_ += period;
            return 0;
        }
        const int64_t wait = next_ - now_ns;
        next_ += period;
        return wait;
    }

private:
    double refresh_hz_;
    int64_t next_ = -1;  // when the next frame is due; < 0: no schedule yet
};

}  // namespace lgp::boot
