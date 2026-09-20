#include "tradebot/risk/risk_manager.hpp"

#include "support/fake_venue.hpp"

#include <doctest/doctest.h>

using namespace tradebot;
using namespace tradebot::execution;
using namespace tradebot::risk;
using namespace tradebot::literals;

namespace {

const InstrumentId kEth{1};
const StrategyId kS{1};
const Timestamp kT0 = *Timestamp::parse_iso8601("2024-03-15T10:00:00Z");

struct Reports final : ExecutionListener {
    std::vector<ExecutionReport> all;
    void on_execution_report(const ExecutionReport& r) override { all.push_back(r); }
    [[nodiscard]] const ExecutionReport& last() const { return all.back(); }
};

OrderRequest limit(std::uint64_t id, Side side, const char* price, const char* qty) {
    return OrderRequest{ClientOrderId{id}, kEth, kS, side, OrderType::limit, TimeInForce::gtc,
                        *Price::parse(price), *Quantity::parse(qty)};
}

OrderRequest market(std::uint64_t id, Side side, const char* qty) {
    return OrderRequest{ClientOrderId{id}, kEth, kS, side, OrderType::market, TimeInForce::ioc,
                        Price{}, *Quantity::parse(qty)};
}

struct Fixture {
    test::FakeVenue venue;
    portfolio::Portfolio pf{"10000"_ntl};
    SimClock clock{kT0};
    RiskLimits limits;
    std::unique_ptr<RiskManager> risk;
    Reports reports;

    explicit Fixture(RiskLimits l) : limits(std::move(l)) {
        risk = std::make_unique<RiskManager>(venue, pf, clock, limits);
        // Reports flow venue -> risk -> (portfolio, strategy). Chain them here.
        risk->set_listener(&reports);
        pf.set_mark(kEth, "3000"_px);
    }
    // Forward reports to the portfolio as the runner will.
    void sync_portfolio() {
        for (const auto& r : reports.all) pf.on_execution_report(r);
        reports.all.clear();
    }
};

}  // namespace

TEST_CASE("RiskManager: per-order limits") {
    RiskLimits l;
    l.max_order_quantity = "2"_qty;
    l.max_order_notional = "5000"_ntl;
    l.max_price_deviation = 0.02;
    Fixture f(l);

    REQUIRE(f.risk->submit(limit(1, Side::buy, "2990", "1")).has_value());
    CHECK(f.venue.submitted.size() == 1);
    CHECK(f.reports.last().type == ReportType::accepted);

    REQUIRE(f.risk->submit(limit(2, Side::buy, "2990", "3")).has_value());  // quantity
    CHECK(f.venue.submitted.size() == 1);
    CHECK(f.reports.last().type == ReportType::rejected);
    CHECK(f.reports.last().reason.find("max_order_quantity") != std::string::npos);
    CHECK(f.reports.last().client_id == ClientOrderId{2});

    REQUIRE(f.risk->submit(limit(3, Side::buy, "2990", "1.7")).has_value());  // notional 5083
    CHECK(f.reports.last().reason.find("max_order_notional") != std::string::npos);

    REQUIRE(f.risk->submit(limit(4, Side::buy, "2900", "1")).has_value());  // 3.3% below mark
    CHECK(f.reports.last().reason.find("far from mark") != std::string::npos);

    REQUIRE(f.risk->submit(market(5, Side::buy, "1")).has_value());  // market: no band check
    CHECK(f.reports.last().type == ReportType::accepted);

    REQUIRE(f.risk->submit(limit(6, Side::buy, "0", "1")).has_value());
    CHECK(f.reports.last().reason.find("price must be positive") != std::string::npos);
    REQUIRE(f.risk->submit(limit(7, Side::buy, "2990", "0")).has_value());
    CHECK(f.reports.last().reason.find("quantity must be positive") != std::string::npos);

    l.allowed_instruments = {InstrumentId{2}};
    f.risk->set_limits(l);
    REQUIRE(f.risk->submit(limit(8, Side::buy, "2990", "1")).has_value());
    CHECK(f.reports.last().reason.find("not allowed") != std::string::npos);

    CHECK(f.risk->stats().checked == 8);
    CHECK(f.risk->stats().rejected == 6);
    CHECK(f.risk->stats().rejections_by_reason.size() == 6);
}

