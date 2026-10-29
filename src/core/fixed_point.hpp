#pragma once

#include <compare>
#include <cstdint>

namespace cb {

// Every price and quantity in the system is an integer count of 1e-8 units.
// One universal scale (instead of per-instrument ticks) keeps cross-venue comparison and
// USDT->USD conversion as plain integer math; see docs/DESIGN.md §5.1.
inline constexpr int kFixedDecimals = 8;
inline constexpr std::int64_t kFixedScale = 100'000'000;

// Distinct types so a (price, qty) argument swap is a compile error, not a silent bug.
struct Price {
  std::int64_t raw = 0;
  constexpr auto operator<=>(const Price&) const = default;
};

struct Qty {
  std::int64_t raw = 0;
  constexpr auto operator<=>(const Qty&) const = default;
  [[nodiscard]] constexpr bool is_zero() const noexcept { return raw == 0; }
};

enum class Side : std::uint8_t { kBid = 0, kAsk = 1 };

}  // namespace cb
