#include "core/log.hpp"

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace cb {

void log(LogLevel level, const char* fmt, ...) {
  static std::mutex mu;  // keeps lines from different threads from interleaving
  const char* tag = level == LogLevel::kInfo ? "INFO" : level == LogLevel::kWarn ? "WARN" : "ERROR";

  std::timespec ts{};
  std::timespec_get(&ts, TIME_UTC);
  std::tm tm{};
  gmtime_r(&ts.tv_sec, &tm);

  std::va_list args;
  va_start(args, fmt);
  {
    const std::lock_guard lock(mu);
    std::fprintf(stderr, "%02d:%02d:%02d.%03ld %-5s ", tm.tm_hour, tm.tm_min, tm.tm_sec,
                 ts.tv_nsec / 1'000'000, tag);
    std::vfprintf(stderr, fmt, args);
    std::fputc('\n', stderr);
  }
  va_end(args);
}

}  // namespace cb
