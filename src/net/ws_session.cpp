#include "net/ws_session.hpp"

#include "core/alloc_counter.hpp"
#include "core/clock.hpp"
#include "core/log.hpp"

#include <boost/asio.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include <simdjson.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <random>
#include <variant>

namespace cb {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;
using Stream = websocket::stream<ssl::stream<beast::tcp_stream>>;
using namespace std::chrono_literals;
using namespace asio::experimental::awaitable_operators;

bool parse_wss_url(const std::string& url, ParsedUrl& out) {
  constexpr std::string_view kScheme = "wss://";
  if (url.rfind(kScheme, 0) != 0) {
    return false;
  }
  const std::string rest = url.substr(kScheme.size());
  const auto slash = rest.find('/');
  const std::string authority = rest.substr(0, slash);
  out.path = slash == std::string::npos ? "/" : rest.substr(slash);
  const auto colon = authority.find(':');
  out.host = authority.substr(0, colon);
  out.port = colon == std::string::npos ? "443" : authority.substr(colon + 1);
  return !out.host.empty() && !out.port.empty();
}

struct WsSession::Impl {
  Impl(VenueProtocol& p, VenueShared& s, const VenueSettings& vs, Recorder* r,
       const std::atomic<bool>& st)
      : protocol(p), shared(s), settings(vs), recorder(r), stop(st),
        padded_cap(p.max_frame() + simdjson::SIMDJSON_PADDING),
        padded(std::make_unique<char[]>(padded_cap)),
        rng(std::random_device{}()) {
    ssl_ctx.set_default_verify_paths();
    ssl_ctx.set_verify_mode(ssl::verify_peer);
  }

  asio::awaitable<void> main_loop();
  asio::awaitable<DownReason> connection();
  asio::awaitable<DownReason> read_loop(Stream& s);
  asio::awaitable<DownReason> watchdog(Stream& s);

  [[nodiscard]] const char* name() const { return venue_name(protocol.venue()).data(); }

  VenueProtocol& protocol;
  VenueShared& shared;
  const VenueSettings& settings;
  Recorder* recorder;
  const std::atomic<bool>& stop;

