// Software keyboard on the host (lgp/platform/text_input.hpp): UTF-16
// lengths, the fixed and console backends.
#include <sstream>
#include <string>

#include "../runtime/check.hpp"
#include "lgp/platform/text_input_backends.hpp"

using namespace lgp::platform;

namespace {

void test_lengths() {
    CHECK_EQ(utf16_length("Red"), 3u);
    CHECK_EQ(utf16_length("R\xC3\xB6t"), 3u);           // ö: 2 bytes, 1 unit
    CHECK_EQ(utf16_length("\xE3\x83\xAC\xE3\x83\x83"), 2u);  // two kana
    CHECK_EQ(utf16_length("\xF0\x9F\x98\x80"), 2u);      // emoji: a surrogate pair
    TextRequest r;
    r.max_length = 5;
    CHECK(text_fits(r, "Paul"));
    CHECK(!text_fits(r, ""));
    CHECK(!text_fits(r, "Pauline"));
    r.min_length = 5;
    CHECK(!text_fits(r, "Paul"));
}

void test_fixed() {
    FixedTextInput in("Ash");
    std::string text;
    CHECK(in.ask({}, &text) && text == "Ash");
}

void test_console() {
    TextRequest r;
    r.header = "Name?";
    r.initial = "Red";
    r.max_length = 5;
    {
        std::istringstream in("Pauline\nPaul\n");
        std::ostringstream out;
        ConsoleTextInput c(in, out);
        std::string text;
        CHECK(c.ask(r, &text) && text == "Paul");  // too long first, asked again
        CHECK(out.str().find("Name?") != std::string::npos);
        CHECK(out.str().find("[Red]") != std::string::npos);
        CHECK(out.str().find("outside") != std::string::npos);
    }
    {
        std::istringstream in("\r\n");
        std::ostringstream out;
        ConsoleTextInput c(in, out);
        std::string text;
        CHECK(c.ask(r, &text) && text == "Red");  // empty line keeps the initial text
    }
    {
        std::istringstream in("");
        std::ostringstream out;
        ConsoleTextInput c(in, out);
        std::string text;
        CHECK(!c.ask(r, &text));  // end of input cancels
    }
    {
        std::istringstream in("\n");
        std::ostringstream out;
        ConsoleTextInput c(in, out);
        c.show_message({"application error 15", "Communication with the server was interrupted."});
        CHECK(out.str().find("application error 15") != std::string::npos);
        CHECK(out.str().find("interrupted") != std::string::npos);
        FixedTextInput f("x");
        f.show_message({"error 2110-1100", ""});  // returns at once
    }
}

}  // namespace

int main() {
    RUN(test_lengths);
    RUN(test_fixed);
    RUN(test_console);
    return finish();
}
