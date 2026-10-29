#include "venue/protocol.hpp"

#include "core/clock.hpp"
#include "core/log.hpp"
#include "core/rfc3339.hpp"

#include <cstdlib>

namespace cb {

VenueProtocol::VenueProtocol(VenueId venue, const VenueSettings& settings, EventSink& sink,
                             VenueShared& shared, std::size_t max_frame)
    : settings_(settings),
      shared_(shared),
      venue_(venue),
      sink_(sink),
      max_frame_(max_frame),
      parser_(max_frame) {
  // Allocate the parser's internal buffers once, up front, so iterate() never grows them
  // on the hot path (part of the zero-allocation-after-warm-up guarantee).
  if (parser_.allocate(max_frame) != simdjson::SUCCESS) {
    CB_ERROR("%s: simdjson parser allocation of %zu bytes failed", venue_name(venue).data(),
             max_frame);
    std::abort();
  }
}

void VenueProtocol::on_connect(std::uint32_t epoch) {
  epoch_ = epoch;
  failed_ = false;
  sink_.discard();
  reset_session_state();
}

void VenueProtocol::on_disconnect(DownReason reason) {
  if (!failed_) {
    (void)fail(reason);
  }
}

FrameResult VenueProtocol::on_frame(const char* data, std::size_t len, std::size_t capacity,
                                    const FrameMeta& meta) {
  if (failed_) {
    return {};  // session already declared bad; ignore until the reconnect
  }
  const std::string_view raw(data, len);
  if (handle_non_json(raw)) {
    return {};
  }
  if (len > max_frame_) {
    CB_WARN("%s: frame of %zu bytes exceeds max_frame %zu", venue_name(venue_).data(), len,
            max_frame_);
    return fail(DownReason::kParseError);
  }

  FrameResult result;
  try {
    simdjson::ondemand::document doc =
        parser_.iterate(simdjson::padded_string_view(data, len, capacity));
    result = parse(doc, raw, meta);
  } catch (const simdjson::simdjson_error& e) {
    // simdjson's exceptions cost nothing on the success path; this branch is cold.
    CB_WARN("%s: JSON error '%s' in frame: %.200s", venue_name(venue_).data(), e.what(),
            std::string(raw.substr(0, 200)).c_str());
    return fail(DownReason::kParseError);
  }
  if (result.status == FrameStatus::kResync) {
    return result;  // parse() already called fail()
  }

  const std::uint64_t t_parsed = clock::ticks();
  sink_.stamp_parsed(t_parsed);
  parse_hist_.record(t_parsed - meta.t_recv);
  if (!sink_.commit()) {
    return fail(DownReason::kQueueOverflow);
  }
  return {};
}

void VenueProtocol::emit_snapshot_begin(InstrumentId inst) noexcept {
  Event e{};
  e.kind = EventKind::kSnapshotBegin;
  e.venue = venue_;
  e.instrument = inst;
  e.epoch = epoch_;
  sink_.push(e);
}

void VenueProtocol::emit_level(InstrumentId inst, Side side, std::int64_t price,
                               std::int64_t qty) noexcept {
  Event e{};
  e.kind = EventKind::kLevel;
  e.venue = venue_;
  e.instrument = inst;
  e.side = side;
  e.epoch = epoch_;
  e.price = price;
  e.qty = qty;
  sink_.push(e);
}

void VenueProtocol::emit_message_end(InstrumentId inst, std::uint8_t flags,
                                     std::uint32_t checksum, std::int64_t exch_ts_ns,
                                     const FrameMeta& meta) noexcept {
  Event e{};
  e.kind = EventKind::kMessageEnd;
  e.venue = venue_;
  e.instrument = inst;
  e.epoch = epoch_;
  e.flags = flags;
  e.checksum = checksum;
  e.exch_ts_ns = exch_ts_ns;
  e.recv_wall_ns = meta.recv_wall_ns;
  e.t_recv = meta.t_recv;
  sink_.push(e);
}

FrameResult VenueProtocol::fail(DownReason reason) noexcept {
  sink_.discard();
  Event e{};
  e.kind = EventKind::kVenueDown;
  e.venue = venue_;
  e.epoch = epoch_;
  e.reason = reason;
  sink_.push(e);
  (void)sink_.commit();  // best effort: the book thread also sees conn_state
  failed_ = true;
  shared_.last_io_down_reason.store(static_cast<std::uint8_t>(reason),
                                    std::memory_order_relaxed);
  if (reason != DownReason::kDisconnected && reason != DownReason::kShutdown) {
    CB_WARN("%s: epoch %u down: %s", venue_name(venue_).data(), epoch_,
            reason_name(reason).data());
  }
  return {FrameStatus::kResync, reason};
}

std::string_view VenueProtocol::scalar_text(simdjson::ondemand::value value) {
  std::string_view t = value.raw_json_token();
  while (!t.empty() && (t.back() == ' ' || t.back() == '\n' || t.back() == '\r' ||
                        t.back() == '\t')) {
    t.remove_suffix(1);
  }
  if (t.size() >= 2 && t.front() == '"' && t.back() == '"') {
    t.remove_prefix(1);
    t.remove_suffix(1);
  }
  return t;
}

}  // namespace cb
