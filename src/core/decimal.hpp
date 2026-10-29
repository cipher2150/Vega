#pragma once

// Decimal text <-> fixed-point int64 (1e-8 units) without ever touching floating point.

#include "core/fixed_point.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

namespace cb {

enum class DecimalError : std::uint8_t {
  kOk = 0,
  kEmpty,      // no characters at all
  kSyntax,     // anything other than digits[.digits]: signs, exponents, spaces, "5.", ".5"
  kPrecision,  // a non-zero digit beyond the 8th fractional place (would need rounding)
  kOverflow,   // value * 1e8 does not fit in int64
};

struct DecimalResult {
  std::int64_t value = 0;
  DecimalError error = DecimalError::kOk;
  [[nodiscard]] constexpr bool ok() const noexcept { return error == DecimalError::kOk; }
};

namespace detail {

inline constexpr std::array<std::int64_t, kFixedDecimals + 1> kPow10 = {
    1, 10, 100, 1'000, 10'000, 100'000, 1'000'000, 10'000'000, 100'000'000};

// Largest integer part whose scaled value can fit in int64: floor(INT64_MAX / 1e8).
inline constexpr std::int64_t kMaxIntPart = std::numeric_limits<std::int64_t>::max() / kFixedScale;

constexpr bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

}  // namespace detail

// Parses an unsigned decimal ("80006.3", "0.00006120", "80000") into 1e-8 units.
// Strict on purpose: exchange feeds never send signs or exponents for prices/sizes, so
// anything unexpected is reported as an error (the venue then resyncs) instead of being
// guessed at. Trailing fractional zeros beyond 8 places are accepted because they carry
// no information; a non-zero 9th digit is rejected rather than rounded.
[[nodiscard]] constexpr DecimalResult parse_decimal(std::string_view text) noexcept {
  using detail::is_digit;
  const char* p = text.data();
  const char* const end = p + text.size();
  if (p == end) {
    return {0, DecimalError::kEmpty};
  }

  std::int64_t int_part = 0;
  const char* const int_begin = p;
  for (; p != end && is_digit(*p); ++p) {
    int_part = int_part * 10 + (*p - '0');
    // int_part only grows, so checking every step bounds it below 10*kMaxIntPart+9,
    // which cannot overflow int64.
    if (int_part > detail::kMaxIntPart) {
      return {0, DecimalError::kOverflow};
    }
  }
  if (p == int_begin) {
    return {0, DecimalError::kSyntax};
  }

  std::int64_t frac = 0;
  int frac_digits = 0;
  if (p != end && *p == '.') {
    ++p;
    const char* const frac_begin = p;
    for (; p != end && is_digit(*p); ++p) {
      if (frac_digits < kFixedDecimals) {
        frac = frac * 10 + (*p - '0');
        ++frac_digits;
      } else if (*p != '0') {
        return {0, DecimalError::kPrecision};
      }
    }
    if (p == frac_begin) {
      return {0, DecimalError::kSyntax};
    }
  }
  if (p != end) {
    return {0, DecimalError::kSyntax};
  }

  frac *= detail::kPow10[static_cast<std::size_t>(kFixedDecimals - frac_digits)];
  if (int_part == detail::kMaxIntPart &&
      frac > std::numeric_limits<std::int64_t>::max() - detail::kMaxIntPart * kFixedScale) {
    return {0, DecimalError::kOverflow};
  }
  return {int_part * kFixedScale + frac, DecimalError::kOk};
}

// Longest output of format_decimal: 19 integer digits + '.' + 8 fraction digits.
inline constexpr std::size_t kMaxDecimalChars = 28;

// Formats a non-negative fixed-point value with the minimal number of fractional digits
// ("80006.3", "80000", "0.0000612"). Returns the number of characters written.
// Used for logging and for round-trip tests; never on the hot path.
constexpr std::size_t format_decimal(std::int64_t value,
                                     std::span<char, kMaxDecimalChars> out) noexcept {
  const auto int_part = static_cast<std::uint64_t>(value / kFixedScale);
  auto frac = static_cast<std::uint64_t>(value % kFixedScale);

  std::array<char, 20> digits{};
  std::size_t nd = 0;
  std::uint64_t v = int_part;
  do {
    digits[nd++] = static_cast<char>('0' + v % 10);
    v /= 10;
  } while (v != 0);

  std::size_t n = 0;
  while (nd > 0) {
    out[n++] = digits[--nd];
  }
  if (frac != 0) {
    int frac_len = kFixedDecimals;
    while (frac % 10 == 0) {
      frac /= 10;
      --frac_len;
    }
    out[n++] = '.';
    for (int i = frac_len - 1; i >= 0; --i) {
      out[n + static_cast<std::size_t>(i)] = static_cast<char>('0' + frac % 10);
      frac /= 10;
    }
    n += static_cast<std::size_t>(frac_len);
  }
  return n;
}

}  // namespace cb
