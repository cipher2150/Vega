#pragma once

// Log-linear latency histogram (HdrHistogram-style), in clock ticks.
//
// Values < 32 get exact buckets; above that each power of two is split into 32
// sub-buckets, so any recorded value is known to within ~3% (1/32). Range: 0 .. 2^40 ticks
// (~12 hours at 24 MHz); larger values clamp into the last bucket and still update max.
//
// Concurrency: exactly ONE thread records (the stage's owner); any thread may snapshot.
// Counters are atomics written with relaxed load+store - no read-modify-write is needed
// with a single writer - so recording costs a few plain instructions. A concurrent
// snapshot may be torn across buckets (some updated, some not); that is acceptable for a
// dashboard and for end-of-run reports taken after the writer stops.

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace cb {

class Histogram {
 public:
  static constexpr int kSubBits = 5;
  static constexpr std::uint64_t kSub = 1u << kSubBits;  // 32
  static constexpr int kMaxExp = 40;
  static constexpr std::size_t kBuckets = kSub + (kMaxExp - kSubBits + 1) * kSub;

  static constexpr std::size_t index_of(std::uint64_t v) noexcept {
    if (v < kSub) {
      return static_cast<std::size_t>(v);
    }
    const int e = 63 - std::countl_zero(v);
    if (e > kMaxExp) {
      return kBuckets - 1;
    }
    const int shift = e - kSubBits;
    const std::uint64_t sub = (v >> shift) - kSub;
    return static_cast<std::size_t>(kSub + static_cast<std::uint64_t>(shift) * kSub + sub);
  }

  // Highest value that maps to bucket `idx` (what percentiles report: conservative).
  static constexpr std::uint64_t upper_bound_of(std::size_t idx) noexcept {
    if (idx < kSub) {
      return idx;
    }
    const std::size_t shift = (idx - kSub) / kSub;
    const std::uint64_t sub = (idx - kSub) % kSub;
    const std::uint64_t lower = (kSub + sub) << shift;
    return lower + ((std::uint64_t{1} << shift) - 1);
  }

  void record(std::uint64_t v) noexcept {
    bump(counts_[index_of(v)], 1);
    bump(count_, 1);
    bump(sum_, v);
    if (v > max_.load(std::memory_order_relaxed)) {
      max_.store(v, std::memory_order_relaxed);
    }
  }

  void reset() noexcept {  // writer thread only
    for (auto& c : counts_) {
      c.store(0, std::memory_order_relaxed);
    }
    count_.store(0, std::memory_order_relaxed);
    sum_.store(0, std::memory_order_relaxed);
    max_.store(0, std::memory_order_relaxed);
  }

  struct Snapshot {
    std::array<std::uint64_t, kBuckets> counts{};
    std::uint64_t count = 0;
    std::uint64_t sum = 0;
    std::uint64_t max = 0;

    void merge(const Snapshot& o) noexcept {
      for (std::size_t i = 0; i < kBuckets; ++i) {
        counts[i] += o.counts[i];
      }
      count += o.count;
      sum += o.sum;
      max = std::max(max, o.max);
    }

    // q in [0, 1]. Returns ticks (bucket upper bound, clamped to the observed max).
    [[nodiscard]] std::uint64_t percentile(double q) const noexcept {
      if (count == 0) {
        return 0;
      }
      auto target = static_cast<std::uint64_t>(q * static_cast<double>(count));
      target = std::clamp<std::uint64_t>(target, 1, count);
      std::uint64_t seen = 0;
      for (std::size_t i = 0; i < kBuckets; ++i) {
        seen += counts[i];
        if (seen >= target) {
          return std::min(upper_bound_of(i), max);
        }
      }
      return max;
    }

    [[nodiscard]] double mean() const noexcept {
      return count == 0 ? 0.0 : static_cast<double>(sum) / static_cast<double>(count);
    }
  };

  void snapshot_into(Snapshot& out) const noexcept {
    for (std::size_t i = 0; i < kBuckets; ++i) {
      out.counts[i] = counts_[i].load(std::memory_order_relaxed);
    }
    out.count = count_.load(std::memory_order_relaxed);
    out.sum = sum_.load(std::memory_order_relaxed);
    out.max = max_.load(std::memory_order_relaxed);
  }

 private:
  static void bump(std::atomic<std::uint64_t>& a, std::uint64_t by) noexcept {
    a.store(a.load(std::memory_order_relaxed) + by, std::memory_order_relaxed);
  }

  std::array<std::atomic<std::uint64_t>, kBuckets> counts_{};
  std::atomic<std::uint64_t> count_{0};
  std::atomic<std::uint64_t> sum_{0};
  std::atomic<std::uint64_t> max_{0};
};

}  // namespace cb
