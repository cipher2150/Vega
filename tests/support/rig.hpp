#pragma once

// Test harness: a full Pipeline (protocols + rings + engine) driven synchronously.
// feed() parses one frame on the calling thread and drains its events into the engine,
// exactly like inline replay.

#include "core/clock.hpp"
#include "engine/pipeline.hpp"

#include <simdjson.h>

#include <fstream>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace cb::test {

class Rig {
 public:
  explicit Rig(std::initializer_list<VenueId> enabled, AppConfig cfg = AppConfig{})
      : config_(std::move(cfg)) {
    for (const VenueId v : kAllVenues) config_.venue(v).enabled = false;
    for (const VenueId v : enabled) config_.venue(v).enabled = true;
    pipeline_ = std::make_unique<Pipeline>(config_, waiter_);
    waiter_.attach(pipeline_->engine());
  }

  void connect(VenueId v, std::uint32_t epoch) {
    pipeline_->protocol(v)->on_connect(epoch);
    drain(v);
  }

  void disconnect(VenueId v, DownReason reason = DownReason::kDisconnected) {
    pipeline_->protocol(v)->on_disconnect(reason);
    drain(v);
  }

  FrameResult feed(VenueId v, std::string_view json, std::int64_t wall_ns = 0) {
    buf_.assign(json.begin(), json.end());
    buf_.resize(json.size() + simdjson::SIMDJSON_PADDING, '\0');
    const FrameResult r = pipeline_->protocol(v)->on_frame(buf_.data(), json.size(), buf_.size(),
                                                           FrameMeta{clock::ticks(), wall_ns});
    drain(v);
    return r;
  }

  Engine& engine() { return pipeline_->engine(); }
  Pipeline& pipeline() { return *pipeline_; }
  BookState state(VenueId v, InstrumentId i) { return engine().state(v, i); }
  const Book& book(VenueId v, InstrumentId i) { return engine().book(v, i); }
  const BookSummary& summary(VenueId v, InstrumentId i) {
    return engine().snapshot().books[idx(v)][idx(i)];
  }

 private:
  void drain(VenueId v) { pipeline_->engine().poll(pipeline_->ring(v), EventRing::kCapacity); }

  AppConfig config_;
  DrainWaiter waiter_;
  std::unique_ptr<Pipeline> pipeline_;
  std::vector<char> buf_;
};

inline std::vector<std::string> read_lines(const std::string& path) {
  std::vector<std::string> lines;
  std::ifstream in(path);
  for (std::string line; std::getline(in, line);) {
    if (!line.empty()) lines.push_back(line);
  }
  return lines;
}

}  // namespace cb::test
