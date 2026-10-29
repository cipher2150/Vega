#include "platform/cacheline.hpp"
#include "platform/thread.hpp"

#include <gtest/gtest.h>

#include <thread>

namespace cb {
namespace {

TEST(Platform, CompileTimeCacheLineIsNotSmallerThanHardware) {
  const std::size_t runtime = runtime_cache_line_size();
  if (runtime == 0) {
    GTEST_SKIP() << "OS does not report a cache line size";
  }
  EXPECT_LE(runtime, kCacheLine) << "padding constant too small for this machine";
}

TEST(Platform, ThreadRoleIsApplied) {
  PlacementResult result;
  std::thread t([&] { result = set_current_thread_role(ThreadRole::kBook, "cb-test-book", 0); });
  t.join();
  EXPECT_TRUE(result.named);
#if defined(__APPLE__)
  EXPECT_TRUE(result.qos_applied);
  EXPECT_FALSE(result.pinned);  // macOS has no hard affinity; we must not claim it
#endif
}

}  // namespace
}  // namespace cb
