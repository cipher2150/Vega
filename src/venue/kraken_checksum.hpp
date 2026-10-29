#pragma once

// Kraken WS v2 book checksum (docs.kraken.com/api/docs/guides/spot-ws-book-v2):
// for the top 10 asks (best first) then the top 10 bids (best first), take price and qty
// formatted at the instrument's precision, remove the '.', strip leading zeros, append
// price then qty; CRC32 the concatenation and compare as unsigned 32-bit.
//
// We rebuild the digits from fixed-point integers instead of keeping the wire text, so the
// book stays a pair of int64 arrays; this works because at the instrument's precision the
// formatted digits are exactly the integer value / 10^(8 - precision).

#include "core/crc32.hpp"
#include "core/decimal.hpp"
#include "core/fixed_point.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace cb {

namespace detail {

// v / 10^D with D a compile-time constant, so the compiler emits multiply+shift instead of
// a hardware divide (~10x cheaper on arm64). `exact` reports whether nothing was dropped.
template <int D>
inline std::uint64_t div_pow10(std::uint64_t v, bool& exact) noexcept {
  constexpr std::uint64_t d = static_cast<std::uint64_t>(kPow10[D]);
  exact = v % d == 0;
  return v / d;
}

inline std::uint64_t scale_to_precision(std::uint64_t v, int precision, bool& exact) noexcept {
  switch (kFixedDecimals - precision) {
    case 0: exact = true; return v;
    case 1: return div_pow10<1>(v, exact);
    case 2: return div_pow10<2>(v, exact);
    case 3: return div_pow10<3>(v, exact);
    case 4: return div_pow10<4>(v, exact);
    case 5: return div_pow10<5>(v, exact);
    case 6: return div_pow10<6>(v, exact);
    case 7: return div_pow10<7>(v, exact);
    case 8: return div_pow10<8>(v, exact);
    default: exact = false; return 0;
  }
}

inline constexpr char kDigitPairs[] =
    "00010203040506070809101112131415161718192021222324252627282930313233343536373839"
    "40414243444546474849505152535455565758596061626364656667686970717273747576777879"
    "8081828384858687888990919293949596979899";

inline int count_digits(std::uint64_t v) noexcept {
  int n = 1;
  for (; v >= 10'000; v /= 10'000) n += 4;
  if (v >= 1'000) return n + 3;
  if (v >= 100) return n + 2;
  if (v >= 10) return n + 1;
  return n;
}

// Appends `raw` (1e-8 units) as digits at `precision` decimals, '.' and leading zeros
// removed. Returns false if the value has more decimals than the precision allows (then
// the exchange's string could not have produced it, so the checksum cannot match).
// Digits are written right-to-left two at a time from a lookup table: half the divisions
// of the naive loop and no reversal buffer (bench_checksum measures the difference).
inline bool append_kraken_digits(std::int64_t raw, int precision, char*& out) noexcept {
  if (raw < 0) {
    return false;
  }
  bool exact = false;
  std::uint64_t v = scale_to_precision(static_cast<std::uint64_t>(raw), precision, exact);
  if (!exact) {
    return false;
  }
  if (v == 0) {
    return true;  // "0.000" minus leading zeros is empty
  }
  const int n = count_digits(v);
  char* p = out + n;
  while (v >= 100) {
    const auto pair = static_cast<std::size_t>(v % 100) * 2;
    v /= 100;
    *--p = kDigitPairs[pair + 1];
    *--p = kDigitPairs[pair];
  }
  if (v >= 10) {
    const auto pair = static_cast<std::size_t>(v) * 2;
    *--p = kDigitPairs[pair + 1];
    *--p = kDigitPairs[pair];
  } else {
    *--p = static_cast<char>('0' + v);
  }
  out += n;
  return true;
}

}  // namespace detail

inline constexpr std::size_t kKrakenChecksumLevels = 10;

template <class Book>
std::optional<std::uint32_t> kraken_checksum(const Book& book, int price_precision,
                                             int qty_precision) noexcept {
  // 20 levels x (<=19 + <=19 digits) = 760 bytes worst case.
  std::array<char, 1024> buf;
  char* p = buf.data();
  auto append_side = [&](const auto& side) {
    const std::size_t n = side.size() < kKrakenChecksumLevels ? side.size() : kKrakenChecksumLevels;
    for (std::size_t r = 0; r < n; ++r) {
      if (!detail::append_kraken_digits(side.price(r).raw, price_precision, p) ||
          !detail::append_kraken_digits(side.qty(r).raw, qty_precision, p)) {
        return false;
      }
    }
    return true;
  };
  if (!append_side(book.asks()) || !append_side(book.bids())) {
    return std::nullopt;
  }
  return crc32(std::span<const char>(buf.data(), static_cast<std::size_t>(p - buf.data())));
}

}  // namespace cb
