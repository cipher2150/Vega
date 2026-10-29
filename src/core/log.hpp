#pragma once

// Minimal printf-style logging to stderr. Cold paths only (connects, errors, resyncs):
// the hot path never formats text.

namespace cb {

enum class LogLevel { kInfo, kWarn, kError };

void log(LogLevel level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

}  // namespace cb

#define CB_INFO(...) ::cb::log(::cb::LogLevel::kInfo, __VA_ARGS__)
#define CB_WARN(...) ::cb::log(::cb::LogLevel::kWarn, __VA_ARGS__)
#define CB_ERROR(...) ::cb::log(::cb::LogLevel::kError, __VA_ARGS__)
