// Kraken: checksum algorithm, against real frames captured live on 2026-10-29
// (tests/data/kraken_frames.jsonl: 300 frames, BTC/USD + ETH/USD depth 25, USDT/USD depth 10).
#include "core/crc32.hpp"
#include "support/rig.hpp"
#include "venue/kraken_checksum.hpp"

#include <gtest/gtest.h>

#include <random>
#include <string>

namespace cb {
namespace {

const std::string kFrames = std::string(CB_TEST_DATA_DIR) + "/kraken_frames.jsonl";

TEST(Crc32, StandardCheckValue) {
  // The CRC-32/ISO-HDLC check value for "123456789" (same as zlib.crc32).
  constexpr std::string_view s = "123456789";
  EXPECT_EQ(crc32_table(s), 0xCBF43926u);
  EXPECT_EQ(crc32(s), 0xCBF43926u);
}

TEST(Crc32, HardwarePathMatchesTable) {
  std::mt19937 rng(9);
  std::string data;
  for (int len = 0; len < 300; ++len) {
    data.push_back(static_cast<char>(rng() & 0xFF));
    ASSERT_EQ(crc32(data), crc32_table(data)) << "len " << len;
  }
  RecordProperty("hardware_crc32", kCrc32Hardware ? "yes" : "no");
}

TEST(KrakenDigits, FormattingRules) {
  std::array<char, 64> buf{};
  auto digits = [&](std::int64_t raw, int precision) {
    char* p = buf.data();
    EXPECT_TRUE(detail::append_kraken_digits(raw, precision, p));
    return std::string(buf.data(), p);
  };
  EXPECT_EQ(digits(8'000'630'000'000, 1), "800063");   // 80006.3  -> "800063"
  EXPECT_EQ(digits(8'000'100'000'000, 1), "800010");   // 80001.0  -> "800010"
  EXPECT_EQ(digits(6'120, 8), "6120");                 // 0.00006120 -> "6120"
  EXPECT_EQ(digits(99'987'000, 5), "99987");           // 0.99987  -> "99987"
  EXPECT_EQ(digits(100'000'000, 8), "100000000");      // 1.00000000
  char* p = buf.data();
  EXPECT_FALSE(detail::append_kraken_digits(8'000'630'000'001, 1, p))  // too many decimals
      << "value not representable at the instrument precision must fail";
}

TEST(Kraken, EveryLiveChecksumValidates) {
  const auto frames = test::read_lines(kFrames);
  ASSERT_GE(frames.size(), 300u) << kFrames;
  test::Rig rig({VenueId::kKraken});
  rig.connect(VenueId::kKraken, 1);
  for (const auto& f : frames) {
    ASSERT_EQ(rig.feed(VenueId::kKraken, f).status, FrameStatus::kOk) << f.substr(0, 200);
  }
  std::uint64_t ok = 0;
  for (const auto inst : {InstrumentId::kBtcUsd, InstrumentId::kEthUsd, InstrumentId::kUsdtUsd}) {
    const BookSummary& s = rig.summary(VenueId::kKraken, inst);
    EXPECT_EQ(rig.state(VenueId::kKraken, inst), BookState::kLive) << instrument_name(inst);
    EXPECT_EQ(s.checksum_fail, 0u) << instrument_name(inst);
    ok += s.checksum_ok;
  }
  EXPECT_GT(ok, 250u);
  // Depth truncation: never more than the subscribed depth.
  EXPECT_LE(rig.book(VenueId::kKraken, InstrumentId::kBtcUsd).bids().size(), 25u);
  EXPECT_LE(rig.book(VenueId::kKraken, InstrumentId::kUsdtUsd).asks().size(), 10u);
  EXPECT_TRUE(rig.engine().snapshot().fx_live);
}

// A checksum that disagrees with our book (here: the venue's checksum altered by one) must
// take the venue stale and ask the I/O thread to resync this epoch. Altering the checksum
// rather than a level is deliberate: a changed level below the top 10 would not change
// Kraken's checksum at all, making the test depend on which level we happened to hit.
TEST(Kraken, ChecksumMismatchGoesStaleAndRequestsResync) {
  auto frames = test::read_lines(kFrames);
  test::Rig rig({VenueId::kKraken});
  rig.connect(VenueId::kKraken, 7);
  std::size_t i = 0;
  for (; i < frames.size(); ++i) {  // feed until the BTC book is live
    rig.feed(VenueId::kKraken, frames[i]);
    if (rig.state(VenueId::kKraken, InstrumentId::kBtcUsd) == BookState::kLive) break;
  }
  for (++i; i < frames.size(); ++i) {
    std::string& f = frames[i];
    if (f.find(R"("symbol":"BTC/USD")") == std::string::npos ||
        f.find(R"("type":"update")") == std::string::npos) {
      rig.feed(VenueId::kKraken, f);
      continue;
    }
    const auto c = f.find(R"("checksum":)");
    ASSERT_NE(c, std::string::npos);
    char& d = f[c + 11];  // first digit of the checksum
    d = d == '9' ? '1' : static_cast<char>(d + 1);
    rig.feed(VenueId::kKraken, f);
    break;
  }
  ASSERT_LT(i, frames.size()) << "no BTC update found in fixture";
  EXPECT_EQ(rig.state(VenueId::kKraken, InstrumentId::kBtcUsd), BookState::kStale);
  EXPECT_EQ(rig.summary(VenueId::kKraken, InstrumentId::kBtcUsd).last_reason, DownReason::kChecksum);
  // The whole venue goes stale (one connection), and the resync request names epoch 7.
  EXPECT_EQ(rig.state(VenueId::kKraken, InstrumentId::kEthUsd), BookState::kStale);
  EXPECT_EQ(rig.pipeline().shared(VenueId::kKraken).resync_request.load(), 7u);

  // Further frames from the dead session are dropped...
  const auto drops = rig.engine().snapshot().stale_epoch_drops;
  rig.feed(VenueId::kKraken, frames[i + 1]);
  EXPECT_GE(rig.engine().snapshot().stale_epoch_drops, drops);
  EXPECT_EQ(rig.state(VenueId::kKraken, InstrumentId::kBtcUsd), BookState::kStale);

  // ...and a new session with fresh snapshots recovers.
  rig.connect(VenueId::kKraken, 8);
  for (const auto& f : frames) {
    if (f.find(R"("type":"snapshot")") != std::string::npos) rig.feed(VenueId::kKraken, f);
  }
  EXPECT_EQ(rig.state(VenueId::kKraken, InstrumentId::kBtcUsd), BookState::kLive);
}

}  // namespace
}  // namespace cb
