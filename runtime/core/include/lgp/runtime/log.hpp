// Minimal runtime log to stderr. Platform backends may redirect it later.
#pragma once

namespace lgp::rt {

enum class LogLevel { Trace = 0, Debug, Info, Warn, Error };

void set_log_level(LogLevel level);
LogLevel log_level();
[[gnu::format(printf, 2, 3)]] void log(LogLevel level, const char* fmt, ...);

}  // namespace lgp::rt
