// Signals, USDT->USD conversion and the consolidated cross-venue book.
#include "core/histogram.hpp"
#include "core/rfc3339.hpp"
#include "core/triple_buffer.hpp"
#include "engine/signals.hpp"
#include "support/rig.hpp"

#include <gtest/gtest.h>

#include <random>
#include <thread>

namespace cb {
namespace {

TEST(Signals, Microprice) {
  // Equal sizes -> plain mid; heavier bid -> leans toward the ask.
  EXPECT_EQ(signals::microprice(100, 1, 102, 1), 101);
  EXPECT_EQ(signals::microprice(100'00000000, 3'00000000, 101'00000000, 1'00000000), 100'75000000);
  EXPECT_EQ(signals::microprice(100, 0, 102, 0), 101);
}

TEST(Signals, ImbalanceAndSpread) {
  EXPECT_EQ(signals::imbalance_e8(3, 1), 50'000'000);  // (3-1)/(3+1) = 0.5
  EXPECT_EQ(signals::imbalance_e8(0, 5), -100'000'000);
  EXPECT_EQ(signals::imbalance_e8(0, 0), 0);
  // 1 unit spread at mid 10000 -> 1 bp -> 100 (bp x 100)
  EXPECT_EQ(signals::spread_bp_e2(99'995'000'000, 100'005'000'000), 100);
}

TEST(Signals, FastConvertMatchesInt128Reference) {
  std::mt19937_64 rng(1);
  std::uniform_int_distribution<std::int64_t> price(0, 10'000'000'000'000);  // up to $100k
  std::uniform_int_distribution<std::int64_t> rate(98'000'000, 102'000'000);  // 0.98 .. 1.02
  for (int i = 0; i < 1'000'000; ++i) {
    const auto p = price(rng);
    const auto r = rate(rng);
    ASSERT_EQ(signals::convert(p, r), signals::convert_reference(p, r)) << p << " " << r;
  }
}

std::string kraken_fx(const char* bid, const char* ask) {
  return std::string(R"({"channel":"book","type":"snapshot","data":[{"symbol":"USDT/USD","bids":[{"price":)") +
         bid + R"(,"qty":1000.00000000}],"asks":[{"price":)" + ask +
         R"(,"qty":1000.00000000}],"timestamp":"2026-10-29T13:11:48.466869Z"}]})";
}

// Single-level test books: relax Coinbase's horizon minimum so they count as healthy.
AppConfig small_books() {
  AppConfig c;
  c.venue(VenueId::kCoinbase).min_levels = 1;
  return c;
}

TEST(Consolidated, MergesVenuesAndConvertsUsdtAtLiveRate) {
  test::Rig rig({VenueId::kCoinbase, VenueId::kKraken, VenueId::kOkx}, small_books());
  rig.connect(VenueId::kKraken, 1);
  rig.connect(VenueId::kCoinbase, 1);
  rig.connect(VenueId::kOkx, 1);
  // USDT/USD mid = (0.99980 + 1.00000) / 2 = 0.9999
  rig.feed(VenueId::kKraken, kraken_fx("0.99980", "1.00000"));
  ASSERT_TRUE(rig.engine().snapshot().fx_live);
  EXPECT_EQ(rig.engine().snapshot().fx_usdt_usd_e8, 99'990'000);

  rig.feed(VenueId::kOkx, R"({"arg":{"channel":"books","instId":"BTC-USDT"},"action":"snapshot","data":[{"asks":[["80010","1","0","1"]],"bids":[["80000","2","0","1"]],"ts":"1","checksum":0,"seqId":5,"prevSeqId":-1}]})");
  rig.feed(VenueId::kCoinbase, R"({"channel":"l2_data","timestamp":"2026-10-29T13:11:48Z","sequence_num":0,"events":[{"type":"snapshot","product_id":"BTC-USD","updates":[{"side":"bid","event_time":"x","price_level":"79995","new_quantity":"1"},{"side":"offer","event_time":"x","price_level":"80003","new_quantity":"1"}]}]})");

  const ConsolidatedSummary& cs = rig.engine().snapshot().consolidated[idx(InstrumentId::kBtcUsd)];
  ASSERT_EQ(cs.n_bids, 2);
  ASSERT_EQ(cs.n_asks, 2);
  // OKX 80000 USDT x 0.9999 = 79992.0 USD -> below Coinbase's 79995 bid.
  EXPECT_EQ(cs.bids[0].venue, VenueId::kCoinbase);
  EXPECT_EQ(cs.bids[1].venue, VenueId::kOkx);
  EXPECT_EQ(cs.bids[1].price, 7'999'200'000'000);
  // OKX ask 80010 x 0.9999 = 80001.999 -> beats Coinbase's 80003.
  EXPECT_EQ(cs.asks[0].venue, VenueId::kOkx);
  EXPECT_EQ(cs.asks[0].price, 8'000'199'900'000);
  EXPECT_EQ(cs.spread, 8'000'199'900'000 - 7'999'500'000'000);

  // FX feed dies -> USDT venues leave the consolidated book instead of using a stale rate.
  rig.disconnect(VenueId::kKraken);
  EXPECT_FALSE(rig.engine().snapshot().fx_live);
  const ConsolidatedSummary& after = rig.engine().snapshot().consolidated[idx(InstrumentId::kBtcUsd)];
  EXPECT_EQ(after.n_bids, 1);
  EXPECT_EQ(after.bids[0].venue, VenueId::kCoinbase);
  EXPECT_EQ(after.venues_mask, 1u << idx(VenueId::kCoinbase));
}

TEST(Consolidated, StaleVenueIsExcluded) {
  test::Rig rig({VenueId::kCoinbase}, small_books());
  rig.connect(VenueId::kCoinbase, 1);
  rig.feed(VenueId::kCoinbase, R"({"channel":"l2_data","timestamp":"2026-10-29T13:11:48Z","sequence_num":0,"events":[{"type":"snapshot","product_id":"BTC-USD","updates":[{"side":"bid","event_time":"x","price_level":"100","new_quantity":"1"},{"side":"offer","event_time":"x","price_level":"101","new_quantity":"1"}]}]})");
  EXPECT_EQ(rig.engine().snapshot().consolidated[0].n_bids, 1);
  rig.disconnect(VenueId::kCoinbase);
  EXPECT_EQ(rig.engine().snapshot().consolidated[0].n_bids, 0);
}

TEST(Rfc3339, ParsesExchangeFormats) {
  EXPECT_EQ(parse_rfc3339_ns("1970-01-01T00:00:00Z"), 0);
  EXPECT_EQ(parse_rfc3339_ns("1970-01-01T00:00:01.5Z"), 1'500'000'000);
  // Coinbase nanoseconds and Kraken microseconds.
  EXPECT_EQ(parse_rfc3339_ns("2026-10-29T13:11:48.925743005Z"), 1'793'279'508'925'743'005);
  EXPECT_EQ(parse_rfc3339_ns("2026-10-29T13:11:48.466869Z"), 1'793'279'508'466'869'000);
  EXPECT_EQ(parse_rfc3339_ns("2026-10-29 13:11:48Z"), kInvalidTime);
  EXPECT_EQ(parse_rfc3339_ns("2026-10-29T13:11:48+01:00"), kInvalidTime);
}

TEST(Histogram, BucketsAreWithinThreePercent) {
  for (std::uint64_t v : {0ull, 1ull, 31ull, 32ull, 33ull, 1000ull, 123456ull, 1ull << 39}) {
    const auto i = Histogram::index_of(v);
    const auto hi = Histogram::upper_bound_of(i);
    EXPECT_GE(hi, v);
    EXPECT_LE(static_cast<double>(hi - v), static_cast<double>(v) / 32.0 + 1.0) << v;
  }
}

TEST(Histogram, Percentiles) {
  auto h = std::make_unique<Histogram>();
  for (std::uint64_t v = 1; v <= 1000; ++v) h->record(v);
  auto s = std::make_unique<Histogram::Snapshot>();
  h->snapshot_into(*s);
  EXPECT_EQ(s->count, 1000u);
  EXPECT_EQ(s->max, 1000u);
  EXPECT_NEAR(static_cast<double>(s->percentile(0.5)), 500.0, 500.0 / 32 + 1);
  EXPECT_NEAR(static_cast<double>(s->percentile(0.99)), 990.0, 990.0 / 32 + 1);
  EXPECT_EQ(s->percentile(1.0), 1000u);
  EXPECT_DOUBLE_EQ(s->mean(), 500.5);
}

struct Big {
  std::uint64_t seq;
  std::array<std::uint64_t, 63> copy;  // every word equals seq: a torn read is detectable
};

TEST(TripleBuffer, ReaderOnlySeesCompleteNewestValues) {
  auto tb = std::make_unique<TripleBuffer<Big>>();
  constexpr std::uint64_t kN = 200'000;
  std::thread writer([&] {
    for (std::uint64_t i = 1; i <= kN; ++i) {
      Big& b = tb->write_slot();
      b.seq = i;
      b.copy.fill(i);
      tb->publish();
    }
  });
  std::uint64_t last = 0;
  bool ok = true;
  while (last < kN) {
    if (!tb->update()) continue;
    const Big& b = tb->read_slot();
    for (const auto w : b.copy) ok &= w == b.seq;
    ok &= b.seq > last;  // never goes backwards, never repeats a stale value as fresh
    last = b.seq;
  }
  writer.join();
  EXPECT_TRUE(ok);
}

}  // namespace
}  // namespace cb
