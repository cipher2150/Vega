#pragma once

#include "venue/protocol.hpp"

#include <array>
#include <cstdint>

namespace cb {

// OKX v5 public `books` channel (400-level snapshot, then ~100 ms increments).
// Integrity: each update's prevSeqId must equal the previous message's seqId. The
// checksum field has been fixed at 0 since 2026-06-23 (deprecated) and is ignored.
class OkxProtocol final : public VenueProtocol {
 public:
  using VenueProtocol::VenueProtocol;

  [[nodiscard]] std::vector<std::string> subscribe_messages() const override;
  [[nodiscard]] std::string ping_message() const override { return "ping"; }
  [[nodiscard]] std::uint32_t ping_interval_ms() const override { return 15'000; }
  [[nodiscard]] std::uint32_t liveness_timeout_ms() const override { return 30'000; }

 protected:
  FrameResult parse(simdjson::ondemand::document& doc, std::string_view raw,
                    const FrameMeta& meta) override;
  bool handle_non_json(std::string_view raw) override { return raw == "pong"; }
  void reset_session_state() override;

 private:
  FrameResult parse_item(simdjson::ondemand::object item, const VenueInstrument& vi,
                         bool snapshot, const FrameMeta& meta);

  struct SeqState {
    std::int64_t last = 0;
    bool have = false;
  };
  std::array<SeqState, kNumInstruments> seq_{};
};

}  // namespace cb
