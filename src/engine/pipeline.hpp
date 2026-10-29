#pragma once

// Owns and wires every per-venue component: shared state, SPSC ring, staging sink,
// protocol, plus the engine and its output triple buffer. Used identically by the live
// service, by replay and by tests, so they all exercise the same object graph.

#include "core/triple_buffer.hpp"
#include "engine/config.hpp"
#include "engine/engine.hpp"
#include "engine/publish_snapshot.hpp"
#include "venue/event_sink.hpp"
#include "venue/protocol.hpp"
#include "venue/venue_shared.hpp"

#include <array>
#include <cstdint>
#include <memory>

namespace cb {

// Live I/O threads: spin (with a CPU hint) while the book thread catches up; after
// `timeout_ns` give up so the venue resyncs instead of stalling its socket forever.
// timeout_ns == 0 waits indefinitely (pipelined replay).
class SpinWaiter final : public SpaceWaiter {
 public:
  explicit SpinWaiter(std::uint64_t timeout_ns) : timeout_ns_(timeout_ns) {}
  bool wait_for_space(EventRing& ring) override;

 private:
  std::uint64_t timeout_ns_;
};

// Inline replay / tests: the producer and the consumer are the same thread, so "waiting"
// means draining the ring into the engine right now.
class DrainWaiter final : public SpaceWaiter {
 public:
  void attach(Engine& engine) noexcept { engine_ = &engine; }
  bool wait_for_space(EventRing& ring) override;

 private:
  Engine* engine_ = nullptr;
};

class Pipeline {
 public:
  Pipeline(const AppConfig& config, SpaceWaiter& waiter);

  [[nodiscard]] const AppConfig& config() const noexcept { return config_; }
  [[nodiscard]] Engine& engine() noexcept { return *engine_; }
  [[nodiscard]] VenueShared& shared(VenueId v) noexcept { return shared_[idx(v)]; }
  [[nodiscard]] EventRing& ring(VenueId v) noexcept { return *rings_[idx(v)]; }
  // nullptr when the venue is disabled in config.
  [[nodiscard]] VenueProtocol* protocol(VenueId v) noexcept { return protocols_[idx(v)].get(); }
  [[nodiscard]] TripleBuffer<PublishSnapshot>& published() noexcept { return *published_; }

  // One pass over every enabled venue's ring (book thread). Returns events processed.
  std::size_t poll_all(std::size_t max_per_venue = 1024) noexcept;

 private:
  const AppConfig& config_;
  std::array<VenueShared, kNumVenues> shared_;
  std::array<std::unique_ptr<EventRing>, kNumVenues> rings_;
  std::unique_ptr<TripleBuffer<PublishSnapshot>> published_;
  std::unique_ptr<Engine> engine_;
  std::array<std::unique_ptr<EventSink>, kNumVenues> sinks_;
  std::array<std::unique_ptr<VenueProtocol>, kNumVenues> protocols_;
};

}  // namespace cb
