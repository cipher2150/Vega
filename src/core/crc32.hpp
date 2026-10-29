#pragma once

// CRC-32 as used by zlib / IEEE 802.3 (reflected polynomial 0xEDB88320) - the variant
// Kraken's book checksum uses (verified against 1,755 live messages, DESIGN §6.2).
//
// ARMv8's optional CRC32 instructions (__crc32b/__crc32d) implement exactly this
// polynomial (the CRC32C variant is the separate __crc32c* family), so when the target
// has them we use the hardware path; otherwise a 256-entry table.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#if defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#endif

namespace cb {

namespace detail {

constexpr std::array<std::uint32_t, 256> make_crc32_table() noexcept {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t i = 0; i < 256; ++i) {
    std::uint32_t c = i;
    for (int k = 0; k < 8; ++k) {
      c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    }
    table[i] = c;
  }
  return table;
}

inline constexpr auto kCrc32Table = make_crc32_table();

}  // namespace detail

// Portable reference implementation (also used by tests to cross-check the hardware path).
constexpr std::uint32_t crc32_table(std::span<const char> data,
                                    std::uint32_t crc = 0) noexcept {
  crc = ~crc;
  for (const char ch : data) {
    crc = detail::kCrc32Table[(crc ^ static_cast<std::uint8_t>(ch)) & 0xFFu] ^ (crc >> 8);
  }
  return ~crc;
}

inline std::uint32_t crc32(std::span<const char> data, std::uint32_t crc = 0) noexcept {
#if defined(__ARM_FEATURE_CRC32)
  crc = ~crc;
  const char* p = data.data();
  std::size_t n = data.size();
  for (; n >= 8; p += 8, n -= 8) {
    std::uint64_t word;
    std::memcpy(&word, p, 8);  // unaligned-safe load; compiles to a single ldr
    crc = __crc32d(crc, word);
  }
  for (; n > 0; ++p, --n) {
    crc = __crc32b(crc, static_cast<std::uint8_t>(*p));
  }
  return ~crc;
#else
  return crc32_table(data, crc);
#endif
}

inline constexpr bool kCrc32Hardware =
#if defined(__ARM_FEATURE_CRC32)
    true;
#else
    false;
#endif

}  // namespace cb
