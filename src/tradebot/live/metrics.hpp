#pragma once

// Operational metrics of a running TradingRuntime, exported in two forms:
// Prometheus text exposition (for node_exporter's textfile collector or a
// sidecar that serves the file) and JSON (for humans and tradebot-monitor).
// Both are rewritten atomically on the heartbeat cadence, so a scrape never
// sees a torn file.

#include "tradebot/core/fixed_point.hpp"
#include "tradebot/core/time.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace tradebot::live {

struct GatewayMetrics {
    std::uint64_t sent = 0;
    std::uint64_t send_failures = 0;
    std::uint64_t cancels_sent = 0;
    std::uint64_t stream_events = 0;
    std::uint64_t duplicates = 0;
    std::uint64_t late_fills = 0;
    std::uint64_t queries = 0;
    std::uint64_t open_orders = 0;
    std::uint64_t reconciliations = 0;
    std::uint64_t reconcile_mismatches = 0;
    std::uint64_t user_stream_connects = 0;
};

struct MetricsSnapshot {
    Timestamp time;
    std::string mode;
    std::string label;
    Duration uptime;
    // Portfolio
    Notional equity;
    Notional cash;
    Notional peak_equity;
    Notional drawdown;
    Notional realized_pnl_net;
    Notional fees;
    Quantity position;
    std::optional<Price> mark;
    std::uint64_t fills = 0;
    // Orders and risk
    std::uint64_t orders = 0;
    std::uint64_t rejected = 0;
    std::uint64_t open_orders = 0;
    std::uint64_t kill_switch_trips = 0;
    bool tripped = false;
    bool halted = false;
    // Feed
    std::string feed_state;
    Duration feed_silence;
    std::uint64_t feed_events = 0;
    std::uint64_t feed_messages = 0;
    std::uint64_t feed_reconnects = 0;
    std::uint64_t feed_depth_gaps = 0;
    std::uint64_t feed_stale_episodes = 0;
    // Runtime
    std::uint64_t dispatched_events = 0;
    std::uint64_t journal_appended = 0;
    std::optional<GatewayMetrics> gateway;
};

// Prometheus text format; every series carries mode and label.
[[nodiscard]] std::string to_prometheus(const MetricsSnapshot& m, std::string_view prefix = "tradebot");
[[nodiscard]] std::string to_json(const MetricsSnapshot& m);

// Writes <dir>/metrics.prom and <dir>/status.json atomically.
[[nodiscard]] Result<void> write_metrics(const std::filesystem::path& dir, const MetricsSnapshot& m);

}  // namespace tradebot::live
