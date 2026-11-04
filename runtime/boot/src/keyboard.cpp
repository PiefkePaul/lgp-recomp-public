#include "lgp/boot/keyboard.hpp"

#include "lgp/runtime/log.hpp"

namespace lgp::boot {

hle::KeyboardHandler keyboard_handler(platform::ITextInput* input) {
    return [input](const hle::KeyboardRequest& req) {
        platform::TextRequest r;
        r.header = hle::utf16_to_utf8(req.header);
        r.sub = hle::utf16_to_utf8(req.sub);
        r.guide = hle::utf16_to_utf8(req.guide);
        r.initial = hle::utf16_to_utf8(req.initial);
        r.message = hle::utf16_to_utf8(req.check_message);
        r.min_length = req.min_length;
        r.max_length = req.max_length;
        r.password = req.password;
        rt::log(rt::LogLevel::Info, "swkbd: asking the host (header \"%s\", initial \"%s\", length %u..%u%s)",
                r.header.c_str(), r.initial.c_str(), r.min_length, r.max_length,
                req.rejected != 0 ? ", after a rejected answer" : "");
        hle::KeyboardResult result;
        std::string text;
        result.accepted = input->ask(r, &text);
        if (result.accepted)
            result.text = hle::utf8_to_utf16(text);
        rt::log(rt::LogLevel::Info, "swkbd: host %s", result.accepted ? "answered" : "cancelled");
        return result;
    };
}

hle::MessageHandler message_handler(platform::ITextInput* input) {
    return [input](const hle::ErrorMessage& m) {
        platform::MessageRequest r;
        r.code = m.code;
        r.text = m.text;
        input->show_message(r);
    };
}

}  // namespace lgp::boot
