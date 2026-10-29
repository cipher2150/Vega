#pragma once

#include "record/cbrec.hpp"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace cb {

// Appends records to a .cbrec file. Called from the venue I/O threads (never the book
// thread). A mutex serializes the writers; with a few hundred frames/s across four
// threads it is essentially uncontended, and a large stdio buffer turns most writes into
// a memcpy. Recording perturbs I/O-thread timing slightly, which is acceptable because
// published latency numbers come from replay, not from recording runs (DESIGN §7).
class Recorder {
 public:
  ~Recorder();
  bool open(const std::string& path);
  void write(VenueId venue, cbrec::RecordKind kind, std::uint32_t epoch, std::uint64_t t_recv,
             std::int64_t wall_ns, std::string_view payload, std::uint16_t flags = 0);
  void close();

 private:
  std::mutex mu_;
  std::FILE* file_ = nullptr;
  std::unique_ptr<char[]> buffer_;
};

}  // namespace cb
