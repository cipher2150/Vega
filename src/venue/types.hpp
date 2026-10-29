#pragma once

// Static venue / instrument tables. Symbols, quote currencies and Kraken display
// precisions were checked live on 2026-10-29 (Kraken `instrument` channel: BTC/USD 1/8,
// ETH/USD 2/8, USDT/USD 5/8).

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace cb {

enum class VenueId : std::uint8_t { kCoinbase = 0, kKraken = 1, kOkx = 2, kBybit = 3 };
inline constexpr std::size_t kNumVenues = 4;
inline constexpr std::array<VenueId, kNumVenues> kAllVenues = {
    VenueId::kCoinbase, VenueId::kKraken, VenueId::kOkx, VenueId::kBybit};

// Canonical instruments. BTC/ETH are consolidated across venues; USDT-USD is the FX book
// used to convert USDT-quoted venues to USD (only Kraken carries it).
enum class InstrumentId : std::uint8_t { kBtcUsd = 0, kEthUsd = 1, kUsdtUsd = 2 };
inline constexpr std::size_t kNumInstruments = 3;
inline constexpr std::size_t kNumConsolidated = 2;  // BTC-USD, ETH-USD

enum class Quote : std::uint8_t { kUsd, kUsdt };

struct VenueInstrument {
  std::string_view symbol;  // the venue's own symbol
  InstrumentId id;
  Quote quote;
  std::uint8_t price_precision;  // Kraken checksum formatting only
  std::uint8_t qty_precision;
};

inline constexpr std::array<VenueInstrument, 2> kCoinbaseInstruments = {{
    {"BTC-USD", InstrumentId::kBtcUsd, Quote::kUsd, 0, 0},
    {"ETH-USD", InstrumentId::kEthUsd, Quote::kUsd, 0, 0},
}};
inline constexpr std::array<VenueInstrument, 3> kKrakenInstruments = {{
    {"BTC/USD", InstrumentId::kBtcUsd, Quote::kUsd, 1, 8},
    {"ETH/USD", InstrumentId::kEthUsd, Quote::kUsd, 2, 8},
    {"USDT/USD", InstrumentId::kUsdtUsd, Quote::kUsd, 5, 8},
}};
inline constexpr std::array<VenueInstrument, 2> kOkxInstruments = {{
    {"BTC-USDT", InstrumentId::kBtcUsd, Quote::kUsdt, 0, 0},
    {"ETH-USDT", InstrumentId::kEthUsd, Quote::kUsdt, 0, 0},
}};
inline constexpr std::array<VenueInstrument, 2> kBybitInstruments = {{
    {"BTCUSDT", InstrumentId::kBtcUsd, Quote::kUsdt, 0, 0},
    {"ETHUSDT", InstrumentId::kEthUsd, Quote::kUsdt, 0, 0},
}};

constexpr std::span<const VenueInstrument> instruments_of(VenueId v) noexcept {
  switch (v) {
    case VenueId::kCoinbase: return kCoinbaseInstruments;
    case VenueId::kKraken: return kKrakenInstruments;
    case VenueId::kOkx: return kOkxInstruments;
    case VenueId::kBybit: return kBybitInstruments;
  }
  return {};
}

// Linear scan over <= 3 entries: cheaper than hashing and allocation-free.
constexpr const VenueInstrument* find_instrument(VenueId v, std::string_view symbol) noexcept {
  for (const auto& vi : instruments_of(v)) {
    if (vi.symbol == symbol) {
      return &vi;
    }
  }
  return nullptr;
}

constexpr const VenueInstrument* find_instrument(VenueId v, InstrumentId id) noexcept {
  for (const auto& vi : instruments_of(v)) {
    if (vi.id == id) {
      return &vi;
    }
  }
  return nullptr;
}

constexpr std::string_view venue_name(VenueId v) noexcept {
  switch (v) {
    case VenueId::kCoinbase: return "coinbase";
    case VenueId::kKraken: return "kraken";
    case VenueId::kOkx: return "okx";
    case VenueId::kBybit: return "bybit";
  }
  return "?";
}

constexpr std::string_view instrument_name(InstrumentId i) noexcept {
  switch (i) {
    case InstrumentId::kBtcUsd: return "BTC-USD";
    case InstrumentId::kEthUsd: return "ETH-USD";
    case InstrumentId::kUsdtUsd: return "USDT-USD";
  }
  return "?";
}

constexpr std::size_t idx(VenueId v) noexcept { return static_cast<std::size_t>(v); }
constexpr std::size_t idx(InstrumentId i) noexcept { return static_cast<std::size_t>(i); }

// Why a venue (or one of its books) stopped being trusted.
enum class DownReason : std::uint8_t {
  kNone = 0,
  kSequenceGap,
  kChecksum,
  kCrossed,
  kParseError,
  kQueueOverflow,
  kLiveness,
  kHorizon,
  kBookFull,
  kDisconnected,
  kVenueError,
  kShutdown,
};

constexpr std::string_view reason_name(DownReason r) noexcept {
  switch (r) {
    case DownReason::kNone: return "none";
    case DownReason::kSequenceGap: return "sequence_gap";
    case DownReason::kChecksum: return "checksum";
    case DownReason::kCrossed: return "crossed";
    case DownReason::kParseError: return "parse_error";
    case DownReason::kQueueOverflow: return "queue_overflow";
    case DownReason::kLiveness: return "liveness";
    case DownReason::kHorizon: return "horizon";
    case DownReason::kBookFull: return "book_full";
    case DownReason::kDisconnected: return "disconnected";
    case DownReason::kVenueError: return "venue_error";
    case DownReason::kShutdown: return "shutdown";
  }
  return "?";
}

}  // namespace cb
