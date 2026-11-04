// The software keyboard on a host text input (lgp/boot/keyboard.hpp): the
// game's UTF-16 request reaches the host as UTF-8 and the answer comes back
// as UTF-16; cancelling and the default without a handler. The error
// applet's message reaches the same host input.
#include <string>
#include <vector>

#include "check.hpp"
#include "lgp/boot/keyboard.hpp"

using namespace lgp;

namespace {

class Recorder final : public platform::ITextInput {
public:
    bool ask(const platform::TextRequest& request, std::string* text) override {
        last = request;
        ++calls;
        *text = answer;
        return accept;
    }
    void show_message(const platform::MessageRequest& message) override { messages.push_back(message); }
    platform::TextRequest last;
    std::vector<platform::MessageRequest> messages;
    std::string answer;
    bool accept = true;
    int calls = 0;
};

void test_conversions() {
    const std::u16string mixed = u"R\u00F6t \u30EC\U0001F600";
    CHECK(hle::utf16_to_utf8(mixed) == "R\xC3\xB6t \xE3\x83\xAC\xF0\x9F\x98\x80");
    CHECK(hle::utf8_to_utf16(hle::utf16_to_utf8(mixed)) == mixed);
    CHECK(hle::utf16_to_utf8(std::u16string(1, char16_t(0xD800))) == "\xEF\xBF\xBD");  // lone surrogate
}

void test_handler() {
    hle::SystemConfig config;
    config.user_nickname = "Nick";
    hle::AppletState state(config);
    hle::KeyboardRequest req;
    req.header = u"Wie hei\u00DFt du?";
    req.initial = u"Red";
    req.max_length = 10;
    CHECK(state.show_keyboard(req).text == u"Red");  // no handler: the initial text

    Recorder rec;
    rec.answer = "P\xC3\xA4ul";
    state.set_keyboard_handler(boot::keyboard_handler(&rec));
    req.check_message = u"zu kurz";
    req.rejected = 1;
    const hle::KeyboardResult r = state.show_keyboard(req);
    CHECK(rec.calls == 1);
    CHECK(rec.last.header == "Wie hei\xC3\x9F" "t du?");
    CHECK(rec.last.initial == "Red" && rec.last.message == "zu kurz" && rec.last.max_length == 10);
    CHECK(r.accepted && r.text == u"P\u00E4ul");

    rec.accept = false;
    CHECK(!state.show_keyboard(req).accepted);
}

void test_message() {
    hle::AppletState state(hle::SystemConfig{});
    state.show_message({"error 2110-1100", ""});  // no handler: nothing happens
    Recorder rec;
    state.set_message_handler(boot::message_handler(&rec));
    state.show_message({"application error 15", "Communication was interrupted."});
    CHECK(rec.messages.size() == 1);
    CHECK(!rec.messages.empty() && rec.messages[0].code == "application error 15" &&
          rec.messages[0].text == "Communication was interrupted.");
}

}  // namespace

int main() {
    RUN(test_conversions);
    RUN(test_handler);
    RUN(test_message);
    return finish();
}
