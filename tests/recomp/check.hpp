// Minimal test harness (no dependency): CHECK/CHECK_EQ + registration.
#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace tst {

struct Case {
    const char* name;
    std::function<void()> fn;
};
inline std::vector<Case>& cases() { static std::vector<Case> c; return c; }
inline int& failures() { static int f = 0; return f; }
inline const char*& current() { static const char* c = ""; return c; }

struct Reg {
    Reg(const char* name, std::function<void()> fn) { cases().push_back({name, std::move(fn)}); }
};

inline void fail(const char* file, int line, const std::string& msg) {
    std::fprintf(stderr, "FAIL [%s] %s:%d: %s\n", current(), file, line, msg.c_str());
    ++failures();
}

inline int run(int argc, char** argv) {
    std::string filter = argc > 1 ? argv[1] : "";
    int ran = 0;
    for (auto& c : cases()) {
        if (!filter.empty() && std::string(c.name).find(filter) == std::string::npos) continue;
        current() = c.name;
        int before = failures();
        try {
            c.fn();
        } catch (const std::exception& e) {
            fail(__FILE__, __LINE__, std::string("exception: ") + e.what());
        }
        std::printf("%s %s\n", failures() == before ? "ok  " : "FAIL", c.name);
        ++ran;
    }
    std::printf("%d tests, %d failures\n", ran, failures());
    return failures() ? 1 : 0;
}

}  // namespace tst

#define TEST(name) \
    static void test_##name(); \
    static tst::Reg reg_##name(#name, test_##name); \
    static void test_##name()

#define CHECK(cond) \
    do { if (!(cond)) tst::fail(__FILE__, __LINE__, "CHECK(" #cond ")"); } while (0)
#define CHECK_EQ(a, b) \
    do { auto va_ = (a); auto vb_ = (b); \
         if (!(va_ == vb_)) tst::fail(__FILE__, __LINE__, std::string("CHECK_EQ(" #a ", " #b "): ") + \
             std::to_string(static_cast<long long>(va_)) + " != " + std::to_string(static_cast<long long>(vb_))); } while (0)
#define CHECK_STR_EQ(a, b) \
    do { std::string va_ = (a); std::string vb_ = (b); \
         if (va_ != vb_) tst::fail(__FILE__, __LINE__, std::string("CHECK_STR_EQ(" #a ", " #b "): '") + va_ + "' != '" + vb_ + "'"); } while (0)
#define CHECK_THROWS(expr, ExType) \
    do { bool thrown_ = false; try { (void)(expr); } catch (const ExType&) { thrown_ = true; } \
         if (!thrown_) tst::fail(__FILE__, __LINE__, "CHECK_THROWS(" #expr ") did not throw " #ExType); } while (0)