TEST_CASE("RiskManager: position projection, shorting, open orders and rate limit") {
    RiskLimits l;
    l.max_position = "3"_qty;
    l.max_open_orders = 2;
    l.max_orders_per_minute = 3;
    l.max_price_deviation = 0;
    Fixture f(l);

    REQUIRE(f.risk->submit(limit(1, Side::buy, "3000", "2")).has_value());
    f.sync_portfolio();  // open buy of 2 counts toward projection
    REQUIRE(f.risk->submit(limit(2, Side::buy, "3000", "1.5")).has_value());  // 0 + 2 + 1.5 > 3
    CHECK(f.reports.last().type == ReportType::rejected);
    CHECK(f.reports.last().reason.find("max_position") != std::string::npos);
    f.sync_portfolio();

    // Fill order 1 -> position 2. Sell 3 would go short.
    f.venue.fill(ClientOrderId{1}, "3000"_px);
    f.sync_portfolio();
    CHECK(f.pf.position(kEth) == "2"_qty);
    REQUIRE(f.risk->submit(limit(3, Side::sell, "3000", "3")).has_value());
    CHECK(f.reports.last().reason.find("shorting") != std::string::npos);
    f.sync_portfolio();
    REQUIRE(f.risk->submit(limit(4, Side::sell, "3000", "1")).has_value());
    CHECK(f.reports.last().type == ReportType::accepted);
    f.sync_portfolio();
    // Another sell of 1.5 with 1 already working: 2 - 1 - 1.5 < 0.
    REQUIRE(f.risk->submit(limit(5, Side::sell, "3000", "1.5")).has_value());
    CHECK(f.reports.last().reason.find("shorting") != std::string::npos);
    f.sync_portfolio();

    // Open orders: order 4 is open; add one more, then a third is refused.
    REQUIRE(f.risk->submit(limit(6, Side::buy, "3000", "0.5")).has_value());
    CHECK(f.reports.last().type == ReportType::accepted);
    f.sync_portfolio();
    REQUIRE(f.risk->submit(limit(7, Side::buy, "3000", "0.1")).has_value());
    CHECK(f.reports.last().reason.find("max_open_orders") != std::string::npos);
    f.sync_portfolio();

    // Rate limit: 3 accepted submits in the window so far (1, 4, 6)? No: counted
    // only when forwarded: 1, 4, 6 => next forwarded attempt is refused.
    REQUIRE(f.risk->cancel(ClientOrderId{6}).has_value());
    f.sync_portfolio();
    REQUIRE(f.risk->submit(limit(8, Side::buy, "3000", "0.1")).has_value());
    CHECK(f.reports.last().reason.find("max_orders_per_minute") != std::string::npos);
    f.clock.advance(Duration::minutes(1));
    REQUIRE(f.risk->submit(limit(9, Side::buy, "3000", "0.1")).has_value());
    CHECK(f.reports.last().type == ReportType::accepted);
}

TEST_CASE("RiskManager: kill switch on drawdown cancels open orders and blocks; reset restores") {
    RiskLimits l;
    l.max_drawdown = "100"_ntl;
    l.max_price_deviation = 0;
    Fixture f(l);
    // Chain venue reports into the portfolio too, as the runner does.
    struct Chain final : ExecutionListener {
        portfolio::Portfolio& pf;
        Reports& reports;
        Chain(portfolio::Portfolio& p, Reports& r) : pf(p), reports(r) {}
        void on_execution_report(const ExecutionReport& r) override {
            pf.on_execution_report(r);
            reports.on_execution_report(r);
        }
    } chain(f.pf, f.reports);
    f.risk->set_listener(&chain);

    REQUIRE(f.risk->submit(limit(1, Side::buy, "3000", "1")).has_value());
    f.venue.fill(ClientOrderId{1}, "3000"_px);
    REQUIRE(f.risk->submit(limit(2, Side::sell, "3200", "0.5")).has_value());  // resting
    CHECK_FALSE(f.risk->tripped());

    // Price falls 150: drawdown 150 > 100.
    f.pf.set_mark(kEth, "2850"_px);
    CHECK(f.risk->check_limits());
    CHECK(f.risk->tripped());
    CHECK(f.risk->trip_reason().find("drawdown") != std::string::npos);
    CHECK(f.venue.cancelled == std::vector<ClientOrderId>{ClientOrderId{2}});
    CHECK(f.risk->stats().kill_switch_trips == 1);

    REQUIRE(f.risk->submit(limit(3, Side::buy, "2850", "0.1")).has_value());
    CHECK(f.reports.last().type == ReportType::rejected);
    CHECK(f.reports.last().reason.find("kill switch") != std::string::npos);
    CHECK(f.venue.submitted.size() == 2);

    f.risk->reset();
    CHECK_FALSE(f.risk->tripped());
    // Still in drawdown: the next submit re-trips immediately.
    REQUIRE(f.risk->submit(limit(4, Side::buy, "2850", "0.1")).has_value());
    CHECK(f.risk->tripped());
    CHECK(f.reports.last().type == ReportType::rejected);
}

