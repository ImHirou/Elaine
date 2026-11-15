#include "base/log.hpp"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <ctime>

namespace base {

static std::atomic<int> g_level{static_cast<int>(LogLevel::Info)};

void set_log_level(LogLevel level) { g_level.store(static_cast<int>(level), std::memory_order_relaxed); }
bool log_enabled(LogLevel level) { return static_cast<int>(level) >= g_level.load(std::memory_order_relaxed); }

void log_write(LogLevel level, const char* fmt, ...) {
    if (!log_enabled(level)) return;
    static const char* const kNames[] = {"DEBUG", "INFO", "WARN", "ERROR"};
    char line[2048];
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    tm tmv;
    localtime_r(&ts.tv_sec, &tmv);
    int n = std::snprintf(line, sizeof line, "[%02d:%02d:%02d.%03ld %s] ", tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
                          ts.tv_nsec / 1000000, kNames[static_cast<int>(level)]);
    va_list ap;
    va_start(ap, fmt);
    int m = std::vsnprintf(line + n, sizeof line - static_cast<unsigned>(n) - 1, fmt, ap);
    va_end(ap);
    if (m < 0) m = 0;
    n += m;
    if (n > static_cast<int>(sizeof line) - 2) n = static_cast<int>(sizeof line) - 2;
    line[n++] = '\n';
    // One write per line keeps lines from different threads intact without a lock.
    std::fwrite(line, 1, static_cast<unsigned>(n), stderr);
}

}  // namespace base
