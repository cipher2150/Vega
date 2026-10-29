#include "core/alloc_counter.hpp"

namespace cb::alloc {

namespace {
thread_local std::atomic<std::uint64_t>* tl_counter = nullptr;
}

void set_thread_counter(std::atomic<std::uint64_t>* counter) noexcept { tl_counter = counter; }

void note_allocation() noexcept {
  if (tl_counter != nullptr) {
    // Single writer per counter (its owning thread): relaxed load+store, no RMW.
    tl_counter->store(tl_counter->load(std::memory_order_relaxed) + 1,
                      std::memory_order_relaxed);
  }
}

}  // namespace cb::alloc
