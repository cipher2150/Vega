#pragma once

// Allocation-free RFC 3339 UTC timestamp parsing ("2026-10-29T13:11:48.466869Z").
// Used for exchange timestamps (Coinbase, Kraken); returns kInvalidTime on anything
// unexpected, because a bad exchange timestamp must never take a venue down.

#include <cstdint>
#include <limits>
#include <string_view>

namespace cb {

inline constexpr std::int64_t kInvalidTime = std::numeric_limits<std::int64_t>::min();

namespace detail {

// Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's algorithm).
constexpr std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) noexcept {
  y -= m <= 2 ? 1 : 0;
  const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
  const auto yoe = static_cast<unsigned>(y - era * 400);
  const unsigned mp = m > 2 ? m - 3 : m + 9;
  const unsigned doy = (153 * mp + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

constexpr bool read_digits(std::string_view s, std::size_t pos, std::size_t n,
                           std::int64_t& out) noexcept {
  if (pos + n > s.size()) {
    return false;
  }
  out = 0;
  for (std::size_t i = pos; i < pos + n; ++i) {
    if (s[i] < '0' || s[i] > '9') {
      return false;
    }
    out = out * 10 + (s[i] - '0');
  }
  return true;
}

}  // namespace detail

// Accepts YYYY-MM-DDTHH:MM:SS[.f{1,}]Z (UTC only; both exchanges send 'Z').
// Fractional digits beyond nanoseconds are ignored.
constexpr std::int64_t parse_rfc3339_ns(std::string_view s) noexcept {
  using detail::read_digits;
  std::int64_t y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
  if (s.size() < 20 || !read_digits(s, 0, 4, y) || s[4] != '-' ||
      !read_digits(s, 5, 2, mo) || s[7] != '-' || !read_digits(s, 8, 2, d) ||
      s[10] != 'T' || !read_digits(s, 11, 2, h) || s[13] != ':' ||
      !read_digits(s, 14, 2, mi) || s[16] != ':' || !read_digits(s, 17, 2, sec)) {
    return kInvalidTime;
  }
  if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || sec > 60) {
    return kInvalidTime;
  }
  std::size_t pos = 19;
  std::int64_t frac_ns = 0;
  if (s[pos] == '.') {
    ++pos;
    int n = 0;
    while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
      if (n < 9) {
        frac_ns = frac_ns * 10 + (s[pos] - '0');
        ++n;
      }
      ++pos;
    }
    if (n == 0) {
      return kInvalidTime;
    }
    for (; n < 9; ++n) {
      frac_ns *= 10;
    }
  }
  if (pos + 1 != s.size() || s[pos] != 'Z') {
    return kInvalidTime;
  }
  const std::int64_t days =
      detail::days_from_civil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d));
  return ((days * 24 + h) * 60 + mi) * 60'000'000'000LL + sec * 1'000'000'000LL + frac_ns;
}

}  // namespace cb
