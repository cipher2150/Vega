#pragma once

// Bounded single-producer / single-consumer ring buffer.
//
// Exactly one thread may call the producer API (try_push, try_push_batch) and exactly
// one thread the consumer API (try_pop, consume). With one writer per index, no
// read-modify-write atomics are needed: each index is advanced with a plain release
// store, and the other side observes it with an acquire load.

#include "platform/cacheline.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace cb {

template <class T, std::size_t Capacity>
class SpscRing {
  static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                "Capacity must be a power of two so slot = index & mask (no division)");
  static_assert(std::is_trivially_copyable_v<T>,
                "slots are copied with plain assignment; no destructors run on overwrite");

 public:
  using value_type = T;
  static constexpr std::size_t kCapacity = Capacity;

  SpscRing() = default;
  SpscRing(const SpscRing&) = delete;
  SpscRing& operator=(const SpscRing&) = delete;

  // ---- producer side ---------------------------------------------------------------

  [[nodiscard]] bool try_push(const T& value) noexcept {
    // relaxed: tail_ is written only by this (producer) thread, so we always see our
    // own latest store.
    const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
    if (tail - head_cache_ == Capacity && !refresh_head(tail, 1)) {
      return false;
    }
    slots_[tail & kMask] = value;
    // release: makes the slot write above visible to a consumer that observes the new
    // tail via its acquire load. Without it the consumer could read a stale slot.
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

  // All-or-nothing push of several elements, published with a single release store.
  // A venue message (N levels + end marker) becomes visible to the consumer atomically
  // and the cross-core index traffic is paid once per message instead of once per level.
  [[nodiscard]] bool try_push_batch(std::span<const T> values) noexcept {
    const std::uint64_t n = values.size();
    if (n == 0) {
      return true;
    }
    if (n > Capacity) {
      return false;
    }
    const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
    if (tail - head_cache_ > Capacity - n && !refresh_head(tail, n)) {
      return false;
    }
    for (std::uint64_t i = 0; i < n; ++i) {
      slots_[(tail + i) & kMask] = values[i];
    }
    tail_.store(tail + n, std::memory_order_release);  // release: as in try_push
    return true;
  }

  // ---- consumer side ---------------------------------------------------------------

  [[nodiscard]] bool try_pop(T& out) noexcept {
    // relaxed: head_ is written only by this (consumer) thread.
    const std::uint64_t head = head_.load(std::memory_order_relaxed);
    if (head == tail_cache_ && !refresh_tail(head)) {
      return false;
    }
    out = slots_[head & kMask];
    // release: orders the slot *read* above before the index store. A producer that
    // acquires the new head may then overwrite the slot; without release the store
    // could become visible first and the producer could overwrite data mid-read.
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

  // Drains up to `max_items` elements, calling fn(const T&) for each, and frees them
  // with a single head store. Returns the number consumed.
  template <class Fn>
  std::size_t consume(Fn&& fn, std::size_t max_items = Capacity) noexcept(
      std::is_nothrow_invocable_v<Fn&, const T&>) {
    const std::uint64_t head = head_.load(std::memory_order_relaxed);
    if (head == tail_cache_ && !refresh_tail(head)) {
      return 0;
    }
    std::uint64_t available = tail_cache_ - head;
    if (available > max_items) {
      available = max_items;
    }
    for (std::uint64_t i = 0; i < available; ++i) {
      fn(static_cast<const T&>(slots_[(head + i) & kMask]));
    }
    head_.store(head + available, std::memory_order_release);  // release: as in try_pop
    return available;
  }

  // Approximate: exact only when called from a quiescent state. For metrics/tests.
  [[nodiscard]] std::size_t size_approx() const noexcept {
    const std::uint64_t tail = tail_.load(std::memory_order_acquire);
    const std::uint64_t head = head_.load(std::memory_order_acquire);
    return tail >= head ? static_cast<std::size_t>(tail - head) : 0;
  }

  // Byte offsets of the hot fields, exposed so a test can verify they sit on distinct
  // cache lines (offsetof needs the complete class, hence a member function).
  struct Layout {
    std::size_t tail, head_cache, head, tail_cache, slots;
  };
  static constexpr Layout layout() noexcept {
    return {offsetof(SpscRing, tail_), offsetof(SpscRing, head_cache_),
            offsetof(SpscRing, head_), offsetof(SpscRing, tail_cache_),
            offsetof(SpscRing, slots_)};
  }

 private:
  static constexpr std::uint64_t kMask = Capacity - 1;

  // Producer believes the ring lacks room for `needed`; re-read the real head.
  bool refresh_head(std::uint64_t tail, std::uint64_t needed) noexcept {
    // acquire: pairs with the consumer's release store of head_, so every slot read the
    // consumer did before freeing a slot happens-before our overwrite of that slot.
    head_cache_ = head_.load(std::memory_order_acquire);
    return tail - head_cache_ <= Capacity - needed;
  }

  // Consumer believes the ring is empty; re-read the real tail.
  bool refresh_tail(std::uint64_t head) noexcept {
    // acquire: pairs with the producer's release store of tail_, so the slot contents
    // written before that store are visible to our subsequent reads.
    tail_cache_ = tail_.load(std::memory_order_acquire);
    return head != tail_cache_;
  }

  // Indices increase monotonically and are never wrapped (2^64 pushes would take
  // centuries), so full == (tail - head == Capacity) and no slot is sacrificed.
  //
  // Each field below has its own cache line. tail_/head_ are each written by one thread
  // and polled by the other; if they shared a line, every push would invalidate the
  // line the consumer is spinning on (false sharing). The *_cache_ fields are private
  // to one thread; separate lines keep the other thread's polling from stealing them.
  alignas(kCacheLine) std::atomic<std::uint64_t> tail_{0};  // written by producer
  alignas(kCacheLine) std::uint64_t head_cache_ = 0;        // producer-private
  alignas(kCacheLine) std::atomic<std::uint64_t> head_{0};  // written by consumer
  alignas(kCacheLine) std::uint64_t tail_cache_ = 0;        // consumer-private
  alignas(kCacheLine) std::array<T, Capacity> slots_;
};

}  // namespace cb
