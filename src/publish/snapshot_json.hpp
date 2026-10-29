#pragma once

// Converts engine state into the JSON messages the dashboard consumes. Runs on the
// publisher thread only (allocates freely; never on the hot path).

#include "core/histogram.hpp"
#include "engine/engine.hpp"
#include "engine/publish_snapshot.hpp"
#include "venue/venue_shared.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace cb {

struct LatencySources {
  std::array<const Histogram*, kNumVenues> parse{};  // nullptr for disabled venues
  const StageHistograms* stages = nullptr;
};

struct ThreadAllocs {
  const std::atomic<std::uint64_t>* book = nullptr;
};

// {"t":"snap", ...}: books, consolidated ladders, signals, venue health.
std::string snapshot_json(const PublishSnapshot& snap,
                          const std::array<const VenueShared*, kNumVenues>& shared,
                          const ThreadAllocs& allocs, std::uint64_t now_ticks);

// {"t":"lat", ...}: per-stage percentiles + log2 histogram bins, in nanoseconds.
std::string latency_json(const LatencySources& sources);

}  // namespace cb
