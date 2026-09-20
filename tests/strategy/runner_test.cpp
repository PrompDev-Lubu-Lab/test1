// Integration test: replay engine + simulated exchange + risk gate +
// portfolio + strategy runner, with a scripted strategy.

#include "tradebot/strategy/runner.hpp"

#include "tradebot/execution/simulated_exchange.hpp"
#include "tradebot/risk/risk_manager.hpp"

#include <doctest/doctest.h>

using namespace tradebot;
using namespace tradebot::market_data;
using namespace tradebot::execution;
using namespace tradebot::strategy;
using namespace tradebot::literals;

namespace {

const InstrumentId kEth{1};
const Timestamp kT0 = *Timestamp::parse_iso8601("2024-03-15T10:00:00Z");

Instrument eth() {
    return Instrument{.id = kEth, .venue = VenueId{1}, .symbol = "ETHUSDT", .base = "ETH",
                      .quote = "USDT", .tick_size = "0.01"_px, .lot_size = "0.0001"_qty,
                      .min_quantity = "0.0001"_qty, .min_notional = "5"_ntl};
}

BookSnapshot snapshot(Duration at) {
    BookSnapshot s;
    s.instrument = kEth;
    s.recv_time = kT0 + at;
    s.last_update_id = 100;
    s.bids = {{"3000.00"_px, "5"_qty}, {"2999.00"_px, "5"_qty}};
    s.asks = {{"3001.00"_px, "5"_qty}, {"3002.00"_px, "5"_qty}};
    return s;
}

Trade trade(Duration at, const char* price, std::uint64_t id) {
    Trade t;
    t.instrument = kEth;
    t.exchange_time = kT0 + at;
    t.recv_time = kT0 + at;
    t.id = TradeId{id};
    t.price = *Price::parse(price);
    t.quantity = "0.5"_qty;
    t.aggressor = Side::buy;
    return t;
}

// Buys once the book is synced, records everything it sees, sells on a timer.
class ScriptedStrategy final : public Strategy {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "scripted"; }
    void on_start(StrategyContext& ctx) override {
        Strategy::on_start(ctx);
        ctx.request_candles(Duration::minutes(1));
        ctx.log().info("started with param x={}", *ctx.params().get_int_or("x", 0));
        ctx.schedule_at(kT0 + Duration::seconds(30), [this](Timestamp) {
            auto id = this->ctx().sell_market("0.5"_qty);
            CHECK(id.has_value());
        });
        started_at = ctx.now();
        timer_id = ctx.schedule_every(Duration::seconds(10), [this](Timestamp t) { timer_fires.push_back(t); });
    }
    void on_book_update() override {
        ++book_updates;
        if (!bought && ctx().book_synced()) {
            bought = true;
            auto id = ctx().buy_limit("1"_qty, "3001"_px);  // marketable: takes 1 @ 3001
            CHECK(id.has_value());
            first_order = *id;
            ctx().metric("bought_at", ctx().book().best_ask()->price.to_double());
        }
    }
    void on_trade(const Trade& t) override { trades.push_back(t.price); }
    void on_candle(const Candle& c) override { candles.push_back(c); }
    void on_execution_report(const ExecutionReport& r) override {
        reports.push_back(r);
        if (r.type == ReportType::fill) {
            position_seen_at_fill.push_back(ctx().position());
        }
    }
    void on_stop() override {
        stopped = true;
        ctx().cancel_timer(timer_id);
    }

    bool bought = false;
    bool stopped = false;
    ClientOrderId first_order;
    Timestamp started_at;
    TimerId timer_id = 0;
    int book_updates = 0;
    std::vector<Price> trades;
    std::vector<Candle> candles;
    std::vector<ExecutionReport> reports;
    std::vector<Quantity> position_seen_at_fill;
    std::vector<Timestamp> timer_fires;
};

}  // namespace

