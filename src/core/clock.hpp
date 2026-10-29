#pragma once

// Monotonic timestamps for latency measurement.
//
// On arm64 we read the architectural generic timer (CNTVCT_EL0) directly: it is a single
// system-wide counter, so values taken on different cores are directly comparable (this
// is what makes cross-thread "queue" latency measurable). Its frequency is low:
// 24 MHz on Apple M1 (measured), ~25 MHz on Ampere Altra, i.e. ~40 ns per tick. Stages
// shorter than a few ticks are therefore reported as batch averages (sum/count), never as
// fake-precise percentiles (docs/DESIGN.md §7).
//
// Elsewhere we fall back to std::chrono::steady_clock in nanoseconds.

#include <chrono>
#include <cstdint>

namespace cb::clock {

#if defined(__aarch64__)

inline std::uint64_t ticks() noexcept {
  std::uint64_t v;
  // isb: stops the counter read being speculated earlier than surrounding instructions,
  // otherwise a "start" stamp could be taken before the work it is meant to precede.
  asm volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(v) : : "memory");
  return v;
}

inline std::uint64_t ticks_per_second() noexcept {
  std::uint64_t f;
  asm volatile("mrs %0, cntfrq_el0" : "=r"(f));
  return f;
}

#else

inline std::uint64_t ticks() noexcept {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

inline std::uint64_t ticks_per_second() noexcept { return 1'000'000'000; }

#endif

inline double ticks_to_ns(double ticks) noexcept {
  static const double ns_per_tick = 1e9 / static_cast<double>(ticks_per_second());
  return ticks * ns_per_tick;
}

// Wall clock (CLOCK_REALTIME) in ns: only for comparing against exchange timestamps,
// never for latency (it can jump when NTP/chrony steps the clock).
inline std::int64_t wall_ns() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace cb::clock
