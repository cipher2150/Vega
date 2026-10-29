#pragma once

#include <cstddef>

namespace cb {

// Destructive-interference size used for padding shared atomics apart.
// Not std::hardware_destructive_interference_size: GCC reports 256 on aarch64 and
// warns (-Winterference-size) that the value is not ABI-stable across tunings.
#if defined(__APPLE__) && defined(__aarch64__)
inline constexpr std::size_t kCacheLine = 128;  // Apple M1: sysctl hw.cachelinesize == 128
#else
inline constexpr std::size_t kCacheLine = 64;   // Neoverse N1 (Ampere Altra), x86-64
#endif

// Runtime line size reported by the OS, or 0 if unavailable. Used by a test to check
// the compile-time constant is not too small for the machine we run on.
std::size_t runtime_cache_line_size() noexcept;

}  // namespace cb
