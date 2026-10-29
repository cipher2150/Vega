#include "bench_util.hpp"

#include <benchmark/benchmark.h>

#include <cstdio>

int main(int argc, char** argv) {
  const auto placement = cb::bench::place_thread(cb::ThreadRole::kBook, "bench-main", 0);
  benchmark::AddCustomContext("main_thread_pinned", placement.pinned ? "yes" : "no");
  benchmark::AddCustomContext("main_thread_qos", placement.qos_applied ? "USER_INTERACTIVE" : "no");

  benchmark::Initialize(&argc, argv);
  if (benchmark::ReportUnrecognizedArguments(argc, argv)) {
    return 1;
  }
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
