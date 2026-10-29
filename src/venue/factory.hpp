#pragma once

#include "venue/protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace cb {

// Largest frame each venue may send. Coinbase's full-book snapshot was 4.53 MB live;
// 8 MiB leaves ~1.8x headroom. simdjson preallocates several bytes of index per input
// byte, so this is also the main memory knob (~45 MB for Coinbase).
std::size_t max_frame_bytes(VenueId venue) noexcept;

// Depth the book thread truncates to after each message (0 = never truncate).
std::uint32_t book_depth(VenueId venue, InstrumentId inst, const VenueSettings& s) noexcept;

std::unique_ptr<VenueProtocol> make_protocol(VenueId venue, const VenueSettings& settings,
                                             EventSink& sink, VenueShared& shared);

}  // namespace cb
