#include "publish/snapshot_json.hpp"

#include "core/clock.hpp"
#include "core/decimal.hpp"

#include <bit>
#include <cinttypes>
#include <cstdio>
#include <memory>

namespace cb {

namespace {

const char* conn_name(ConnState s) {
  switch (s) {
    case ConnState::kDisabled: return "disabled";
    case ConnState::kDisconnected: return "disconnected";
    case ConnState::kConnecting: return "connecting";
    case ConnState::kSubscribed: return "connected";
  }
  return "?";
}

class Json {
 public:
  Json() { s_.reserve(16 * 1024); }
  Json& raw(std::string_view v) {
    s_.append(v);
    return *this;
  }
  Json& key(const char* k) {
    comma();
    s_.push_back('"');
    s_.append(k);
    s_.append("\":");
    need_comma_ = false;
    return *this;
  }
  Json& str(std::string_view v) {
    comma();
    s_.push_back('"');
    s_.append(v);  // only internal identifiers: no escaping needed
    s_.push_back('"');
    need_comma_ = true;
    return *this;
  }
  // Exact decimal from fixed-point: prices and sizes are never rounded for display.
  Json& fixed(std::int64_t v) {
    comma();
    if (v < 0) {
      s_.push_back('-');
      v = -v;
    }
    std::array<char, kMaxDecimalChars> buf{};
    s_.append(buf.data(), format_decimal(v, buf));
    need_comma_ = true;
    return *this;
  }
  Json& num(std::int64_t v) {
    comma();
    s_.append(std::to_string(v));
    need_comma_ = true;
    return *this;
  }
  Json& real(double v, int precision = 3) {
    comma();
    std::array<char, 64> buf{};
    const int n = std::snprintf(buf.data(), buf.size(), "%.*f", precision, v);
    s_.append(buf.data(), static_cast<std::size_t>(n));
    need_comma_ = true;
    return *this;
  }
  Json& boolean(bool v) {
    comma();
    s_.append(v ? "true" : "false");
    need_comma_ = true;
    return *this;
  }
  Json& open(char c) {
    comma();
    s_.push_back(c);
    need_comma_ = false;
    return *this;
  }
  Json& close(char c) {
    s_.push_back(c);
    need_comma_ = true;
    return *this;
  }
  std::string take() { return std::move(s_); }

