#include "publish/publisher.hpp"

#include "core/clock.hpp"
#include "core/log.hpp"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cb {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
using namespace std::chrono_literals;

namespace {

using Message = std::shared_ptr<const std::string>;

// One browser. Never queues more than one pending message of each kind: if the browser
// (or its network) is slower than the publish rate, older snapshots are dropped and it
// simply receives the newest one next. Memory per client is therefore bounded, and a
// slow client can only ever slow itself.
struct Client : std::enable_shared_from_this<Client> {
  explicit Client(beast::tcp_stream&& stream) : ws(std::move(stream)) {}

  void send_snapshot(Message m) { enqueue(pending_snap, std::move(m)); }
  void send_latency(Message m) { enqueue(pending_lat, std::move(m)); }

  websocket::stream<beast::tcp_stream> ws;
  bool closed = false;
  std::uint64_t dropped = 0;

 private:
  void enqueue(Message& slot, Message m) {
    if (closed) return;
    if (slot) ++dropped;
    slot = std::move(m);
    if (!writing) write_next();
  }

  void write_next() {
    // Latency updates (1/s) go first so a busy snapshot stream cannot starve them.
    Message m = pending_lat ? std::exchange(pending_lat, nullptr)
                            : std::exchange(pending_snap, nullptr);
    if (!m) {
      writing = false;
      return;
    }
    writing = true;
    ws.text(true);
    ws.async_write(asio::buffer(*m), [self = shared_from_this(), m](beast::error_code ec, std::size_t) {
      if (ec) {
        self->closed = true;
        self->writing = false;
        return;
      }
      self->write_next();
    });
  }

  Message pending_snap;
  Message pending_lat;
  bool writing = false;
};

std::string read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

}  // namespace

struct Publisher::Impl {
  Impl(const PublisherInputs& i, const std::atomic<bool>& s) : in(i), stop(s) {}

  asio::awaitable<void> listener(tcp::acceptor acceptor);
  asio::awaitable<void> http_session(beast::tcp_stream stream);
  asio::awaitable<void> ticker();
  void broadcast_snapshot(const Message& m) {
    for (auto& c : clients) c->send_snapshot(m);
  }

  PublisherInputs in;
  const std::atomic<bool>& stop;
  asio::io_context ioc{1};
  std::vector<std::shared_ptr<Client>> clients;
  std::unordered_map<std::string, std::pair<std::string, std::string>> files;  // target -> (type, body)
};

Publisher::Publisher(const PublisherInputs& inputs, const std::atomic<bool>& stop)
    : impl_(std::make_unique<Impl>(inputs, stop)) {}

Publisher::~Publisher() = default;

bool Publisher::run() {
  Impl& im = *impl_;
  const std::string& root = im.in.config->web_root;
  for (const auto& [target, file, type] :
       {std::tuple{"/", "index.html", "text/html; charset=utf-8"},
        std::tuple{"/app.js", "app.js", "application/javascript; charset=utf-8"}}) {
    std::string body = read_file(root + "/" + file);
    if (body.empty()) {
      CB_WARN("publisher: %s/%s not found; dashboard page will 404", root.c_str(), file);
    }
    im.files[target] = {type, std::move(body)};
  }

  beast::error_code ec;
  const auto address = asio::ip::make_address(im.in.config->http_bind, ec);
  if (ec) {
    CB_ERROR("publisher: bad bind address %s", im.in.config->http_bind.c_str());
    return false;
  }
  tcp::acceptor acceptor(im.ioc);
  const tcp::endpoint ep(address, im.in.config->http_port);
  acceptor.open(ep.protocol(), ec);
  if (!ec) acceptor.set_option(asio::socket_base::reuse_address(true), ec);
  if (!ec) acceptor.bind(ep, ec);
  if (!ec) acceptor.listen(asio::socket_base::max_listen_connections, ec);
  if (ec) {
    CB_ERROR("publisher: cannot listen on %s:%u: %s", im.in.config->http_bind.c_str(),
             im.in.config->http_port, ec.message().c_str());
    return false;
  }
  CB_INFO("publisher: dashboard on http://%s:%u/", im.in.config->http_bind.c_str(),
          im.in.config->http_port);
  asio::co_spawn(im.ioc, im.listener(std::move(acceptor)), asio::detached);
  asio::co_spawn(im.ioc, im.ticker(), asio::detached);
  im.ioc.run();
  return true;
}

