#pragma once

// One venue's live connection: TLS WebSocket client, subscribe, ping, liveness watchdog,
// resync on request, reconnect with exponential backoff + jitter. Runs its own
// io_context on the calling thread (one I/O thread per venue, DESIGN §4).

#include "record/recorder.hpp"
#include "venue/protocol.hpp"
#include "venue/venue_shared.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace cb {

struct ParsedUrl {
  std::string host;
  std::string port;
  std::string path;
};
// Accepts wss://host[:port][/path]. Returns false for anything else.
bool parse_wss_url(const std::string& url, ParsedUrl& out);

class WsSession {
 public:
  WsSession(VenueProtocol& protocol, VenueShared& shared, const VenueSettings& settings,
            Recorder* recorder, const std::atomic<bool>& stop);
  ~WsSession();
  WsSession(const WsSession&) = delete;
  WsSession& operator=(const WsSession&) = delete;

  // Blocks until `stop` becomes true.
  void run();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;  // keeps Beast/Asio out of every includer's compile
};

}  // namespace cb
