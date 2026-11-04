// ITextInput: the host asks the user for a line of text (the Switch software
// keyboard, swkbd). Text is UTF-8; length limits count UTF-16 units as the
// game does (a character outside the BMP counts 2).
//
// ask() runs on the guest thread that opened the keyboard and blocks until
// the user answers; the game waits there as it would on the console.
//
// The same backends show the error applet's message (nn::err): show_message()
// blocks until the user dismissed it. The default shows nothing.
#pragma once

#include <cstdint>
#include <string>

namespace lgp::platform {

struct TextRequest {
    std::string header, sub, guide;  // what the game shows above and in the field
    std::string initial;             // pre-filled text
    std::string message;             // why the game rejected the previous answer (may be empty)
    uint32_t min_length = 0;         // 0 = none
    uint32_t max_length = 0;         // 0 = none
    bool password = false;
};

struct MessageRequest {
    std::string code;  // "error 2110-1100", "application error 15"
    std::string text;  // the game's message (may be empty)
};

class ITextInput {
public:
    virtual ~ITextInput() = default;
    // true and *text when the user confirmed, false when they cancelled.
    virtual bool ask(const TextRequest& request, std::string* text) = 0;
    virtual void show_message(const MessageRequest&) {}
};

}  // namespace lgp::platform