asio::awaitable<void> Publisher::Impl::listener(tcp::acceptor acceptor) {
  for (;;) {
    beast::error_code ec;
    tcp::socket socket = co_await acceptor.async_accept(asio::redirect_error(asio::use_awaitable, ec));
    if (ec) {
      if (stop.load(std::memory_order_relaxed)) co_return;
      continue;
    }
    asio::co_spawn(ioc, http_session(beast::tcp_stream(std::move(socket))), asio::detached);
  }
}

asio::awaitable<void> Publisher::Impl::http_session(beast::tcp_stream stream) {
  try {
    beast::flat_buffer buffer;
    for (;;) {
      http::request<http::string_body> req;
      stream.expires_after(30s);
      co_await http::async_read(stream, buffer, req, asio::use_awaitable);

      if (websocket::is_upgrade(req)) {
        if (req.target() != "/ws") co_return;
        stream.expires_never();
        auto client = std::make_shared<Client>(std::move(stream));
        client->ws.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
        co_await client->ws.async_accept(req, asio::use_awaitable);
        clients.push_back(client);
        CB_INFO("publisher: browser connected (%zu total)", clients.size());
        // Don't make a new page wait up to a second for its first latency histograms.
        client->send_latency(std::make_shared<const std::string>(latency_json(in.latency)));
        beast::flat_buffer rb;
        beast::error_code ec;
        // Browsers send nothing we need; reading keeps control frames (ping/close) flowing.
        while (!ec) {
          co_await client->ws.async_read(rb, asio::redirect_error(asio::use_awaitable, ec));
          rb.consume(rb.size());
        }
        client->closed = true;
        std::erase(clients, client);
        co_return;
      }

      std::string target(req.target());
      target = target.substr(0, target.find('?'));
      http::response<http::string_body> res;
      res.version(req.version());
      res.keep_alive(req.keep_alive());
      res.set(http::field::server, "consolidated-book");
      res.set(http::field::cache_control, "no-cache");
      if (const auto it = files.find(target); it != files.end() && !it->second.second.empty()) {
        res.result(http::status::ok);
        res.set(http::field::content_type, it->second.first);
        res.body() = it->second.second;
      } else {
        res.result(http::status::not_found);
        res.set(http::field::content_type, "text/plain");
        res.body() = "not found\n";
      }
      res.prepare_payload();
      co_await http::async_write(stream, res, asio::use_awaitable);
      if (!res.keep_alive()) co_return;
    }
  } catch (const std::exception&) {
    // client went away or sent garbage: nothing to clean up beyond the coroutine frame
  }
}

asio::awaitable<void> Publisher::Impl::ticker() {
  asio::steady_timer timer(ioc);
  const auto period = std::chrono::milliseconds(1000 / std::max(1u, in.config->publish_hz));
  std::uint32_t n = 0;
  for (;;) {
    timer.expires_after(period);
    co_await timer.async_wait(asio::use_awaitable);
    if (stop.load(std::memory_order_relaxed)) {
      ioc.stop();
      co_return;
    }
    if (clients.empty()) {
      (void)in.snapshots->update();  // keep consuming so the next client gets fresh data
      continue;
    }
    if (in.snapshots->update()) {
      broadcast_snapshot(std::make_shared<const std::string>(
          snapshot_json(in.snapshots->read_slot(), in.shared, in.allocs, clock::ticks())));
    }
    if (++n % in.config->publish_hz == 0) {  // latency histograms once per second
      const auto lat = std::make_shared<const std::string>(latency_json(in.latency));
      for (auto& c : clients) c->send_latency(lat);
    }
  }
}

}  // namespace cb