  asio::io_context ioc{1};
  ssl::context ssl_ctx{ssl::context::tls_client};
  ParsedUrl url;
  std::size_t padded_cap;
  std::unique_ptr<char[]> padded;  // frame copy with simdjson's required tail padding
  std::uint32_t epoch = 0;
  bool connected = false;
  std::minstd_rand rng;
};

WsSession::WsSession(VenueProtocol& protocol, VenueShared& shared,
                     const VenueSettings& settings, Recorder* recorder,
                     const std::atomic<bool>& stop)
    : impl_(std::make_unique<Impl>(protocol, shared, settings, recorder, stop)) {}

WsSession::~WsSession() = default;

void WsSession::run() {
  Impl& im = *impl_;
  alloc::set_thread_counter(&im.shared.io_heap_allocs);
  if (!parse_wss_url(im.settings.url, im.url)) {
    CB_ERROR("%s: invalid url '%s' (expected wss://host[:port]/path)", im.name(),
             im.settings.url.c_str());
    return;
  }
  asio::co_spawn(im.ioc, im.main_loop(), [&im](std::exception_ptr e) {
    if (e) {
      try {
        std::rethrow_exception(e);
      } catch (const std::exception& ex) {
        CB_ERROR("%s: session loop died: %s", im.name(), ex.what());
      }
    }
  });
  im.ioc.run();
}

asio::awaitable<void> WsSession::Impl::main_loop() {
  int attempt = 0;
  asio::steady_timer timer(ioc);
  while (!stop.load(std::memory_order_relaxed)) {
    shared.conn_state.store(ConnState::kConnecting, std::memory_order_relaxed);
    const auto started = std::chrono::steady_clock::now();
    connected = false;
    DownReason reason = DownReason::kDisconnected;
    try {
      reason = co_await connection();
    } catch (const boost::system::system_error& e) {
      CB_WARN("%s: connection error: %s", name(), e.code().message().c_str());
    } catch (const std::exception& e) {
      CB_WARN("%s: connection error: %s", name(), e.what());
    }
    protocol.on_disconnect(reason);
    if (connected && recorder != nullptr) {
      recorder->write(protocol.venue(), cbrec::RecordKind::kDisconnect, epoch, clock::ticks(),
                      clock::wall_ns(), {}, static_cast<std::uint16_t>(reason));
    }
    shared.conn_state.store(ConnState::kDisconnected, std::memory_order_relaxed);
    if (stop.load(std::memory_order_relaxed)) {
      break;
    }

    // Exponential backoff with +/-20% jitter so reconnecting clients do not synchronize;
    // a connection that stayed up for a minute resets the backoff.
    if (std::chrono::steady_clock::now() - started > 60s) {
      attempt = 0;
    }
    const auto base = std::min<std::chrono::milliseconds>(250ms * (1 << attempt), 30'000ms);
    std::uniform_real_distribution<double> jitter(0.8, 1.2);
    auto remaining = std::chrono::milliseconds(
        static_cast<std::int64_t>(static_cast<double>(base.count()) * jitter(rng)));
    attempt = std::min(attempt + 1, 7);
    CB_INFO("%s: reconnecting in %lld ms (%s)", name(),
            static_cast<long long>(remaining.count()), reason_name(reason).data());
    while (remaining > 0ms && !stop.load(std::memory_order_relaxed)) {
      const auto slice = std::min<std::chrono::milliseconds>(remaining, 100ms);
      timer.expires_after(slice);
      co_await timer.async_wait(asio::use_awaitable);
      remaining -= slice;
    }
  }
}

asio::awaitable<DownReason> WsSession::Impl::connection() {
  tcp::resolver resolver(ioc);
  const auto endpoints = co_await resolver.async_resolve(url.host, url.port, asio::use_awaitable);

  Stream s(ioc, ssl_ctx);
  auto& tcp_layer = beast::get_lowest_layer(s);
  tcp_layer.expires_after(10s);  // bounds connect + TLS handshake
  co_await tcp_layer.async_connect(endpoints, asio::use_awaitable);

  // SNI: CDN-fronted endpoints (all four venues) pick the certificate by server name.
  // SSL_set_tlsext_host_name() is a macro wrapping this call with a C-style cast, which
  // GCC flags in our code (-Wold-style-cast); calling SSL_ctrl directly is equivalent.
  if (SSL_ctrl(s.next_layer().native_handle(), SSL_CTRL_SET_TLSEXT_HOSTNAME,
               TLSEXT_NAMETYPE_host_name, const_cast<char*>(url.host.c_str())) != 1) {
    throw beast::system_error(beast::error_code(static_cast<int>(::ERR_get_error()),
                                                asio::error::get_ssl_category()));
  }
  s.next_layer().set_verify_callback(ssl::host_name_verification(url.host));
  co_await s.next_layer().async_handshake(ssl::stream_base::client, asio::use_awaitable);
  tcp_layer.expires_never();

  s.set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
  s.set_option(websocket::stream_base::decorator([](websocket::request_type& req) {
    req.set(beast::http::field::user_agent, "consolidated-book/0.1");
  }));
  s.read_message_max(protocol.max_frame());
  const std::string host_header = url.port == "443" ? url.host : url.host + ":" + url.port;
  co_await s.async_handshake(host_header, url.path, asio::use_awaitable);

  ++epoch;
  connected = true;
  shared.epoch.store(epoch, std::memory_order_relaxed);
  bump_relaxed(shared.connects);
  shared.last_rx_ticks.store(clock::ticks(), std::memory_order_relaxed);
  protocol.on_connect(epoch);
  if (recorder != nullptr) {
    recorder->write(protocol.venue(), cbrec::RecordKind::kConnect, epoch, clock::ticks(),
                    clock::wall_ns(), {});
  }
  CB_INFO("%s: connected (epoch %u) %s", name(), epoch, settings.url.c_str());

  s.text(true);
  for (const std::string& msg : protocol.subscribe_messages()) {
    co_await s.async_write(asio::buffer(msg), asio::use_awaitable);
  }
  shared.conn_state.store(ConnState::kSubscribed, std::memory_order_relaxed);

  // Whichever finishes first decides why the connection ends; the other is cancelled.
  const auto result = co_await (read_loop(s) || watchdog(s));
  const DownReason reason = std::visit([](DownReason r) { return r; }, result);

  beast::error_code ec;
  tcp_layer.socket().close(ec);  // no graceful close: the session is being discarded
  co_return reason;
}

asio::awaitable<DownReason> WsSession::Impl::read_loop(Stream& s) {
  beast::flat_buffer buffer;
  buffer.reserve(protocol.max_frame());  // grow once at connect, not during the session
  for (;;) {
    // Read in pieces and refresh the liveness stamp on every piece: Coinbase's ~4.5 MB
    // snapshot took >5 s to arrive on a slow link, and a whole-message read made a
    // healthy connection look silent to the watchdog (found in a live run).
    do {
      co_await s.async_read_some(buffer, 1u << 20, asio::use_awaitable);
      shared.last_rx_ticks.store(clock::ticks(), std::memory_order_relaxed);
    } while (!s.is_message_done());
    const std::uint64_t t_recv = clock::ticks();
    const std::int64_t wall = clock::wall_ns();
    const auto data = buffer.cdata();
    const std::size_t len = data.size();
    if (len + simdjson::SIMDJSON_PADDING > padded_cap) {
      co_return DownReason::kParseError;
    }
    // One memcpy buys simdjson's 64-byte readable tail without depending on Beast's
    // buffer layout; it is small next to the TLS decryption that produced these bytes.
    std::memcpy(padded.get(), data.data(), len);
    buffer.consume(len);

    bump_relaxed(shared.frames);
    bump_relaxed(shared.bytes, len);
    shared.last_rx_ticks.store(t_recv, std::memory_order_relaxed);
    if (recorder != nullptr) {
      recorder->write(protocol.venue(), cbrec::RecordKind::kFrame, epoch, t_recv, wall,
                      std::string_view(padded.get(), len));
    }
    const FrameResult r = protocol.on_frame(padded.get(), len, padded_cap, FrameMeta{t_recv, wall});
    if (r.status == FrameStatus::kResync) {
      co_return r.reason;
    }
  }
}

asio::awaitable<DownReason> WsSession::Impl::watchdog(Stream& s) {
  asio::steady_timer timer(ioc);
  const std::string ping = protocol.ping_message();
  const auto ping_every = std::chrono::milliseconds(protocol.ping_interval_ms());
  const auto liveness_ticks = static_cast<std::uint64_t>(protocol.liveness_timeout_ms()) *
                              clock::ticks_per_second() / 1000;
  auto last_ping = std::chrono::steady_clock::now();
  for (;;) {
    timer.expires_after(100ms);
    co_await timer.async_wait(asio::use_awaitable);
    if (stop.load(std::memory_order_relaxed)) {
      co_return DownReason::kShutdown;
    }
    if (clock::ticks() - shared.last_rx_ticks.load(std::memory_order_relaxed) > liveness_ticks) {
      CB_WARN("%s: no data for %u ms", name(), protocol.liveness_timeout_ms());
      co_return DownReason::kLiveness;
    }
    // acquire: pairs with the book thread's release store of the request.
    if (shared.resync_request.load(std::memory_order_acquire) == epoch) {
      CB_INFO("%s: book thread requested resync of epoch %u", name(), epoch);
      co_return DownReason::kDisconnected;
    }
    const auto now = std::chrono::steady_clock::now();
    if (!ping.empty() && ping_every.count() > 0 && now - last_ping >= ping_every) {
      protocol.note_ping_sent(clock::wall_ns());
      co_await s.async_write(asio::buffer(ping), asio::use_awaitable);
      last_ping = now;
    }
  }
}

}  // namespace cb
