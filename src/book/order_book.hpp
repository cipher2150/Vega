#pragma once

#include "book/book_side.hpp"
#include "core/fixed_point.hpp"

#include <cstddef>

namespace cb {

// L2 book for one instrument on one venue. Allocation-free: both sides are fixed arrays
// embedded in the object, so a book is created once at startup and reused across resyncs.
template <std::size_t Capacity, std::size_t LinearWindow = 0>
class OrderBook {
 public:
  using Bids = BookSide<Side::kBid, Capacity, LinearWindow>;
  using Asks = BookSide<Side::kAsk, Capacity, LinearWindow>;

  LevelOp apply(Side side, Price price, Qty qty) noexcept {
    return side == Side::kBid ? bids_.set(price, qty) : asks_.set(price, qty);
  }

  void truncate(std::size_t depth) noexcept {
    bids_.truncate(depth);
    asks_.truncate(depth);
  }

  void clear() noexcept {
    bids_.clear();
    asks_.clear();
  }

  // best bid >= best ask on a single venue means our book is wrong (a missed delete),
  // never a real market state, so the caller treats it as an integrity failure.
  [[nodiscard]] bool crossed() const noexcept {
    return !bids_.empty() && !asks_.empty() && bids_.price(0) >= asks_.price(0);
  }

  [[nodiscard]] const Bids& bids() const noexcept { return bids_; }
  [[nodiscard]] const Asks& asks() const noexcept { return asks_; }

 private:
  Bids bids_;
  Asks asks_;
};

}  // namespace cb
