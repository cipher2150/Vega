#pragma once

// Base class for one venue's wire protocol: frame bytes in -> normalized events out.
//
// Owns everything that is per-connection protocol state (sequence numbers, Coinbase
// horizon bands, the simdjson parser) and runs on the venue's I/O thread. It knows
// nothing about sockets, so the same object is driven by the live WebSocket session and
// by the replay reader - which is what makes replay benchmarks measure the real code.

#include "core/histogram.hpp"
#include "venue/event.hpp"
#include "venue/event_sink.hpp"
#include "venue/types.hpp"
#include "venue/venue_shared.hpp"

#include <simdjson.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cb {

struct VenueSettings {
  bool enabled = true;
  std::string url;
  std::uint32_t depth = 0;          // subscribed depth / truncation depth (0 = full)
  std::uint32_t horizon_bp = 200;   // Coinbase: keep levels within +/- this many bp of mid
  std::uint32_t min_levels = 20;    // Coinbase: resync if fewer remain inside the horizon
};

struct FrameMeta {
  std::uint64_t t_recv = 0;      // monotonic ticks, frame complete
  std::int64_t recv_wall_ns = 0;  // wall clock at receive
};

enum class FrameStatus : std::uint8_t { kOk, kResync };

struct FrameResult {
  FrameStatus status = FrameStatus::kOk;
  DownReason reason = DownReason::kNone;
};

class VenueProtocol {
 public:
  // max_frame bounds the parser's preallocated capacity; larger frames are a parse error.
  VenueProtocol(VenueId venue, const VenueSettings& settings, EventSink& sink,
                VenueShared& shared, std::size_t max_frame);
  virtual ~VenueProtocol() = default;
  VenueProtocol(const VenueProtocol&) = delete;
  VenueProtocol& operator=(const VenueProtocol&) = delete;

  [[nodiscard]] VenueId venue() const noexcept { return venue_; }
  [[nodiscard]] std::uint32_t epoch() const noexcept { return epoch_; }
  [[nodiscard]] std::size_t max_frame() const noexcept { return max_frame_; }
  [[nodiscard]] const Histogram& parse_histogram() const noexcept { return parse_hist_; }

  // A new connection (session `epoch`) has been established; forget per-session state.
  void on_connect(std::uint32_t epoch);
  // The connection ended (or is being torn down): tell the book thread.
  void on_disconnect(DownReason reason);

  // `data` must be readable for `capacity` >= len + SIMDJSON_PADDING bytes.
  FrameResult on_frame(const char* data, std::size_t len, std::size_t capacity,
                       const FrameMeta& meta);

  // ---- session configuration (cold path) ----
  [[nodiscard]] virtual std::vector<std::string> subscribe_messages() const = 0;
  [[nodiscard]] virtual std::string ping_message() const { return {}; }
  [[nodiscard]] virtual std::uint32_t ping_interval_ms() const { return 0; }
  [[nodiscard]] virtual std::uint32_t liveness_timeout_ms() const { return 10'000; }
  virtual void note_ping_sent(std::int64_t /*wall_ns*/) {}

 protected:
  // Parse one JSON document. Throws simdjson_error on malformed input (cold path).
  virtual FrameResult parse(simdjson::ondemand::document& doc, std::string_view raw,
                            const FrameMeta& meta) = 0;
  // Non-JSON frames (OKX answers "ping" with a bare "pong"). Return true if handled.
  virtual bool handle_non_json(std::string_view /*raw*/) { return false; }
  virtual void reset_session_state() = 0;

  void emit_snapshot_begin(InstrumentId inst) noexcept;
  void emit_level(InstrumentId inst, Side side, std::int64_t price, std::int64_t qty) noexcept;
  void emit_message_end(InstrumentId inst, std::uint8_t flags, std::uint32_t checksum,
                        std::int64_t exch_ts_ns, const FrameMeta& meta) noexcept;
  // Drops staged events, emits VenueDown and stops processing until the next connect.
  FrameResult fail(DownReason reason) noexcept;

  // Text of a scalar JSON value without quotes or trailing whitespace. Points into the
  // input buffer: no copy, no unescaping (prices/sizes never contain escapes).
  static std::string_view scalar_text(simdjson::ondemand::value value);

  const VenueSettings& settings_;
  VenueShared& shared_;

 private:
  VenueId venue_;
  EventSink& sink_;
  std::size_t max_frame_;
  simdjson::ondemand::parser parser_;
  Histogram parse_hist_;
  std::uint32_t epoch_ = 0;
  bool failed_ = true;  // nothing is accepted before the first on_connect
};

}  // namespace cb
