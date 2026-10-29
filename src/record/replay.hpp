#pragma once

#include "engine/pipeline.hpp"
#include "record/recording.hpp"

#include <cstdint>
#include <span>

namespace cb {

struct ReplayResult {
  std::uint64_t frames = 0;
  std::uint64_t bytes = 0;
  double seconds = 0;  // wall time of the replay loop only (file already in memory)
};

// Single thread: parse a frame, then drain its events into the engine, in record order.
// Fully deterministic - the same file always yields bit-identical books - so golden tests
// and the allocation test use it. `begin`/`end` select a record range (for warm-up splits).
// `scratch` (optional) is the frame buffer; it must hold max_payload() + SIMDJSON_PADDING
// bytes. Passing one keeps the call allocation-free (the allocation test relies on it).
ReplayResult replay_inline(Pipeline& pipeline, const Recording& rec, std::size_t begin = 0,
                           std::size_t end = SIZE_MAX, std::span<char> scratch = {});

// Real thread layout: a producer thread plays the I/O threads (parse + ring push) while
// the calling thread runs the book loop. Same final state as inline, but stage timings
// include the cross-core hand-off.
//
// speed == 0: as fast as possible. This measures saturated THROUGHPUT; queue/e2e times
//             are then dominated by backlog (Little's law) and are not latencies.
// speed  > 0: frames are released at their recorded inter-arrival times divided by
//             `speed` (1 = real time). Use this for cross-thread LATENCY numbers.
ReplayResult replay_pipelined(Pipeline& pipeline, const Recording& rec, double speed = 0);

}  // namespace cb
