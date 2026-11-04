#include "lgp/runtime/log.hpp"

#include <atomic>
#include <cstdarg>
#include <cstdio>

namespace lgp::rt {
namespace {
std::atomic<LogLevel> g_level{LogLevel::Info};
constexpr const char* kNames[] = {"trace", "debug", "info", "warn", "error"};
}  // namespace

void set_log_level(LogLevel level) { g_level.store(level, std::memory_order_relaxed); }

LogLevel log_level() { return g_level.load(std::memory_order_relaxed); }

void log(LogLevel level, const char* fmt, ...) {
    if (level < log_level())
        return;
    char line[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    std::fprintf(stderr, "[lgp %s] %s\n", kNames[static_cast<int>(level)], line);
}

}  // namespace lgp::rt
