#pragma once

// Signal arithmetic on fixed-point values. Products of two 1e-8 quantities reach ~1e24,
// beyond int64, so intermediate math uses __int128; results are rounded back to 1e-8.

#include "core/fixed_point.hpp"
#include "core/int128.hpp"

#include <cstdint>

namespace cb::signals {

using cb::i128;

// Size-weighted mid: leans toward the side with LESS size (the side more likely to be
// consumed next). Falls back to the plain mid when both sizes are zero.
constexpr std::int64_t microprice(std::int64_t bid, std::int64_t bid_qty, std::int64_t ask,
                                  std::int64_t ask_qty) noexcept {
  const i128 den = static_cast<i128>(bid_qty) + ask_qty;
  if (den == 0) {
    return bid / 2 + ask / 2;
  }
  const i128 num = static_cast<i128>(bid) * ask_qty + static_cast<i128>(ask) * bid_qty;
  return static_cast<std::int64_t>(num / den);
}

// (B - A) / (B + A) scaled by 1e8: +1e8 = all bid size, -1e8 = all ask size.
constexpr std::int64_t imbalance_e8(std::int64_t bid_size, std::int64_t ask_size) noexcept {
  const i128 den = static_cast<i128>(bid_size) + ask_size;
  if (den == 0) {
    return 0;
  }
  return static_cast<std::int64_t>((static_cast<i128>(bid_size) - ask_size) * kFixedScale / den);
}

// Reference conversion: USDT price -> USD with rate (USD per USDT, 1e-8 units), rounded
// half up. Uses a 128-bit division, which on arm64 is a library call (__divti3, tens of
// ns) - too slow for the per-row merge loop, so `convert` below is used there.
constexpr std::int64_t convert_reference(std::int64_t price, std::int64_t rate_e8) noexcept {
  return static_cast<std::int64_t>(
      (static_cast<i128>(price) * rate_e8 + kFixedScale / 2) / kFixedScale);
}

// Same result using only 64-bit operations: split price = whole*1e8 + frac, then
// price*rate/1e8 = whole*rate + frac*rate/1e8 exactly (whole*rate*1e8 is a multiple of
// 1e8, so rounding only touches the second term). Every product fits in int64 for
// price < 9.2e10 units and rate < 9.2e10 (any plausible USDT rate is ~1e8).
// Preconditions: price >= 0, 0 <= rate_e8 < 9.2e10. Tested against convert_reference.
constexpr std::int64_t convert(std::int64_t price, std::int64_t rate_e8) noexcept {
  const std::int64_t whole = price / kFixedScale;
  const std::int64_t frac = price % kFixedScale;
  return whole * rate_e8 + (frac * rate_e8 + kFixedScale / 2) / kFixedScale;
}

// Spread in basis points x 100 (so 1.23 bp -> 123), relative to mid.
constexpr std::int64_t spread_bp_e2(std::int64_t bid, std::int64_t ask) noexcept {
  const i128 mid2 = static_cast<i128>(bid) + ask;  // 2 x mid
  if (mid2 <= 0) {
    return 0;
  }
  return static_cast<std::int64_t>((static_cast<i128>(ask) - bid) * 2 * 1'000'000 / mid2);
}

}  // namespace cb::signals
