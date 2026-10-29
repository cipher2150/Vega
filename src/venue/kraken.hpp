#pragma once

#include "venue/protocol.hpp"

namespace cb {

// Kraken Spot WS v2 `book` channel. No sequence numbers: integrity comes from the CRC32
// checksum carried by every message, validated on the book thread (DESIGN §6.2).
class KrakenProtocol final : public VenueProtocol {
 public:
  using VenueProtocol::VenueProtocol;

  [[nodiscard]] std::vector<std::string> subscribe_messages() const override;
  [[nodiscard]] std::string ping_message() const override { return R"({"method":"ping"})"; }
  [[nodiscard]] std::uint32_t ping_interval_ms() const override { return 5'000; }
  [[nodiscard]] std::uint32_t liveness_timeout_ms() const override { return 10'000; }
  void note_ping_sent(std::int64_t wall_ns) override { ping_sent_wall_ns_ = wall_ns; }

  static constexpr std::uint32_t kFxDepth = 10;

 protected:
  FrameResult parse(simdjson::ondemand::document& doc, std::string_view raw,
                    const FrameMeta& meta) override;
  void reset_session_state() override { ping_sent_wall_ns_ = 0; }

 private:
  FrameResult parse_book_item(simdjson::ondemand::object item, bool snapshot,
                              const FrameMeta& meta);
  void on_pong(std::int64_t time_in, std::int64_t time_out, const FrameMeta& meta);

  std::int64_t ping_sent_wall_ns_ = 0;
};

}  // namespace cb