 private:
  void comma() {
    if (need_comma_) s_.push_back(',');
  }
  std::string s_;
  bool need_comma_ = false;
};

double ticks_ms(std::uint64_t ticks) { return clock::ticks_to_ns(static_cast<double>(ticks)) / 1e6; }

}  // namespace

std::string snapshot_json(const PublishSnapshot& snap,
                          const std::array<const VenueShared*, kNumVenues>& shared,
                          const ThreadAllocs& allocs, std::uint64_t now_ticks) {
  Json j;
  j.open('{').key("t").str("snap").key("seq").num(static_cast<std::int64_t>(snap.seq));
  j.key("fx").open('{').key("usdt_usd").fixed(snap.fx_usdt_usd_e8).key("live").boolean(snap.fx_live).close('}');
  j.key("events").num(static_cast<std::int64_t>(snap.events));
  j.key("messages").num(static_cast<std::int64_t>(snap.messages));
  j.key("stale_drops").num(static_cast<std::int64_t>(snap.stale_epoch_drops));
  if (allocs.book != nullptr) {
    j.key("book_allocs").num(static_cast<std::int64_t>(allocs.book->load(std::memory_order_relaxed)));
  }

  j.key("venues").open('[');
  for (const VenueId v : kAllVenues) {
    const VenueShared& vs = *shared[idx(v)];
    const std::uint64_t last_rx = vs.last_rx_ticks.load(std::memory_order_relaxed);
    j.open('{').key("name").str(venue_name(v));
    j.key("conn").str(conn_name(vs.conn_state.load(std::memory_order_relaxed)));
    j.key("rx_age_ms").real(last_rx == 0 ? -1.0 : ticks_ms(now_ticks - last_rx), 1);
    j.key("frames").num(static_cast<std::int64_t>(vs.frames.load(std::memory_order_relaxed)));
    j.key("connects").num(static_cast<std::int64_t>(vs.connects.load(std::memory_order_relaxed)));
    j.key("io_allocs").num(static_cast<std::int64_t>(vs.io_heap_allocs.load(std::memory_order_relaxed)));
    const std::int64_t rtt = vs.ntp_rtt_ns.load(std::memory_order_relaxed);
    if (rtt >= 0) {
      j.key("ntp_offset_ms").real(static_cast<double>(vs.ntp_offset_ns.load(std::memory_order_relaxed)) / 1e6, 2);
      j.key("ntp_rtt_ms").real(static_cast<double>(rtt) / 1e6, 2);
    }
    j.key("books").open('[');
    for (std::size_t i = 0; i < kNumInstruments; ++i) {
      const BookSummary& b = snap.books[idx(v)][i];
      if (!b.exists) continue;
      j.open('{').key("inst").str(instrument_name(static_cast<InstrumentId>(i)));
      j.key("quote").str(b.quote == Quote::kUsdt ? "USDT" : "USD");
      j.key("state").str(book_state_name(b.state));
      j.key("reason").str(reason_name(b.last_reason));
      j.key("bid").fixed(b.best_bid).key("bid_qty").fixed(b.best_bid_qty);
      j.key("ask").fixed(b.best_ask).key("ask_qty").fixed(b.best_ask_qty);
      j.key("micro").fixed(b.microprice);
      j.key("imb").real(static_cast<double>(b.imbalance_e8) / 1e8, 4);
      j.key("levels").open('[').num(b.bid_levels).num(b.ask_levels).close(']');
      j.key("age_ms").real(b.last_change_ticks == 0 ? -1.0 : ticks_ms(now_ticks - b.last_change_ticks), 1);
      j.key("lag_ms").real(static_cast<double>(b.lag_last_ns) / 1e6, 1);
      j.key("lag_min_ms").real(static_cast<double>(b.lag_min_ns) / 1e6, 1);
      j.key("msgs").num(static_cast<std::int64_t>(b.messages));
      j.key("ck_ok").num(static_cast<std::int64_t>(b.checksum_ok));
      j.key("ck_fail").num(static_cast<std::int64_t>(b.checksum_fail));
      j.key("resyncs").num(static_cast<std::int64_t>(b.resyncs));
      j.close('}');
    }
    j.close(']').close('}');
  }
  j.close(']');

  j.key("cons").open('[');
  for (std::size_t c = 0; c < kNumConsolidated; ++c) {
    const ConsolidatedSummary& cs = snap.consolidated[c];
    j.open('{').key("sym").str(instrument_name(static_cast<InstrumentId>(c)));
    j.key("venues").open('[');
    for (const VenueId v : kAllVenues) {
      if ((cs.venues_mask >> idx(v)) & 1u) j.str(venue_name(v));
    }
    j.close(']');
    auto rows = [&](const char* name, const auto& arr, std::uint8_t n) {
      j.key(name).open('[');
      for (std::size_t r = 0; r < n; ++r) {
        j.open('[').fixed(arr[r].price).fixed(arr[r].qty).str(venue_name(arr[r].venue)).close(']');
      }
      j.close(']');
    };
    rows("bids", cs.bids, cs.n_bids);
    rows("asks", cs.asks, cs.n_asks);
    if (cs.n_bids > 0 && cs.n_asks > 0) {
      j.key("bid").fixed(cs.bids[0].price).key("bid_venue").str(venue_name(cs.bids[0].venue));
      j.key("ask").fixed(cs.asks[0].price).key("ask_venue").str(venue_name(cs.asks[0].venue));
      j.key("spread").fixed(cs.spread);  // negative = crossed across venues
      j.key("spread_bp").real(static_cast<double>(cs.spread_bp_e2) / 100.0, 2);
      j.key("micro").fixed(cs.microprice);
      j.key("imb").real(static_cast<double>(cs.imbalance_e8) / 1e8, 4);
    }
    j.close('}');
  }
  j.close(']');
  j.close('}');
  return j.take();
}

std::string latency_json(const LatencySources& sources) {
  const double tick_ns = clock::ticks_to_ns(1.0);
  Json j;
  j.open('{').key("t").str("lat").key("tick_ns").real(tick_ns, 2);
  j.key("stages").open('[');

  auto stage = [&](const char* name, const Histogram::Snapshot& h) {
    j.open('{').key("name").str(name).key("count").num(static_cast<std::int64_t>(h.count));
    auto ns = [&](std::uint64_t t) { return clock::ticks_to_ns(static_cast<double>(t)); };
    // mean = total ticks / count: the honest number for stages shorter than one tick.
    j.key("mean_ns").real(clock::ticks_to_ns(h.mean()), 1);
    j.key("p50_ns").real(ns(h.percentile(0.50)), 0);
    j.key("p99_ns").real(ns(h.percentile(0.99)), 0);
    j.key("p999_ns").real(ns(h.percentile(0.999)), 0);
    j.key("max_ns").real(ns(h.max), 0);
    // Coarse log2(ns) bins for drawing: bin k counts values in [2^k, 2^(k+1)) ns.
    std::array<std::uint64_t, 40> bins{};
    for (std::size_t i = 0; i < Histogram::kBuckets; ++i) {
      if (h.counts[i] == 0) continue;
      const auto v = static_cast<std::uint64_t>(ns(Histogram::upper_bound_of(i)));
      const int k = v == 0 ? 0 : std::min(39, 63 - std::countl_zero(v));
      bins[static_cast<std::size_t>(k)] += h.counts[i];
    }
    j.key("bins").open('[');
    for (const auto c : bins) j.num(static_cast<std::int64_t>(c));
    j.close(']').close('}');
  };

  auto parse = std::make_unique<Histogram::Snapshot>();
  auto tmp = std::make_unique<Histogram::Snapshot>();
  for (const Histogram* h : sources.parse) {
    if (h == nullptr) continue;
    h->snapshot_into(*tmp);
    parse->merge(*tmp);
  }
  stage("parse", *parse);
  if (sources.stages != nullptr) {
    const StageHistograms& s = *sources.stages;
    const std::array<std::pair<const char*, const Histogram*>, 5> list = {{
        {"queue", &s.queue}, {"book", &s.book}, {"signal", &s.signal},
        {"publish", &s.publish}, {"e2e", &s.e2e}}};
    for (const auto& [name, h] : list) {
      h->snapshot_into(*tmp);
      stage(name, *tmp);
    }
  }
  j.close(']').close('}');
  return j.take();
}

}  // namespace cb
