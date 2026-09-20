#include "tradebot/live/metrics.hpp"

#include "tradebot/core/error.hpp"

#include <nlohmann/json.hpp>

#include <fstream>

namespace tradebot::live {

namespace {

std::string escape_label(std::string_view v) {
    std::string out;
    for (char c : v) {
        if (c == '\\' || c == '"') out.push_back('\\');
        if (c == '\n') {
            out += "\\n";
            continue;
        }
        out.push_back(c);
    }
    return out;
}

Result<void> write_atomic(const std::filesystem::path& file, const std::string& text) {
    const std::filesystem::path tmp = file.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) {
            return make_error(ErrorCode::io_error, "cannot write " + tmp.string());
        }
        out << text;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, file, ec);
    if (ec) {
        return make_error(ErrorCode::io_error, "cannot replace " + file.string() + ": " + ec.message());
    }
    return {};
}

}  // namespace

std::string to_prometheus(const MetricsSnapshot& m, std::string_view prefix) {
    std::string out;
    const std::string labels = "{mode=\"" + escape_label(m.mode) + "\",label=\"" + escape_label(m.label) + "\"}";
    auto gauge = [&](std::string_view name, std::string_view help, const std::string& value) {
        out += "# HELP " + std::string(prefix) + "_" + std::string(name) + " " + std::string(help) + "\n";
        out += "# TYPE " + std::string(prefix) + "_" + std::string(name) + " gauge\n";
        out += std::string(prefix) + "_" + std::string(name) + labels + " " + value + "\n";
    };
    auto counter = [&](std::string_view name, std::string_view help, std::uint64_t value) {
        out += "# HELP " + std::string(prefix) + "_" + std::string(name) + " " + std::string(help) + "\n";
        out += "# TYPE " + std::string(prefix) + "_" + std::string(name) + " counter\n";
        out += std::string(prefix) + "_" + std::string(name) + labels + " " + std::to_string(value) + "\n";
    };
    auto seconds = [](Duration d) { return std::to_string(static_cast<double>(d.count_nanos()) / 1e9); };

    gauge("up", "1 while the runtime writes metrics", "1");
    gauge("uptime_seconds", "seconds since the runtime started", seconds(m.uptime));
    gauge("last_update_timestamp_seconds", "unix time of this snapshot",
          std::to_string(static_cast<double>(m.time.nanos_since_epoch()) / 1e9));
    gauge("equity", "cash plus market value of positions (quote)", m.equity.to_string());
    gauge("cash", "quote balance", m.cash.to_string());
    gauge("peak_equity", "highest equity seen", m.peak_equity.to_string());
    gauge("drawdown", "peak equity minus equity (quote)", m.drawdown.to_string());
    gauge("realized_pnl_net", "realized profit net of fees", m.realized_pnl_net.to_string());
    gauge("fees_total", "fees paid (quote)", m.fees.to_string());
    gauge("position", "net position (base)", m.position.to_string());
    if (m.mark) gauge("mark_price", "last mark price", m.mark->to_string());
    counter("fills_total", "fills applied to the portfolio", m.fills);
    counter("orders_total", "orders submitted by strategies", m.orders);
    counter("orders_rejected_total", "orders rejected by the risk gate or the venue", m.rejected);
    gauge("open_orders", "orders accepted and not yet done", std::to_string(m.open_orders));
    counter("kill_switch_trips_total", "times the kill switch tripped", m.kill_switch_trips);
    gauge("kill_switch_tripped", "1 while the kill switch is tripped", m.tripped ? "1" : "0");
    gauge("halted", "1 while the risk gate refuses new orders (shutdown)", m.halted ? "1" : "0");
    gauge("feed_healthy", "1 while market data is flowing", m.feed_state == "healthy" ? "1" : "0");
    gauge("feed_silence_seconds", "seconds since the last market-data event", seconds(m.feed_silence));
    counter("feed_events_total", "market-data events seen by the feed monitor", m.feed_events);
    counter("feed_messages_total", "raw messages received from the venue", m.feed_messages);
    counter("feed_reconnects_total", "feed reconnections", m.feed_reconnects);
    counter("feed_depth_gaps_total", "order-book sequence gaps (resynced)", m.feed_depth_gaps);
    counter("feed_stale_episodes_total", "times the feed went stale", m.feed_stale_episodes);
    counter("dispatched_events_total", "events dispatched to the strategy stack", m.dispatched_events);
    counter("journal_reports_total", "execution reports journaled", m.journal_appended);
    if (m.gateway) {
        const auto& g = *m.gateway;
        counter("gateway_orders_sent_total", "orders sent to the venue", g.sent);
        counter("gateway_send_failures_total", "order sends with an unknown outcome", g.send_failures);
        counter("gateway_cancels_sent_total", "cancels sent to the venue", g.cancels_sent);
        counter("gateway_stream_events_total", "user-stream execution reports received", g.stream_events);
        counter("gateway_duplicates_total", "execution reports deduplicated", g.duplicates);
        counter("gateway_late_fills_total", "fills after a cancel request", g.late_fills);
        counter("gateway_queries_total", "order status queries", g.queries);
        gauge("gateway_open_orders", "orders the gateway tracks that are not done", std::to_string(g.open_orders));
        counter("reconciliations_total", "balance reconciliations run", g.reconciliations);
        counter("reconcile_mismatches_total", "reconciliations outside tolerance", g.reconcile_mismatches);
        counter("user_stream_connects_total", "user data stream connections", g.user_stream_connects);
    }
    return out;
}

