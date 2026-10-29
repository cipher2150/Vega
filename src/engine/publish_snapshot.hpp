#pragma once

// Everything the dashboard shows, as one trivially copyable value handed from the book
// thread to the publisher through a TripleBuffer. All prices/sizes stay fixed-point here;
// conversion to text/double happens on the publisher thread, off the hot path.

#include "venue/types.hpp"

#include <array>
#include <cstdint>
#include <type_traits>

namespace cb {

inline constexpr std::size_t kConsolidatedRows = 20;

enum class BookState : std::uint8_t { kEmpty = 0, kSyncing, kLive, kStale };

constexpr const char* book_state_name(BookState s) noexcept {
  switch (s) {
    case BookState::kEmpty: return "empty";
    case BookState::kSyncing: return "syncing";
    case BookState::kLive: return "live";
    case BookState::kStale: return "stale";
  }
  return "?";
}

struct Row {
  std::int64_t price = 0;  // USD (USDT venues converted at the live FX rate)
  std::int64_t qty = 0;
  VenueId venue = VenueId::kCoinbase;
};

struct BookSummary {
  bool exists = false;  // venue lists this instrument
  BookState state = BookState::kEmpty;
  DownReason last_reason = DownReason::kNone;
  Quote quote = Quote::kUsd;
  std::uint32_t bid_levels = 0;
  std::uint32_t ask_levels = 0;
  std::int64_t best_bid = 0, best_bid_qty = 0;  // native quote currency
  std::int64_t best_ask = 0, best_ask_qty = 0;
  std::int64_t microprice = 0;                  // native quote currency
  std::int64_t imbalance_e8 = 0;                // (B - A) / (B + A) x 1e8, top-N levels
  std::uint64_t last_change_ticks = 0;
  std::int64_t lag_last_ns = 0;  // local recv wall - exchange ts (latency + clock offset)
  std::int64_t lag_min_ns = 0;   // rolling ~60 s minimum of the above
  std::uint64_t messages = 0;
  std::uint64_t checksum_ok = 0;
  std::uint64_t checksum_fail = 0;
  std::uint64_t resyncs = 0;
};

struct ConsolidatedSummary {
  std::array<Row, kConsolidatedRows> bids{};
  std::array<Row, kConsolidatedRows> asks{};
  std::uint8_t n_bids = 0;
  std::uint8_t n_asks = 0;
  std::uint8_t venues_mask = 0;  // bit per venue contributing
  std::int64_t spread = 0;       // best ask - best bid (negative = crossed across venues)
  std::int64_t spread_bp_e2 = 0; // spread in basis points x 100
  std::int64_t microprice = 0;
  std::int64_t imbalance_e8 = 0;
};

struct PublishSnapshot {
  std::uint64_t seq = 0;
  std::uint64_t ticks = 0;
  std::array<std::array<BookSummary, kNumInstruments>, kNumVenues> books{};
  std::array<ConsolidatedSummary, kNumConsolidated> consolidated{};
  std::int64_t fx_usdt_usd_e8 = 0;  // 0 when unavailable
  bool fx_live = false;
  std::uint64_t events = 0;
  std::uint64_t messages = 0;
  std::uint64_t stale_epoch_drops = 0;
};
static_assert(std::is_trivially_copyable_v<PublishSnapshot>);

}  // namespace cb
