#include "venue/bybit.hpp"

#include "core/decimal.hpp"
#include "core/log.hpp"
#include "core/rfc3339.hpp"

#include <string>

namespace cb {

namespace ondemand = simdjson::ondemand;

std::vector<std::string> BybitProtocol::subscribe_messages() const {
  const std::string depth = std::to_string(settings_.depth == 0 ? 50 : settings_.depth);
  return {R"({"op":"subscribe","args":["orderbook.)" + depth + R"(.BTCUSDT","orderbook.)" +
          depth + R"(.ETHUSDT"]})"};
}

// {"topic":"orderbook.50.BTCUSDT","ts":..,"type":"snapshot|delta",
//  "data":{"s":..,"b":[[px,sz]],"a":[[px,sz]],"u":..,"seq":..},"cts":..}
// or control replies {"success":bool,"ret_msg":..,"op":"subscribe|ping",...}.
FrameResult BybitProtocol::parse(ondemand::document& doc, std::string_view raw,
                                 const FrameMeta& meta) {
  const VenueInstrument* vi = nullptr;
  std::string_view type;
  bool have_data = false;
  bool snapshot = false;
  std::int64_t u = 0;
  bool have_u = false;
  std::int64_t ts_ms = 0;
  std::int64_t cts_ms = 0;

  for (auto field : doc.get_object()) {
    const std::string_view key = field.escaped_key();
    if (key == "topic") {
      const std::string_view topic = field.value().get_string();
      const auto dot = topic.rfind('.');
      if (dot != std::string_view::npos) {
        vi = find_instrument(VenueId::kBybit, topic.substr(dot + 1));
      }
    } else if (key == "type") {
      type = field.value().get_string();
    } else if (key == "ts") {
      ts_ms = field.value().get_int64();
    } else if (key == "cts") {
      cts_ms = field.value().get_int64();
    } else if (key == "success") {
      if (!bool(field.value().get_bool())) {
        CB_WARN("bybit: request failed: %.300s", std::string(raw.substr(0, 300)).c_str());
        return fail(DownReason::kVenueError);
      }
    } else if (key == "data") {
      if (vi == nullptr || type.empty()) {
        return fail(DownReason::kParseError);
      }
      snapshot = type == "snapshot";
      if (snapshot) {
        emit_snapshot_begin(vi->id);
      }
      have_data = true;
      for (auto df : field.value().get_object()) {
        const std::string_view dk = df.escaped_key();
        if (dk == "b" || dk == "a") {
          const Side side = dk == "b" ? Side::kBid : Side::kAsk;
          for (auto level : df.value().get_array()) {
            DecimalResult px;
            DecimalResult qty;
            int i = 0;
            for (auto element : level.get_array()) {
              if (i == 0) {
                px = parse_decimal(scalar_text(element.value()));
              } else if (i == 1) {
                qty = parse_decimal(scalar_text(element.value()));
              }
              ++i;
            }
            if (!px.ok() || !qty.ok()) {
              return fail(DownReason::kParseError);
            }
            emit_level(vi->id, side, px.value, qty.value);
          }
        } else if (dk == "u") {
          u = df.value().get_int64();
          have_u = true;
        }
      }
    }
  }
  if (!have_data) {
    return {};  // control message (subscribe ack, pong)
  }
  if (!have_u) {
    return fail(DownReason::kParseError);
  }

  // `u` follows the levels inside "data"; the levels are only staged, so a gap discards
  // them before the book thread sees them.
  UState& s = last_u_[idx(vi->id)];
  if (!snapshot && (!s.have || u != s.last + 1)) {
    CB_WARN("bybit: %s u=%lld after %lld", vi->symbol.data(), static_cast<long long>(u),
            static_cast<long long>(s.last));
    return fail(DownReason::kSequenceGap);
  }
  s.last = u;
  s.have = true;
  // cts = matching-engine time (preferred); ts = when Bybit's system produced the push.
  const std::int64_t exch_ms = cts_ms != 0 ? cts_ms : ts_ms;
  emit_message_end(vi->id, snapshot ? kFlagSnapshot : 0, 0,
                   exch_ms != 0 ? exch_ms * 1'000'000 : kInvalidTime, meta);
  return {};
}

}  // namespace cb