std::string to_json(const MetricsSnapshot& m) {
    nlohmann::json j;
    j["time"] = m.time.to_iso8601();
    j["mode"] = m.mode;
    j["label"] = m.label;
    j["uptime"] = m.uptime.to_string();
    j["equity"] = m.equity.to_string();
    j["cash"] = m.cash.to_string();
    j["peak_equity"] = m.peak_equity.to_string();
    j["drawdown"] = m.drawdown.to_string();
    j["realized_pnl_net"] = m.realized_pnl_net.to_string();
    j["fees"] = m.fees.to_string();
    j["position"] = m.position.to_string();
    if (m.mark) j["mark"] = m.mark->to_string();
    j["fills"] = m.fills;
    j["orders"] = m.orders;
    j["rejected"] = m.rejected;
    j["open_orders"] = m.open_orders;
    j["kill_switch_trips"] = m.kill_switch_trips;
    j["tripped"] = m.tripped;
    j["halted"] = m.halted;
    j["feed"] = {{"state", m.feed_state},
                 {"silence", m.feed_silence.to_string()},
                 {"events", m.feed_events},
                 {"messages", m.feed_messages},
                 {"reconnects", m.feed_reconnects},
                 {"depth_gaps", m.feed_depth_gaps},
                 {"stale_episodes", m.feed_stale_episodes}};
    j["dispatched_events"] = m.dispatched_events;
    j["journal_appended"] = m.journal_appended;
    if (m.gateway) {
        const auto& g = *m.gateway;
        j["gateway"] = {{"sent", g.sent},
                        {"send_failures", g.send_failures},
                        {"cancels_sent", g.cancels_sent},
                        {"stream_events", g.stream_events},
                        {"duplicates", g.duplicates},
                        {"late_fills", g.late_fills},
                        {"queries", g.queries},
                        {"open_orders", g.open_orders},
                        {"reconciliations", g.reconciliations},
                        {"reconcile_mismatches", g.reconcile_mismatches},
                        {"user_stream_connects", g.user_stream_connects}};
    }
    return j.dump(2) + "\n";
}

Result<void> write_metrics(const std::filesystem::path& dir, const MetricsSnapshot& m) {
    if (auto p = write_atomic(dir / "metrics.prom", to_prometheus(m)); !p) return p;
    return write_atomic(dir / "status.json", to_json(m));
}

}  // namespace tradebot::live
