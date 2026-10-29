// Golden-file tests: replay a recorded live session (40 s, all four venues, recorded
// 2026-10-29) and compare the final books with tests/data/golden_session.expected.txt.
//
// The expected file is NOT produced by this C++ code: it comes from tools/
// golden_reference.py, an independent Python/Decimal implementation of the same protocol
// rules. Agreement means two unrelated implementations reach identical books.
#include "record/recording.hpp"
#include "record/replay.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace cb {
namespace {

const std::string kRecording = std::string(CB_TEST_DATA_DIR) + "/golden_session.cbrec.gz";
const std::string kExpected = std::string(CB_TEST_DATA_DIR) + "/golden_session.expected.txt";

std::string dump(Engine& e) {
  std::FILE* f = std::tmpfile();
  e.dump(f, false);
  std::rewind(f);
  std::string out;
  char buf[4096];
  for (std::size_t n; (n = std::fread(buf, 1, sizeof(buf), f)) > 0;) out.append(buf, n);
  std::fclose(f);
  return out;
}

std::string read_file(const std::string& path) {
  std::ifstream in(path);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

class Golden : public ::testing::Test {
 protected:
  void SetUp() override {
    std::string err;
    ASSERT_TRUE(rec_.load(kRecording, err)) << err;
    expected_ = read_file(kExpected);
    ASSERT_FALSE(expected_.empty()) << kExpected;
  }
  Recording rec_;
  std::string expected_;
  AppConfig config_;  // defaults == the config used when recording
};

TEST_F(Golden, InlineReplayMatchesIndependentReference) {
  DrainWaiter waiter;
  auto p = std::make_unique<Pipeline>(config_, waiter);
  waiter.attach(p->engine());
  replay_inline(*p, rec_);
  EXPECT_EQ(dump(p->engine()), expected_);

  for (const VenueId v : kAllVenues) {
    for (const VenueInstrument& vi : instruments_of(v)) {
      EXPECT_EQ(p->engine().state(v, vi.id), BookState::kLive) << venue_name(v) << " " << vi.symbol;
      const BookSummary& s = p->engine().snapshot().books[idx(v)][idx(vi.id)];
      EXPECT_EQ(s.checksum_fail, 0u);
      EXPECT_EQ(s.resyncs, 0u);
    }
  }
  EXPECT_GT(p->engine().snapshot().books[idx(VenueId::kKraken)][0].checksum_ok, 1000u);
}

// Same input through the real two-thread layout (producer thread + book thread over the
// SPSC rings) must give the same books: threading changes timing, never results.
TEST_F(Golden, PipelinedReplayIsDeterministic) {
  SpinWaiter waiter(0);
  auto p = std::make_unique<Pipeline>(config_, waiter);
  replay_pipelined(*p, rec_);
  EXPECT_EQ(dump(p->engine()), expected_);
}

}  // namespace
}  // namespace cb
