#include "core/spsc_ring.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace cb {
namespace {

using SmallRing = SpscRing<std::uint64_t, 8>;

TEST(SpscRing, HotFieldsSitOnDistinctCacheLines) {
  using Ring = SpscRing<std::uint64_t, 1024>;
  constexpr auto l = Ring::layout();
  const std::array<std::size_t, 5> offsets = {l.tail, l.head_cache, l.head, l.tail_cache,
                                              l.slots};
  for (std::size_t i = 0; i < offsets.size(); ++i) {
    EXPECT_EQ(offsets[i] % kCacheLine, 0u) << "field " << i << " not line-aligned";
    for (std::size_t j = i + 1; j < offsets.size(); ++j) {
      EXPECT_NE(offsets[i] / kCacheLine, offsets[j] / kCacheLine)
          << "fields " << i << " and " << j << " share a cache line";
    }
  }
  EXPECT_EQ(alignof(Ring), kCacheLine);
}

TEST(SpscRing, EmptyPopFails) {
  auto ring = std::make_unique<SmallRing>();
  std::uint64_t v = 0;
  EXPECT_FALSE(ring->try_pop(v));
  EXPECT_EQ(ring->consume([](std::uint64_t) {}), 0u);
}

TEST(SpscRing, FillsToExactlyCapacityThenRejects) {
  auto ring = std::make_unique<SmallRing>();
  for (std::uint64_t i = 0; i < SmallRing::kCapacity; ++i) {
    ASSERT_TRUE(ring->try_push(i));
  }
  EXPECT_FALSE(ring->try_push(99));  // no slot is wasted, and none is overwritten
  EXPECT_EQ(ring->size_approx(), SmallRing::kCapacity);
  std::uint64_t v = 0;
  ASSERT_TRUE(ring->try_pop(v));
  EXPECT_EQ(v, 0u);
  EXPECT_TRUE(ring->try_push(8));  // one slot freed -> exactly one push succeeds
  EXPECT_FALSE(ring->try_push(9));
}

TEST(SpscRing, FifoAcrossManyWraparounds) {
  auto ring = std::make_unique<SmallRing>();
  std::uint64_t next_in = 0;
  std::uint64_t next_out = 0;
  for (int round = 0; round < 1000; ++round) {
    const int pushes = 1 + round % 7;
    for (int i = 0; i < pushes; ++i) {
      if (ring->try_push(next_in)) {
        ++next_in;
      }
    }
    std::uint64_t v = 0;
    while (ring->try_pop(v)) {
      ASSERT_EQ(v, next_out++);
    }
  }
  EXPECT_EQ(next_in, next_out);
}

TEST(SpscRing, BatchPushIsAllOrNothing) {
  auto ring = std::make_unique<SmallRing>();
  const std::array<std::uint64_t, 5> five = {1, 2, 3, 4, 5};
  ASSERT_TRUE(ring->try_push_batch(five));
  EXPECT_FALSE(ring->try_push_batch(five));  // only 3 free: nothing must be written
  EXPECT_EQ(ring->size_approx(), 5u);
  const std::array<std::uint64_t, 3> three = {6, 7, 8};
  ASSERT_TRUE(ring->try_push_batch(three));
  const std::array<std::uint64_t, 9> too_big{};
  EXPECT_FALSE(ring->try_push_batch(too_big));

  std::vector<std::uint64_t> out;
  EXPECT_EQ(ring->consume([&](std::uint64_t v) { out.push_back(v); }), 8u);
  EXPECT_EQ(out, (std::vector<std::uint64_t>{1, 2, 3, 4, 5, 6, 7, 8}));
}

TEST(SpscRing, ConsumeRespectsMaxItems) {
  auto ring = std::make_unique<SmallRing>();
  for (std::uint64_t i = 0; i < 6; ++i) {
    ASSERT_TRUE(ring->try_push(i));
  }
  std::uint64_t sum = 0;
  EXPECT_EQ(ring->consume([&](std::uint64_t v) { sum += v; }, 4), 4u);
  EXPECT_EQ(sum, 0u + 1 + 2 + 3);
  EXPECT_EQ(ring->size_approx(), 2u);
}

// Payload with a redundant field: a torn or stale slot read shows up as a mismatch.
struct Msg {
  std::uint64_t seq;
  std::uint64_t check;
  std::array<std::uint64_t, 6> pad;  // 64 bytes total, like the real Event
};
constexpr std::uint64_t check_of(std::uint64_t seq) { return seq * 0x9E3779B97F4A7C15ull ^ 0xABCDu; }

// Two real threads; run under TSan in CI to validate the memory orders, not just the logic.
TEST(SpscRing, CrossThreadStressPreservesOrderAndContent) {
  constexpr std::uint64_t kCount = 2'000'000;
  using Ring = SpscRing<Msg, 1024>;
  auto ring = std::make_unique<Ring>();

  std::thread producer([&] {
    std::array<Msg, 7> batch{};
    std::uint64_t seq = 0;
    while (seq < kCount) {
      // Mix single and batch pushes to exercise both publication paths.
      if (seq % 3 == 0 && seq + batch.size() <= kCount) {
        for (std::size_t i = 0; i < batch.size(); ++i) {
          batch[i] = Msg{seq + i, check_of(seq + i), {}};
        }
        if (ring->try_push_batch(batch)) {
          seq += batch.size();
        }
      } else if (ring->try_push(Msg{seq, check_of(seq), {}})) {
        ++seq;
      }
    }
  });

  std::uint64_t expected = 0;
  bool ok = true;
  while (expected < kCount && ok) {
    ring->consume([&](const Msg& m) {
      if (m.seq != expected || m.check != check_of(m.seq)) {
        ok = false;
      }
      ++expected;
    });
  }
  producer.join();
  EXPECT_TRUE(ok) << "out-of-order or torn element near seq " << expected;
  EXPECT_EQ(expected, kCount);
}

}  // namespace
}  // namespace cb
