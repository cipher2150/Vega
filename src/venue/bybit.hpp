#pragma once

#include "venue/protocol.hpp"

#include <array>
#include <cstdint>

namespace cb {

// Bybit v5 spot `orderbook.{depth}.{symbol}`. No checksum. Gap rule: delta `u` must be
// last_u + 1. The docs do not promise contiguity, but 2,036/2,036 live deltas were
// contiguous (DESIGN §6.4), so a jump is treated conservatively as a gap.
class BybitProtocol final : public VenueProtocol {
 public:
  using VenueProtocol::VenueProtocol;

  [[nodiscard]] std::vector<std::string> subscribe_messages() const override;
  [[nodiscard]] std::string ping_message() const override { return R"({"op":"ping"})"; }
  [[nodiscard]] std::uint32_t ping_interval_ms() const override { return 15'000; }
  [[nodiscard]] std::uint32_t liveness_timeout_ms() const override { return 30'000; }

 protected:
  FrameResult parse(simdjson::ondemand::document& doc, std::string_view raw,
                    const FrameMeta& meta) override;
  void reset_session_state() override { last_u_ = {}; }

 private:
  struct UState {
    std::int64_t last = 0;
    bool have = false;
  };
  std::array<UState, kNumInstruments> last_u_{};
};

}  // namespace cb
