#pragma once

// Per-venue state shared between threads outside the event stream.
// Each field has exactly one writer thread (noted below); readers use relaxed loads
// because these are status/metrics values, not data that guards other memory.

#include "platform/cacheline.hpp"

#include <atomic>
#include <cstdint>

namespace cb {

enum class ConnState : std::uint8_t {
  kDisabled = 0,
  kDisconnected,
  kConnecting,
  kSubscribed,  // websocket open and subscriptions sent; books may still be syncing
};

struct VenueShared {
  // ---- written by the venue's I/O thread ----
  alignas(kCacheLine) std::atomic<ConnState> conn_state{ConnState::kDisabled};
  std::atomic<std::uint32_t> epoch{0};
  std::atomic<std::uint64_t> last_rx_ticks{0};
  std::atomic<std::uint64_t> frames{0};
  std::atomic<std::uint64_t> bytes{0};
  std::atomic<std::uint64_t> connects{0};
  std::atomic<std::uint8_t> last_io_down_reason{0};
  std::atomic<std::int64_t> ntp_offset_ns{0};  // Kraken ping/pong estimate (server - local)
  std::atomic<std::int64_t> ntp_rtt_ns{-1};    // -1 = no estimate yet
  std::atomic<std::uint64_t> io_heap_allocs{0};

  // ---- written by the book thread ----
  // Epoch whose session the book thread wants torn down (checksum/crossed/horizon/full).
  // The I/O thread compares it with its current epoch and reconnects on a match.
  alignas(kCacheLine) std::atomic<std::uint32_t> resync_request{UINT32_MAX};
};

// Relaxed single-writer increment (no RMW needed with one writer).
inline void bump_relaxed(std::atomic<std::uint64_t>& a, std::uint64_t by = 1) noexcept {
  a.store(a.load(std::memory_order_relaxed) + by, std::memory_order_relaxed);
}

}  // namespace cb
