// Sequence/gap rules, snapshot handling and integrity checks for Coinbase, OKX and Bybit,
// using frames shaped exactly like the live ones in docs/probes/probe_2026-10-29.txt.
#include "support/rig.hpp"

#include <gtest/gtest.h>

#include <string>

namespace cb {
namespace {

std::string cb_msg(int seq, const std::string& type, const std::string& updates,
                   const std::string& product = "BTC-USD") {
  return R"({"channel":"l2_data","timestamp":"2026-10-29T13:11:48.925743005Z","sequence_num":)" +
         std::to_string(seq) + R"(,"events":[{"type":")" + type + R"(","product_id":")" + product +
         R"(","updates":[)" + updates + "]}]}";
}
std::string cb_lvl(const char* side, const char* px, const char* qty) {
  return std::string(R"({"side":")") + side +
         R"(","event_time":"2026-10-29T13:11:48.213046Z","price_level":")" + px +
         R"(","new_quantity":")" + qty + R"("})";
}

AppConfig coinbase_cfg() {
  AppConfig c;
  c.venue(VenueId::kCoinbase).horizon_bp = 200;  // +/-2%
  c.venue(VenueId::kCoinbase).min_levels = 1;
  return c;
}

TEST(Coinbase, SnapshotThenUpdatesInSequence) {
  test::Rig rig({VenueId::kCoinbase}, coinbase_cfg());
  rig.connect(VenueId::kCoinbase, 1);
  ASSERT_EQ(rig.feed(VenueId::kCoinbase,
                     R"({"channel":"subscriptions","timestamp":"2026-10-29T13:11:48.7Z","sequence_num":0,"events":[]})")
                .status, FrameStatus::kOk);
  rig.feed(VenueId::kCoinbase,
           cb_msg(1, "snapshot", cb_lvl("bid", "100.00", "1") + "," + cb_lvl("bid", "99.50", "2") + "," +
                                     cb_lvl("offer", "100.10", "3")));
  EXPECT_EQ(rig.state(VenueId::kCoinbase, InstrumentId::kBtcUsd), BookState::kLive);
  rig.feed(VenueId::kCoinbase, cb_msg(2, "update", cb_lvl("bid", "100.05", "4") + "," +
                                                       cb_lvl("bid", "99.50", "0")));
  const Book& b = rig.book(VenueId::kCoinbase, InstrumentId::kBtcUsd);
  ASSERT_EQ(b.bids().size(), 2u);
  EXPECT_EQ(b.bids().price(0), (Price{10'005'000'000}));
  EXPECT_EQ(b.bids().qty(0), (Qty{400'000'000}));
}

TEST(Coinbase, HeartbeatsShareTheConnectionSequence) {
  test::Rig rig({VenueId::kCoinbase}, coinbase_cfg());
  rig.connect(VenueId::kCoinbase, 1);
  rig.feed(VenueId::kCoinbase, cb_msg(5, "snapshot", cb_lvl("bid", "100", "1") + "," + cb_lvl("offer", "101", "1")));
  // seq 6 is a heartbeat; a following l2 message must be 7.
  rig.feed(VenueId::kCoinbase, R"({"channel":"heartbeats","timestamp":"2026-10-29T13:11:49Z","sequence_num":6,"events":[{"current_time":"x","heartbeat_counter":3}]})");
  EXPECT_EQ(rig.feed(VenueId::kCoinbase, cb_msg(7, "update", cb_lvl("bid", "100", "2"))).status, FrameStatus::kOk);
  EXPECT_EQ(rig.state(VenueId::kCoinbase, InstrumentId::kBtcUsd), BookState::kLive);
}

TEST(Coinbase, SequenceGapGoesStale) {
  test::Rig rig({VenueId::kCoinbase}, coinbase_cfg());
  rig.connect(VenueId::kCoinbase, 1);
  rig.feed(VenueId::kCoinbase, cb_msg(1, "snapshot", cb_lvl("bid", "100", "1") + "," + cb_lvl("offer", "101", "1")));
  const FrameResult r = rig.feed(VenueId::kCoinbase, cb_msg(3, "update", cb_lvl("bid", "100", "2")));
  EXPECT_EQ(r.status, FrameStatus::kResync);
  EXPECT_EQ(r.reason, DownReason::kSequenceGap);
  EXPECT_EQ(rig.state(VenueId::kCoinbase, InstrumentId::kBtcUsd), BookState::kStale);
  // The update in the gapped message was never applied.
  EXPECT_EQ(rig.book(VenueId::kCoinbase, InstrumentId::kBtcUsd).bids().qty(0), (Qty{100'000'000}));
}

TEST(Coinbase, OutOfOrderLowerSequenceIsAlsoAnError) {
  test::Rig rig({VenueId::kCoinbase}, coinbase_cfg());
  rig.connect(VenueId::kCoinbase, 1);
  rig.feed(VenueId::kCoinbase, cb_msg(10, "snapshot", cb_lvl("bid", "100", "1") + "," + cb_lvl("offer", "101", "1")));
  EXPECT_EQ(rig.feed(VenueId::kCoinbase, cb_msg(9, "update", cb_lvl("bid", "100", "2"))).reason,
            DownReason::kSequenceGap);
}

TEST(Coinbase, HorizonDropsFarLevelsAndKeepsBandExact) {
  test::Rig rig({VenueId::kCoinbase}, coinbase_cfg());
  rig.connect(VenueId::kCoinbase, 1);
  // mid = 100.05 -> band = mid +/- 2% = [98.049, 102.051]
  rig.feed(VenueId::kCoinbase, cb_msg(1, "snapshot", cb_lvl("bid", "100", "1") + "," + cb_lvl("bid", "50", "9") + "," +
                                                         cb_lvl("offer", "100.10", "1") + "," + cb_lvl("offer", "500", "9")));
  const Book& b = rig.book(VenueId::kCoinbase, InstrumentId::kBtcUsd);
  EXPECT_EQ(b.bids().size(), 1u);
  EXPECT_EQ(b.asks().size(), 1u);
  rig.feed(VenueId::kCoinbase, cb_msg(2, "update", cb_lvl("bid", "99", "5") + "," + cb_lvl("bid", "60", "5")));
  EXPECT_EQ(b.bids().size(), 2u);  // 99 kept (in band), 60 dropped
  EXPECT_EQ(rig.state(VenueId::kCoinbase, InstrumentId::kBtcUsd), BookState::kLive);
}

TEST(Coinbase, HorizonExhaustionTriggersResync) {
  AppConfig c = coinbase_cfg();
  c.venue(VenueId::kCoinbase).min_levels = 2;
  test::Rig rig({VenueId::kCoinbase}, c);
  rig.connect(VenueId::kCoinbase, 1);
  rig.feed(VenueId::kCoinbase, cb_msg(1, "snapshot", cb_lvl("bid", "100", "1") + "," + cb_lvl("bid", "99", "1") + "," +
                                                         cb_lvl("offer", "101", "1") + "," + cb_lvl("offer", "101.5", "1")));
  EXPECT_EQ(rig.state(VenueId::kCoinbase, InstrumentId::kBtcUsd), BookState::kLive);
  rig.feed(VenueId::kCoinbase, cb_msg(2, "update", cb_lvl("offer", "101", "0")));  // 1 ask left < 2
  EXPECT_EQ(rig.state(VenueId::kCoinbase, InstrumentId::kBtcUsd), BookState::kStale);
  EXPECT_EQ(rig.summary(VenueId::kCoinbase, InstrumentId::kBtcUsd).last_reason, DownReason::kHorizon);
}

// ---- OKX --------------------------------------------------------------------------
std::string okx_msg(const std::string& action, long long seq, long long prev, const std::string& bids,
                    const std::string& asks) {
  return R"({"arg":{"channel":"books","instId":"BTC-USDT"},"action":")" + action +
         R"(","data":[{"asks":[)" + asks + R"(],"bids":[)" + bids +
         R"(],"ts":"1793279526701","checksum":0,"seqId":)" + std::to_string(seq) +
         R"(,"prevSeqId":)" + std::to_string(prev) + "}]}";
}

TEST(Okx, ChainedUpdatesApply) {
  test::Rig rig({VenueId::kOkx});
  rig.connect(VenueId::kOkx, 1);
  rig.feed(VenueId::kOkx, okx_msg("snapshot", 100, -1, R"(["80000","1","0","1"])", R"(["80001","2","0","1"])"));
  EXPECT_EQ(rig.state(VenueId::kOkx, InstrumentId::kBtcUsd), BookState::kLive);
  EXPECT_EQ(rig.feed(VenueId::kOkx, okx_msg("update", 107, 100, R"(["80000.5","3","0","2"])", "")).status, FrameStatus::kOk);
  // No-change keep-alive: seqId == prevSeqId == last.
  EXPECT_EQ(rig.feed(VenueId::kOkx, okx_msg("update", 107, 107, "", "")).status, FrameStatus::kOk);
  // seqId may go DOWN after maintenance; only the prev->last chain matters.
  EXPECT_EQ(rig.feed(VenueId::kOkx, okx_msg("update", 50, 107, "", "")).status, FrameStatus::kOk);
  EXPECT_EQ(rig.book(VenueId::kOkx, InstrumentId::kBtcUsd).bids().price(0), (Price{8'000'050'000'000}));
  EXPECT_EQ(rig.feed(VenueId::kOkx, "pong").status, FrameStatus::kOk);  // bare text pong
}

TEST(Okx, BrokenChainDiscardsTheMessageAndGoesStale) {
  test::Rig rig({VenueId::kOkx});
  rig.connect(VenueId::kOkx, 1);
  rig.feed(VenueId::kOkx, okx_msg("snapshot", 100, -1, R"(["80000","1","0","1"])", R"(["80001","2","0","1"])"));
  const FrameResult r = rig.feed(VenueId::kOkx, okx_msg("update", 120, 110, R"(["80000.9","5","0","1"])", ""));
  EXPECT_EQ(r.reason, DownReason::kSequenceGap);
  EXPECT_EQ(rig.state(VenueId::kOkx, InstrumentId::kBtcUsd), BookState::kStale);
  // Levels precede seqId on the wire, but they were only staged: never applied.
  EXPECT_EQ(rig.book(VenueId::kOkx, InstrumentId::kBtcUsd).bids().price(0), (Price{8'000'000'000'000}));
}

TEST(Okx, CrossedBookIsAnIntegrityFailure) {
  test::Rig rig({VenueId::kOkx});
  rig.connect(VenueId::kOkx, 1);
  rig.feed(VenueId::kOkx, okx_msg("snapshot", 1, -1, R"(["80002","1","0","1"])", R"(["80001","2","0","1"])"));
  EXPECT_EQ(rig.state(VenueId::kOkx, InstrumentId::kBtcUsd), BookState::kStale);
  EXPECT_EQ(rig.summary(VenueId::kOkx, InstrumentId::kBtcUsd).last_reason, DownReason::kCrossed);
  EXPECT_EQ(rig.pipeline().shared(VenueId::kOkx).resync_request.load(), 1u);
}

TEST(Okx, ErrorEventAndMalformedJsonResync) {
  test::Rig rig({VenueId::kOkx});
  rig.connect(VenueId::kOkx, 1);
  EXPECT_EQ(rig.feed(VenueId::kOkx, R"({"event":"error","code":"60012","msg":"Invalid request"})").reason,
            DownReason::kVenueError);
  rig.connect(VenueId::kOkx, 2);
  EXPECT_EQ(rig.feed(VenueId::kOkx, R"({"arg":{"channel":"books","instId":"BTC-USDT"},"action":)").reason,
            DownReason::kParseError);
  rig.connect(VenueId::kOkx, 3);
  EXPECT_EQ(rig.feed(VenueId::kOkx, okx_msg("snapshot", 1, -1, R"(["8e4","1","0","1"])", "")).reason,
            DownReason::kParseError) << "exponent notation must be rejected, not guessed";
}

// ---- Bybit ------------------------------------------------------------------------
std::string bybit_msg(const std::string& type, long long u, const std::string& b, const std::string& a) {
  return R"({"topic":"orderbook.50.BTCUSDT","ts":1793279507515,"type":")" + type +
         R"(","data":{"s":"BTCUSDT","b":[)" + b + R"(],"a":[)" + a + R"(],"u":)" + std::to_string(u) +
         R"(,"seq":116369800697},"cts":1793279507513})";
}

TEST(Bybit, ContiguousDeltasApplyAndGapGoesStale) {
  test::Rig rig({VenueId::kBybit});
  rig.connect(VenueId::kBybit, 1);
  rig.feed(VenueId::kBybit, bybit_msg("snapshot", 707863, R"(["80113.8","0.19"])", R"(["80114","1"])"));
  EXPECT_EQ(rig.state(VenueId::kBybit, InstrumentId::kBtcUsd), BookState::kLive);
  EXPECT_EQ(rig.feed(VenueId::kBybit, bybit_msg("delta", 707864, "", R"(["80114","0"],["80115","2"])")).status,
            FrameStatus::kOk);
  EXPECT_EQ(rig.book(VenueId::kBybit, InstrumentId::kBtcUsd).asks().price(0), (Price{8'011'500'000'000}));
  EXPECT_EQ(rig.feed(VenueId::kBybit, bybit_msg("delta", 707866, R"(["80100","1"])", "")).reason,
            DownReason::kSequenceGap);
  EXPECT_EQ(rig.state(VenueId::kBybit, InstrumentId::kBtcUsd), BookState::kStale);
}

TEST(Bybit, NewSnapshotResetsTheBook) {
  test::Rig rig({VenueId::kBybit});
  rig.connect(VenueId::kBybit, 1);
  rig.feed(VenueId::kBybit, bybit_msg("snapshot", 5, R"(["100","1"],["99","1"])", R"(["101","1"])"));
  // Service restart: Bybit pushes a snapshot with u=1; the old levels must disappear.
  rig.feed(VenueId::kBybit, bybit_msg("snapshot", 1, R"(["98","1"])", R"(["102","1"])"));
  const Book& b = rig.book(VenueId::kBybit, InstrumentId::kBtcUsd);
  EXPECT_EQ(b.bids().size(), 1u);
  EXPECT_EQ(b.bids().price(0), (Price{9'800'000'000}));
  EXPECT_EQ(rig.feed(VenueId::kBybit, bybit_msg("delta", 2, R"(["97","1"])", "")).status, FrameStatus::kOk);
}

TEST(Bybit, ControlMessagesAreIgnoredAndFailuresResync) {
  test::Rig rig({VenueId::kBybit});
  rig.connect(VenueId::kBybit, 1);
  EXPECT_EQ(rig.feed(VenueId::kBybit, R"({"success":true,"ret_msg":"pong","conn_id":"x","op":"ping"})").status,
            FrameStatus::kOk);
  EXPECT_EQ(rig.feed(VenueId::kBybit, R"({"success":false,"ret_msg":"error:handler not found","op":"subscribe"})").reason,
            DownReason::kVenueError);
}

TEST(Venue, DisconnectMarksAllBooksStale) {
  test::Rig rig({VenueId::kBybit});
  rig.connect(VenueId::kBybit, 1);
  rig.feed(VenueId::kBybit, bybit_msg("snapshot", 5, R"(["100","1"])", R"(["101","1"])"));
  rig.disconnect(VenueId::kBybit);
  EXPECT_EQ(rig.state(VenueId::kBybit, InstrumentId::kBtcUsd), BookState::kStale);
  EXPECT_EQ(rig.summary(VenueId::kBybit, InstrumentId::kBtcUsd).last_reason, DownReason::kDisconnected);
}

}  // namespace
}  // namespace cb
