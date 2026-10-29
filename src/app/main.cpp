// cbook: Consolidated Book service.
//
//   cbook live   [--config FILE] [--record FILE.cbrec]
//   cbook replay --file FILE.cbrec[.gz] [--config FILE] [--pipelined [--speed X]]
//                [--dump OUT.txt] [--dump-consolidated] [--json OUT.json]

#include "core/alloc_counter.hpp"
#include "core/clock.hpp"
#include "core/log.hpp"
#include "engine/config.hpp"
#include "engine/pipeline.hpp"
#include "net/ws_session.hpp"
#include "platform/thread.hpp"
#include "publish/publisher.hpp"
#include "publish/snapshot_json.hpp"
#include "record/recorder.hpp"
#include "record/recording.hpp"
#include "record/replay.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> g_stop{false};

extern "C" void on_signal(int) { g_stop.store(true); }  // lock-free atomic store: signal-safe

struct Args {
  std::string mode;
  std::string config;
  std::string record;
  std::string file;
  std::string dump;
  std::string json;
  bool pipelined = false;
  bool dump_consolidated = false;
  double speed = 0;  // pipelined replay pacing (0 = as fast as possible)
};

int usage() {
  std::fprintf(stderr,
               "usage:\n"
               "  cbook live   [--config FILE] [--record FILE.cbrec]\n"
               "  cbook replay --file FILE.cbrec[.gz] [--config FILE] [--pipelined [--speed X]]\n"
               "               [--dump OUT.txt] [--dump-consolidated] [--json OUT.json]\n");
  return 2;
}

bool parse_args(int argc, char** argv, Args& a) {
  if (argc < 2) return false;
  a.mode = argv[1];
  for (int i = 2; i < argc; ++i) {
    const std::string k = argv[i];
    auto next = [&](std::string& out) {
      if (i + 1 >= argc) return false;
      out = argv[++i];
      return true;
    };
    if (k == "--config") { if (!next(a.config)) return false; }
    else if (k == "--record") { if (!next(a.record)) return false; }
    else if (k == "--file") { if (!next(a.file)) return false; }
    else if (k == "--dump") { if (!next(a.dump)) return false; }
    else if (k == "--json") { if (!next(a.json)) return false; }
    else if (k == "--pipelined") a.pipelined = true;
    else if (k == "--speed") { std::string v; if (!next(v)) return false; a.speed = std::stod(v); a.pipelined = true; }
    else if (k == "--dump-consolidated") a.dump_consolidated = true;
    else return false;
  }
  return a.mode == "live" || a.mode == "replay";
}

std::optional<cb::AppConfig> load(const Args& a) {
  std::string path = a.config;
  if (path.empty() && std::filesystem::exists("config/default.json")) {
    path = "config/default.json";
  }
  if (path.empty()) return cb::AppConfig{};
  std::string err;
  auto cfg = cb::load_config(path, err);
  if (!cfg) CB_ERROR("%s", err.c_str());
  return cfg;
}

void print_stage(std::FILE* out, const char* name, const cb::Histogram& h) {
  auto snap = std::make_unique<cb::Histogram::Snapshot>();
  h.snapshot_into(*snap);
  auto ns = [](std::uint64_t t) { return cb::clock::ticks_to_ns(static_cast<double>(t)); };
  std::fprintf(out, "  %-15s %10llu %10.1f %9.0f %9.0f %9.0f %10.0f\n", name,
               static_cast<unsigned long long>(snap->count), cb::clock::ticks_to_ns(snap->mean()),
               ns(snap->percentile(0.5)), ns(snap->percentile(0.99)),
               ns(snap->percentile(0.999)), ns(snap->max));
}

