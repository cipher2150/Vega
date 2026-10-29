#pragma once

#include "platform/thread.hpp"

#include <optional>
#include <string_view>
#include <thread>

namespace cb::bench {

// Pins to `cpu` only when the machine has enough cores that the requested CPU exists;
// on macOS pinning is unavailable and only QoS applies (reported via AddCustomContext).
inline PlacementResult place_thread(ThreadRole role, std::string_view name, int cpu) {
  const unsigned cores = std::thread::hardware_concurrency();
  const std::optional<int> target =
      (cpu >= 0 && static_cast<unsigned>(cpu) < cores) ? std::optional<int>(cpu) : std::nullopt;
  return set_current_thread_role(role, name, target);
}

}  // namespace cb::bench
