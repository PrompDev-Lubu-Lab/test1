// tradebot-collect: capture a Binance spot symbol's live market data to disk.
//
//   tradebot-collect --config configs/collect.conf [--symbol ETHUSDT] [--data-dir data]
//
// Runs until SIGINT/SIGTERM. See configs/collect.example.conf for options.

#include "tradebot/core/clock.hpp"
#include "tradebot/core/config.hpp"
#include "tradebot/core/log.hpp"
#include "tradebot/market_data/binance/collector.hpp"
#include "tradebot/market_data/raw_capture.hpp"
#include "tradebot/net/tls.hpp"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <string>
#include <thread>

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int) { g_stop.store(true); }

int usage(const char* argv0) {
    std::fprintf(stderr, "usage: %s [--config FILE] [--symbol SYMBOL] [--data-dir DIR] [--log-level L]\n",
                 argv0);
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace tradebot;
    using namespace tradebot::market_data;

    std::string config_path;
    std::string symbol_override;
    std::string data_dir_override;
    std::string log_level_text = "info";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (arg == "--config") {
            const char* v = value();
            if (!v) return usage(argv[0]);
            config_path = v;
        } else if (arg == "--symbol") {
            const char* v = value();
            if (!v) return usage(argv[0]);
            symbol_override = v;
        } else if (arg == "--data-dir") {
            const char* v = value();
            if (!v) return usage(argv[0]);
            data_dir_override = v;
        } else if (arg == "--log-level") {
            const char* v = value();
            if (!v) return usage(argv[0]);
            log_level_text = v;
        } else {
            return usage(argv[0]);
        }
    }

    Config cfg;
    if (!config_path.empty()) {
        auto loaded = Config::load_file(config_path);
        if (!loaded) {
            std::fprintf(stderr, "%s\n", loaded.error().to_string().c_str());
            return 1;
        }
        cfg = *loaded;
    }
    cfg.apply_env_overrides();
    if (!symbol_override.empty()) cfg.set("collector.symbol", symbol_override);
    if (!data_dir_override.empty()) cfg.set("data.dir", data_dir_override);

    auto level = parse_log_level(log_level_text);
    if (!level) {
        std::fprintf(stderr, "%s\n", level.error().to_string().c_str());
        return 1;
    }
    Logger log = Logger::stderr_logger("collect", *level);

    binance::CollectorConfig cc;
    auto get = [&](auto&& result, auto& out) -> bool {
        if (!result) {
            log.error("{}", result.error().to_string());
            return false;
        }
        out = *result;
        return true;
    };
    std::int64_t depth_limit = cc.depth_snapshot_limit;
    if (!get(cfg.get_string_or("collector.symbol", cc.symbol), cc.symbol) ||
        !get(cfg.get_string_or("collector.ws_base", cc.ws_base), cc.ws_base) ||
        !get(cfg.get_string_or("collector.rest_base", cc.rest_base), cc.rest_base) ||
        !get(cfg.get_int_or("collector.depth_snapshot_limit", depth_limit), depth_limit) ||
        !get(cfg.get_duration_or("collector.stale_timeout", cc.stale_timeout), cc.stale_timeout) ||
        !get(cfg.get_duration_or("collector.flush_interval", cc.flush_interval),
             cc.flush_interval) ||
        !get(cfg.get_duration_or("collector.snapshot_interval", cc.snapshot_interval),
             cc.snapshot_interval) ||
        !get(cfg.get_duration_or("collector.max_connection_age", cc.max_connection_age),
             cc.max_connection_age)) {
        return 1;
    }
    cc.depth_snapshot_limit = static_cast<int>(depth_limit);
    if (auto streams = cfg.get_string("collector.streams"); streams) {
        cc.streams.clear();
        std::string cur;
        for (char c : *streams + ',') {
            if (c == ',') {
                if (!cur.empty()) cc.streams.push_back(cur);
                cur.clear();
            } else if (c != ' ') {
                cur.push_back(c);
            }
        }
    }

    std::string data_dir;
    if (!get(cfg.get_string_or("data.dir", "data"), data_dir)) return 1;
    std::string ca_file;
    if (!get(cfg.get_string_or("tls.ca_file", ""), ca_file)) return 1;

    auto tls = net::TlsContext::create({.ca_file = ca_file});
    if (!tls) {
        log.error("{}", tls.error().to_string());
        return 1;
    }

    WallClock clock;
    RawCaptureWriter writer(RawCapturePath{data_dir, "binance", cc.symbol});
    binance::Collector collector(cc, writer, *tls, clock, log.child("binance"));

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    log.info("capturing {} to {} (streams: {})", cc.symbol, writer.current_file().empty()
                                                                  ? data_dir
                                                                  : writer.current_file().string(),
             cc.streams.size());

    std::thread reporter([&] {
        std::uint64_t last_messages = 0;
        int ticks = 0;
        while (!g_stop.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (++ticks % 60 != 0) continue;
            const auto& s = collector.stats();
            log.info("stats: msgs={} ({}/min) bytes={} connects={} reconnects={} snapshots={} gaps={} stale={}",
                     s.messages, s.messages - last_messages, s.bytes, s.connects, s.reconnects,
                     s.depth_snapshots, s.depth_gaps, s.stale_timeouts);
            last_messages = s.messages;
        }
    });

    auto r = collector.run(g_stop);
    g_stop.store(true);
    reporter.join();
    if (auto c = writer.close(); !c) {
        log.error("{}", c.error().to_string());
    }
    if (!r) {
        log.error("collector stopped: {}", r.error().to_string());
        return 1;
    }
    log.info("stopped cleanly after {} messages", collector.stats().messages);
    return 0;
}
