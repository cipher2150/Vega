#pragma once

// .cbrec: binary capture of raw venue frames, for deterministic replay.
//
//   FileHeader (32 bytes) then records, each: RecordHeader (32 bytes) + payload[len].
//   All integers little-endian (both target platforms are little-endian arm64).
//
// Frames are stored exactly as received (after TLS + WebSocket deframing), so replay
// exercises the real parsers. Connect/disconnect records carry the session epoch so the
// replayed protocol objects see the same session boundaries as the live run.

#include "venue/types.hpp"

#include <cstdint>
#include <cstring>

namespace cb::cbrec {

inline constexpr char kMagic[8] = {'C', 'B', 'R', 'E', 'C', '0', '0', '1'};
inline constexpr std::uint32_t kVersion = 1;

struct FileHeader {
  char magic[8];
  std::uint32_t version;
  std::uint32_t reserved;
  std::uint64_t tick_freq;     // ticks per second of the recording machine
  std::int64_t start_wall_ns;
};
static_assert(sizeof(FileHeader) == 32);

enum class RecordKind : std::uint8_t { kFrame = 0, kConnect = 1, kDisconnect = 2 };

struct RecordHeader {
  std::uint32_t len;     // payload bytes
  VenueId venue;
  RecordKind kind;
  std::uint16_t flags;   // kDisconnect: the DownReason that ended the session
  std::uint32_t epoch;
  std::uint32_t reserved;
  std::uint64_t t_recv;  // monotonic ticks at receive (recording machine)
  std::int64_t wall_ns;  // wall clock at receive
};
static_assert(sizeof(RecordHeader) == 32);

}  // namespace cb::cbrec
