// Software keyboard library applet (swkbd, AppletId 0x11) as HLE: the text
// comes from the host's KeyboardHandler instead of an on-screen keyboard.
// Storage layouts from libnx swkbd.h/swkbd.c and libapplet.c (interface
// documentation only):
//   in 0: LibAppletArgs (CommonArguments), in 1: the keyboard config
//   (SwkbdArgCommon first, version-dependent tail), in 2: work buffer as
//   transfer memory (initial text at initialStringOffset).
//   interactive out (text check): u64 byte size, UTF-16 text (0x7D4 bytes)
//   interactive in:  u32 TextCheckResult, UTF-16 message (0x7D4 bytes)
//   out:             u32 CloseResult (0 OK, 1 cancel), UTF-16 text (0x7D4 bytes)
#include <algorithm>
#include <cstring>

#include "am_internal.hpp"
#include "lgp/runtime/log.hpp"

namespace lgp::hle {
namespace {

using am_detail::StorageData;
using rt::LogLevel;

constexpr size_t kTextBytes = 0x7D4;
constexpr size_t kTextUnits = kTextBytes / 2;

// SwkbdArgCommon field offsets.
constexpr size_t kHeaderText = 0x24, kHeaderUnits = 130 / 2;
constexpr size_t kSubText = 0xA6, kSubUnits = 258 / 2;
constexpr size_t kGuideText = 0x1A8, kGuideUnits = 514 / 2;
constexpr size_t kStringLenMax = 0x3AC, kStringLenMin = 0x3B0, kPasswordFlag = 0x3B4;
constexpr size_t kInitialStringOffset = 0x3C0, kInitialStringSize = 0x3C4;
constexpr size_t kTextCheckFlag = 0x3D0;
constexpr size_t kArgCommonSize = 0x3D1;

uint32_t u32_at(const std::vector<uint8_t>& v, size_t at) {
    uint32_t x = 0;
    if (at + 4 <= v.size())
        std::memcpy(&x, v.data() + at, 4);
    return x;
}

// NUL-terminated UTF-16 of at most `units` units at `at`.
std::u16string text_at(const std::vector<uint8_t>& v, size_t at, size_t units) {
    std::u16string s;
    for (size_t i = 0; i < units && at + 2 * i + 2 <= v.size(); ++i) {
        char16_t c = 0;
        std::memcpy(&c, v.data() + at + 2 * i, 2);
        if (c == 0)
            break;
        s.push_back(c);
    }
    return s;
}

std::vector<uint8_t> with_text(const void* head, size_t head_size, const std::u16string& text) {
    std::vector<uint8_t> out(head_size + kTextBytes, 0);
    std::memcpy(out.data(), head, head_size);
    const size_t n = std::min(text.size(), kTextUnits - 1);  // keeps the NUL
    std::memcpy(out.data() + head_size, text.data(), 2 * n);
    return out;
}

class SoftwareKeyboard final : public am_detail::LibraryApplet {
public:
    explicit SoftwareKeyboard(std::shared_ptr<AppletState> state) : state_(std::move(state)) {}

private:
    void on_start(const Request& req, std::vector<std::shared_ptr<StorageData>> in) override {
        std::vector<uint8_t> config, work;
        if (in.size() < 2 || !in[1]->contents(req, &config) || config.size() < kArgCommonSize) {
            rt::log(LogLevel::Error, "swkbd: missing or short keyboard config (%zu in storages)", in.size());
            close(false, {});
            return;
        }
        if (in.size() >= 3 && !in[2]->contents(req, &work))
            rt::log(LogLevel::Warn, "swkbd: work buffer not readable");
        if (config.size() > kArgCommonSize && config[kArgCommonSize] != 0)
            rt::log(LogLevel::Warn, "swkbd: config byte 0x3D1 is %u (UTF-8 output?), answering in UTF-16",
                    config[kArgCommonSize]);

        request_.header = text_at(config, kHeaderText, kHeaderUnits);
        request_.sub = text_at(config, kSubText, kSubUnits);
        request_.guide = text_at(config, kGuideText, kGuideUnits);
        const int32_t max_len = static_cast<int32_t>(u32_at(config, kStringLenMax));
        const int32_t min_len = static_cast<int32_t>(u32_at(config, kStringLenMin));
        request_.max_length = max_len > 0 ? static_cast<uint32_t>(max_len) : 0;
        request_.min_length = min_len > 0 ? static_cast<uint32_t>(min_len) : 0;
        request_.password = u32_at(config, kPasswordFlag) != 0;
        const uint32_t init_off = u32_at(config, kInitialStringOffset);
        const uint32_t init_units = u32_at(config, kInitialStringSize);
        if (init_units != 0)
            request_.initial = text_at(work, init_off, std::min<size_t>(init_units, kTextUnits));
        text_check_ = config[kTextCheckFlag] != 0;
        ask();
    }

