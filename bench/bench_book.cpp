// Book update cost as a function of book size N and the rank r (distance from best) of the
// touched level. This is the data that decides flat-array vs. ladder (DESIGN §5.2):
// the array's insert/delete cost grows with r because levels [best .. r] are shifted.
//
// Arguments: {N levels per side, rank r}. Window = linear-scan window (0 = pure binary).
#include "book/book_side.hpp"

#include <benchmark/benchmark.h>

#include <cstdint>
#include <memory>

namespace {

constexpr std::size_t kCap = 32768;  // > Coinbase's ~21k levels/side seen live
constexpr std::int64_t kTop = 10'000'000;

// Bids at kTop, kTop-2, kTop-4, ... so odd prices are free slots between levels.
template <std::size_t Window>
std::unique_ptr<cb::BookSide<cb::Side::kBid, kCap, Window>> make_side(std::int64_t n) {
  auto side = std::make_unique<cb::BookSide<cb::Side::kBid, kCap, Window>>();
  for (std::int64_t i = 0; i < n; ++i) {
    side->set(cb::Price{kTop - 2 * i}, cb::Qty{100 + i});
  }
  return side;
}

// Existing level at rank r: changes qty only, no shifting. Measures search cost.
template <std::size_t Window>
void BM_Modify(benchmark::State& state) {
  auto side = make_side<Window>(state.range(0));
  const cb::Price p{kTop - 2 * state.range(1)};
  std::int64_t q = 1;
  for (auto _ : state) {
    auto op = side->set(p, cb::Qty{q++});
    benchmark::DoNotOptimize(op);
  }
}

// New level just behind rank r, then removed again: two shifts of r+1 elements.
// Reported time is per (insert + delete) pair.
template <std::size_t Window>
void BM_InsertDelete(benchmark::State& state) {
  auto side = make_side<Window>(state.range(0));
  const cb::Price p{kTop - 2 * state.range(1) - 1};
  for (auto _ : state) {
    auto a = side->set(p, cb::Qty{5});
    auto b = side->set(p, cb::Qty{0});
    benchmark::DoNotOptimize(a);
    benchmark::DoNotOptimize(b);
  }
}

void Shapes(benchmark::Benchmark* b) {
  for (std::int64_t n : {25, 400, 4096, 20000}) {
    for (std::int64_t r : {0, 10, 100, 1000, 19999}) {
      if (r < n) {
        b->Args({n, r});
      }
    }
    if (n != 20000) {
      b->Args({n, n - 1});  // worst case: touch the deepest level
    }
  }
  b->ArgNames({"N", "rank"});
}

BENCHMARK_TEMPLATE(BM_Modify, 32)->Apply(Shapes);
BENCHMARK_TEMPLATE(BM_Modify, 0)->Apply(Shapes);
BENCHMARK_TEMPLATE(BM_InsertDelete, 32)->Apply(Shapes);
BENCHMARK_TEMPLATE(BM_InsertDelete, 0)->Apply(Shapes);

}  // namespace