int run_replay(const Args& a, const cb::AppConfig& cfg) {
  if (a.file.empty()) return usage();
  cb::Recording rec;
  std::string err;
  if (!rec.load(a.file, err)) {
    CB_ERROR("%s", err.c_str());
    return 1;
  }

  cb::DrainWaiter drain;
  cb::SpinWaiter spin(0);
  cb::SpaceWaiter& waiter = a.pipelined ? static_cast<cb::SpaceWaiter&>(spin) : drain;
  auto pipeline = std::make_unique<cb::Pipeline>(cfg, waiter);
  drain.attach(pipeline->engine());

  cb::set_current_thread_role(cb::ThreadRole::kBook, "replay-book", cfg.book_cpu >= 0
                                                                       ? std::optional<int>(cfg.book_cpu)
                                                                       : std::nullopt);
  const cb::ReplayResult r =
      a.pipelined ? cb::replay_pipelined(*pipeline, rec, a.speed) : cb::replay_inline(*pipeline, rec);

  const auto& snap = pipeline->engine().snapshot();
  if (a.pipelined && a.speed > 0) {
    std::printf("replay %s (pipelined, paced at %.1fx recorded speed)\n", a.file.c_str(), a.speed);
  } else {
    std::printf("replay %s (%s)\n", a.file.c_str(),
                a.pipelined ? "pipelined, as fast as possible: throughput only" : "inline");
  }
  std::printf("  records %zu  frames %llu  bytes %llu  wall %.3f s\n", rec.records().size(),
              static_cast<unsigned long long>(r.frames), static_cast<unsigned long long>(r.bytes),
              r.seconds);
  std::printf("  throughput %.0f frames/s  %.1f MB/s  %.0f book-messages/s  %.0f events/s\n",
              static_cast<double>(r.frames) / r.seconds,
              static_cast<double>(r.bytes) / r.seconds / 1e6,
              static_cast<double>(snap.messages) / r.seconds,
              static_cast<double>(snap.events) / r.seconds);
  std::printf("  clock: %.2f ns/tick (%llu Hz)\n", cb::clock::ticks_to_ns(1.0),
              static_cast<unsigned long long>(cb::clock::ticks_per_second()));
  std::printf("  %-15s %10s %10s %9s %9s %9s %10s   (ns; mean = total/count)\n", "stage",
              "count", "mean", "p50", "p99", "p99.9", "max");

  for (const cb::VenueId v : cb::kAllVenues) {
    if (const auto* p = pipeline->protocol(v)) {
      print_stage(stdout, (std::string("parse/") + std::string(cb::venue_name(v))).c_str(),
                  p->parse_histogram());
    }
  }
  const auto& st = pipeline->engine().stages();
  print_stage(stdout, "queue", st.queue);
  print_stage(stdout, "book", st.book);
  print_stage(stdout, "signal", st.signal);
  print_stage(stdout, "publish", st.publish);
  print_stage(stdout, "e2e", st.e2e);

  for (const cb::VenueId v : cb::kAllVenues) {
    for (std::size_t i = 0; i < cb::kNumInstruments; ++i) {
      const auto& b = snap.books[cb::idx(v)][i];
      if (!b.exists) continue;
      std::printf("  %-8s %-8s %-7s msgs %8llu  ck_ok %7llu  ck_fail %llu  resyncs %llu  last_reason %s\n",
                  cb::venue_name(v).data(), cb::instrument_name(static_cast<cb::InstrumentId>(i)).data(),
                  cb::book_state_name(b.state), static_cast<unsigned long long>(b.messages),
                  static_cast<unsigned long long>(b.checksum_ok),
                  static_cast<unsigned long long>(b.checksum_fail),
                  static_cast<unsigned long long>(b.resyncs), cb::reason_name(b.last_reason).data());
    }
  }

  if (!a.dump.empty()) {
    std::FILE* f = std::fopen(a.dump.c_str(), "w");
    if (f == nullptr) {
      CB_ERROR("cannot write %s", a.dump.c_str());
      return 1;
    }
    pipeline->engine().dump(f, a.dump_consolidated);
    std::fclose(f);
  }
  if (!a.json.empty()) {
    cb::LatencySources src;
    for (const cb::VenueId v : cb::kAllVenues) {
      if (const auto* p = pipeline->protocol(v)) src.parse[cb::idx(v)] = &p->parse_histogram();
    }
    src.stages = &st;
    const std::string j = cb::latency_json(src);
    if (std::FILE* f = std::fopen(a.json.c_str(), "w")) {
      std::fwrite(j.data(), 1, j.size(), f);
      std::fclose(f);
    }
  }
  return 0;
}

