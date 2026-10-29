#include "venue/okx.hpp"

#include "core/decimal.hpp"
#include "core/log.hpp"
#include "core/rfc3339.hpp"

#include <charconv>
#include <string>

namespace cb {

namespace ondemand = simdjson::ondemand;

std::vector<std::string> OkxProtocol::subscribe_messages() const {
  return {
      R"({"op":"subscribe","args":[{"channel":"books","instId":"BTC-USDT"},{"channel":"books","instId":"ETH-USDT"}]})",
  };
}

void OkxProtocol::reset_session_state() { seq_ = {}; }

// {"arg":{"channel":"books","instId":"BTC-USDT"},"action":"snapshot|update","data":[{...}]}
// or {"event":"subscribe|error",...}. "arg" and "action" precede "data" on the wire.
FrameResult OkxProtocol::parse(ondemand::document& doc, std::string_view raw,
                               const FrameMeta& meta) {
  const VenueInstrument* vi = nullptr;
  std::string_view action;
  for (auto field : doc.get_object()) {
    const std::string_view key = field.escaped_key();
    if (key == "event") {
      if (std::string_view(field.value().get_string()) == "error") {
        CB_WARN("okx: error event: %.300s", std::string(raw.substr(0, 300)).c_str());
        return fail(DownReason::kVenueError);
      }
    } else if (key == "arg") {
      for (auto af : field.value().get_object()) {
        if (std::string_view(af.escaped_key()) == "instId") {
          vi = find_instrument(VenueId::kOkx, std::string_view(af.value().get_string()));
        }
      }
    } else if (key == "action") {
      action = field.value().get_string();
    } else if (key == "data") {
      if (vi == nullptr || action.empty()) {
        return fail(DownReason::kParseError);
      }
      const bool snapshot = action == "snapshot";
      for (auto item : field.value().get_array()) {
        const FrameResult r = parse_item(item.get_object(), *vi, snapshot, meta);
        if (r.status == FrameStatus::kResync) {
          return r;
        }
      }
    }
  }
  return {};
}

FrameResult OkxProtocol::parse_item(ondemand::object item, const VenueInstrument& vi,
                                    bool snapshot, const FrameMeta& meta) {
  if (snapshot) {
    emit_snapshot_begin(vi.id);
  }
  std::int64_t seq_id = 0;
  std::int64_t prev_seq_id = 0;
  bool have_seq = false;
  bool have_prev = false;
  std::int64_t exch_ts = kInvalidTime;

  for (auto field : item) {
    const std::string_view key = field.escaped_key();
    if (key == "asks" || key == "bids") {
      const Side side = key == "bids" ? Side::kBid : Side::kAsk;
      for (auto level : field.value().get_array()) {
        // [price, size, "0" (deprecated), number_of_orders]
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
        emit_level(vi.id, side, px.value, qty.value);
      }
    } else if (key == "ts") {
      const std::string_view ts = field.value().get_string();
      std::int64_t ms = 0;
      if (std::from_chars(ts.data(), ts.data() + ts.size(), ms).ec == std::errc{}) {
        exch_ts = ms * 1'000'000;
      }
    } else if (key == "seqId") {
      seq_id = field.value().get_int64();
      have_seq = true;
    } else if (key == "prevSeqId") {
      prev_seq_id = field.value().get_int64();
      have_prev = true;
    }
  }
  if (!have_seq || !have_prev) {
    return fail(DownReason::kParseError);
  }

  // The sequence fields arrive after the levels; the levels are still only staged, so a
  // broken chain discards them (fail() clears the staging area) before the book sees them.
  SeqState& s = seq_[idx(vi.id)];
  if (snapshot) {
    if (prev_seq_id != -1) {
      CB_WARN("okx: snapshot prevSeqId=%lld (expected -1)", static_cast<long long>(prev_seq_id));
    }
  } else if (!s.have || prev_seq_id != s.last) {
    // seqId == prevSeqId (no-change keep-alive) and seqId decreasing after maintenance
    // both still satisfy prevSeqId == last, so neither is reported as a gap.
    CB_WARN("okx: %s prevSeqId %lld != last seqId %lld", vi.symbol.data(),
            static_cast<long long>(prev_seq_id), static_cast<long long>(s.last));
    return fail(DownReason::kSequenceGap);
  }
  s.last = seq_id;
  s.have = true;
  emit_message_end(vi.id, snapshot ? kFlagSnapshot : 0, 0, exch_ts, meta);
  return {};
}

}  // namespace cb
