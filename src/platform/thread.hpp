#pragma once

#include <optional>
#include <string_view>

namespace cb {

enum class ThreadRole {
  kBook,       // latency-critical consumer: pinned / highest QoS
  kIo,         // venue network threads
  kPublisher,  // dashboard server
  kBackground  // recorder, housekeeping
};

struct PlacementResult {
  bool named = false;
  bool pinned = false;        // true only when a hard CPU affinity was applied (Linux)
  bool qos_applied = false;   // true when a scheduling hint was applied (macOS QoS)
};

// Names the calling thread and places it according to its role.
// Linux: pthread affinity to `cpu` when given. macOS has no public hard-affinity API,
// so `cpu` is ignored there and latency-critical roles get QOS_CLASS_USER_INTERACTIVE,
// which keeps them on performance cores but does not prevent migration.
PlacementResult set_current_thread_role(ThreadRole role, std::string_view name,
                                        std::optional<int> cpu = std::nullopt) noexcept;

}  // namespace cb
