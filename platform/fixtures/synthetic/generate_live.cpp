// Writes a synthetic live-surface fixture with the bot's own writers, so
// every file has exactly the field names and encodings a paper instance
// produces. Usage: gen_live OUT_DIR
#include "tradebot/live/metrics.hpp"
#include "tradebot/live/feed_health.hpp"
#include "tradebot/live/journal.hpp"
#include "tradebot/portfolio/portfolio.hpp"
#include <cstdio>
#include <filesystem>
#include <fstream>
using namespace tradebot;
using namespace tradebot::execution;
using namespace tradebot::literals;
namespace fs = std::filesystem;

static ExecutionReport report(ReportType type, std::uint64_t cid, std::uint64_t oid, Side side, OrderType ot,
                              Price price, Timestamp t, OrderStatus st, Quantity filled, Quantity remaining) {
    ExecutionReport r;
    r.type = type; r.client_id = ClientOrderId{cid}; r.order_id = OrderId{oid};
    r.instrument = InstrumentId{1}; r.strategy = StrategyId{1}; r.side = side; r.order_type = ot;
    r.price = price; r.time = t; r.status = st; r.filled_quantity = filled; r.remaining_quantity = remaining;
    return r;
}

int main(int argc, char** argv) {
    if (argc != 2) { std::fprintf(stderr, "usage: gen_live OUT_DIR\n"); return 2; }
    const fs::path out = argv[1];
    fs::create_directories(out);
    const Timestamp start = *Timestamp::parse_iso8601("2026-09-21T00:00:00Z");
    const InstrumentId eth{1};

    // Journal: accepted -> fill (0.5) -> fill (rest) for a buy; accepted -> cancelled for a sell.
    live::Journal journal;
    if (!journal.open(out / "journal.jsonl")) return 1;
    portfolio::Portfolio pf("10000"_ntl);
    auto apply = [&](const ExecutionReport& r) { if (!journal.append(r, true)) std::abort(); pf.on_execution_report(r); };
    Timestamp t = start + Duration::minutes(5);
    apply(report(ReportType::accepted, 1, 1001, Side::buy, OrderType::limit, "3000.00"_px, t, OrderStatus::open, "0"_qty, "1"_qty));
    auto f1 = report(ReportType::fill, 1, 1001, Side::buy, OrderType::limit, "3000.00"_px, t + Duration::seconds(3), OrderStatus::partially_filled, "0.5"_qty, "0.5"_qty);
    f1.fill = Fill{"3000.00"_px, "0.5"_qty, "1.5"_ntl, Liquidity::maker, TradeId{500001}};
    apply(f1);
    auto f2 = report(ReportType::fill, 1, 1001, Side::buy, OrderType::limit, "3000.00"_px, t + Duration::seconds(41), OrderStatus::filled, "1"_qty, "0"_qty);
    f2.fill = Fill{"3000.00"_px, "0.5"_qty, "1.5"_ntl, Liquidity::maker, TradeId{500002}};
    apply(f2);
    Timestamp t2 = start + Duration::minutes(47);
    apply(report(ReportType::accepted, 2, 1002, Side::sell, OrderType::limit, "3040.00"_px, t2, OrderStatus::open, "0"_qty, "1"_qty));
    apply(report(ReportType::cancelled, 2, 1002, Side::sell, OrderType::limit, "3040.00"_px, t2 + Duration::minutes(10), OrderStatus::cancelled, "0"_qty, "1"_qty));
    auto rej = report(ReportType::rejected, 3, 0, Side::buy, OrderType::market, "0"_px, start + Duration::minutes(70), OrderStatus::rejected, "0"_qty, "2"_qty);
    rej.reason = "max_order_notional exceeded";
    apply(rej);
    if (!journal.close()) return 1;

    // state.json from the portfolio after those reports, marked at 3012.50.
    pf.set_mark(eth, "3012.50"_px);
    { std::ofstream s(out / "state.json"); s << pf.state_to_json() << '\n'; }

    // status.json + metrics.prom: healthy/armed.
    live::MetricsSnapshot m;
    m.time = start + Duration::minutes(75); m.mode = "paper"; m.label = "ma_synthetic";
    m.uptime = Duration::minutes(75);
    m.equity = pf.equity(); m.cash = pf.cash(); m.peak_equity = "10015.00"_ntl;
    m.drawdown = "10015.00"_ntl - pf.equity(); m.realized_pnl_net = pf.realized_pnl_net(); m.fees = pf.account().fees;
    m.position = pf.position(eth); m.mark = "3012.50"_px;
    m.fills = 2; m.orders = 3; m.rejected = 1; m.open_orders = 0; m.kill_switch_trips = 0;
    m.tripped = false; m.halted = false;
    m.feed_state = "healthy"; m.feed_silence = Duration::seconds(1); m.feed_events = 48210; m.feed_messages = 51877;
    m.feed_reconnects = 0; m.feed_depth_gaps = 0; m.feed_stale_episodes = 0;
    m.dispatched_events = 48210; m.journal_appended = journal.appended();
    if (!live::write_metrics(out, m)) return 1;
    if (!live::write_heartbeat(out / "heartbeat", m.time, "paper healthy armed")) return 1;

    // A second sample: the feed went stale and the kill switch tripped.
    fs::create_directories(out / "sample-stale-tripped");
    live::MetricsSnapshot s = m;
    s.time = m.time + Duration::minutes(20); s.uptime = s.uptime + Duration::minutes(20);
    s.feed_state = "stale"; s.feed_silence = Duration::seconds(37); s.feed_stale_episodes = 1;
    s.tripped = true; s.kill_switch_trips = 1; s.open_orders = 0;
    if (!live::write_metrics(out / "sample-stale-tripped", s)) return 1;
    if (!live::write_heartbeat(out / "sample-stale-tripped" / "heartbeat", s.time, "paper stale tripped")) return 1;
    std::printf("wrote %s\n", out.c_str());
    return 0;
}
