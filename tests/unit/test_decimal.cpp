#include "core/decimal.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <string_view>

namespace cb {
namespace {

constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();

DecimalResult parse(std::string_view s) { return parse_decimal(s); }

std::string format(std::int64_t v) {
  std::array<char, kMaxDecimalChars> buf{};
  const auto n = format_decimal(v, buf);
  return std::string(buf.data(), n);
}

TEST(Decimal, ParsesRealFeedValues) {
  // Shapes taken from the 2026-10-29 probe (docs/probes/probe_2026-10-29.txt).
  EXPECT_EQ(parse("80006.3").value, 8'000'630'000'000);      // Kraken price
  EXPECT_EQ(parse("0.00006120").value, 6'120);               // Kraken qty, trailing zero
  EXPECT_EQ(parse("80000.62").value, 8'000'062'000'000);     // Coinbase price
  EXPECT_EQ(parse("0.10705196").value, 10'705'196);          // Coinbase qty, 8 dp
  EXPECT_EQ(parse("79998").value, 7'999'800'000'000);        // Coinbase integer price
  EXPECT_EQ(parse("80001.0").value, 8'000'100'000'000);      // Kraken ".0"
  EXPECT_EQ(parse("0").value, 0);                            // deletion marker
  EXPECT_EQ(parse("0.00000001").value, 1);                   // smallest unit
}

TEST(Decimal, ZeroLeadingAndTrailingDigitsCarryNoValue) {
  EXPECT_EQ(parse("000123.4500").value, parse("123.45").value);
  EXPECT_TRUE(parse("1.000000000000").ok());  // zeros beyond 8 dp are not precision loss
  EXPECT_EQ(parse("1.000000000000").value, kFixedScale);
}

TEST(Decimal, RejectsNinthSignificantFractionalDigit) {
  EXPECT_EQ(parse("0.000000001").error, DecimalError::kPrecision);
  EXPECT_EQ(parse("1.123456789").error, DecimalError::kPrecision);
  EXPECT_EQ(parse("1.123456780001").error, DecimalError::kPrecision);
}

TEST(Decimal, RejectsMalformedInput) {
  EXPECT_EQ(parse("").error, DecimalError::kEmpty);
  for (const char* bad : {".5", "5.", "-1", "+1", " 1", "1 ", "1e5", "1E-8", "1.2.3", "abc",
                          "1,5", "0x10", "."}) {
    EXPECT_EQ(parse(bad).error, DecimalError::kSyntax) << "input: '" << bad << "'";
  }
}

TEST(Decimal, OverflowBoundaryIsExact) {
  // INT64_MAX = 9223372036854775807 -> 92233720368.54775807 in 1e-8 units.
  const auto max_ok = parse("92233720368.54775807");
  ASSERT_TRUE(max_ok.ok());
  EXPECT_EQ(max_ok.value, kMax);
  EXPECT_EQ(parse("92233720368.54775808").error, DecimalError::kOverflow);
  EXPECT_EQ(parse("92233720369").error, DecimalError::kOverflow);
  EXPECT_EQ(parse("99999999999999999999999").error, DecimalError::kOverflow);
  EXPECT_TRUE(parse("00000000000000000000001").ok());  // leading zeros never overflow
}

TEST(Decimal, IsUsableAtCompileTime) {
  static_assert(parse_decimal("1.5").value == 150'000'000);
  static_assert(parse_decimal("1e5").error == DecimalError::kSyntax);
}

TEST(Decimal, FormatIsMinimal) {
  EXPECT_EQ(format(8'000'630'000'000), "80006.3");
  EXPECT_EQ(format(6'120), "0.0000612");
  EXPECT_EQ(format(0), "0");
  EXPECT_EQ(format(kFixedScale), "1");
  EXPECT_EQ(format(kMax), "92233720368.54775807");
}

TEST(Decimal, FormatParseRoundTripRandom) {
  std::mt19937_64 rng(12345);
  std::uniform_int_distribution<std::int64_t> any(0, kMax);
  std::uniform_int_distribution<std::int64_t> small(0, 10'000'000'000'000);
  for (int i = 0; i < 200'000; ++i) {
    const std::int64_t v = (i % 2 == 0) ? any(rng) : small(rng);
    const auto r = parse(format(v));
    ASSERT_TRUE(r.ok()) << v;
    ASSERT_EQ(r.value, v);
  }
}

}  // namespace
}  // namespace cb
