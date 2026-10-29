#include "engine/config.hpp"

#include <simdjson.h>

namespace cb {

AppConfig::AppConfig() {
  venue(VenueId::kCoinbase).url = "wss://advanced-trade-ws.coinbase.com";
  venue(VenueId::kKraken).url = "wss://ws.kraken.com/v2";
  venue(VenueId::kKraken).depth = 25;
  // Port 443, not the documented :8443 - the latter was TCP-reset from India (DESIGN D9).
  venue(VenueId::kOkx).url = "wss://ws.okx.com/ws/v5/public";
  venue(VenueId::kOkx).depth = 400;
  venue(VenueId::kBybit).url = "wss://stream.bybit.com/v5/public/spot";
  venue(VenueId::kBybit).depth = 50;
}

std::optional<AppConfig> load_config(const std::string& path, std::string& error) {
  AppConfig cfg;
  try {
    simdjson::dom::parser parser;
    const simdjson::dom::element root = parser.load(path);

    auto get_u32 = [](simdjson::dom::object obj, const char* key, std::uint32_t& out) {
      std::uint64_t v = 0;
      if (obj[key].get(v) == simdjson::SUCCESS) {
        out = static_cast<std::uint32_t>(v);
      }
    };

    simdjson::dom::object venues;
    if (root["venues"].get(venues) == simdjson::SUCCESS) {
      for (const VenueId v : kAllVenues) {
        simdjson::dom::object vo;
        if (venues[venue_name(v)].get(vo) != simdjson::SUCCESS) {
          continue;
        }
        VenueSettings& s = cfg.venue(v);
        bool enabled = true;
        if (vo["enabled"].get(enabled) == simdjson::SUCCESS) {
          s.enabled = enabled;
        }
        std::string_view url;
        if (vo["url"].get(url) == simdjson::SUCCESS) {
          s.url = std::string(url);
        }
        get_u32(vo, "depth", s.depth);
        get_u32(vo, "horizon_bp", s.horizon_bp);
        get_u32(vo, "min_levels", s.min_levels);
      }
    }

    simdjson::dom::object obj;
    if (root.get(obj) == simdjson::SUCCESS) {
      get_u32(obj, "consolidated_levels_per_venue", cfg.consolidated_levels_per_venue);
      get_u32(obj, "imbalance_levels", cfg.imbalance_levels);
      get_u32(obj, "publish_hz", cfg.publish_hz);
      std::int64_t cpu = 0;
      if (obj["book_cpu"].get(cpu) == simdjson::SUCCESS) {
        cfg.book_cpu = static_cast<int>(cpu);
      }
      std::string_view idle;
      if (obj["idle"].get(idle) == simdjson::SUCCESS) {
        cfg.idle = idle == "spin" ? IdlePolicy::kSpin : IdlePolicy::kBackoff;
      }
      simdjson::dom::object http;
      if (obj["http"].get(http) == simdjson::SUCCESS) {
        std::string_view bind;
        if (http["bind"].get(bind) == simdjson::SUCCESS) {
          cfg.http_bind = std::string(bind);
        }
        std::uint32_t port = cfg.http_port;
        get_u32(http, "port", port);
        cfg.http_port = static_cast<std::uint16_t>(port);
        std::string_view root_dir;
        if (http["web_root"].get(root_dir) == simdjson::SUCCESS) {
          cfg.web_root = std::string(root_dir);
        }
      }
    }
  } catch (const simdjson::simdjson_error& e) {
    error = std::string("config ") + path + ": " + e.what();
    return std::nullopt;
  }
  if (cfg.consolidated_levels_per_venue == 0 || cfg.consolidated_levels_per_venue > 20) {
    error = "consolidated_levels_per_venue must be 1..20";
    return std::nullopt;
  }
  if (cfg.imbalance_levels == 0 || cfg.imbalance_levels > 20) {
    error = "imbalance_levels must be 1..20";
    return std::nullopt;
  }
  if (cfg.publish_hz == 0 || cfg.publish_hz > 100) {
    error = "publish_hz must be 1..100";
    return std::nullopt;
  }
  return cfg;
}

}  // namespace cb
