// SYNTHETIC parse -> book-update pipeline: decimal strings -> fixed-point -> OrderBook.
// No JSON and no real feed yet; the real parse->update pipeline benchmark replays recorded
// venue data and arrives with the first venue parser (Phase 2) and replay (Phase 4).
//
// The update rank distribution mimics what we measured live on Coinbase BTC-USD
// (DESIGN §5.2): ~85% within 100 levels of best, ~13% within 1000, ~2% deeper.
#include "book/order_book.hpp"
#include "core/decimal.hpp"

#include <benchmark/benchmark.h>

#include <array>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace {

constexpr std::size_t kCap = 8192;
constexpr std::int64_t kTick = 1'000'000;  // 0.01 in 1e-8 units
constexpr std::int64_t kMid = 8'000'000'000'000;

struct RawUpdate {
  cb::Side side;
  std::string price;
  std::string qty;
};

std::vector<RawUpdate> make_updates(std::size_t count) {
  std::mt19937_64 rng(7);
  std::discrete_distribution<int> bucket({85, 13, 2});
  std::uniform_int_distribution<std::int64_t> near(0, 99), mid(100, 999), deep(1000, 3999);
  std::uniform_int_distribution<std::int64_t> size(1, 200'000'000);
  std::bernoulli_distribution remove(0.3), bid(0.5);

  std::vector<RawUpdate> out;
  out.reserve(count);
  std::array<char, cb::kMaxDecimalChars> buf{};
  for (std::size_t i = 0; i < count; ++i) {
    const int b = bucket(rng);
    const std::int64_t rank = b == 0 ? near(rng) : b == 1 ? mid(rng) : deep(rng);
    const bool is_bid = bid(rng);
    const std::int64_t px = is_bid ? kMid - kTick * (1 + rank) : kMid + kTick * (1 + rank);
    const std::int64_t q = remove(rng) ? 0 : size(rng);
    out.push_back({is_bid ? cb::Side::kBid : cb::Side::kAsk,
                   std::string(buf.data(), cb::format_decimal(px, buf)),
                   std::string(buf.data(), cb::format_decimal(q, buf))});
  }
  return out;
}

// Window = linear-scan window of the book search (0 = pure binary search); this
// benchmark picks the default because it uses the measured rank distribution.
template <std::size_t Window>
void BM_SyntheticParseAndApply(benchmark::State& state) {
  static const auto updates = make_updates(1 << 16);
  auto book = std::make_unique<cb::OrderBook<kCap, Window>>();
  for (const auto& u : updates) {  // warm the book to its steady-state size
    book->apply(u.side, cb::Price{cb::parse_decimal(u.price).value},
                cb::Qty{cb::parse_decimal(u.qty).value});
  }
  std::size_t i = 0;
  for (auto _ : state) {
    const auto& u = updates[i++ & (updates.size() - 1)];
    const auto p = cb::parse_decimal(u.price);
    const auto q = cb::parse_decimal(u.qty);
    auto op = book->apply(u.side, cb::Price{p.value}, cb::Qty{q.value});
    benchmark::DoNotOptimize(op);
  }
  state.SetItemsProcessed(state.iterations());
  state.counters["bid_levels"] = static_cast<double>(book->bids().size());
  state.counters["ask_levels"] = static_cast<double>(book->asks().size());
}
BENCHMARK_TEMPLATE(BM_SyntheticParseAndApply, 0);
BENCHMARK_TEMPLATE(BM_SyntheticParseAndApply, 8);
BENCHMARK_TEMPLATE(BM_SyntheticParseAndApply, 16);
BENCHMARK_TEMPLATE(BM_SyntheticParseAndApply, 32);
BENCHMARK_TEMPLATE(BM_SyntheticParseAndApply, 64);

}  // namespace
