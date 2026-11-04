// Scripted input for runs without a window (lgp_game --press): player 1 and,
// with p2: entries, player 2.
//
// SPEC is a comma-separated list of [p2:]TRIGGER:KEYS[:HOLD]:
//   p2:      the entry presses on player 2's controller (npad No2, connected
//            from the start when any entry has p2:); p1: is the default.
//            Player 2 has the --controller style, a single right Joy-Con when
//            that is handheld; p2:…:throw shakes it (LGP lets player 2 join
//            by shaking the second Joy-Con)
//   TRIGGER  N             presented frame N (the original form)
//            Ts            T seconds of guest time (the clock behind
//                          svcGetSystemTick), decimals allowed: 12.5s
//            EVENT[+OFF]   when EVENT happened, optionally OFF later; OFF is
//                          N frames or Ts seconds
//   EVENT    movie-start, movie-end   a video (not a sound track) of the
//            movie:: HLE started decoding / handed out its end marker;
//            "#K" picks the K-th occurrence (default 1): movie-end#2
//            battle-start, battle-end   the wild-battles mod requested a
//                          battle at a touch / saw it done: battle-end+2s:a
//            stable:DUR    the presented image has not changed for DUR (Nms or
//                          Ts) since this entry was armed: at the start for
//                          the first entry, otherwise once the previous entry
//                          has fired and been released. stable:500ms:a,
//                          stable:500ms:a pages through a dialogue whose text
//                          scrolls in, however long each page takes.
//            BASE+after-stable[=DUR]   once BASE (any trigger above but
//                          stable) is reached, wait until the image has not
//                          changed for DUR (default 500ms) after that:
//                          movie-end+after-stable:a
//   KEYS     a, b, x, y, l, r, zl, zr, plus, minus, up, down, left, right
//            and the sticks lup, ldown, lleft, lright, rup, ... joined by '+';
//            throw: the throw gesture of a single Joy-Con (as Space in the
//            window, --throw tunes it; once per press, needs
//            --controller joycon-right|joycon-left)
//            touch@X/Y: a finger on the touch screen at pixel X, Y of the
//            game's picture (as --frame-dump writes it; with the widescreen
//            mod the wider picture), held like a key: 30s:touch@640/850:0.3s
//   HOLD     N frames or Ts seconds; default 10 frames for frame triggers,
//            0.2 s for the others
// Wherever seconds are allowed, Nms gives milliseconds (500ms = 0.5s).
// Example: --press 300:a,40s:a,movie-end+1s:a:0.3s,stable:500ms:a,p2:50s:a
//
// The image is watched by a 64-bit hash of the presented frame (ImageWatch);
// no pixels are kept. A screen with a running animation never becomes stable.
//
// A press is evaluated whenever hid samples the input (every few ms) and is
// latched: even if its whole window falls between two samples (the game can
// present over 1000 frames per second while a video plays), the next sample
// still reports it once.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "lgp/platform/input.hpp"

