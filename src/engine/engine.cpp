#include "engine/engine.hpp"

#include "core/clock.hpp"
#include "core/decimal.hpp"
#include "core/log.hpp"
#include "core/rfc3339.hpp"
#include "engine/signals.hpp"
#include "venue/factory.hpp"
#include "venue/kraken_checksum.hpp"

#include <algorithm>

namespace cb {

namespace {

constexpr bool accepting(BookState s) noexcept {
  return s == BookState::kSyncing || s == BookState::kLive;
}

std::string_view fmt(std::int64_t v, std::array<char, kMaxDecimalChars>& buf) {
  return {buf.data(), format_decimal(v, buf)};
}

}  // namespace

void Engine::LagTracker::add(std::int64_t lag, std::int64_t wall) noexcept {
  if (wall - window_start > kWindowNs) {
    prev_min = cur_min;
    cur_min = INT64_MAX;
    window_start = wall;
  }
  cur_min = std::min(cur_min, lag);
  last = lag;
}

Engine::Engine(const AppConfig& config, std::array<VenueShared*, kNumVenues> shared,
               TripleBuffer<PublishSnapshot>* out)
    : config_(config),
      shared_(shared),
      out_(out),
      slots_(std::make_unique<std::array<std::array<Slot, kNumInstruments>, kNumVenues>>()) {
  for (const VenueId v : kAllVenues) {
    const VenueSettings& vs = config_.venue(v);
    venues_[idx(v)].horizon = v == VenueId::kCoinbase && vs.horizon_bp > 0;
    venues_[idx(v)].min_levels = vs.min_levels;
    for (const VenueInstrument& vi : instruments_of(v)) {
      Slot& s = slot(v, vi.id);
      s.exists = vs.enabled;
      s.quote = vi.quote;
      s.depth = book_depth(v, vi.id, vs);
      s.price_precision = vi.price_precision;
      s.qty_precision = vi.qty_precision;
    }
  }
}

void Engine::on_event(const Event& e) noexcept {
  VenueRuntime& vr = venues_[idx(e.venue)];
  ++snap_.events;
  if (e.epoch < vr.min_epoch) {
    ++snap_.stale_epoch_drops;  // from a session we already declared dead
    return;
  }
  if (!vr.in_message) {
    vr.in_message = true;
    vr.msg_start = clock::ticks();
  }

  switch (e.kind) {
    case EventKind::kVenueDown:
      vr.in_message = false;
      for (const VenueInstrument& vi : instruments_of(e.venue)) {
        Slot& s = slot(e.venue, vi.id);
        if (s.state != BookState::kEmpty) {
          s.state = BookState::kStale;
          s.last_reason = e.reason;
        }
      }
      vr.min_epoch = std::max(vr.min_epoch, e.epoch + 1);
      recompute(InstrumentId::kUsdtUsd);  // recomputes everything
      publish();
      return;

    case EventKind::kSnapshotBegin: {
      Slot& s = slot(e.venue, e.instrument);
      s.book.clear();
      s.full = false;
      s.state = BookState::kSyncing;
      return;
    }

    case EventKind::kLevel: {
      Slot& s = slot(e.venue, e.instrument);
      if (accepting(s.state) &&
          s.book.apply(e.side, Price{e.price}, Qty{e.qty}) == LevelOp::kFull) {
        s.full = true;
      }
      return;
    }

    case EventKind::kMessageEnd:
      vr.in_message = false;
      on_message_end(e, slot(e.venue, e.instrument));
      return;
  }
}

void Engine::on_message_end(const Event& e, Slot& s) noexcept {
  if (!accepting(s.state)) {
    return;  // updates for a book that has not (re)synced yet are meaningless
  }
  const VenueRuntime& vr = venues_[idx(e.venue)];

  if (s.full) {
    fail_venue(e.venue, e.epoch, DownReason::kBookFull);
    return;
  }
  if (s.depth != 0) {
    s.book.truncate(s.depth);
  }
  if ((e.flags & kFlagHasChecksum) != 0) {
    const auto ck = kraken_checksum(s.book, s.price_precision, s.qty_precision);
    if (!ck || *ck != e.checksum) {
      ++s.checksum_fail;
      CB_WARN("%s %s: checksum mismatch (ours %u, venue %u)", venue_name(e.venue).data(),
              instrument_name(e.instrument).data(), ck.value_or(0), e.checksum);
      fail_venue(e.venue, e.epoch, DownReason::kChecksum);
      return;
    }
    ++s.checksum_ok;
  }
  if (s.book.crossed()) {
    CB_WARN("%s %s: crossed book", venue_name(e.venue).data(),
            instrument_name(e.instrument).data());
    fail_venue(e.venue, e.epoch, DownReason::kCrossed);
    return;
  }
  if (vr.horizon && (s.book.bids().size() < vr.min_levels ||
                     s.book.asks().size() < vr.min_levels)) {
    CB_INFO("%s %s: price horizon exhausted (%zu bids, %zu asks); resyncing",
            venue_name(e.venue).data(), instrument_name(e.instrument).data(),
            s.book.bids().size(), s.book.asks().size());
    fail_venue(e.venue, e.epoch, DownReason::kHorizon);
    return;
  }
  if (s.state == BookState::kSyncing && (e.flags & kFlagSnapshot) != 0) {
    s.state = BookState::kLive;
    s.last_reason = DownReason::kNone;
  }

  ++s.messages;
  ++snap_.messages;
  const std::uint64_t t_book = clock::ticks();
  s.last_change_ticks = t_book;
  if (e.exch_ts_ns != kInvalidTime) {
    s.lag.add(e.recv_wall_ns - e.exch_ts_ns, e.recv_wall_ns);
  }

  recompute(e.instrument);
  const std::uint64_t t_signal = clock::ticks();
  publish();
  const std::uint64_t t_pub = clock::ticks();

  // Cross-thread stage: both stamps come from the same system counter, so the difference
  // is meaningful; guard anyway against a counter read reordered across cores.
  if (vr.msg_start >= e.t_parsed) {
    stages_.queue.record(vr.msg_start - e.t_parsed);
  }
  stages_.book.record(t_book - vr.msg_start);
  stages_.signal.record(t_signal - t_book);
  stages_.publish.record(t_pub - t_signal);
  if (t_pub >= e.t_recv) {
    stages_.e2e.record(t_pub - e.t_recv);
  }
}

void Engine::fail_venue(VenueId v, std::uint32_t epoch, DownReason reason) noexcept {
  VenueRuntime& vr = venues_[idx(v)];
  for (const VenueInstrument& vi : instruments_of(v)) {
    Slot& s = slot(v, vi.id);
    if (s.state != BookState::kEmpty) {
      s.state = BookState::kStale;
      s.last_reason = reason;
      ++s.resyncs;
    }
  }
  vr.min_epoch = std::max(vr.min_epoch, epoch + 1);
  if (shared_[idx(v)] != nullptr) {
    // release: pairs with the I/O thread's acquire; nothing else is published by it, but
    // it keeps the request ordered after the state changes above for any reader.
    shared_[idx(v)]->resync_request.store(epoch, std::memory_order_release);
  }
  recompute(InstrumentId::kUsdtUsd);
  publish();
}

void Engine::recompute(InstrumentId changed) noexcept {
  if (changed == InstrumentId::kUsdtUsd) {
    // FX (or a venue state change) affects every USDT-quoted row: recompute everything.
    const Slot& fx = slot(VenueId::kKraken, InstrumentId::kUsdtUsd);
    fx_live_ = fx.state == BookState::kLive && !fx.book.bids().empty() &&
               !fx.book.asks().empty();
    fx_rate_e8_ = fx_live_ ? fx.book.bids().price(0).raw / 2 + fx.book.asks().price(0).raw / 2
                           : 0;
    for (std::size_t c = 0; c < kNumConsolidated; ++c) {
      recompute_consolidated(c);
    }
  } else {
    recompute_consolidated(idx(changed));
  }
}

void Engine::recompute_consolidated(std::size_t c) noexcept {
  const auto inst = static_cast<InstrumentId>(c);
  ConsolidatedSummary& cs = snap_.consolidated[c];
  const std::size_t per_venue = config_.consolidated_levels_per_venue;

  std::array<const Slot*, kNumVenues> live{};
  std::array<VenueId, kNumVenues> live_venue{};
  std::size_t n_live = 0;
  cs.venues_mask = 0;
  for (const VenueId v : kAllVenues) {
    const Slot& s = slot(v, inst);
    if (!s.exists || s.state != BookState::kLive) {
      continue;
    }
    if (s.quote == Quote::kUsdt && !fx_live_) {
      continue;  // never guess the USDT basis
    }
    live[n_live] = &s;
    live_venue[n_live++] = v;
    cs.venues_mask = static_cast<std::uint8_t>(cs.venues_mask | (1u << idx(v)));
  }

  // K-way merge of already-sorted per-venue ladders: O(rows x venues), no sorting and no
  // allocation. Each source's current candidate price (converted to USD if needed) is
  // cached and recomputed only when that source advances. Ties go to the lower venue
  // index so output is deterministic.
  auto merge = [&](auto side_of, auto better, auto& out, std::uint8_t& n_out) {
    std::array<std::size_t, kNumVenues> next{};
    std::array<std::size_t, kNumVenues> limit{};
    std::array<std::int64_t, kNumVenues> cand{};
    auto load = [&](std::size_t i) {
      const auto& side = side_of(*live[i]);
      if (next[i] < limit[i]) {
        const std::int64_t p = side.price(next[i]).raw;
        cand[i] = live[i]->quote == Quote::kUsdt ? signals::convert(p, fx_rate_e8_) : p;
      }
    };
    for (std::size_t i = 0; i < n_live; ++i) {
      limit[i] = std::min(per_venue, side_of(*live[i]).size());
      load(i);
    }
    n_out = 0;
    while (n_out < kConsolidatedRows) {
      std::size_t best = kNumVenues;
      for (std::size_t i = 0; i < n_live; ++i) {
        if (next[i] < limit[i] && (best == kNumVenues || better(cand[i], cand[best]))) {
          best = i;
        }
      }
      if (best == kNumVenues) break;
      out[n_out++] = Row{cand[best], side_of(*live[best]).qty(next[best]).raw, live_venue[best]};
      ++next[best];
      load(best);
    }
  };
  merge([](const Slot& s) -> const Book::Bids& { return s.book.bids(); },
        [](std::int64_t a, std::int64_t b) { return a > b; }, cs.bids, cs.n_bids);
  merge([](const Slot& s) -> const Book::Asks& { return s.book.asks(); },
        [](std::int64_t a, std::int64_t b) { return a < b; }, cs.asks, cs.n_asks);

  if (cs.n_bids > 0 && cs.n_asks > 0) {
    const Row& b = cs.bids[0];
    const Row& a = cs.asks[0];
    cs.spread = a.price - b.price;
    cs.spread_bp_e2 = signals::spread_bp_e2(b.price, a.price);
    cs.microprice = signals::microprice(b.price, b.qty, a.price, a.qty);
    std::int64_t sb = 0;
    std::int64_t sa = 0;
    const std::size_t n = config_.imbalance_levels;
    for (std::size_t i = 0; i < n && i < cs.n_bids; ++i) sb += cs.bids[i].qty;
    for (std::size_t i = 0; i < n && i < cs.n_asks; ++i) sa += cs.asks[i].qty;
    cs.imbalance_e8 = signals::imbalance_e8(sb, sa);
  } else {
    cs.spread = cs.spread_bp_e2 = cs.microprice = cs.imbalance_e8 = 0;
  }
}

void Engine::fill_book_summaries() noexcept {
  const std::size_t n_imb = config_.imbalance_levels;
  for (const VenueId v : kAllVenues) {
    for (std::size_t i = 0; i < kNumInstruments; ++i) {
      const Slot& s = (*slots_)[idx(v)][i];
      BookSummary& bs = snap_.books[idx(v)][i];
      bs.exists = s.exists && find_instrument(v, static_cast<InstrumentId>(i)) != nullptr;
      if (!bs.exists) continue;
      const auto& bids = s.book.bids();
      const auto& asks = s.book.asks();
      bs.state = s.state;
      bs.last_reason = s.last_reason;
      bs.quote = s.quote;
      bs.bid_levels = static_cast<std::uint32_t>(bids.size());
      bs.ask_levels = static_cast<std::uint32_t>(asks.size());
      bs.best_bid = bids.empty() ? 0 : bids.price(0).raw;
      bs.best_bid_qty = bids.empty() ? 0 : bids.qty(0).raw;
      bs.best_ask = asks.empty() ? 0 : asks.price(0).raw;
      bs.best_ask_qty = asks.empty() ? 0 : asks.qty(0).raw;
      bs.microprice = (bids.empty() || asks.empty())
                          ? 0
                          : signals::microprice(bs.best_bid, bs.best_bid_qty, bs.best_ask,
                                                bs.best_ask_qty);
      std::int64_t sb = 0;
      std::int64_t sa = 0;
      for (std::size_t r = 0; r < n_imb && r < bids.size(); ++r) sb += bids.qty(r).raw;
      for (std::size_t r = 0; r < n_imb && r < asks.size(); ++r) sa += asks.qty(r).raw;
      bs.imbalance_e8 = signals::imbalance_e8(sb, sa);
      bs.last_change_ticks = s.last_change_ticks;
      bs.lag_last_ns = s.lag.last;
      bs.lag_min_ns = s.lag.min() == INT64_MAX ? 0 : s.lag.min();
      bs.messages = s.messages;
      bs.checksum_ok = s.checksum_ok;
      bs.checksum_fail = s.checksum_fail;
      bs.resyncs = s.resyncs;
    }
  }
}

void Engine::publish() noexcept {
  fill_book_summaries();
  snap_.fx_usdt_usd_e8 = fx_rate_e8_;
  snap_.fx_live = fx_live_;
  ++snap_.seq;
  snap_.ticks = clock::ticks();
  if (out_ != nullptr) {
    out_->write_slot() = snap_;
    out_->publish();
  }
}

void Engine::dump(std::FILE* out, bool consolidated) const {
  std::array<char, kMaxDecimalChars> b1{};
  std::array<char, kMaxDecimalChars> b2{};
  constexpr std::size_t kLevels = 20;
  for (const VenueId v : kAllVenues) {
    for (const VenueInstrument& vi : instruments_of(v)) {
      const Slot& s = slot(v, vi.id);
      if (!s.exists) continue;
      const auto& bids = s.book.bids();
      const auto& asks = s.book.asks();
      std::fprintf(out, "%s %s %s %zu %zu\n", venue_name(v).data(), vi.symbol.data(),
                   book_state_name(s.state), bids.size(), asks.size());
      for (std::size_t r = 0; r < kLevels && r < bids.size(); ++r) {
        const auto p = fmt(bids.price(r).raw, b1);
        const auto q = fmt(bids.qty(r).raw, b2);
        std::fprintf(out, "  B %.*s %.*s\n", static_cast<int>(p.size()), p.data(),
                     static_cast<int>(q.size()), q.data());
      }
      for (std::size_t r = 0; r < kLevels && r < asks.size(); ++r) {
        const auto p = fmt(asks.price(r).raw, b1);
        const auto q = fmt(asks.qty(r).raw, b2);
        std::fprintf(out, "  A %.*s %.*s\n", static_cast<int>(p.size()), p.data(),
                     static_cast<int>(q.size()), q.data());
      }
    }
  }
  if (!consolidated) return;
  const auto fx = fmt(fx_rate_e8_, b1);
  std::fprintf(out, "fx %s %.*s\n", fx_live_ ? "live" : "down", static_cast<int>(fx.size()),
               fx.data());
  for (std::size_t c = 0; c < kNumConsolidated; ++c) {
    const ConsolidatedSummary& cs = snap_.consolidated[c];
    std::fprintf(out, "consolidated %s mask=%u\n",
                 instrument_name(static_cast<InstrumentId>(c)).data(), cs.venues_mask);
    for (std::size_t r = 0; r < cs.n_bids; ++r) {
      const auto p = fmt(cs.bids[r].price, b1);
      const auto q = fmt(cs.bids[r].qty, b2);
      std::fprintf(out, "  B %.*s %.*s %s\n", static_cast<int>(p.size()), p.data(),
                   static_cast<int>(q.size()), q.data(), venue_name(cs.bids[r].venue).data());
    }
    for (std::size_t r = 0; r < cs.n_asks; ++r) {
      const auto p = fmt(cs.asks[r].price, b1);
      const auto q = fmt(cs.asks[r].qty, b2);
      std::fprintf(out, "  A %.*s %.*s %s\n", static_cast<int>(p.size()), p.data(),
                   static_cast<int>(q.size()), q.data(), venue_name(cs.asks[r].venue).data());
    }
  }
}

}  // namespace cb
