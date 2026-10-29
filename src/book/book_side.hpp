#pragma once

// One side of an L2 book: fixed-capacity sorted arrays, best level at the BACK.
//
// Why the back: live data shows most updates land near the top of book (Coinbase BTC-USD:
// 85% within 100 levels of best; docs/DESIGN.md §5.2). Inserting or deleting at index i
// shifts the elements in [i, n), so keeping the best end at n makes typical shifts tiny.
//
// Why one sort direction for both sides: prices are stored as a *key* that is the price
// for bids and the negated price for asks. Keys are always ascending and "better" is
// always "larger key", so a single search/insert path serves both sides.

#include "core/fixed_point.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>

namespace cb {

enum class LevelOp : std::uint8_t {
  kInserted,
  kModified,
  kDeleted,
  kDeleteMissing,  // qty 0 for a price we do not hold (harmless, but counted)
  kFull,           // insert rejected: capacity exhausted (caller must resync)
};

// LinearWindow: how many levels from the best end are scanned linearly before falling
// back to binary search (0 = pure binary search). Linear wins for rank ~0 on small books
// (2.3 ns vs 4.6 ns) but loses past the window, and on the measured Coinbase rank mix pure
// binary search was fastest (bench_pipeline_synthetic: 74.6 ns vs 80-82 ns for windows
// 8..64), so it is the default. Kept as a parameter so replay data can revisit it.
template <Side S, std::size_t Capacity, std::size_t LinearWindow = 0>
class BookSide {
  static_assert(Capacity > 0);

 public:
  static constexpr std::size_t kCapacity = Capacity;

  // Precondition: qty.raw >= 0 (the decimal parser cannot produce negatives).
  LevelOp set(Price price, Qty qty) noexcept {
    assert(qty.raw >= 0);
    const std::int64_t k = key(price);
    const std::size_t i = lower_bound(k);
    const bool found = i < n_ && keys_[i] == k;

    if (qty.is_zero()) {
      if (!found) {
        return LevelOp::kDeleteMissing;
      }
      erase_at(i);
      return LevelOp::kDeleted;
    }
    if (found) {
      qtys_[i] = qty.raw;
      return LevelOp::kModified;
    }
    if (n_ == Capacity) {
      return LevelOp::kFull;
    }
    insert_at(i, k, qty.raw);
    return LevelOp::kInserted;
  }

  // Keeps only the best `depth` levels. Worst levels live at the front, so this shifts
  // the survivors down; it is O(depth) and only runs on depth-limited venues, which
  // normally delete out-of-window levels themselves (DESIGN §5.2).
  void truncate(std::size_t depth) noexcept {
    if (n_ <= depth) {
      return;
    }
    const std::size_t drop = n_ - depth;
    std::copy(keys_.begin() + diff(drop), keys_.begin() + diff(n_), keys_.begin());
    std::copy(qtys_.begin() + diff(drop), qtys_.begin() + diff(n_), qtys_.begin());
    n_ = depth;
  }

  void clear() noexcept { n_ = 0; }

  [[nodiscard]] std::size_t size() const noexcept { return n_; }
  [[nodiscard]] bool empty() const noexcept { return n_ == 0; }

  // rank 0 = best level. Precondition: rank < size().
  [[nodiscard]] Price price(std::size_t rank) const noexcept {
    assert(rank < n_);
    return from_key(keys_[n_ - 1 - rank]);
  }
  [[nodiscard]] Qty qty(std::size_t rank) const noexcept {
    assert(rank < n_);
    return Qty{qtys_[n_ - 1 - rank]};
  }

  // Strictly better on this side: higher bid, lower ask.
  [[nodiscard]] static constexpr bool better(Price a, Price b) noexcept {
    return key(a) > key(b);
  }

 private:
  static constexpr std::int64_t key(Price p) noexcept {
    return S == Side::kBid ? p.raw : -p.raw;
  }
  static constexpr Price from_key(std::int64_t k) noexcept {
    return Price{S == Side::kBid ? k : -k};
  }
  static constexpr std::ptrdiff_t diff(std::size_t i) noexcept {
    return static_cast<std::ptrdiff_t>(i);
  }

  // First index whose key is >= k (n_ if none).
  [[nodiscard]] std::size_t lower_bound(std::int64_t k) const noexcept {
    std::size_t i = n_;
    const std::size_t stop = n_ > LinearWindow ? n_ - LinearWindow : 0;
    while (i > stop && keys_[i - 1] >= k) {
      --i;
    }
    if (i > stop || stop == 0) {
      return i;  // boundary found inside the linear window
    }
    // Every key in the window is >= k, so the boundary lies in [0, stop].
    return static_cast<std::size_t>(
        std::lower_bound(keys_.begin(), keys_.begin() + diff(stop), k) - keys_.begin());
  }

  void insert_at(std::size_t i, std::int64_t k, std::int64_t q) noexcept {
    // copy_backward on int64 lowers to memmove; the moved range is [i, n_), i.e. only
    // the levels *better* than the new one.
    std::copy_backward(keys_.begin() + diff(i), keys_.begin() + diff(n_),
                       keys_.begin() + diff(n_ + 1));
    std::copy_backward(qtys_.begin() + diff(i), qtys_.begin() + diff(n_),
                       qtys_.begin() + diff(n_ + 1));
    keys_[i] = k;
    qtys_[i] = q;
    ++n_;
  }

  void erase_at(std::size_t i) noexcept {
    std::copy(keys_.begin() + diff(i + 1), keys_.begin() + diff(n_), keys_.begin() + diff(i));
    std::copy(qtys_.begin() + diff(i + 1), qtys_.begin() + diff(n_), qtys_.begin() + diff(i));
    --n_;
  }

  // Structure of arrays: the search touches only keys_, so 8 keys share a 64-byte line
  // instead of 4 (key, qty) pairs.
  std::array<std::int64_t, Capacity> keys_{};
  std::array<std::int64_t, Capacity> qtys_{};
  std::size_t n_ = 0;
};

}  // namespace cb
