#include "record/recorder.hpp"

#include "core/clock.hpp"
#include "core/log.hpp"

namespace cb {

namespace {
constexpr std::size_t kBufferBytes = 8u << 20;
}

Recorder::~Recorder() { close(); }

bool Recorder::open(const std::string& path) {
  const std::lock_guard lock(mu_);
  file_ = std::fopen(path.c_str(), "wb");
  if (file_ == nullptr) {
    CB_ERROR("recorder: cannot open %s", path.c_str());
    return false;
  }
  buffer_ = std::make_unique<char[]>(kBufferBytes);
  std::setvbuf(file_, buffer_.get(), _IOFBF, kBufferBytes);
  cbrec::FileHeader h{};
  std::memcpy(h.magic, cbrec::kMagic, sizeof(h.magic));
  h.version = cbrec::kVersion;
  h.tick_freq = clock::ticks_per_second();
  h.start_wall_ns = clock::wall_ns();
  std::fwrite(&h, sizeof(h), 1, file_);
  return true;
}

void Recorder::write(VenueId venue, cbrec::RecordKind kind, std::uint32_t epoch,
                     std::uint64_t t_recv, std::int64_t wall_ns, std::string_view payload,
                     std::uint16_t flags) {
  const std::lock_guard lock(mu_);
  if (file_ == nullptr) {
    return;
  }
  cbrec::RecordHeader h{};
  h.len = static_cast<std::uint32_t>(payload.size());
  h.venue = venue;
  h.kind = kind;
  h.flags = flags;
  h.epoch = epoch;
  h.t_recv = t_recv;
  h.wall_ns = wall_ns;
  std::fwrite(&h, sizeof(h), 1, file_);
  std::fwrite(payload.data(), 1, payload.size(), file_);
}

void Recorder::close() {
  const std::lock_guard lock(mu_);
  if (file_ != nullptr) {
    std::fclose(file_);
    file_ = nullptr;
  }
}

}  // namespace cb
