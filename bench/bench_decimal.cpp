// Fixed-point decimal parsing vs. the floating-point baseline it replaces.
#include "core/decimal.hpp"

#include <benchmark/benchmark.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

namespace {

// Strings shaped like the probe data: BTC prices with 0-2 decimals, sizes with up to 8,
// some with Kraken-style trailing zeros. Random order defeats the branch predictor
// learning a single length.
std::vector<std::string> make_corpus() {
  std::mt19937_64 rng(42);
  std::uniform_int_distribution<std::int64_t> price_cents(7'000'000, 9'000'000);  // 70000.00..90000.00
  std::uniform_int_distribution<std::int64_t> size_units(1, 500'000'000);          // 1e-8 .. 5
  std::uniform_int_distribution<int> kind(0, 3);
  std::vector<std::string> out;
  out.reserve(4096);
  std::array<char, cb::kMaxDecimalChars> buf{};
  for (int i = 0; i < 4096; ++i) {
    std::int64_t fixed = 0;
    switch (kind(rng)) {
      case 0: fixed = price_cents(rng) * 1'000'000; break;          // "80006.31"
      case 1: fixed = (price_cents(rng) / 10) * 10'000'000; break;  // "80006.3"
      default: fixed = size_units(rng); break;                      // "0.10705196"
    }
    std::string s(buf.data(), cb::format_decimal(fixed, buf));
    if (i % 5 == 0 && s.find('.') != std::string::npos) {
      s.append(8 - (s.size() - s.find('.') - 1), '0');  // Kraken pads qty to 8 dp
    }
    out.push_back(std::move(s));
  }
  return out;
}

const std::vector<std::string>& corpus() {
  static const auto c = make_corpus();
  return c;
}

void BM_ParseDecimal_Corpus(benchmark::State& state) {
  const auto& c = corpus();
  std::size_t i = 0;
  for (auto _ : state) {
    auto r = cb::parse_decimal(c[i++ & 4095]);
    benchmark::DoNotOptimize(r);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ParseDecimal_Corpus);

void BM_ParseDecimal_Price(benchmark::State& state) {
  std::string s = "80006.3";
  for (auto _ : state) {
    benchmark::DoNotOptimize(s.data());  // stop the compiler constant-folding the parse
    auto r = cb::parse_decimal(s);
    benchmark::DoNotOptimize(r);
  }
}
BENCHMARK(BM_ParseDecimal_Price);

void BM_ParseDecimal_Qty8dp(benchmark::State& state) {
  std::string s = "0.10705196";
  for (auto _ : state) {
    benchmark::DoNotOptimize(s.data());
    auto r = cb::parse_decimal(s);
    benchmark::DoNotOptimize(r);
  }
}
BENCHMARK(BM_ParseDecimal_Qty8dp);

// Baseline: what "just use strtod" costs, including the scale-and-round step needed to get
// back to an integer. It is also inexact in general; that, not speed, is the main reason
// we do not use it (docs/INTERVIEW_NOTES.md).
void BM_Strtod_Corpus(benchmark::State& state) {
  const auto& c = corpus();
  std::size_t i = 0;
  for (auto _ : state) {
    const double d = std::strtod(c[i++ & 4095].c_str(), nullptr);
    auto fixed = std::llround(d * 1e8);
    benchmark::DoNotOptimize(fixed);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Strtod_Corpus);

}  // namespace
