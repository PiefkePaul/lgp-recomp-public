#include "lgp/boot/press_script.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <utility>

namespace lgp::boot {

namespace {

std::vector<std::string> split(const std::string& v, char sep) {
    std::vector<std::string> out;
    for (size_t at = 0; at <= v.size();) {
        const size_t end = std::min(v.find(sep, at), v.size());
        out.push_back(v.substr(at, end - at));
        at = end + 1;
    }
    return out;
}

bool ends_with(const std::string& text, const std::string& tail) {
    return text.size() >= tail.size() && text.compare(text.size() - tail.size(), tail.size(), tail) == 0;
}

// "N" (frames), "Ts" (seconds) or "Nms" (milliseconds, as seconds); false if
// malformed or negative.
bool parse_amount(const std::string& text, double* value, bool* seconds) {
    if (text.empty())
        return false;
    const bool millis = ends_with(text, "ms");
    *seconds = text.back() == 's';
    const std::string number = text.substr(0, text.size() - (millis ? 2 : *seconds ? 1 : 0));
    if (number.empty())
        return false;
    char* end = nullptr;
    if (*seconds) {
        *value = std::strtod(number.c_str(), &end);
    } else {
        if (number.find_first_not_of("0123456789") != std::string::npos)
            return false;
        *value = double(std::strtoull(number.c_str(), &end, 10));
    }
    if (millis)
        *value /= 1000;
    return end != nullptr && *end == '\0' && *value >= 0;
}

// Ts or Nms only (no frame counts).
bool parse_duration(const std::string& text, double* seconds) {
    bool is_seconds = false;
    return parse_amount(text, seconds, &is_seconds) && is_seconds;
}

bool parse_base_trigger(const std::string& text, Press* p, std::string* error) {
    static const std::pair<const char*, PressEvent> kEvents[] = {{"movie-start", PressEvent::MovieStart},
                                                                 {"movie-end", PressEvent::MovieEnd},
                                                                 {"battle-start", PressEvent::BattleStart},
                                                                 {"battle-end", PressEvent::BattleEnd}};
    for (const auto& [name, event] : kEvents) {
        const std::string n = name;
        if (text.compare(0, n.size(), n) != 0)
            continue;
        std::string rest = text.substr(n.size());
        p->trigger = Press::Trigger::Event;
        p->event = event;
        if (!rest.empty() && rest[0] == '#') {
            const size_t plus = rest.find('+');
            const std::string count = rest.substr(1, plus == std::string::npos ? std::string::npos : plus - 1);
            if (count.empty() || count.find_first_not_of("0123456789") != std::string::npos ||
                std::strtoul(count.c_str(), nullptr, 10) == 0) {
                *error = "bad occurrence in " + text;
                return false;
            }
            p->occurrence = uint32_t(std::strtoul(count.c_str(), nullptr, 10));
            rest = plus == std::string::npos ? "" : rest.substr(plus);
        }
        if (!rest.empty()) {
            if (rest[0] != '+' || !parse_amount(rest.substr(1), &p->offset, &p->offset_seconds)) {
                *error = "bad offset in " + text + " (+N frames or +Ts seconds)";
                return false;
            }
        }
        return true;
    }
    double value = 0;
    bool seconds = false;
    if (!parse_amount(text, &value, &seconds)) {
        *error = "bad trigger " + text + " (frame N, Ts seconds, movie-start, movie-end, battle-start, battle-end or stable:DUR)";
        return false;
    }
    if (seconds) {
        p->trigger = Press::Trigger::Seconds;
        p->seconds = value;
    } else {
        p->trigger = Press::Trigger::Frame;
        p->frame = uint64_t(value);
    }
    return true;
}

// stable:DUR, or a base trigger with an optional +after-stable[=DUR].
bool parse_trigger(const std::string& text, Press* p, std::string* error) {
    const std::string stable = "stable:";
    if (text.compare(0, stable.size(), stable) == 0) {
        p->trigger = Press::Trigger::Stable;
        if (!parse_duration(text.substr(stable.size()), &p->stable)) {
            *error = "bad duration in " + text + " (stable:Nms or stable:Ts)";
            return false;
        }
        return true;
    }
    const std::string after = "+after-stable";
    const size_t at = text.find(after);
    if (at == std::string::npos)
        return parse_base_trigger(text, p, error);
    const std::string rest = text.substr(at + after.size());
    p->after_stable = true;
    p->stable = 0.5;
    if (!rest.empty() && (rest[0] != '=' || !parse_duration(rest.substr(1), &p->stable))) {
        *error = "bad +after-stable in " + text + " (+after-stable or +after-stable=Nms|Ts)";
        return false;
    }
    if (at == 0) {
        *error = "+after-stable needs a trigger before it: " + text;
        return false;
    }
    return parse_base_trigger(text.substr(0, at), p, error);
}

bool parse_keys(const std::string& text, Press* p, std::string* error) {
    static const std::pair<const char*, uint64_t> kKeys[] = {
        {"a", platform::kButtonA},         {"b", platform::kButtonB},       {"x", platform::kButtonX},
        {"y", platform::kButtonY},         {"l", platform::kButtonL},       {"r", platform::kButtonR},
        {"zl", platform::kButtonZL},       {"zr", platform::kButtonZR},     {"plus", platform::kButtonPlus},
        {"minus", platform::kButtonMinus}, {"up", platform::kButtonUp},     {"down", platform::kButtonDown},
        {"left", platform::kButtonLeft},   {"right", platform::kButtonRight},  {"throw", kPressThrow}};
    for (const std::string& key : split(text, '+')) {
        if (key.compare(0, 6, "touch@") == 0) {  // touch@X/Y, picture pixels
            const std::vector<std::string> xy = split(key.substr(6), '/');
            char* ex = nullptr;
            char* ey = nullptr;
            if (xy.size() == 2 && !xy[0].empty() && !xy[1].empty()) {
                p->touch_x = std::strtof(xy[0].c_str(), &ex);
                p->touch_y = std::strtof(xy[1].c_str(), &ey);
            }
            if (ex == nullptr || ey == nullptr || *ex != '\0' || *ey != '\0' || p->touch_x < 0 || p->touch_y < 0) {
                *error = "bad touch " + key + " (touch@X/Y, picture pixels)";
                return false;
            }
            p->touch = true;
            continue;
        }
        // Sticks: lup, ldown, lleft, lright (left), rup ... (right), full deflection.
        if (key.size() > 1 && (key[0] == 'l' || key[0] == 'r') && key != "l" && key != "r") {
            const std::string dir = key.substr(1);
            platform::StickState& s = key[0] == 'l' ? p->left : p->right;
            if (dir == "up") { s.y = 32767; continue; }
            if (dir == "down") { s.y = -32767; continue; }
            if (dir == "left") { s.x = -32767; continue; }
            if (dir == "right") { s.x = 32767; continue; }
        }
        const auto it =
            std::find_if(std::begin(kKeys), std::end(kKeys), [&](const auto& k) { return key == k.first; });
        if (it == std::end(kKeys)) {
            *error = "unknown key " + key;
            return false;
        }
        p->buttons |= it->second;
    }
    return true;
}

}  // namespace

bool parse_press_script(const std::string& spec, std::vector<Press>* out, std::string* error) {
    out->clear();
    for (const std::string& entry : split(spec, ',')) {
        std::string item = entry;
        uint8_t player = 0;
        if (item.compare(0, 3, "p1:") == 0 || item.compare(0, 3, "p2:") == 0) {
            player = uint8_t(item[1] - '1');
            item = item.substr(3);
        }
        auto parts = split(item, ':');
        // stable:DUR carries the trigger's own ':'.
        if (parts.size() >= 2 && parts[0] == "stable") {
            parts[1] = parts[0] + ":" + parts[1];
            parts.erase(parts.begin());
        }
        if (parts.size() < 2 || parts.size() > 3) {
            *error = "TRIGGER:KEYS[:HOLD], got " + item;
            return false;
        }
        Press p;
        p.player = player;
        if (!parse_trigger(parts[0], &p, error) || !parse_keys(parts[1], &p, error))
            return false;
        if (parts.size() == 3) {
            if (!parse_amount(parts[2], &p.hold, &p.hold_seconds)) {
                *error = "bad hold " + parts[2] + " (N frames or Ts seconds)";
                return false;
            }
        } else if (p.trigger != Press::Trigger::Frame) {
            p.hold_seconds = true;  // frame counts are meaningless while a video runs at 1000 fps
            p.hold = 0.2;
        }
        out->push_back(p);
    }
    return true;
}

size_t press_players(const std::vector<Press>& presses) {
    return std::any_of(presses.begin(), presses.end(), [](const Press& p) { return p.player != 0; }) ? 2 : 1;
}

bool uses_image(const std::vector<Press>& presses) {
    return std::any_of(presses.begin(), presses.end(),
                       [](const Press& p) { return p.trigger == Press::Trigger::Stable || p.after_stable; });
}

uint64_t hash_image(const uint8_t* data, size_t size) {
    constexpr uint64_t kPrime = 0x100000001b3ull;
    uint64_t h = 0xcbf29ce484222325ull;
    size_t i = 0;
    for (; i + 8 <= size; i += 8) {
        uint64_t w;
        std::memcpy(&w, data + i, 8);
        h = (h ^ w) * kPrime;
        h ^= h >> 29;
    }
    for (; i < size; ++i) h = (h ^ data[i]) * kPrime;
    return h ^ size;
}

void ImageWatch::observe(uint64_t hash, double seconds) {
    if (changed_.load(std::memory_order_relaxed) >= 0 && hash == hash_)
        return;
    hash_ = hash;
    changes_.fetch_add(1, std::memory_order_relaxed);
    changed_.store(seconds, std::memory_order_release);
}

PressScript::PressScript(std::vector<Press> presses) : presses_(std::move(presses)), state_(presses_.size()) {}

platform::ControllerState PressScript::sample(const PressClock& now, const platform::ControllerState& idle) {
    platform::ControllerState st;
    sample(now, &idle, &st, 1);
    return st;
}

void PressScript::sample(const PressClock& now, const platform::ControllerState* idle, platform::ControllerState* out,
                         size_t players) {
    for (size_t k = 0; k < players; ++k)
        out[k] = idle[k];
    touch_ = false;
    for (size_t i = 0; i < presses_.size(); ++i) {
        const Press& p = presses_[i];
        State& s = state_[i];
        if (p.player >= players)
            continue;
        platform::ControllerState& st = out[p.player];
        if (!s.fired) {
            uint64_t start_frame = 0;
            double start_seconds = 0;
            bool reached = false;
            switch (p.trigger) {
            case Press::Trigger::Frame:
                // The press belongs to frame p.frame even if this sample sees a later one
                // (hold counts from there, as when it was evaluated per frame).
                reached = now.frame >= p.frame;
                start_frame = p.frame;
                start_seconds = now.seconds;
                break;
            case Press::Trigger::Seconds:
                reached = now.seconds >= p.seconds;
                start_frame = now.frame;
                start_seconds = p.seconds;
                break;
            case Press::Trigger::Event:
                if (!s.event_seen && now.events[size_t(p.event)] >= p.occurrence) {
                    s.event_seen = true;
                    s.event_frame = now.frame;
                    s.event_seconds = now.seconds;
                }
                if (s.event_seen) {
                    if (p.offset_seconds) {
                        start_seconds = s.event_seconds + p.offset;
                        reached = now.seconds >= start_seconds;
                        start_frame = now.frame;
                    } else {
                        start_frame = s.event_frame + uint64_t(p.offset);
                        reached = now.frame >= start_frame;
                        start_seconds = now.seconds;
                    }
                }
                break;
            case Press::Trigger::Stable:
                // Armed once the previous entry is through (the first at once).
                reached = i == 0 || state_[i - 1].released;
                break;
            }
            if (p.trigger == Press::Trigger::Stable || p.after_stable) {
                if (reached && !s.armed) {
                    s.armed = true;
                    s.armed_seconds = now.seconds;
                }
                reached = s.armed && now.image_changed >= 0 &&
                          now.seconds - std::max(s.armed_seconds, now.image_changed) >= p.stable;
                start_frame = now.frame;
                start_seconds = now.seconds;
            }
            if (!reached)
                continue;
            s.fired = true;
            s.fire_frame = start_frame;
            s.fire_seconds = start_seconds;
            if (on_fire)
                on_fire(i, now.frame, now.seconds);
        }
        const bool within = p.hold_seconds ? now.seconds < s.fire_seconds + p.hold
                                           : now.frame < s.fire_frame + uint64_t(p.hold);
        if (!within && s.reported) {
            s.released = true;
            continue;
        }
        s.reported = true;  // latched: every press shows up in at least one sample
        st.buttons |= p.buttons;
        if (p.left.x || p.left.y)
            st.left = p.left;
        if (p.right.x || p.right.y)
            st.right = p.right;
        if (p.touch) {
            touch_ = true;
            touch_x_ = p.touch_x;
            touch_y_ = p.touch_y;
        }
    }
}

bool PressScript::touch(float* x, float* y) const {
    *x = touch_x_;
    *y = touch_y_;
    return touch_;
}

size_t PressScript::fired() const {
    return size_t(std::count_if(state_.begin(), state_.end(), [](const State& s) { return s.fired; }));
}

}  // namespace lgp::boot
