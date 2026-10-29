#include "venue/kraken.hpp"

#include "core/decimal.hpp"
#include "core/log.hpp"
#include "core/rfc3339.hpp"

#include <string>

namespace cb {

namespace ondemand = simdjson::ondemand;

std::vector<std::string> KrakenProtocol::subscribe_messages() const {
  const std::string depth = std::to_string(settings_.depth == 0 ? 25 : settings_.depth);
  return {
      R"({"method":"subscribe","params":{"channel":"book","symbol":["BTC/USD","ETH/USD"],"depth":)" +
          depth + R"(,"snapshot":true}})",
      R"({"method":"subscribe","params":{"channel":"book","symbol":["USDT/USD"],"depth":)" +
          std::to_string(kFxDepth) + R"(,"snapshot":true}})",
  };
}

// Messages: {"channel":"book","type":"snapshot|update","data":[{...}]},
// {"channel":"heartbeat"|"status",...}, {"method":"subscribe|pong",...}.
// Fields are dispatched by key, so the parser does not depend on key order except where
// noted ("type" must precede "data", which is how Kraken sends it).
FrameResult KrakenProtocol::parse(ondemand::document& doc, std::string_view /*raw*/,
                                  const FrameMeta& meta) {
  std::string_view channel;
  std::string_view type;
  std::string_view method;
  bool success = true;
  std::int64_t time_in = kInvalidTime;
  std::int64_t time_out = kInvalidTime;

  for (auto field : doc.get_object()) {
    const std::string_view key = field.escaped_key();
    if (key == "channel") {
      channel = field.value().get_string();
    } else if (key == "type") {
      type = field.value().get_string();
    } else if (key == "data") {
      if (channel != "book") {
        continue;  // status/heartbeat payloads are not needed
      }
      const bool snapshot = type == "snapshot";
      if (!snapshot && type != "update") {
        return fail(DownReason::kParseError);
      }
      for (auto item : field.value().get_array()) {
        const FrameResult r = parse_book_item(item.get_object(), snapshot, meta);
        if (r.status == FrameStatus::kResync) {
          return r;
        }
      }
    } else if (key == "method") {
      method = field.value().get_string();
    } else if (key == "success") {
      success = field.value().get_bool();
    } else if (key == "error") {
      CB_WARN("kraken: error from venue: %s",
              std::string(std::string_view(field.value().get_string())).c_str());
    } else if (key == "time_in") {
      time_in = parse_rfc3339_ns(field.value().get_string());
    } else if (key == "time_out") {
      time_out = parse_rfc3339_ns(field.value().get_string());
    }
  }

  if (method == "pong") {
    on_pong(time_in, time_out, meta);
  } else if (method == "subscribe" && !success) {
    return fail(DownReason::kVenueError);
  }
  return {};
}

FrameResult KrakenProtocol::parse_book_item(ondemand::object item, bool snapshot,
                                            const FrameMeta& meta) {
  const VenueInstrument* vi = nullptr;
  bool began = false;
  std::uint32_t checksum = 0;
  bool has_checksum = false;
  std::int64_t exch_ts = kInvalidTime;

  for (auto field : item) {
    const std::string_view key = field.escaped_key();
    if (key == "symbol") {
      vi = find_instrument(VenueId::kKraken, std::string_view(field.value().get_string()));
      if (vi == nullptr) {
        return {};  // not one of ours (cannot happen with our subscriptions)
      }
    } else if (key == "bids" || key == "asks") {
      if (vi == nullptr) {
        return fail(DownReason::kParseError);  // levels before symbol: unexpected layout
      }
      if (snapshot && !began) {
        emit_snapshot_begin(vi->id);
        began = true;
      }
      const Side side = key == "bids" ? Side::kBid : Side::kAsk;
      for (auto level : field.value().get_array()) {
        DecimalResult px;
        DecimalResult qty;
        for (auto lf : level.get_object()) {
          const std::string_view lk = lf.escaped_key();
          // Kraken sends JSON numbers ("price":80006.3); reading the raw token keeps
          // them exact - get_double() would round-trip through binary floating point.
          if (lk == "price") {
            px = parse_decimal(scalar_text(lf.value()));
          } else if (lk == "qty") {
            qty = parse_decimal(scalar_text(lf.value()));
          }
        }
        if (!px.ok() || !qty.ok()) {
          return fail(DownReason::kParseError);
        }
        emit_level(vi->id, side, px.value, qty.value);
      }
    } else if (key == "checksum") {
      checksum = static_cast<std::uint32_t>(std::uint64_t(field.value().get_uint64()));
      has_checksum = true;
    } else if (key == "timestamp") {
      exch_ts = parse_rfc3339_ns(field.value().get_string());
    }
  }
  if (vi == nullptr) {
    return fail(DownReason::kParseError);
  }
  if (snapshot && !began) {
    emit_snapshot_begin(vi->id);  // empty snapshot is still a snapshot
  }
  std::uint8_t flags = snapshot ? kFlagSnapshot : 0;
  if (has_checksum) {
    flags |= kFlagHasChecksum;
  }
  emit_message_end(vi->id, flags, checksum, exch_ts, meta);
  return {};
}

// NTP-style offset from the app-level ping: t0 = our send, t1/t2 = server time_in/out,
// t3 = our receive. offset = ((t1 - t0) + (t2 - t3)) / 2, error bound = +/- rtt / 2.
void KrakenProtocol::on_pong(std::int64_t time_in, std::int64_t time_out,
                             const FrameMeta& meta) {
  if (ping_sent_wall_ns_ == 0 || time_in == kInvalidTime || time_out == kInvalidTime) {
    return;
  }
  const std::int64_t t0 = ping_sent_wall_ns_;
  const std::int64_t t3 = meta.recv_wall_ns;
  const std::int64_t offset = ((time_in - t0) + (time_out - t3)) / 2;
  const std::int64_t rtt = (t3 - t0) - (time_out - time_in);
  shared_.ntp_offset_ns.store(offset, std::memory_order_relaxed);
  shared_.ntp_rtt_ns.store(rtt, std::memory_order_relaxed);
  ping_sent_wall_ns_ = 0;
}

}  // namespace cb
