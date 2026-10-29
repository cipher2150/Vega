#pragma once

#include "venue/protocol.hpp"

#include <array>
#include <cstdint>

namespace cb {

// Coinbase Advanced Trade `level2` + `heartbeats` on one connection.
//  - sequence_num is per CONNECTION and shared by every channel: must be prev+1.
//  - No checksum: crossed-book and horizon checks on the book thread are the backstop.
//  - The snapshot is the whole book (~20k levels/side live), so we keep only a price
//    band around mid (the "horizon", DESIGN §5.2) and drop updates outside it.
class CoinbaseProtocol final : public VenueProtocol {
 public:
  using VenueProtocol::VenueProtocol;

  [[nodiscard]] std::vector<std::string> subscribe_messages() const override;
  [[nodiscard]] std::uint32_t liveness_timeout_ms() const override { return 5'000; }

  [[nodiscard]] std::uint64_t dropped_outside_horizon() const noexcept { return dropped_; }

 protected:
  FrameResult parse(simdjson::ondemand::document& doc, std::string_view raw,
                    const FrameMeta& meta) override;
  void reset_session_state() override;

 private:
  struct Band {
    std::int64_t bid_floor = 0;     // bids below this are ignored
    std::int64_t ask_ceiling = 0;   // asks above this are ignored
    bool valid = false;
  };

  void compute_bands(simdjson::ondemand::document& doc);
  FrameResult parse_event(simdjson::ondemand::object event, std::int64_t exch_ts,
                          const FrameMeta& meta);

  std::array<Band, kNumInstruments> bands_{};
  std::int64_t last_seq_ = 0;
  bool have_seq_ = false;
  std::uint64_t dropped_ = 0;
};

}  // namespace cb
