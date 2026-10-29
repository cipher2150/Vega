#pragma once

#include "venue/protocol.hpp"
#include "venue/types.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace cb {

enum class IdlePolicy : std::uint8_t {
  kSpin,     // never sleeps: lowest latency, burns a core (production)
  kBackoff,  // spin, then sleep briefly when idle (laptop-friendly)
};

struct AppConfig {
  std::array<VenueSettings, kNumVenues> venues;

  std::uint32_t consolidated_levels_per_venue = 20;
  std::uint32_t imbalance_levels = 10;

  int book_cpu = -1;  // Linux: pin the book thread here (-1 = don't pin)
  IdlePolicy idle = IdlePolicy::kBackoff;

  std::string http_bind = "0.0.0.0";
  std::uint16_t http_port = 8080;
  std::string web_root = "web";
  std::uint32_t publish_hz = 20;

  AppConfig();  // fills defaults (DESIGN §6)

  [[nodiscard]] const VenueSettings& venue(VenueId v) const { return venues[idx(v)]; }
  [[nodiscard]] VenueSettings& venue(VenueId v) { return venues[idx(v)]; }
};

// Reads a JSON config; any key that is absent keeps its default. Returns nullopt and
// fills `error` on malformed input. Cold path (startup only).
std::optional<AppConfig> load_config(const std::string& path, std::string& error);

}  // namespace cb