TEST_CASE("RiskManager: daily loss limit resets at UTC day boundary") {
    RiskLimits l;
    l.max_daily_loss = "50"_ntl;
    l.max_price_deviation = 0;
    Fixture f(l);
    struct Chain final : ExecutionListener {
        portfolio::Portfolio& pf;
        explicit Chain(portfolio::Portfolio& p) : pf(p) {}
        void on_execution_report(const ExecutionReport& r) override { pf.on_execution_report(r); }
    } chain(f.pf);
    f.risk->set_listener(&chain);

    CHECK_FALSE(f.risk->check_limits());  // establishes day-start equity 10000
    REQUIRE(f.risk->submit(limit(1, Side::buy, "3000", "1")).has_value());
    f.venue.fill(ClientOrderId{1}, "3000"_px);
    f.pf.set_mark(kEth, "2960"_px);  // -40: fine
    CHECK_FALSE(f.risk->check_limits());
    f.pf.set_mark(kEth, "2940"_px);  // -60: trip
    CHECK(f.risk->check_limits());
    CHECK(f.risk->trip_reason().find("daily loss") != std::string::npos);

    // Next day: reset, day-start equity re-based to the current equity.
    f.clock.set(*Timestamp::parse_iso8601("2024-03-16T00:00:01Z"));
    f.risk->reset();
    CHECK_FALSE(f.risk->check_limits());
    f.pf.set_mark(kEth, "2900"_px);  // -40 from the new day start
    CHECK_FALSE(f.risk->check_limits());
}

TEST_CASE("RiskManager: flatten_on_trip closes positions through the venue") {
    RiskLimits l;
    l.max_drawdown = "10"_ntl;
    l.max_price_deviation = 0;
    l.flatten_on_trip = true;
    Fixture f(l);
    struct Chain final : ExecutionListener {
        portfolio::Portfolio& pf;
        explicit Chain(portfolio::Portfolio& p) : pf(p) {}
        void on_execution_report(const ExecutionReport& r) override { pf.on_execution_report(r); }
    } chain(f.pf);
    f.risk->set_listener(&chain);
    REQUIRE(f.risk->submit(limit(1, Side::buy, "3000", "2")).has_value());
    f.venue.fill(ClientOrderId{1}, "3000"_px);
    REQUIRE(f.risk->submit(limit(2, Side::sell, "3100", "1")).has_value());  // resting
    CHECK(f.pf.position(kEth) == "2"_qty);
    f.pf.set_mark(kEth, "2900"_px);
    CHECK(f.risk->check_limits());
    CHECK(f.risk->tripped());
    // Resting order cancelled, then a market sell of the full position sent straight to the venue.
    REQUIRE(f.venue.cancelled.size() == 1);
    REQUIRE(f.venue.submitted.size() == 3);
    const auto& flat = f.venue.submitted.back();
    CHECK(flat.side == Side::sell);
    CHECK(flat.type == OrderType::market);
    CHECK(flat.quantity == "2"_qty);
    CHECK(flat.strategy == kS);
    CHECK(flat.client_id != ClientOrderId{1});
    CHECK(f.risk->stats().flatten_orders == 1);
    f.venue.fill(flat.client_id, "2900"_px);
    CHECK(f.pf.position(kEth).is_zero());
}
