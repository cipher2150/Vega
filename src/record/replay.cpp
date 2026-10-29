#include "record/replay.hpp"

#include "core/clock.hpp"
#include "platform/thread.hpp"

#include <simdjson.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>

namespace cb {

namespace {

// Feeds one record to its venue's protocol. `buf` has room for any payload + padding.
void feed(Pipeline& pipeline, const RecordView& r, char* buf, std::size_t cap,
          ReplayResult& result) {
  VenueProtocol* p = pipeline.protocol(r.venue);
  if (p == nullptr) {
    return;
  }
  switch (r.kind) {
    case cbrec::RecordKind::kConnect:
      p->on_connect(r.epoch);
      break;
    case cbrec::RecordKind::kDisconnect:
      // A shutdown is the end of the recording, not a market event: the books were valid
      // up to that point, so replay leaves them live instead of marking them stale.
      if (static_cast<DownReason>(r.flags) != DownReason::kShutdown) {
        p->on_disconnect(static_cast<DownReason>(r.flags));
      }
      break;
    case cbrec::RecordKind::kFrame: {
      std::memcpy(buf, r.payload.data(), r.payload.size());
      // t_recv is *now*: replay measures our processing, not the recorded network timing.
      // recv_wall is the recorded one, so exchange-lag statistics replay identically.
      const FrameMeta meta{clock::ticks(), r.wall_ns};
      (void)p->on_frame(buf, r.payload.size(), cap, meta);
      ++result.frames;
      result.bytes += r.payload.size();
      break;
    }
  }
}

}  // namespace

ReplayResult replay_inline(Pipeline& pipeline, const Recording& rec, std::size_t begin,
                           std::size_t end, std::span<char> scratch) {
  std::unique_ptr<char[]> owned;
  if (scratch.size() < rec.max_payload() + simdjson::SIMDJSON_PADDING) {
    const std::size_t n = rec.max_payload() + simdjson::SIMDJSON_PADDING;
    owned = std::make_unique<char[]>(n);
    scratch = std::span<char>(owned.get(), n);
  }
  char* const buf = scratch.data();
  const std::size_t cap = scratch.size();
  ReplayResult result;
  const auto& records = rec.records();
  end = std::min(end, records.size());
  const auto t0 = std::chrono::steady_clock::now();
  for (std::size_t i = begin; i < end; ++i) {
    feed(pipeline, records[i], buf, cap, result);
    pipeline.engine().poll(pipeline.ring(records[i].venue), EventRing::kCapacity);
  }
  result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  return result;
}

ReplayResult replay_pipelined(Pipeline& pipeline, const Recording& rec, double speed) {
  std::atomic<bool> done{false};
  ReplayResult result;
  const auto t0 = std::chrono::steady_clock::now();

  std::thread producer([&] {
    set_current_thread_role(ThreadRole::kIo, "replay-io");
    const std::size_t cap = rec.max_payload() + simdjson::SIMDJSON_PADDING;
    const auto buf = std::make_unique<char[]>(cap);
    const auto& records = rec.records();
    // Recorded ticks -> this machine's ticks (the recording may come from another box).
    const double scale = static_cast<double>(clock::ticks_per_second()) /
                         static_cast<double>(rec.header().tick_freq) / (speed > 0 ? speed : 1.0);
    const std::uint64_t rec0 = records.empty() ? 0 : records.front().t_recv;
    const std::uint64_t base = clock::ticks();
    for (const RecordView& r : records) {
      if (speed > 0) {
        const auto target = base + static_cast<std::uint64_t>(static_cast<double>(r.t_recv - rec0) * scale);
        while (clock::ticks() < target) {
          // Spin rather than sleep: sleep granularity (~1 ms on macOS) would distort the
          // inter-arrival pattern we are trying to reproduce.
        }
      }
      feed(pipeline, r, buf.get(), cap, result);
    }
    // release: everything the producer pushed happens-before the consumer seeing done.
    done.store(true, std::memory_order_release);
  });

  for (;;) {
    // acquire before the final drain, so no event pushed before `done` can be missed.
    const bool finished = done.load(std::memory_order_acquire);
    const std::size_t n = pipeline.poll_all();
    if (n == 0 && finished) {
      break;
    }
  }
  producer.join();
  result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  return result;
}

}  // namespace cb
