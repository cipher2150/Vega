// Kraken checksum cost on a realistic book (BTC/USD: price precision 1, qty precision 8,
// subscribed depth 25), plus CRC32 alone (hardware vs table) to see where the time goes.
#include "book/order_book.hpp"
#include "core/crc32.hpp"
#include "venue/kraken_checksum.hpp"

#include <benchmark/benchmark.h>

#include <array>
#include <memory>
#include <random>

namespace {

std::unique_ptr<cb::OrderBook<64>> make_book() {
  auto book = std::make_unique<cb::OrderBook<64>>();
  std::mt19937_64 rng(3);
  std::uniform_int_distribution<std::int64_t> qty(1, 300'000'000);  // up to 3 BTC
  for (std::int64_t i = 0; i < 25; ++i) {
    book->apply(cb::Side::kBid, cb::Price{(80'000'0 - i) * 10'000'000}, cb::Qty{qty(rng)});
    book->apply(cb::Side::kAsk, cb::Price{(80'000'1 + i) * 10'000'000}, cb::Qty{qty(rng)});
  }
  return book;
}

void BM_KrakenChecksum(benchmark::State& state) {
  const auto book = make_book();
  for (auto _ : state) {
    auto ck = cb::kraken_checksum(*book, 1, 8);
    benchmark::DoNotOptimize(ck);
  }
}
BENCHMARK(BM_KrakenChecksum);

void BM_Crc32_400B(benchmark::State& state) {
  std::array<char, 400> buf{};
  for (std::size_t i = 0; i < buf.size(); ++i) buf[i] = static_cast<char>('0' + i % 10);
  const bool hw = state.range(0) == 1;
  for (auto _ : state) {
    benchmark::DoNotOptimize(buf.data());
    auto c = hw ? cb::crc32(buf) : cb::crc32_table(buf);
    benchmark::DoNotOptimize(c);
  }
  state.SetLabel(hw ? (cb::kCrc32Hardware ? "hardware" : "hardware-unavailable(table)") : "table");
}
BENCHMARK(BM_Crc32_400B)->Arg(1)->Arg(0);

}  // namespace
