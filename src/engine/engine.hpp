#pragma once

// The book/signal thread's state machine. Single-threaded by design: every venue book,
// the consolidated book and the signals are owned by exactly one thread, so none of it
// needs locks or atomics. Other threads see results only through the TripleBuffer.

#include "book/order_book.hpp"
#include "core/histogram.hpp"
#include "core/triple_buffer.hpp"
#include "engine/config.hpp"
#include "engine/publish_snapshot.hpp"
#include "venue/event.hpp"
#include "venue/venue_shared.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>

namespace cb {

// 4,096 levels/side: Coinbase BTC-USD had 1,791 bids within the default +/-2% horizon
// (DESIGN §5.2); deeper venues are depth-limited to <= 400.
inline constexpr std::size_t kBookCapacity = 4096;
using Book = OrderBook<kBookCapacity>;

// Per-stage latency, all recorded on the book thread except `parse` (per-venue, on the
// I/O thread, owned by VenueProtocol).
struct StageHistograms {
  Histogram queue;    // parsed on I/O thread -> first event dequeued here
  Histogram book;     // first event dequeued -> book updated and integrity-checked
  Histogram signal;   // consolidated book + signals recomputed
  Histogram publish;  // snapshot copied into the triple buffer
  Histogram e2e;      // frame received -> published
};

class Engine {
 public:
  Engine(const AppConfig& config, std::array<VenueShared*, kNumVenues> shared,
         TripleBuffer<PublishSnapshot>* out);

  void on_event(const Event& e) noexcept;

  std::size_t poll(EventRing& ring, std::size_t max_items = 1024) noexcept {
    return ring.consume([this](const Event& e) { on_event(e); }, max_items);
  }

  [[nodiscard]] const StageHistograms& stages() const noexcept { return stages_; }
  [[nodiscard]] const PublishSnapshot& snapshot() const noexcept { return snap_; }
  [[nodiscard]] BookState state(VenueId v, InstrumentId i) const noexcept {
    return slot(v, i).state;
  }
  [[nodiscard]] const Book& book(VenueId v, InstrumentId i) const noexcept {
    return slot(v, i).book;
  }

  // Deterministic text dump of all books (golden tests). With `consolidated`, also the
  // FX rate and consolidated books.
  void dump(std::FILE* out, bool consolidated) const;

 private:
  struct LagTracker {
    // Rolling minimum over two adjacent 30 s windows (keyed on wall time so replay
    // reproduces it exactly).
    static constexpr std::int64_t kWindowNs = 30'000'000'000;
    std::int64_t last = 0;
    std::int64_t cur_min = INT64_MAX;
    std::int64_t prev_min = INT64_MAX;
    std::int64_t window_start = 0;
    void add(std::int64_t lag, std::int64_t wall) noexcept;
    [[nodiscard]] std::int64_t min() const noexcept {
      return cur_min < prev_min ? cur_min : prev_min;
    }
  };

  struct Slot {
    Book book;
    BookState state = BookState::kEmpty;
    DownReason last_reason = DownReason::kNone;
    bool exists = false;
    bool full = false;  // an insert was rejected during the current message
    Quote quote = Quote::kUsd;
    std::uint32_t depth = 0;
    std::uint8_t price_precision = 0;
    std::uint8_t qty_precision = 0;
    std::uint64_t messages = 0;
    std::uint64_t checksum_ok = 0;
    std::uint64_t checksum_fail = 0;
    std::uint64_t resyncs = 0;
    std::uint64_t last_change_ticks = 0;
    LagTracker lag;
  };

  struct VenueRuntime {
    std::uint32_t min_epoch = 0;   // events from older sessions are dropped
    bool in_message = false;
    std::uint64_t msg_start = 0;   // ticks when the current message's first event arrived
    bool horizon = false;          // Coinbase-style price-band book
    std::uint32_t min_levels = 0;
  };

  Slot& slot(VenueId v, InstrumentId i) noexcept { return (*slots_)[idx(v)][idx(i)]; }
  const Slot& slot(VenueId v, InstrumentId i) const noexcept { return (*slots_)[idx(v)][idx(i)]; }

  void on_message_end(const Event& e, Slot& s) noexcept;
  // Integrity failure detected here: whole venue goes stale and its session is torn down.
  void fail_venue(VenueId v, std::uint32_t epoch, DownReason reason) noexcept;
  void recompute(InstrumentId changed) noexcept;
  void recompute_consolidated(std::size_t c) noexcept;
  void fill_book_summaries() noexcept;
  void publish() noexcept;

  const AppConfig& config_;
  std::array<VenueShared*, kNumVenues> shared_;
  TripleBuffer<PublishSnapshot>* out_;
  std::unique_ptr<std::array<std::array<Slot, kNumInstruments>, kNumVenues>> slots_;
  std::array<VenueRuntime, kNumVenues> venues_{};
  StageHistograms stages_;
  PublishSnapshot snap_{};
  std::int64_t fx_rate_e8_ = 0;
  bool fx_live_ = false;
};

}  // namespace cb
