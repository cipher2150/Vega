// Proof of "zero heap allocations after warm-up" for the hot path.
//
// This binary replaces global operator new/delete with counting versions. It replays the
// golden recording (real Coinbase/Kraken/OKX/Bybit frames): the first part warms up every
// buffer (parser capacity, books, staging areas), then counting is enabled and the rest of
// the session - parse -> normalize -> SPSC ring -> book update -> integrity checks ->
// consolidated book -> signals -> triple-buffer publish - must allocate nothing.
//
// Scope, stated honestly: operator new does not see malloc() called directly by C code,
// and the live socket/TLS layer is not exercised here (it is measured live via the
// per-thread counters on the dashboard).
#include "record/recording.hpp"
#include "record/replay.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <new>

namespace {

std::atomic<bool> g_counting{false};
std::atomic<std::uint64_t> g_allocs{0};

void* counted_alloc(std::size_t n) {
  if (g_counting.load(std::memory_order_relaxed)) {
    g_allocs.fetch_add(1, std::memory_order_relaxed);
  }
  if (void* p = std::malloc(n == 0 ? 1 : n)) return p;
  throw std::bad_alloc();
}

void* counted_aligned(std::size_t n, std::align_val_t a) {
  if (g_counting.load(std::memory_order_relaxed)) {
    g_allocs.fetch_add(1, std::memory_order_relaxed);
  }
  const auto al = static_cast<std::size_t>(a);
  if (void* p = std::aligned_alloc(al, (n + al - 1) / al * al)) return p;
  throw std::bad_alloc();
}

}  // namespace

void* operator new(std::size_t n) { return counted_alloc(n); }
void* operator new[](std::size_t n) { return counted_alloc(n); }
void* operator new(std::size_t n, std::align_val_t a) { return counted_aligned(n, a); }
void* operator new[](std::size_t n, std::align_val_t a) { return counted_aligned(n, a); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }

namespace cb {
namespace {

TEST(Allocations, CountingHookWorks) {
  g_allocs = 0;
  g_counting = true;
  auto p = std::make_unique<int>(42);  // control: this must be counted
  g_counting = false;
  EXPECT_EQ(g_allocs.load(), 1u);
}

TEST(Allocations, ZeroAfterWarmUpOnRecordedSession) {
  Recording rec;
  std::string err;
  ASSERT_TRUE(rec.load(std::string(CB_TEST_DATA_DIR) + "/golden_session.cbrec.gz", err)) << err;
  AppConfig config;
  DrainWaiter waiter;
  auto p = std::make_unique<Pipeline>(config, waiter);
  waiter.attach(p->engine());
  std::vector<char> scratch(rec.max_payload() + simdjson::SIMDJSON_PADDING);

  const std::size_t n = rec.records().size();
  const std::size_t warm = n / 4;  // includes every venue's initial snapshot
  replay_inline(*p, rec, 0, warm, scratch);

  g_allocs = 0;
  g_counting = true;
  const ReplayResult r = replay_inline(*p, rec, warm, n, scratch);
  g_counting = false;

  RecordProperty("steady_state_frames", static_cast<int>(r.frames));
  EXPECT_GT(r.frames, 5000u) << "steady-state window too small to mean anything";
  EXPECT_GT(p->engine().snapshot().messages, 5000u);
  EXPECT_EQ(g_allocs.load(), 0u) << "heap allocations on the hot path after warm-up";
}

}  // namespace
}  // namespace cb

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
