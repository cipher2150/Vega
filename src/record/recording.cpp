#include "record/recording.hpp"

#include <zlib.h>

#include <algorithm>
#include <cstring>

namespace cb {

bool Recording::load(const std::string& path, std::string& error) {
  gzFile f = gzopen(path.c_str(), "rb");
  if (f == nullptr) {
    error = "cannot open " + path;
    return false;
  }
  data_.clear();
  constexpr std::size_t kChunk = 1u << 20;
  for (;;) {
    const std::size_t old = data_.size();
    data_.resize(old + kChunk);
    const int n = gzread(f, data_.data() + old, static_cast<unsigned>(kChunk));
    if (n < 0) {
      gzclose(f);
      error = "read error in " + path;
      return false;
    }
    data_.resize(old + static_cast<std::size_t>(n));
    if (n == 0) {
      break;
    }
  }
  gzclose(f);

  if (data_.size() < sizeof(cbrec::FileHeader)) {
    error = path + ": too short for a .cbrec header";
    return false;
  }
  std::memcpy(&header_, data_.data(), sizeof(header_));
  if (std::memcmp(header_.magic, cbrec::kMagic, sizeof(header_.magic)) != 0 ||
      header_.version != cbrec::kVersion) {
    error = path + ": not a .cbrec v1 file";
    return false;
  }

  records_.clear();
  max_payload_ = 0;
  std::size_t pos = sizeof(cbrec::FileHeader);
  while (pos + sizeof(cbrec::RecordHeader) <= data_.size()) {
    cbrec::RecordHeader h{};
    std::memcpy(&h, data_.data() + pos, sizeof(h));
    pos += sizeof(h);
    if (pos + h.len > data_.size()) {
      break;  // truncated final record (recording was killed): ignore it
    }
    if (static_cast<std::size_t>(h.venue) >= kNumVenues) {
      error = path + ": corrupt record (venue id)";
      return false;
    }
    records_.push_back(RecordView{h.venue, h.kind, h.flags, h.epoch, h.t_recv, h.wall_ns,
                                  std::string_view(data_.data() + pos, h.len)});
    max_payload_ = std::max<std::size_t>(max_payload_, h.len);
    pos += h.len;
  }
  return true;
}

}  // namespace cb
