#pragma once

// The one message type that crosses from an I/O thread to the book thread.
// Fixed 64 bytes, trivially copyable: it fits the SPSC ring's slot model (plain copy, no
// destructor) and is exactly one cache line on Linux/Ampere.

#include "core/fixed_point.hpp"
#include "core/spsc_ring.hpp"
#include "venue/types.hpp"

#include <cstdint>

namespace cb {

enum class EventKind : std::uint8_t {
  kSnapshotBegin,  // clear this instrument's book; levels that follow form a snapshot
  kLevel,          // absolute size at a price (0 = delete)
  kMessageEnd,     // end of one venue message for this instrument: run integrity checks
  kVenueDown,      // the I/O side detected a problem (gap, parse error, disconnect)
};

inline constexpr std::uint8_t kFlagSnapshot = 1u << 0;      // MessageEnd closes a snapshot
inline constexpr std::uint8_t kFlagHasChecksum = 1u << 1;   // checksum field is valid

struct Event {
  EventKind kind;
  VenueId venue;
  InstrumentId instrument;
  Side side;
  std::uint32_t epoch;         // I/O session id; stale sessions' events are dropped
  std::int64_t price;          // 1e-8 units
  std::int64_t qty;            // 1e-8 units
  std::int64_t exch_ts_ns;     // MessageEnd: exchange timestamp (wall ns) or kInvalidTime
  std::int64_t recv_wall_ns;   // MessageEnd: local wall clock at receive
  std::uint64_t t_recv;        // MessageEnd: monotonic ticks when the frame was complete
  std::uint64_t t_parsed;      // MessageEnd: monotonic ticks when parsing finished
  std::uint32_t checksum;      // MessageEnd: venue checksum (Kraken)
  std::uint8_t flags;
  DownReason reason;           // VenueDown
  std::uint8_t pad[2];
};
static_assert(sizeof(Event) == 64, "Event must stay one 64-byte cache line");
static_assert(std::is_trivially_copyable_v<Event>);

// 65,536 events (4 MiB) per venue: Coinbase's band-filtered snapshot is a few thousand
// events, so a full ring means the book thread is badly behind - treated as overflow.
inline constexpr std::size_t kEventRingCapacity = 1u << 16;
using EventRing = SpscRing<Event, kEventRingCapacity>;

}  // namespace cb
