#pragma once

// Wait-free single-writer / single-reader "latest value" hand-off.
//
// Three slots: the writer owns one (back), the reader owns one (front), and one sits in
// the middle. Publishing swaps back<->middle and sets a "fresh" bit; reading swaps
// front<->middle only when fresh. Neither side ever waits, and the reader always gets the
// newest complete value. That is exactly what a throttled publisher needs: it must never
// slow the book thread, and it only cares about the latest state, not every state.
//
// Chosen over a seqlock because a seqlock reader copies data that may be concurrently
// written, which is a data race (UB) in the C++ memory model unless every word is atomic.

#include "platform/cacheline.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <type_traits>

namespace cb {

template <class T>
class TripleBuffer {
  static_assert(std::is_trivially_copyable_v<T>);

 public:
  // ---- writer ----
  T& write_slot() noexcept { return slots_[back_].value; }

  void publish() noexcept {
    // acq_rel: release makes our writes to the back slot visible to the reader that later
    // acquires it; acquire makes the reader's finished reads of the slot we receive
    // happen-before our next writes into it.
    back_ = middle_.exchange(static_cast<std::uint8_t>(back_ | kFresh),
                             std::memory_order_acq_rel) &
            kIndexMask;
  }

  // ---- reader ----
  // Returns true and swaps in the newest value if one was published since the last call.
  bool update() noexcept {
    // relaxed: only a hint to skip the exchange; the exchange below does the real sync.
    if ((middle_.load(std::memory_order_relaxed) & kFresh) == 0) {
      return false;
    }
    front_ = middle_.exchange(front_, std::memory_order_acq_rel) & kIndexMask;
    return true;
  }

  const T& read_slot() const noexcept { return slots_[front_].value; }

 private:
  static constexpr std::uint8_t kIndexMask = 0x3;
  static constexpr std::uint8_t kFresh = 0x4;

  struct alignas(kCacheLine) Slot {
    T value{};
  };

  std::array<Slot, 3> slots_{};
  alignas(kCacheLine) std::atomic<std::uint8_t> middle_{1};
  alignas(kCacheLine) std::uint8_t back_ = 0;   // writer-private
  alignas(kCacheLine) std::uint8_t front_ = 2;  // reader-private
};

}  // namespace cb
