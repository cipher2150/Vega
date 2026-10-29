// Full parse -> normalize -> ring -> book -> checks -> consolidated -> signals -> publish
// pipeline on REAL recorded frames (the committed golden session: 40 s of all four
// venues). One iteration replays the whole session into a fresh pipeline; pipeline
// construction is excluded from timing.
//
// Override the recording with CB_BENCH_RECORDING=/path/to/file.cbrec[.gz].
#include "record/recording.hpp"
#include "record/replay.hpp"

#include <benchmark/benchmark.h>

#include <cstdlib>
#include <memory>
#include <string>

namespace {

const cb::Recording& recording() {
  static const cb::Recording rec = [] {
    cb::Recording r;
    const char* env = std::getenv("CB_BENCH_RECORDING");
    const std::string path = env != nullptr ? env : CB_GOLDEN_RECORDING;
    std::string err;
    if (!r.load(path, err)) {
      std::fprintf(stderr, "bench_pipeline_replay: %s\n", err.c_str());
      std::exit(1);
    }
    return r;
  }();
  return rec;
}

void BM_ReplayInline(benchmark::State& state) {
  const cb::Recording& rec = recording();
  const cb::AppConfig config;
  std::vector<char> scratch(rec.max_payload() + simdjson::SIMDJSON_PADDING);
  std::uint64_t frames = 0;
  std::uint64_t bytes = 0;
  std::uint64_t messages = 0;
  for (auto _ : state) {
    state.PauseTiming();
    cb::DrainWaiter waiter;
    auto p = std::make_unique<cb::Pipeline>(config, waiter);
    waiter.attach(p->engine());
    state.ResumeTiming();

    const cb::ReplayResult r = cb::replay_inline(*p, rec, 0, SIZE_MAX, scratch);

    state.PauseTiming();
    frames += r.frames;
    bytes += r.bytes;
    messages += p->engine().snapshot().messages;
    p.reset();
    state.ResumeTiming();
  }
  state.counters["frames/s"] = benchmark::Counter(static_cast<double>(frames), benchmark::Counter::kIsRate);
  state.counters["book_msgs/s"] = benchmark::Counter(static_cast<double>(messages), benchmark::Counter::kIsRate);
  state.SetBytesProcessed(static_cast<std::int64_t>(bytes));
}
BENCHMARK(BM_ReplayInline)->Unit(benchmark::kMillisecond)->UseRealTime()->MinTime(2.0);

}  // namespace
