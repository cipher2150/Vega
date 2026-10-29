// SPSC ring microbenchmarks.
//  - SameThread: push+pop on one core; the instruction cost with no cross-core traffic.
//  - Throughput: producer thread -> consumer (benchmark) thread, items/s.
//  - PingPong:   round trip through two rings; one-way hand-off ~= RTT / 2.
#include "bench_util.hpp"
#include "core/spsc_ring.hpp"

#include <benchmark/benchmark.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

namespace {

struct Event64 {  // same size as the planned Event slot type
  std::uint64_t seq;
  std::array<std::uint64_t, 7> payload;
};
static_assert(sizeof(Event64) == 64);

constexpr std::size_t kRingCap = 1 << 16;

template <class T>
T make(std::uint64_t i) {
  if constexpr (std::is_same_v<T, std::uint64_t>) {
    return i;
  } else {
    T t{};
    t.seq = i;
    return t;
  }
}

template <class T>
std::uint64_t seq_of(const T& t) {
  if constexpr (std::is_same_v<T, std::uint64_t>) {
    return t;
  } else {
    return t.seq;
  }
}

template <class T>
void BM_SpscSameThread(benchmark::State& state) {
  auto ring = std::make_unique<cb::SpscRing<T, kRingCap>>();
  T out{};
  std::uint64_t i = 0;
  for (auto _ : state) {
    (void)ring->try_push(make<T>(i++));
    (void)ring->try_pop(out);
    benchmark::DoNotOptimize(out);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK_TEMPLATE(BM_SpscSameThread, std::uint64_t);
BENCHMARK_TEMPLATE(BM_SpscSameThread, Event64);

template <class T>
void BM_SpscThroughput(benchmark::State& state) {
  const auto count = static_cast<std::uint64_t>(state.range(0));
  auto ring = std::make_unique<cb::SpscRing<T, kRingCap>>();
  for (auto _ : state) {
    std::thread producer([&] {
      cb::bench::place_thread(cb::ThreadRole::kIo, "bench-prod", 1);
      for (std::uint64_t i = 0; i < count;) {
        if (ring->try_push(make<T>(i))) {
          ++i;
        }
      }
    });
    std::uint64_t received = 0;
    std::uint64_t last = 0;
    while (received < count) {
      received += ring->consume([&](const T& v) { last = seq_of(v); });
    }
    benchmark::DoNotOptimize(last);
    producer.join();
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK_TEMPLATE(BM_SpscThroughput, std::uint64_t)->Arg(1 << 22)->UseRealTime()->Unit(benchmark::kMillisecond);
BENCHMARK_TEMPLATE(BM_SpscThroughput, Event64)->Arg(1 << 22)->UseRealTime()->Unit(benchmark::kMillisecond);

// Producer publishes `Batch` items per release store (one venue message = several levels).
// Compared with BM_SpscThroughput this isolates the cost of moving the tail_ cache line
// between cores once per item vs. once per batch.
template <std::size_t Batch>
void BM_SpscThroughputBatched(benchmark::State& state) {
  const auto count = static_cast<std::uint64_t>(state.range(0));
  auto ring = std::make_unique<cb::SpscRing<Event64, kRingCap>>();
  for (auto _ : state) {
    std::thread producer([&] {
      cb::bench::place_thread(cb::ThreadRole::kIo, "bench-prod", 1);
      std::array<Event64, Batch> batch{};
      for (std::uint64_t i = 0; i < count;) {
        for (std::size_t j = 0; j < Batch; ++j) {
          batch[j].seq = i + j;
        }
        if (ring->try_push_batch(batch)) {
          i += Batch;
        }
      }
    });
    std::uint64_t received = 0;
    std::uint64_t last = 0;
    while (received < count) {
      received += ring->consume([&](const Event64& v) { last = v.seq; });
    }
    benchmark::DoNotOptimize(last);
    producer.join();
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK_TEMPLATE(BM_SpscThroughputBatched, 4)->Arg(1 << 22)->UseRealTime()->Unit(benchmark::kMillisecond);
BENCHMARK_TEMPLATE(BM_SpscThroughputBatched, 16)->Arg(1 << 22)->UseRealTime()->Unit(benchmark::kMillisecond);
BENCHMARK_TEMPLATE(BM_SpscThroughputBatched, 64)->Arg(1 << 22)->UseRealTime()->Unit(benchmark::kMillisecond);

void BM_SpscPingPong(benchmark::State& state) {
  using Ring = cb::SpscRing<std::uint64_t, 1024>;
  auto to_echo = std::make_unique<Ring>();
  auto back = std::make_unique<Ring>();
  std::atomic<bool> stop{false};

  std::thread echo([&] {
    cb::bench::place_thread(cb::ThreadRole::kIo, "bench-echo", 1);
    std::uint64_t v = 0;
    while (!stop.load(std::memory_order_relaxed)) {
      if (to_echo->try_pop(v)) {
        while (!back->try_push(v)) {
        }
      }
    }
  });

  std::uint64_t i = 0;
  std::uint64_t v = 0;
  for (auto _ : state) {
    while (!to_echo->try_push(i)) {
    }
    while (!back->try_pop(v)) {
    }
    ++i;
  }
  stop.store(true, std::memory_order_relaxed);
  echo.join();
  benchmark::DoNotOptimize(v);
}
BENCHMARK(BM_SpscPingPong)->UseRealTime();

}  // namespace