namespace lgp::boot {

enum class PressEvent : uint8_t { MovieStart, MovieEnd, BattleStart, BattleEnd };

// KEYS "throw": not a button of the console; ScriptedInput turns it into the
// motion of a throw.
constexpr uint64_t kPressThrow = 1ull << 63;
constexpr size_t kPressEventCount = 4;

// What the script reads at one sample.
struct PressClock {
    uint64_t frame = 0;   // presented frames so far
    double seconds = 0;   // guest time
    std::array<uint32_t, kPressEventCount> events{};  // occurrences so far
    double image_changed = -1;  // guest seconds of the last image change; < 0: no image seen yet
};

struct Press {
    enum class Trigger : uint8_t { Frame, Seconds, Event, Stable };
    Trigger trigger = Trigger::Frame;
    uint64_t frame = 0;  // Trigger::Frame
    double seconds = 0;  // Trigger::Seconds
    PressEvent event = PressEvent::MovieEnd;
    uint32_t occurrence = 1;      // Trigger::Event: the K-th event
    bool offset_seconds = false;  // Trigger::Event: offset unit
    double offset = 0;
    bool after_stable = false;  // +after-stable on Frame/Seconds/Event
    double stable = 0;          // Trigger::Stable, after_stable: seconds without a change
    bool hold_seconds = false;  // hold unit
    double hold = 10;
    uint64_t buttons = 0;
    platform::StickState left, right;
    bool touch = false;  // KEYS touch@X/Y, picture pixels
    float touch_x = 0, touch_y = 0;
    uint8_t player = 0;  // 0 = player 1, 1 = player 2 (p2:)
};

constexpr size_t kPressPlayers = 2;

// Parses SPEC; false and *error on a malformed entry.
bool parse_press_script(const std::string& spec, std::vector<Press>* out, std::string* error);

// The number of players the script presses for: 1, or 2 with p2: entries.
size_t press_players(const std::vector<Press>& presses);

// True if any press waits for a stable image (needs ImageWatch).
bool uses_image(const std::vector<Press>& presses);

// 64-bit hash of an image (FNV-1a style over 64-bit words, tail bytewise).
uint64_t hash_image(const uint8_t* data, size_t size);

// Tracks when the presented image last changed, from hashes only. observe()
// comes from the presenting thread, last_change() from hid's.
class ImageWatch {
public:
    // A frame with this hash was presented at guest time `seconds`. The first
    // observation counts as a change.
    void observe(uint64_t hash, double seconds);
    // Guest seconds of the last change; < 0 before the first observation.
    double last_change() const { return changed_.load(std::memory_order_acquire); }
    uint64_t changes() const { return changes_.load(std::memory_order_relaxed); }

private:
    uint64_t hash_ = 0;  // presenting thread only
    std::atomic<double> changed_{-1};
    std::atomic<uint64_t> changes_{0};
};

class PressScript {
public:
    explicit PressScript(std::vector<Press> presses);
    // The controller state at this sample (buttons and sticks of all active
    // presses on top of `idle`). Samples must come in time order. Only
    // player 1's presses; the p2: ones wait.
    platform::ControllerState sample(const PressClock& now, const platform::ControllerState& idle);
    // Every player at once: out[k] = idle[k] plus player k's active presses,
    // for k < players (at most kPressPlayers); the presses of other players wait.
    void sample(const PressClock& now, const platform::ControllerState* idle, platform::ControllerState* out,
                size_t players);
    size_t size() const { return presses_.size(); }
    // Presses that have fired so far.
    size_t fired() const;
    // The touch@X/Y of a press active at the last sample (the latest in the
    // spec wins); false if none.
    bool touch(float* x, float* y) const;
    // Called once per press when its trigger is reached (index into the spec,
    // the sample's frame and guest seconds).
    std::function<void(size_t, uint64_t, double)> on_fire;

private:
    struct State {
        bool armed = false;  // Trigger::Stable / after_stable: stability counts from armed_seconds
        double armed_seconds = 0;
        bool released = false;    // fired, reported and outside its hold
        bool event_seen = false;  // Trigger::Event: the event happened
        uint64_t event_frame = 0;
        double event_seconds = 0;
        bool fired = false;  // trigger reached
        uint64_t fire_frame = 0;
        double fire_seconds = 0;
        bool reported = false;  // seen by at least one sample
    };
    std::vector<Press> presses_;
    std::vector<State> state_;
    bool touch_ = false;
    float touch_x_ = 0, touch_y_ = 0;
};

// Player 1 (and, with p2: entries, player 2) from a PressScript, sampled at
// every hid poll; the clock comes from the caller (presented frames, guest
// seconds, movie:: events).
class ScriptedInput final : public platform::IInput {
public:
    // Motion of the controller from whether "throw" is held and the guest
    // seconds (the synthetic Joy-Con gesture, ControllerMapper); empty: none.
    using MotionSource = std::function<platform::MotionState(bool throwing, double seconds)>;

    // idle2: player 2's controller when it is not pressed, used when
    // `players` is 2 (press_players()); motion2: its motion.
    ScriptedInput(PressScript script, platform::ControllerState idle, std::function<PressClock()> clock,
                  MotionSource motion = {}, size_t players = 1, platform::ControllerState idle2 = {},
                  MotionSource motion2 = {})
        : script_(std::move(script)), clock_(std::move(clock)),
          players_(players < 1 ? 1 : players > kPressPlayers ? kPressPlayers : players) {
        idle_[0] = idle;
        idle_[1] = idle2;
        motion_[0] = std::move(motion);
        motion_[1] = std::move(motion2);
        current_ = idle_;
    }
    void poll() override {
        const PressClock now = clock_();
        std::lock_guard lock(mutex_);
        script_.sample(now, idle_.data(), current_.data(), players_);
        for (size_t k = 0; k < players_; ++k) {
            const bool throwing = (current_[k].buttons & kPressThrow) != 0;
            current_[k].buttons &= ~kPressThrow;
            if (motion_[k])
                current_[k].motion = motion_[k](throwing, now.seconds);
        }
        touch_ = {};
        float x = 0, y = 0;
        if (script_.touch(&x, &y)) {
            platform::TouchPoint p;
            if (touch_mapper_) {
                p = touch_mapper_(x, y);
            } else {
                p.x = std::clamp(x, 0.0f, platform::kTouchScreenWidth);
                p.y = std::clamp(y, 0.0f, platform::kTouchScreenHeight);
            }
            p.id = 0;
            touch_.count = 1;
            touch_.points[0] = p;
        }
    }
    // touch@X/Y (picture pixels) to the console's 1280x720 touch screen;
    // without one the picture is taken as 1280x720.
    using TouchMapper = std::function<platform::TouchPoint(float x, float y)>;
    void set_touch_mapper(TouchMapper mapper) {
        std::lock_guard lock(mutex_);
        touch_mapper_ = std::move(mapper);
    }
    platform::TouchScreenState touch_screen() const override {
        std::lock_guard lock(mutex_);
        return touch_;
    }
    size_t controller_count() const override { return players_; }
    platform::ControllerState controller(size_t i) const override {
        std::lock_guard lock(mutex_);
        return i < players_ ? current_[i] : platform::ControllerState{};
    }
    size_t fired() const {
        std::lock_guard lock(mutex_);
        return script_.fired();
    }

private:
    mutable std::mutex mutex_;
    PressScript script_;
    std::array<platform::ControllerState, kPressPlayers> idle_;
    std::function<PressClock()> clock_;
    std::array<MotionSource, kPressPlayers> motion_;
    size_t players_;
    std::array<platform::ControllerState, kPressPlayers> current_;
    TouchMapper touch_mapper_;
    platform::TouchScreenState touch_;
};

}  // namespace lgp::boot
