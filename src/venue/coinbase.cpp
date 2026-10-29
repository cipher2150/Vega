#include "venue/coinbase.hpp"

#include "core/decimal.hpp"
#include "core/int128.hpp"
#include "core/log.hpp"
#include "core/rfc3339.hpp"

#include <algorithm>
#include <limits>
#include <string>

namespace cb {

namespace ondemand = simdjson::ondemand;

std::vector<std::string> CoinbaseProtocol::subscribe_messages() const {
  // One channel per subscribe message (Coinbase rule). Must arrive within 5 s of connect.
  return {
      R"({"type":"subscribe","channel":"heartbeats"})",
      R"({"type":"subscribe","channel":"level2","product_ids":["BTC-USD","ETH-USD"]})",
  };
}

void CoinbaseProtocol::reset_session_state() {
  bands_ = {};
  have_seq_ = false;
  last_seq_ = 0;
}

FrameResult CoinbaseProtocol::parse(ondemand::document& doc, std::string_view raw,
                                    const FrameMeta& meta) {
  // A snapshot needs its band before any level is emitted, but the band depends on the
  // snapshot's own best prices. So snapshot messages are walked twice: once for best
  // bid/ask, then (after rewind) for real. Snapshots arrive as their own message with
  // the event type in the first few hundred bytes; updates never pay for this.
  if (raw.substr(0, 512).find(R"("type":"snapshot")") != std::string_view::npos) {
    compute_bands(doc);
    doc.rewind();
  }

  std::string_view channel;
  std::int64_t exch_ts = kInvalidTime;
  for (auto field : doc.get_object()) {
    const std::string_view key = field.escaped_key();
    if (key == "channel") {
      channel = field.value().get_string();
    } else if (key == "timestamp") {
      exch_ts = parse_rfc3339_ns(field.value().get_string());
    } else if (key == "sequence_num") {
      const std::int64_t seq = field.value().get_int64();
      // Any gap - or a lower number, which would mean applying an older absolute size
      // over a newer one - invalidates the whole connection.
      if (have_seq_ && seq != last_seq_ + 1) {
        CB_WARN("coinbase: sequence %lld after %lld", static_cast<long long>(seq),
                static_cast<long long>(last_seq_));
        return fail(DownReason::kSequenceGap);
      }
      have_seq_ = true;
      last_seq_ = seq;
    } else if (key == "events") {
      if (channel != "l2_data") {
        continue;  // heartbeats/subscriptions: only their sequence number matters
      }
      for (auto event : field.value().get_array()) {
        const FrameResult r = parse_event(event.get_object(), exch_ts, meta);
        if (r.status == FrameStatus::kResync) {
          return r;
        }
      }
    } else if (key == "type") {
      const std::string_view type = field.value().get_string();
      if (type == "error") {
        CB_WARN("coinbase: error message: %.300s", std::string(raw.substr(0, 300)).c_str());
        return fail(DownReason::kVenueError);
      }
    }
  }
  return {};
}

FrameResult CoinbaseProtocol::parse_event(ondemand::object event, std::int64_t exch_ts,
                                          const FrameMeta& meta) {
  bool snapshot = false;
  const VenueInstrument* vi = nullptr;
  for (auto field : event) {
    const std::string_view key = field.escaped_key();
    if (key == "type") {
      snapshot = std::string_view(field.value().get_string()) == "snapshot";
    } else if (key == "product_id") {
      vi = find_instrument(VenueId::kCoinbase, std::string_view(field.value().get_string()));
    } else if (key == "updates") {
      if (vi == nullptr) {
        return fail(DownReason::kParseError);
      }
      const Band& band = bands_[idx(vi->id)];
      if (snapshot) {
        if (!band.valid) {
          CB_WARN("coinbase: snapshot without precomputed band");
          return fail(DownReason::kParseError);
        }
        emit_snapshot_begin(vi->id);
      }
      for (auto update : field.value().get_array()) {
        Side side = Side::kBid;
        DecimalResult px;
        DecimalResult qty;
        for (auto uf : update.get_object()) {
          const std::string_view uk = uf.escaped_key();
          if (uk == "side") {
            side = std::string_view(uf.value().get_string()) == "bid" ? Side::kBid : Side::kAsk;
          } else if (uk == "price_level") {
            px = parse_decimal(scalar_text(uf.value()));
          } else if (uk == "new_quantity") {
            qty = parse_decimal(scalar_text(uf.value()));
          }
        }
        if (!px.ok() || !qty.ok()) {
          return fail(DownReason::kParseError);
        }
        // Outside the horizon: no price inside the band can be affected by this update
        // (sizes are absolute per price), so dropping it keeps the in-band book exact.
        if (band.valid && ((side == Side::kBid && px.value < band.bid_floor) ||
                           (side == Side::kAsk && px.value > band.ask_ceiling))) {
          ++dropped_;
          continue;
        }
        emit_level(vi->id, side, px.value, qty.value);
      }
      emit_message_end(vi->id, snapshot ? kFlagSnapshot : 0, 0, exch_ts, meta);
    }
  }
  return {};
}

void CoinbaseProtocol::compute_bands(ondemand::document& doc) {
  for (auto field : doc.get_object()) {
    if (std::string_view(field.escaped_key()) != "events") {
      continue;
    }
    for (auto event_value : field.value().get_array()) {
      bool snapshot = false;
      const VenueInstrument* vi = nullptr;
      std::int64_t best_bid = std::numeric_limits<std::int64_t>::min();
      std::int64_t best_ask = std::numeric_limits<std::int64_t>::max();
      for (auto ef : event_value.get_object()) {
        const std::string_view key = ef.escaped_key();
        if (key == "type") {
          snapshot = std::string_view(ef.value().get_string()) == "snapshot";
        } else if (key == "product_id") {
          vi = find_instrument(VenueId::kCoinbase, std::string_view(ef.value().get_string()));
        } else if (key == "updates" && snapshot && vi != nullptr) {
          for (auto update : ef.value().get_array()) {
            bool bid = true;
            std::int64_t px = 0;
            for (auto uf : update.get_object()) {
              const std::string_view uk = uf.escaped_key();
              if (uk == "side") {
                bid = std::string_view(uf.value().get_string()) == "bid";
              } else if (uk == "price_level") {
                px = parse_decimal(scalar_text(uf.value())).value;
              }
            }
            if (bid) {
              best_bid = std::max(best_bid, px);
            } else {
              best_ask = std::min(best_ask, px);
            }
          }
        }
      }
      if (!snapshot || vi == nullptr) {
        continue;
      }
      Band& band = bands_[idx(vi->id)];
      band.valid = true;
      const bool both_sides = best_bid != std::numeric_limits<std::int64_t>::min() &&
                              best_ask != std::numeric_limits<std::int64_t>::max();
      if (settings_.horizon_bp == 0 || !both_sides) {
        band.bid_floor = std::numeric_limits<std::int64_t>::min();
        band.ask_ceiling = std::numeric_limits<std::int64_t>::max();
        continue;
      }
      const std::int64_t mid = best_bid / 2 + best_ask / 2;
      const auto delta = static_cast<std::int64_t>(static_cast<i128>(mid) *
                                                   settings_.horizon_bp / 10'000);
      band.bid_floor = mid - delta;
      band.ask_ceiling = mid + delta;
    }
  }
}

}  // namespace cb
