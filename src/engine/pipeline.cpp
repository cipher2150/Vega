#include "engine/pipeline.hpp"

#include "core/clock.hpp"
#include "venue/factory.hpp"

#include <thread>

namespace cb {

namespace {

inline void cpu_relax() noexcept {
#if defined(__aarch64__)
  asm volatile("yield");
#elif defined(__x86_64__)
  __builtin_ia32_pause();
#endif
}

}  // namespace

bool SpinWaiter::wait_for_space(EventRing& ring) {
  const std::uint64_t start = clock::ticks();
  const double limit_ticks =
      static_cast<double>(timeout_ns_) * static_cast<double>(clock::ticks_per_second()) / 1e9;
  for (std::uint32_t spins = 0;; ++spins) {
    if (ring.size_approx() < EventRing::kCapacity / 2) {
      return true;
    }
    if (timeout_ns_ != 0 && static_cast<double>(clock::ticks() - start) > limit_ticks) {
      return false;
    }
    if (spins < 1024) {
      cpu_relax();
    } else {
      std::this_thread::yield();
    }
  }
}

bool DrainWaiter::wait_for_space(EventRing& ring) {
  if (engine_ == nullptr) {
    return false;
  }
  engine_->poll(ring, EventRing::kCapacity);
  return true;
}

Pipeline::Pipeline(const AppConfig& config, SpaceWaiter& waiter)
    : config_(config), published_(std::make_unique<TripleBuffer<PublishSnapshot>>()) {
  std::array<VenueShared*, kNumVenues> shared_ptrs{};
  for (const VenueId v : kAllVenues) {
    rings_[idx(v)] = std::make_unique<EventRing>();
    shared_ptrs[idx(v)] = &shared_[idx(v)];
  }
  engine_ = std::make_unique<Engine>(config_, shared_ptrs, published_.get());
  for (const VenueId v : kAllVenues) {
    const VenueSettings& vs = config_.venue(v);
    if (!vs.enabled) {
      shared_[idx(v)].conn_state.store(ConnState::kDisabled, std::memory_order_relaxed);
      continue;
    }
    shared_[idx(v)].conn_state.store(ConnState::kDisconnected, std::memory_order_relaxed);
    sinks_[idx(v)] = std::make_unique<EventSink>(*rings_[idx(v)], waiter);
    protocols_[idx(v)] = make_protocol(v, vs, *sinks_[idx(v)], shared_[idx(v)]);
  }
}

std::size_t Pipeline::poll_all(std::size_t max_per_venue) noexcept {
  std::size_t n = 0;
  for (const VenueId v : kAllVenues) {
    if (protocols_[idx(v)] != nullptr) {
      n += engine_->poll(*rings_[idx(v)], max_per_venue);
    }
  }
  return n;
}

}  // namespace cb
