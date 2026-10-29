#pragma once

// Dashboard server: serves web/ over HTTP and pushes throttled JSON snapshots over
// WebSocket. Runs on its own thread and io_context. Its only link to the book thread is
// the TripleBuffer (wait-free) plus relaxed reads of counters, so a slow browser or a
// slow publisher can never block book updates.

#include "core/triple_buffer.hpp"
#include "engine/config.hpp"
#include "engine/publish_snapshot.hpp"
#include "publish/snapshot_json.hpp"

#include <atomic>
#include <memory>

namespace cb {

struct PublisherInputs {
  const AppConfig* config = nullptr;
  TripleBuffer<PublishSnapshot>* snapshots = nullptr;
  std::array<const VenueShared*, kNumVenues> shared{};
  LatencySources latency;
  ThreadAllocs allocs;
};

class Publisher {
 public:
  Publisher(const PublisherInputs& inputs, const std::atomic<bool>& stop);
  ~Publisher();
  Publisher(const Publisher&) = delete;
  Publisher& operator=(const Publisher&) = delete;

  // Blocks until `stop` is set. Returns false if the listen socket could not be opened.
  bool run();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace cb
