// Presents that stopped while the guest runs (BootOptions::stall_report_seconds).
// lgp_boot's ticker thread asks due() every few milliseconds; it is true once
// per stall, so the hang report is logged once and again only after presents
// resumed and stopped anew. A paused guest (app menu, background) is no
// stall, nor is a game that waits for the player in the software keyboard or
// a message box (UserWaitTextInput). Pure arithmetic on a steady clock, so
// tests drive it.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#include "lgp/platform/text_input.hpp"

namespace lgp::boot {

class StallReport {
public:
    using Clock = std::chrono::steady_clock;

    explicit StallReport(uint32_t seconds) : limit_(std::chrono::seconds(seconds)) {}

    // frames: presents so far. resumed() tells afterwards whether this call
    // ended a reported stall (for a "resumed" line).
    bool due(uint64_t frames, bool paused, Clock::time_point now) {
        resumed_ = false;
        if (limit_.count() == 0 || frames == 0)
            return false;
        if (frames != frames_ || paused) {
            resumed_ = reported_ && frames != frames_;
            stalled_for_ = now - since_;
            frames_ = frames;
            since_ = now;
            reported_ = false;
            return false;
        }
        if (reported_ || now - since_ < limit_)
            return false;
        reported_ = true;
        return true;
    }
    bool resumed() const { return resumed_; }
    // Last frame number seen, and how long it has stood (or stood, after resumed()).
    uint64_t frames() const { return frames_; }
    double seconds(Clock::time_point now) const {
        return std::chrono::duration<double>(resumed_ ? stalled_for_ : now - since_).count();
    }

private:
    Clock::duration limit_;
    uint64_t frames_ = 0;
    Clock::time_point since_{};
    Clock::duration stalled_for_{};
    bool reported_ = false;
    bool resumed_ = false;
};

// LGP_STALL_PEEK=VA[*],... (diagnosis, docs/analysis/42): guest words the
// stall report prints, one line each. VA: the 64-bit word there; VA*: that
// word taken as a pointer, and the 32- and 64-bit word it points to (a flag
// behind a GOT entry). Hex with or without 0x. read64 returns false where the
// guest address is not mapped.
inline std::vector<std::string> stall_peek_lines(const std::string& spec,
                                                 const std::function<bool(uint64_t, uint64_t*)>& read64) {
    std::vector<std::string> out;
    size_t at = 0;
    while (at < spec.size()) {
        const size_t comma = std::min(spec.find(',', at), spec.size());
        std::string item = spec.substr(at, comma - at);
        at = comma + 1;
        if (item.empty())
            continue;
        const bool deref = item.back() == '*';
        if (deref)
            item.pop_back();
        char* end = nullptr;
        const uint64_t va = item.empty() ? 0 : std::strtoull(item.c_str(), &end, 16);
        uint64_t word = 0, target = 0;
        char line[160];
        if (item.empty() || end == item.c_str() || *end != '\0' || !read64(va, &word))
            std::snprintf(line, sizeof line, "peek %s: not readable", item.c_str());
        else if (!deref)
            std::snprintf(line, sizeof line, "peek 0x%llx = 0x%016llx", (unsigned long long)va,
                          (unsigned long long)word);
        else if (read64(word, &target))
            std::snprintf(line, sizeof line, "peek 0x%llx -> 0x%llx = 0x%08x (64-bit 0x%016llx)",
                          (unsigned long long)va, (unsigned long long)word, unsigned(uint32_t(target)),
                          (unsigned long long)target);
        else
            std::snprintf(line, sizeof line, "peek 0x%llx -> 0x%llx: not readable", (unsigned long long)va,
                          (unsigned long long)word);
        out.emplace_back(line);
    }
    return out;
}

// Passes the game's keyboard and message requests on and tells while one is
// open: the game presents nothing while the player types.
class UserWaitTextInput final : public platform::ITextInput {
public:
    explicit UserWaitTextInput(platform::ITextInput* inner) : inner_(inner) {}
    bool ask(const platform::TextRequest& request, std::string* text) override {
        Scope scope(open_);
        return inner_->ask(request, text);
    }
    void show_message(const platform::MessageRequest& request) override {
        Scope scope(open_);
        inner_->show_message(request);
    }
    bool waiting() const { return open_.load(std::memory_order_relaxed) != 0; }

private:
    struct Scope {
        explicit Scope(std::atomic<int>& n) : n_(n) { n_.fetch_add(1, std::memory_order_relaxed); }
        ~Scope() { n_.fetch_sub(1, std::memory_order_relaxed); }
        std::atomic<int>& n_;
    };
    platform::ITextInput* inner_;
    std::atomic<int> open_{0};
};

}  // namespace lgp::boot
