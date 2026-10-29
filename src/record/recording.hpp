#pragma once

#include "record/cbrec.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cb {

struct RecordView {
  VenueId venue;
  cbrec::RecordKind kind;
  std::uint16_t flags;
  std::uint32_t epoch;
  std::uint64_t t_recv;
  std::int64_t wall_ns;
  std::string_view payload;  // points into Recording's buffer
};

// A whole .cbrec file loaded into memory (cold path, before any timing starts).
// Reads plain or gzip-compressed files transparently (zlib's gzread passes plain files
// through), so golden test data can be committed compressed.
class Recording {
 public:
  bool load(const std::string& path, std::string& error);

  [[nodiscard]] const cbrec::FileHeader& header() const noexcept { return header_; }
  [[nodiscard]] const std::vector<RecordView>& records() const noexcept { return records_; }
  [[nodiscard]] std::size_t max_payload() const noexcept { return max_payload_; }

 private:
  cbrec::FileHeader header_{};
  std::vector<char> data_;
  std::vector<RecordView> records_;
  std::size_t max_payload_ = 0;
};

}  // namespace cb
