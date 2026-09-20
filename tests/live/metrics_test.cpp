#include "tradebot/live/metrics.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

using namespace tradebot;
using namespace tradebot::live;
using namespace tradebot::literals;
namespace fs = std::filesystem;

namespace {

MetricsSnapshot sample() {
    MetricsSnapshot m;
    m.time = *Timestamp::parse_iso8601("2024-03-01T12:00:00Z");
    m.mode = "testnet";
    m.label = "ma \"1h\"";
    m.uptime = Duration::seconds(90);
    m.equity = "10050.5"_ntl;
    m.cash = "9000"_ntl;
    m.peak_equity = "10100"_ntl;
    m.drawdown = "49.5"_ntl;
    m.realized_pnl_net = "30"_ntl;
    m.fees = "2.5"_ntl;
    m.position = "0.35"_qty;
    m.mark = "3001.5"_px;
    m.fills = 7;
    m.orders = 9;
    m.rejected = 1;
    m.open_orders = 1;
    m.kill_switch_trips = 0;
    m.tripped = false;
    m.halted = false;
    m.feed_state = "healthy";
    m.feed_silence = Duration::millis(250);
    m.feed_events = 1234;
    m.feed_messages = 1300;
    m.feed_reconnects = 1;
    m.feed_depth_gaps = 0;
    m.feed_stale_episodes = 0;
    m.dispatched_events = 1234;
    m.journal_appended = 16;
    m.gateway = GatewayMetrics{.sent = 9, .send_failures = 0, .cancels_sent = 1, .stream_events = 8, .duplicates = 1,
                               .late_fills = 0, .queries = 2, .open_orders = 1, .reconciliations = 3,
                               .reconcile_mismatches = 0, .user_stream_connects = 1};
    return m;
}

}  // namespace

TEST_CASE("to_prometheus: exposition format with escaped labels, gauges and counters") {
    const std::string text = to_prometheus(sample());
    CHECK(text.find("# TYPE tradebot_equity gauge\n") != std::string::npos);
    CHECK(text.find("tradebot_equity{mode=\"testnet\",label=\"ma \\\"1h\\\"\"} 10050.5\n") != std::string::npos);
    CHECK(text.find("# TYPE tradebot_fills_total counter\n") != std::string::npos);
    CHECK(text.find("tradebot_fills_total{mode=\"testnet\",label=\"ma \\\"1h\\\"\"} 7\n") != std::string::npos);
    CHECK(text.find("tradebot_uptime_seconds{mode=\"testnet\",label=\"ma \\\"1h\\\"\"} 90.000000\n") != std::string::npos);
    CHECK(text.find("tradebot_feed_healthy{mode=\"testnet\",label=\"ma \\\"1h\\\"\"} 1\n") != std::string::npos);
    CHECK(text.find("tradebot_kill_switch_tripped{mode=\"testnet\",label=\"ma \\\"1h\\\"\"} 0\n") != std::string::npos);
    CHECK(text.find("tradebot_mark_price{") != std::string::npos);
    CHECK(text.find("tradebot_gateway_orders_sent_total{mode=\"testnet\",label=\"ma \\\"1h\\\"\"} 9\n") != std::string::npos);
    CHECK(text.find("tradebot_reconcile_mismatches_total{") != std::string::npos);
    // Every non-comment line is "name{labels} value".
    std::istringstream in(text);
    std::string line;
    std::size_t series = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        ++series;
        CHECK(line.rfind("tradebot_", 0) == 0);
        CHECK(line.find("{mode=") != std::string::npos);
        CHECK(line.find("} ") != std::string::npos);
    }
    CHECK(series >= 35);

    // Without a gateway the gateway series are absent; a different prefix is honoured.
    MetricsSnapshot paper = sample();
    paper.gateway.reset();
    paper.mark.reset();
    const std::string p = to_prometheus(paper, "bot");
    CHECK(p.find("bot_equity{") != std::string::npos);
    CHECK(p.find("gateway") == std::string::npos);
    CHECK(p.find("mark_price") == std::string::npos);
}

TEST_CASE("to_json and write_metrics") {
    const std::string j = to_json(sample());
    CHECK(j.find("\"equity\": \"10050.5\"") != std::string::npos);
    CHECK(j.find("\"state\": \"healthy\"") != std::string::npos);
    CHECK(j.find("\"reconciliations\": 3") != std::string::npos);
    CHECK(j.find("\"time\": \"2024-03-01T12:00:00") != std::string::npos);

    std::random_device rd;
    const fs::path dir = fs::temp_directory_path() / ("tradebot-metrics-" + std::to_string(rd()));
    fs::create_directories(dir);
    REQUIRE(write_metrics(dir, sample()).has_value());
    CHECK(fs::exists(dir / "metrics.prom"));
    CHECK(fs::exists(dir / "status.json"));
    CHECK_FALSE(fs::exists(dir / "metrics.prom.tmp"));
    std::ifstream in(dir / "metrics.prom");
    std::stringstream ss;
    ss << in.rdbuf();
    CHECK(ss.str() == to_prometheus(sample()));
    CHECK_FALSE(write_metrics(dir / "missing" / "deeper", sample()).has_value());
    fs::remove_all(dir);
}