int run_live(const Args& a, const cb::AppConfig& cfg) {
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
#if defined(SIGPIPE)
  std::signal(SIGPIPE, SIG_IGN);  // a browser closing mid-write must not kill the process
#endif

  cb::SpinWaiter waiter(100'000'000);  // 100 ms: then the venue resyncs (queue overflow)
  auto pipeline = std::make_unique<cb::Pipeline>(cfg, waiter);

  std::unique_ptr<cb::Recorder> recorder;
  if (!a.record.empty()) {
    recorder = std::make_unique<cb::Recorder>();
    if (!recorder->open(a.record)) return 1;
    CB_INFO("recording raw frames to %s", a.record.c_str());
  }

  std::vector<std::thread> threads;
  for (const cb::VenueId v : cb::kAllVenues) {
    cb::VenueProtocol* proto = pipeline->protocol(v);
    if (proto == nullptr) {
      CB_INFO("%s: disabled in config", cb::venue_name(v).data());
      continue;
    }
    threads.emplace_back([&, v, proto] {
      cb::set_current_thread_role(cb::ThreadRole::kIo, std::string("io-") + std::string(cb::venue_name(v)));
      cb::WsSession session(*proto, pipeline->shared(v), cfg.venue(v), recorder.get(), g_stop);
      session.run();
    });
  }

  std::atomic<std::uint64_t> book_allocs{0};
  std::thread book([&] {
    const auto placement = cb::set_current_thread_role(
        cb::ThreadRole::kBook, "book",
        cfg.book_cpu >= 0 ? std::optional<int>(cfg.book_cpu) : std::nullopt);
    CB_INFO("book thread: pinned=%d qos=%d idle=%s", placement.pinned, placement.qos_applied,
            cfg.idle == cb::IdlePolicy::kSpin ? "spin" : "backoff");
    cb::alloc::set_thread_counter(&book_allocs);
    std::uint32_t idle_polls = 0;
    while (!g_stop.load(std::memory_order_relaxed)) {
      if (pipeline->poll_all() != 0) {
        idle_polls = 0;
      } else if (cfg.idle == cb::IdlePolicy::kBackoff && ++idle_polls > 20'000) {
        std::this_thread::sleep_for(std::chrono::microseconds(50));
      }
    }
  });

  cb::PublisherInputs pin;
  pin.config = &cfg;
  pin.snapshots = &pipeline->published();
  for (const cb::VenueId v : cb::kAllVenues) {
    pin.shared[cb::idx(v)] = &pipeline->shared(v);
    if (const auto* p = pipeline->protocol(v)) pin.latency.parse[cb::idx(v)] = &p->parse_histogram();
  }
  pin.latency.stages = &pipeline->engine().stages();
  pin.allocs.book = &book_allocs;
  std::thread publisher([&] {
    cb::set_current_thread_role(cb::ThreadRole::kPublisher, "publisher");
    cb::Publisher pub(pin, g_stop);
    if (!pub.run()) g_stop.store(true);
  });

  while (!g_stop.load()) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    static int tick = 0;
    if (++tick % 10 != 0) continue;
    std::string line;
    for (const cb::VenueId v : cb::kAllVenues) {
      const auto& s = pipeline->shared(v);
      line += std::string(cb::venue_name(v)) + " frames=" +
              std::to_string(s.frames.load(std::memory_order_relaxed)) + " connects=" +
              std::to_string(s.connects.load(std::memory_order_relaxed)) + "  ";
    }
    CB_INFO("%s book_allocs=%llu", line.c_str(),
            static_cast<unsigned long long>(book_allocs.load(std::memory_order_relaxed)));
  }

  CB_INFO("shutting down");
  for (auto& t : threads) t.join();
  book.join();
  publisher.join();
  if (recorder) recorder->close();
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!parse_args(argc, argv, args)) return usage();
  const auto cfg = load(args);
  if (!cfg) return 1;
  return args.mode == "live" ? run_live(args, *cfg) : run_replay(args, *cfg);
}
