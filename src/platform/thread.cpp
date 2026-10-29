#include "platform/thread.hpp"

#include "platform/cacheline.hpp"

#include <pthread.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdio>

#if defined(__APPLE__)
#include <pthread/qos.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#elif defined(__linux__)
#include <sched.h>
#endif

namespace cb {

namespace {

// pthread names are limited (15 chars + NUL on Linux), so copy into a bounded buffer.
std::array<char, 16> bounded_name(std::string_view name) noexcept {
  std::array<char, 16> buf{};
  const auto n = std::min(name.size(), buf.size() - 1);
  std::copy_n(name.data(), n, buf.data());
  return buf;
}

}  // namespace

PlacementResult set_current_thread_role(ThreadRole role, std::string_view name,
                                        std::optional<int> cpu) noexcept {
  PlacementResult result;
  const auto buf = bounded_name(name);

#if defined(__APPLE__)
  result.named = pthread_setname_np(buf.data()) == 0;
  (void)cpu;  // no hard affinity on macOS
  const qos_class_t qos = (role == ThreadRole::kBook || role == ThreadRole::kIo)
                              ? QOS_CLASS_USER_INTERACTIVE
                              : QOS_CLASS_UTILITY;
  result.qos_applied = pthread_set_qos_class_self_np(qos, 0) == 0;
#elif defined(__linux__)
  (void)role;
  result.named = pthread_setname_np(pthread_self(), buf.data()) == 0;
  if (cpu.has_value() && *cpu >= 0) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<unsigned>(*cpu), &set);
    result.pinned = pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
  }
#else
  (void)role;
  (void)cpu;
#endif
  return result;
}

std::size_t runtime_cache_line_size() noexcept {
#if defined(__APPLE__)
  std::size_t value = 0;
  std::size_t len = sizeof(value);
  if (sysctlbyname("hw.cachelinesize", &value, &len, nullptr, 0) == 0) {
    return value;
  }
  return 0;
#elif defined(__linux__)
  // glibc's sysconf(_SC_LEVEL1_DCACHE_LINESIZE) returns 0 on many aarch64 systems,
  // so prefer sysfs and fall back to sysconf.
  if (std::FILE* f = std::fopen(
          "/sys/devices/system/cpu/cpu0/cache/index0/coherency_line_size", "r")) {
    unsigned long value = 0;
    const int matched = std::fscanf(f, "%lu", &value);
    std::fclose(f);
    if (matched == 1 && value > 0) {
      return value;
    }
  }
  const long value = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
  return value > 0 ? static_cast<std::size_t>(value) : 0;
#else
  return 0;
#endif
}

}  // namespace cb
