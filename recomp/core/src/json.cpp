#include "lgp/recomp/json.hpp"

#include <cstdio>

namespace lgp::recomp {

std::string json_escape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 2);
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    o += buf;
                } else {
                    o.push_back(static_cast<char>(c));
                }
        }
    }
    return o;
}

void JsonWriter::sep() {
    if (after_key_) { after_key_ = false; return; }
    if (first_.empty()) return;
    if (!first_.back()) out_ += ",";
    first_.back() = false;
}

JsonWriter& JsonWriter::begin_object() { sep(); out_ += "{"; first_.push_back(true); return *this; }
JsonWriter& JsonWriter::end_object() { out_ += "}"; first_.pop_back(); return *this; }
JsonWriter& JsonWriter::begin_array() { sep(); out_ += "["; first_.push_back(true); return *this; }
JsonWriter& JsonWriter::end_array() { out_ += "]"; first_.pop_back(); return *this; }
JsonWriter& JsonWriter::key(const std::string& k) {
    sep();
    out_ += "\"" + json_escape(k) + "\":";
    after_key_ = true;
    return *this;
}
JsonWriter& JsonWriter::value(const std::string& s) { sep(); out_ += "\"" + json_escape(s) + "\""; return *this; }
JsonWriter& JsonWriter::value(uint64_t v) { sep(); out_ += std::to_string(v); return *this; }
JsonWriter& JsonWriter::value(int64_t v) { sep(); out_ += std::to_string(v); return *this; }
JsonWriter& JsonWriter::value(bool b) { sep(); out_ += b ? "true" : "false"; return *this; }
JsonWriter& JsonWriter::null() { sep(); out_ += "null"; return *this; }
JsonWriter& JsonWriter::hex(uint64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(v));
    return value(std::string(buf));
}

}  // namespace lgp::recomp
