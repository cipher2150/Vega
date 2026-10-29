#include "venue/factory.hpp"

#include "venue/bybit.hpp"
#include "venue/coinbase.hpp"
#include "venue/kraken.hpp"
#include "venue/okx.hpp"

namespace cb {

std::size_t max_frame_bytes(VenueId venue) noexcept {
  return venue == VenueId::kCoinbase ? (8u << 20) : (1u << 20);
}

std::uint32_t book_depth(VenueId venue, InstrumentId inst, const VenueSettings& s) noexcept {
  switch (venue) {
    case VenueId::kCoinbase:
      return 0;  // full depth, bounded by the price horizon instead
    case VenueId::kKraken:
      // Kraken requires truncating to the subscribed depth (it sends no deletes for
      // levels pushed out of range), and its checksum is defined on the truncated book.
      return inst == InstrumentId::kUsdtUsd ? KrakenProtocol::kFxDepth
                                            : (s.depth == 0 ? 25 : s.depth);
    case VenueId::kOkx:
      return 400;  // `books` channel depth; OKX deletes out-of-window levels itself
    case VenueId::kBybit:
      return s.depth == 0 ? 50 : s.depth;
  }
  return 0;
}

std::unique_ptr<VenueProtocol> make_protocol(VenueId venue, const VenueSettings& settings,
                                             EventSink& sink, VenueShared& shared) {
  const std::size_t max_frame = max_frame_bytes(venue);
  switch (venue) {
    case VenueId::kCoinbase:
      return std::make_unique<CoinbaseProtocol>(venue, settings, sink, shared, max_frame);
    case VenueId::kKraken:
      return std::make_unique<KrakenProtocol>(venue, settings, sink, shared, max_frame);
    case VenueId::kOkx:
      return std::make_unique<OkxProtocol>(venue, settings, sink, shared, max_frame);
    case VenueId::kBybit:
      return std::make_unique<BybitProtocol>(venue, settings, sink, shared, max_frame);
  }
  return nullptr;
}

}  // namespace cb
