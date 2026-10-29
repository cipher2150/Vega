#pragma once

// Staging area between a venue parser and its SPSC ring.
//
// A parser pushes a whole message's events here and commits them with one batched ring
// push. Two reasons:
//  1. Measured (bench_spsc): publishing once per batch instead of once per element moves
//     the ring's tail cache line between cores once per message, 3-16x more throughput.
//  2. OKX and Bybit put their sequence fields *after* the levels in the JSON, so we only
//     know whether a message is valid after parsing it. Staging lets us discard an
//     invalid message instead of handing half of it to the book thread.

#include "venue/event.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <span>

namespace cb {

// What to do when the ring is full. Live: spin briefly, then give up (venue resyncs).
// Inline replay: drain the ring into the engine on the same thread.
class SpaceWaiter {
 public:
  virtual ~SpaceWaiter() = default;
  // Returns false if space did not become available (caller treats it as overflow).
  virtual bool wait_for_space(EventRing& ring) = 0;
};

class EventSink {
 public:
  static constexpr std::size_t kStagingCapacity = 8192;

  EventSink(EventRing& ring, SpaceWaiter& waiter)
      : ring_(ring), waiter_(waiter), staging_(std::make_unique<Staging>()) {}

  void push(const Event& e) noexcept {
    // A single message larger than the staging area (only an unfiltered full-depth
    // snapshot can be) is flushed in chunks; correctness does not depend on message
    // atomicity because integrity checks run at MessageEnd.
    if (n_ == kStagingCapacity && !flush()) {
      overflowed_ = true;
    }
    (*staging_)[n_++] = e;
  }

  // Sets t_parsed on every MessageEnd still in staging (all belong to the current frame).
  void stamp_parsed(std::uint64_t t_parsed) noexcept {
    for (std::size_t i = 0; i < n_; ++i) {
      if ((*staging_)[i].kind == EventKind::kMessageEnd) {
        (*staging_)[i].t_parsed = t_parsed;
      }
    }
  }

  // Publishes everything staged. Returns false if the ring stayed full (overflow).
  [[nodiscard]] bool commit() noexcept {
    const bool ok = flush() && !overflowed_;
    overflowed_ = false;
    return ok;
  }

  void discard() noexcept { n_ = 0; }

  [[nodiscard]] std::size_t staged() const noexcept { return n_; }

 private:
  using Staging = std::array<Event, kStagingCapacity>;

  bool flush() noexcept {
    // Chunks of at most half the ring so a batch can always fit once the consumer drains.
    constexpr std::size_t kChunk = kEventRingCapacity / 2;
    std::size_t off = 0;
    while (off < n_) {
      const std::size_t len = std::min(kChunk, n_ - off);
      if (ring_.try_push_batch(std::span<const Event>(staging_->data() + off, len))) {
        off += len;
      } else if (!waiter_.wait_for_space(ring_)) {
        n_ = 0;
        return false;
      }
    }
    n_ = 0;
    return true;
  }

  EventRing& ring_;
  SpaceWaiter& waiter_;
  std::unique_ptr<Staging> staging_;  // 512 KiB, allocated once at startup
  std::size_t n_ = 0;
  bool overflowed_ = false;
};

}  // namespace cb
