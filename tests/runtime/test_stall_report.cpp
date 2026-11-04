// Present-stall detection of lgp_boot (lgp/boot/stall_report.hpp): one report
// per stall, none before the first present or while the guest is paused.
#include <cstdio>
#include <string>

#include "lgp/boot/stall_report.hpp"

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        ++g_failures;
    }
}

}  // namespace

int main() {
    using lgp::boot::StallReport;
    using std::chrono::milliseconds;
    using std::chrono::seconds;
    const StallReport::Clock::time_point t0{};
    {
        StallReport r(10);
        check(!r.due(0, false, t0 + seconds(60)), "no report before the first present (boot, start menu)");
        check(!r.due(5, false, t0 + seconds(61)), "a new frame arms the timer");
        check(!r.due(5, false, t0 + seconds(70)), "9 s without a present is no stall yet");
        check(r.due(5, false, t0 + seconds(71)), "10 s without a present is reported");
        check(r.frames() == 5 && r.seconds(t0 + seconds(71)) == 10.0, "frame and duration of the stall");
        check(!r.due(5, false, t0 + seconds(90)), "reported once per stall");
        check(!r.due(6, false, t0 + seconds(95)), "presents resume");
        check(r.resumed() && r.seconds(t0 + seconds(95)) == 34.0, "the resume ends the reported stall after 34 s");
        check(!r.due(7, false, t0 + seconds(96)) && !r.resumed(), "later frames are no resume");
        check(r.due(7, false, t0 + seconds(106)), "a second stall is reported again");
    }
    {
        StallReport r(10);
        check(!r.due(3, false, t0), "first frame");
        check(!r.due(3, true, t0 + seconds(30)), "a paused guest (menu, background) is no stall");
        check(!r.due(3, false, t0 + seconds(35)), "the timer restarts after the pause");
        check(r.due(3, false, t0 + seconds(40) + milliseconds(1)), "stall counted from the end of the pause");
        check(!r.due(3, true, t0 + seconds(41)) && !r.resumed(), "pausing after a report is no resume");
    }
    {
        StallReport r(0);
        check(!r.due(1, false, t0) && !r.due(1, false, t0 + seconds(3600)), "0 seconds: never");
    }
    {
        // The game waits for the player in the keyboard: counted as paused.
        struct Keyboard final : lgp::platform::ITextInput {
            lgp::boot::UserWaitTextInput* outer = nullptr;
            bool seen_waiting = false;
            bool ask(const lgp::platform::TextRequest&, std::string* text) override {
                seen_waiting = outer->waiting();
                *text = "Paul";
                return true;
            }
            void show_message(const lgp::platform::MessageRequest&) override { seen_waiting &= outer->waiting(); }
        } keyboard;
        lgp::boot::UserWaitTextInput input(&keyboard);
        keyboard.outer = &input;
        std::string text;
        check(!input.waiting(), "no request open");
        check(input.ask({}, &text) && text == "Paul", "requests are passed on");
        input.show_message({});
        check(keyboard.seen_waiting, "waiting while a keyboard or message request is open");
        check(!input.waiting(), "not waiting once it is answered");
        StallReport r(10);
        check(!r.due(2, false, t0), "first frame");
        check(!r.due(2, true, t0 + seconds(11)), "an open keyboard (passed as paused) is no stall");
    }
    // LGP_STALL_PEEK: a word, a pointer to a flag, and what cannot be read.
    {
        const auto read64 = [](uint64_t va, uint64_t* out) {
            if (va == 0x95fff30) *out = 0x7f000010;
            else if (va == 0x7f000010) *out = 0xAAAA'BBBB'1973'1110ull;
            else return false;
            return true;
        };
        const auto lines = lgp::boot::stall_peek_lines("0x95fff30,95fff30*,,zz,0x10,0x10*", read64);
        check(lines.size() == 5, "one line per entry, empty entries skipped");
        check(lines.size() > 0 && lines[0] == "peek 0x95fff30 = 0x000000007f000010", "plain word");
        check(lines.size() > 1 && lines[1] == "peek 0x95fff30 -> 0x7f000010 = 0x19731110 (64-bit 0xaaaabbbb19731110)",
              "dereferenced word");
        check(lines.size() > 2 && lines[2] == "peek zz: not readable", "bad number");
        check(lines.size() > 3 && lines[3] == "peek 0x10: not readable", "unmapped address");
        check(lines.size() > 4 && lines[4] == "peek 0x10: not readable", "unmapped pointer slot");
    }
    if (g_failures == 0)
        std::printf("stall report: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
