#pragma once

// Per-thread heap allocation counting. The service binary replaces global operator new
// (src/app/alloc_hooks.cpp) and calls note_allocation(); each thread that cares registers
// the counter it wants incremented. The dashboard then shows allocations per thread, so
// "zero allocations in steady state" is visible live, not just asserted in a test.
// Threads that never register cost one thread_local load per allocation.

#include <atomic>
#include <cstdint>

namespace cb::alloc {

void set_thread_counter(std::atomic<std::uint64_t>* counter) noexcept;
void note_allocation() noexcept;

}  // namespace cb::alloc