    void on_interactive_in(const Request&, std::vector<uint8_t> data) override {
        if (!awaiting_check_) {
            rt::log(LogLevel::Warn, "swkbd: unexpected interactive data (%zu bytes)", data.size());
            return;
        }
        awaiting_check_ = false;
        const uint32_t result = u32_at(data, 0);
        // 0 OK; 2 Prompt: a confirmation dialog the user is taken to accept.
        if (result == 0 || result == 2) {
            close(true, text_);
            return;
        }
        // 1 Bad: message box, then the keyboard again; 3+: silently again.
        request_.check_message = result == 1 ? text_at(data, 4, kTextUnits) : std::u16string{};
        ++request_.rejected;
        ask();
    }

    void ask() {
        const KeyboardResult r = state_->show_keyboard(request_);
        if (!r.accepted) {
            close(false, {});
            return;
        }
        text_ = r.text;
        if (request_.max_length != 0 && text_.size() > request_.max_length)
            text_.resize(request_.max_length);
        if (!text_check_) {
            close(true, text_);
            return;
        }
        const uint64_t size = 2 * std::min(text_.size() + 1, kTextUnits);
        awaiting_check_ = true;
        emit_interactive_out(with_text(&size, sizeof(size), text_));
    }

    void close(bool ok, const std::u16string& text) {
        const uint32_t close_result = ok ? 0 : 1;
        emit_out(with_text(&close_result, sizeof(close_result), text));
        finish();
    }

    std::shared_ptr<AppletState> state_;
    KeyboardRequest request_;
    std::u16string text_;
    bool text_check_ = false;
    bool awaiting_check_ = false;
};

}  // namespace

namespace am_detail {
std::shared_ptr<LibraryApplet> make_software_keyboard(std::shared_ptr<AppletState> state) {
    return std::make_shared<SoftwareKeyboard>(std::move(state));
}
}  // namespace am_detail

std::u16string utf8_to_utf16(const std::string& s) {
    std::u16string out;
    for (size_t i = 0; i < s.size();) {
        const auto b = static_cast<uint8_t>(s[i]);
        uint32_t cp = 0xFFFD;
        size_t len = 1;
        if (b < 0x80) {
            cp = b;
        } else if ((b >> 5) == 0x6 && i + 1 < s.size()) {
            cp = (b & 0x1F) << 6 | (s[i + 1] & 0x3F);
            len = 2;
        } else if ((b >> 4) == 0xE && i + 2 < s.size()) {
            cp = (b & 0x0F) << 12 | (s[i + 1] & 0x3F) << 6 | (s[i + 2] & 0x3F);
            len = 3;
        } else if ((b >> 3) == 0x1E && i + 3 < s.size()) {
            cp = (b & 0x07) << 18 | (s[i + 1] & 0x3F) << 12 | (s[i + 2] & 0x3F) << 6 | (s[i + 3] & 0x3F);
            len = 4;
        }
        i += len;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
    }
    return out;
}

std::string utf16_to_utf8(const std::u16string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        uint32_t cp = s[i];
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            ++i;
        } else if (cp >= 0xD800 && cp < 0xE000) {
            cp = 0xFFFD;  // lone surrogate
        }
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | cp >> 6));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | cp >> 12));
            out.push_back(static_cast<char>(0x80 | (cp >> 6 & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | cp >> 18));
            out.push_back(static_cast<char>(0x80 | (cp >> 12 & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp >> 6 & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

KeyboardResult AppletState::show_keyboard(const KeyboardRequest& request) {
    KeyboardHandler handler;
    {
        std::lock_guard lock(mutex_);
        handler = keyboard_;
    }
    if (handler)
        return handler(request);
    KeyboardResult r;
    if (request.rejected != 0) {
        rt::log(LogLevel::Warn, "swkbd: the game rejected the default text, cancelling");
        r.accepted = false;
        return r;
    }
    const auto fits = [&](const std::u16string& t) {
        return !t.empty() && t.size() >= request.min_length &&
               (request.max_length == 0 || t.size() <= request.max_length);
    };
    r.text = fits(request.initial) ? request.initial : utf8_to_utf16(config_.user_nickname);
    return r;
}

}  // namespace lgp::hle
