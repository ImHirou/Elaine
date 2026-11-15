// Minimal synchronous logger. Logging is a cold path: never call it per packet or per entity.
#pragma once

namespace base {

enum class LogLevel { Debug, Info, Warn, Error };

void set_log_level(LogLevel level);
void log_write(LogLevel level, const char* fmt, ...) __attribute__((format(printf, 2, 3), cold));
bool log_enabled(LogLevel level);

}  // namespace base

#define LOG_DEBUG(...)                                                         \
    do {                                                                       \
        if (__builtin_expect(::base::log_enabled(::base::LogLevel::Debug), 0)) \
            ::base::log_write(::base::LogLevel::Debug, __VA_ARGS__);           \
    } while (0)
#define LOG_INFO(...) ::base::log_write(::base::LogLevel::Info, __VA_ARGS__)
#define LOG_WARN(...) ::base::log_write(::base::LogLevel::Warn, __VA_ARGS__)
#define LOG_ERROR(...) ::base::log_write(::base::LogLevel::Error, __VA_ARGS__)
