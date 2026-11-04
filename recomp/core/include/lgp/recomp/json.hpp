// Tiny JSON writer (no parser, no dependency). Output never contains code bytes.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lgp::recomp {

class JsonWriter {
public:
    JsonWriter& begin_object();
    JsonWriter& end_object();
    JsonWriter& begin_array();
    JsonWriter& end_array();
    JsonWriter& key(const std::string& k);
    JsonWriter& value(const std::string& s);
    JsonWriter& value(const char* s) { return value(std::string(s)); }
    JsonWriter& value(uint64_t v);
    JsonWriter& value(int64_t v);
    JsonWriter& value(int v) { return value(static_cast<int64_t>(v)); }
    JsonWriter& value(unsigned v) { return value(static_cast<uint64_t>(v)); }
    JsonWriter& value(bool b);
    JsonWriter& hex(uint64_t v);   // "0x..." string, easier to read than decimal addresses
    JsonWriter& null();
    template <class T> JsonWriter& kv(const std::string& k, const T& v) { key(k); return value(v); }
    JsonWriter& khex(const std::string& k, uint64_t v) { key(k); return hex(v); }
    std::string str() const { return out_; }

private:
    void sep();
    std::string out_;
    std::vector<bool> first_;   // per nesting level: no element written yet
    bool after_key_ = false;
};

std::string json_escape(const std::string& s);

}  // namespace lgp::recomp