TEST_CASE("StrategyRunner: full stack wiring with a scripted strategy") {
    std::vector<MarketEvent> events{snapshot(Duration::seconds(1))};
    for (int i = 0; i < 100; ++i) {
        events.push_back(trade(Duration::seconds(2 + i), i % 2 ? "3001" : "3000", static_cast<std::uint64_t>(i + 1)));
    }
    replay::VectorEventSource source(events);
    SimClock clock(kT0);
    replay::ConstantLatency latency(Duration::millis(50), Duration::millis(20), Duration::millis(20));
    replay::ReplayEngine engine(source, clock, latency, {.seed = 3});

    SimulatedExchange exchange(eth(), engine, latency, engine.rng());
    engine.venue_bus().subscribe(exchange);

    portfolio::Portfolio pf("10000"_ntl);
    engine.bus().subscribe(pf);

    risk::RiskLimits limits;
    limits.max_position = "2"_qty;
    limits.max_price_deviation = 0;
    risk::RiskManager risk(exchange, pf, clock, limits);

    auto sink = std::make_shared<MemorySink>();
    Logger log = Logger::make("test", sink, clock, LogLevel::trace);
    StrategyRunner runner(engine, risk, pf, eth(), log);
    engine.bus().subscribe(runner);

    Config params = *Config::parse("x = 42");
    auto strat = std::make_unique<ScriptedStrategy>();
    ScriptedStrategy* s = strat.get();
    const StrategyId id = runner.add(std::move(strat), params, "scripted-1");
    CHECK(id == StrategyId{1});
    CHECK(runner.label(id) == "scripted-1");
    CHECK(runner.strategy(id) == s);

    runner.start();
    REQUIRE(engine.run().has_value());
    runner.stop();

    CHECK(s->stopped);
    CHECK(s->bought);
    CHECK(s->trades.size() == 100);
    CHECK(s->book_updates >= 1);
    // Bought 1 @ 3001 (taker), sold 0.5 at t=30s (market, hits bid 3000).
    REQUIRE(s->reports.size() >= 4);
    CHECK(s->reports[0].type == ReportType::accepted);
    CHECK(s->reports[0].client_id == s->first_order);
    CHECK(s->reports[1].type == ReportType::fill);
    CHECK(s->reports[1].fill->price == "3001"_px);
    CHECK(s->reports[1].fill->quantity == "1"_qty);
    // Position visible to the strategy at the moment of the fill report.
    REQUIRE(s->position_seen_at_fill.size() == 2);
    CHECK(s->position_seen_at_fill[0] == "1"_qty);
    CHECK(s->position_seen_at_fill[1] == "0.5"_qty);
    CHECK(pf.position(id, kEth) == "0.5"_qty);
    CHECK(pf.position(kEth) == "0.5"_qty);
    CHECK(pf.stats().fills == 2);

    // Candles built from trades: 100 trades over 100s starting at 10:00:02 => 10:00 closed, 10:01 closed at stop.
    CHECK(s->candles.size() >= 1);
    CHECK(s->candles[0].open_time == kT0);
    CHECK(s->candles[0].closed);
    CHECK(s->candles[0].trade_count == 58);  // trades at 2..59s

    // Periodic timer fired at 10:00:10, :20, ... while the source lasted.
    CHECK(s->timer_fires.size() >= 9);
    CHECK(s->timer_fires[0] == kT0 + Duration::seconds(10));

    // Metrics and logs captured.
    REQUIRE(runner.metrics().size() == 1);
    CHECK(runner.metrics()[0].name == "bought_at");
    CHECK(runner.metrics()[0].value == doctest::Approx(3001.0));
    CHECK(runner.metrics()[0].strategy == id);
    bool logged = false;
    for (const auto& e : sink->entries()) {
        if (e.component == "test.scripted-1" && e.message.find("x=42") != std::string::npos) logged = true;
    }
    CHECK(logged);
    CHECK(runner.stats().orders_submitted == 2);
    CHECK(runner.stats().reports == 4);
    CHECK(risk.stats().checked == 2);
    CHECK(risk.stats().rejected == 0);
}

TEST_CASE("StrategyRunner: risk rejection reaches the strategy as a report; cancel ownership") {
    std::vector<MarketEvent> events{snapshot(Duration::seconds(1)), trade(Duration::seconds(5), "3000", 1)};
    replay::VectorEventSource source(events);
    SimClock clock(kT0);
    replay::ZeroLatency latency;
    replay::ReplayEngine engine(source, clock, latency);
    SimulatedExchange exchange(eth(), engine, latency, engine.rng());
    engine.venue_bus().subscribe(exchange);
    portfolio::Portfolio pf("10000"_ntl);
    engine.bus().subscribe(pf);
    risk::RiskLimits limits;
    limits.max_order_quantity = "0.5"_qty;
    limits.max_price_deviation = 0;
    risk::RiskManager risk(exchange, pf, clock, limits);
    StrategyRunner runner(engine, risk, pf, eth(), Logger{});
    engine.bus().subscribe(runner);

    class Rejected final : public Strategy {
    public:
        [[nodiscard]] std::string_view name() const noexcept override { return "rejected"; }
        void on_book_update() override {
            if (done) return;
            done = true;
            auto big = ctx().buy_limit("1"_qty, "2990"_px);
            CHECK(big.has_value());
            auto ok = ctx().buy_limit("0.5"_qty, "2990"_px);
            CHECK(ok.has_value());
            resting = *ok;
            // Pending at the venue already counts as open (it occupies risk).
            CHECK(ctx().open_orders().size() == 1);
            CHECK_FALSE(ctx().cancel(ClientOrderId{999}).has_value());
        }
        void on_execution_report(const ExecutionReport& r) override {
            reports.push_back(r);
            if (r.type == ReportType::accepted) {
                CHECK(ctx().open_orders().size() == 1);
                ctx().cancel_all();
            }
        }
        bool done = false;
        ClientOrderId resting;
        std::vector<ExecutionReport> reports;
    };
    auto strat = std::make_unique<Rejected>();
    Rejected* s = strat.get();
    runner.add(std::move(strat), Config{});
    runner.start();
    REQUIRE(engine.run().has_value());
    runner.stop();
    REQUIRE(s->reports.size() == 3);
    CHECK(s->reports[0].type == ReportType::rejected);
    CHECK(s->reports[0].reason.find("risk:") == 0);
    CHECK(s->reports[1].type == ReportType::accepted);
    CHECK(s->reports[2].type == ReportType::cancelled);
    CHECK(s->reports[2].client_id == s->resting);
    CHECK(pf.open_exposure().open_orders == 0);
}
