#include "book/order_book.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <random>

namespace cb {
namespace {

constexpr Price px(std::int64_t v) { return Price{v}; }
constexpr Qty qty(std::int64_t v) { return Qty{v}; }

TEST(BookSide, BidsBestIsHighestAsksBestIsLowest) {
  auto book = std::make_unique<OrderBook<16>>();
  for (std::int64_t p : {100, 105, 95, 103}) {
    EXPECT_EQ(book->apply(Side::kBid, px(p), qty(1)), LevelOp::kInserted);
    EXPECT_EQ(book->apply(Side::kAsk, px(p + 100), qty(1)), LevelOp::kInserted);
  }
  const auto& b = book->bids();
  ASSERT_EQ(b.size(), 4u);
  EXPECT_EQ(b.price(0), px(105));
  EXPECT_EQ(b.price(1), px(103));
  EXPECT_EQ(b.price(2), px(100));
  EXPECT_EQ(b.price(3), px(95));
  const auto& a = book->asks();
  EXPECT_EQ(a.price(0), px(195));
  EXPECT_EQ(a.price(3), px(205));
}

TEST(BookSide, ModifyDeleteAndDeleteMissing) {
  BookSide<Side::kBid, 8> side;
  EXPECT_EQ(side.set(px(10), qty(5)), LevelOp::kInserted);
  EXPECT_EQ(side.set(px(10), qty(7)), LevelOp::kModified);  // absolute size, not a delta
  EXPECT_EQ(side.qty(0), qty(7));
  EXPECT_EQ(side.set(px(11), qty(0)), LevelOp::kDeleteMissing);
  EXPECT_EQ(side.set(px(10), qty(0)), LevelOp::kDeleted);
  EXPECT_TRUE(side.empty());
}

TEST(BookSide, FullRejectsInsertButAllowsModifyAndDelete) {
  BookSide<Side::kAsk, 3> side;
  for (std::int64_t p : {1, 2, 3}) {
    ASSERT_EQ(side.set(px(p), qty(1)), LevelOp::kInserted);
  }
  EXPECT_EQ(side.set(px(4), qty(1)), LevelOp::kFull);
  EXPECT_EQ(side.set(px(2), qty(9)), LevelOp::kModified);
  EXPECT_EQ(side.set(px(3), qty(0)), LevelOp::kDeleted);
  EXPECT_EQ(side.set(px(4), qty(1)), LevelOp::kInserted);
}

TEST(BookSide, TruncateKeepsBestLevels) {
  BookSide<Side::kAsk, 16> side;
  for (std::int64_t p = 1; p <= 10; ++p) {
    side.set(px(p), qty(p));
  }
  side.truncate(3);
  ASSERT_EQ(side.size(), 3u);
  EXPECT_EQ(side.price(0), px(1));
  EXPECT_EQ(side.price(2), px(3));
  EXPECT_EQ(side.qty(2), qty(3));
  side.truncate(5);  // no-op when already within depth
  EXPECT_EQ(side.size(), 3u);
}

TEST(OrderBook, CrossedDetection) {
  auto book = std::make_unique<OrderBook<8>>();
  EXPECT_FALSE(book->crossed());
  book->apply(Side::kBid, px(100), qty(1));
  book->apply(Side::kAsk, px(101), qty(1));
  EXPECT_FALSE(book->crossed());
  book->apply(Side::kBid, px(101), qty(1));  // locked: bid == ask
  EXPECT_TRUE(book->crossed());
  book->apply(Side::kBid, px(101), qty(0));
  EXPECT_FALSE(book->crossed());
}

// Differential test against an obviously-correct reference (std::map is fine in tests).
// Run for several LinearWindow values so both the linear scan and the binary-search
// fallback paths are exercised with books larger than the window.
template <std::size_t Window>
void run_differential(std::uint64_t seed) {
  constexpr std::size_t kCap = 512;
  auto bids = std::make_unique<BookSide<Side::kBid, kCap, Window>>();
  auto asks = std::make_unique<BookSide<Side::kAsk, kCap, Window>>();
  std::map<std::int64_t, std::int64_t, std::greater<>> ref_bids;  // best first
  std::map<std::int64_t, std::int64_t> ref_asks;                  // best first

  std::mt19937_64 rng(seed);
  std::uniform_int_distribution<std::int64_t> price_dist(1, 400);  // dense -> many collisions
  std::uniform_int_distribution<std::int64_t> qty_dist(1, 1'000'000);
  std::bernoulli_distribution is_delete(0.35);

  auto check_equal = [&](const auto& side, const auto& ref) {
    ASSERT_EQ(side.size(), ref.size());
    std::size_t rank = 0;
    for (const auto& [p, q] : ref) {
      ASSERT_EQ(side.price(rank), px(p)) << "rank " << rank;
      ASSERT_EQ(side.qty(rank), qty(q)) << "rank " << rank;
      ++rank;
    }
  };

  for (int i = 0; i < 100'000; ++i) {
    const std::int64_t p = price_dist(rng);
    const std::int64_t q = is_delete(rng) ? 0 : qty_dist(rng);
    if (i % 2 == 0) {
      bids->set(px(p), qty(q));
      if (q == 0) ref_bids.erase(p); else ref_bids[p] = q;
    } else {
      asks->set(px(p), qty(q));
      if (q == 0) ref_asks.erase(p); else ref_asks[p] = q;
    }
    if (i % 997 == 0) {
      check_equal(*bids, ref_bids);
      check_equal(*asks, ref_asks);
    }
  }
  check_equal(*bids, ref_bids);
  check_equal(*asks, ref_asks);
  EXPECT_GT(ref_bids.size(), 64u) << "book must exceed the linear window to test fallback";
}

TEST(BookSide, MatchesReferencePureBinarySearch) { run_differential<0>(1); }
TEST(BookSide, MatchesReferenceSmallWindow) { run_differential<4>(2); }
TEST(BookSide, MatchesReferenceDefaultWindow) { run_differential<32>(3); }

}  // namespace
}  // namespace cb
