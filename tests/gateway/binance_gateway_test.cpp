#include "tradebot/gateway/binance_gateway.hpp"

#include "support/fake_binance_rest.hpp"
#include "tradebot/live/live_scheduler.hpp"

#include <doctest/doctest.h>

#include <thread>

using namespace tradebot;
using namespace tradebot::gateway;
using namespace tradebot::execution;
using namespace tradebot::literals;

namespace {

const InstrumentId kEth{1};

Instrument eth() {
    return Instrument{.id = kEth, .venue = VenueId{1}, .symbol = "ETHUSDT", .base = "ETH", .quote = "USDT",
                      .tick_size = "0.01"_px, .lot_size = "0.0001"_qty, .min_quantity = "0.0001"_qty, .min_notional = "5"_ntl};
}

struct Reports final : ExecutionListener {
    std::vector<ExecutionReport> all;
    void on_execution_report(const ExecutionReport& r) override { all.push_back(r); }
    [[nodiscard]] std::vector<ReportType> types() const {
        std::vector<ReportType> out;
        for (const auto& r : all) out.push_back(r.type);
        return out;
    }
};

// Drives the gateway on a LiveScheduler dispatch loop; posts marshal there.
struct Fixture {
    test::FakeBinanceRest server;
    net::HttpClient http = *net::HttpClient::create(nullptr, {.timeout = Duration::seconds(5), .proxy = net::ProxyConfig{}});
    WallClock clock;
    live::LiveScheduler sched{clock};
    BinanceRestClient rest{http, BinanceCredentials{"key", "secret"}, clock, {.base_url = server.base_url()}};
    Reports reports;
    std::unique_ptr<BinanceGateway> gw;

    explicit Fixture(GatewayOptions opts = {}) {
        gw = std::make_unique<BinanceGateway>(rest, eth(), clock, [this](std::function<void()> fn) { sched.post(std::move(fn)); },
                                              opts, Logger{});
        gw->set_listener(&reports);
    }
    // Pumps the dispatch loop until the predicate holds or a timeout passes.
    bool pump_until(const std::function<bool()>& done, int max_ms = 3000) {
        for (int i = 0; i < max_ms / 10; ++i) {
            sched.run_once(Duration::millis(10));
            if (done()) return true;
        }
        return done();
    }
    OrderRequest limit(std::uint64_t id, Side side, const char* price, const char* qty) {
        return OrderRequest{ClientOrderId{id}, kEth, StrategyId{1}, side, OrderType::limit, TimeInForce::gtc,
                            *Price::parse(price), *Quantity::parse(qty)};
    }
    ParsedExecution stream(std::uint64_t cid, const char* x, const char* X, const char* cum, const char* last_qty,
                           const char* last_px, std::int64_t trade_id, std::uint64_t order_id = 500) {
        const std::string json = std::string(R"({"e":"executionReport","E":1,"s":"ETHUSDT","c":"tb)") + std::to_string(cid) +
                                 R"(","S":"BUY","o":"LIMIT","f":"GTC","q":"1","p":"3000","x":")" + x + R"(","X":")" + X +
                                 R"(","r":"NONE","i":)" + std::to_string(order_id) + R"(,"l":")" + last_qty + R"(","z":")" + cum +
                                 R"(","L":")" + last_px + R"(","n":"0.001","T":5,"t":)" + std::to_string(trade_id) + R"(,"m":true})";
        auto parsed = parse_user_event(json, kEth);
        REQUIRE(parsed.has_value());
        REQUIRE(parsed->has_value());
        return **parsed;
    }
};

}  // namespace

TEST_CASE("BinanceGateway: submit -> REST ack -> stream fills; duplicates ignored") {
    Fixture f;
    f.server.on("POST /api/v3/order", test::FakeBinanceRest::Reply{200, R"({"orderId":500,"transactTime":1700000000000})"});
    REQUIRE(f.gw->submit(f.limit(1, Side::buy, "3000", "1")).has_value());
    CHECK(f.gw->order(ClientOrderId{1})->status == OrderStatus::pending_new);
    REQUIRE(f.pump_until([&] { return !f.reports.all.empty(); }));
    CHECK(f.reports.all[0].type == ReportType::accepted);
    CHECK(f.reports.all[0].order_id == OrderId{500});
    CHECK(f.reports.all[0].strategy == StrategyId{1});
    CHECK(f.gw->order(ClientOrderId{1})->status == OrderStatus::open);

    // The stream also says NEW (duplicate ack), then two trades, one redelivered.
    f.gw->on_stream_execution(f.stream(1, "NEW", "NEW", "0", "0", "0", -1));
    f.gw->on_stream_execution(f.stream(1, "TRADE", "PARTIALLY_FILLED", "0.4", "0.4", "2999.9", 11));
    f.gw->on_stream_execution(f.stream(1, "TRADE", "PARTIALLY_FILLED", "0.4", "0.4", "2999.9", 11));
    f.gw->on_stream_execution(f.stream(1, "TRADE", "FILLED", "1", "0.6", "3000", 12));
    REQUIRE(f.pump_until([&] { return f.reports.all.size() >= 3; }));
    auto types = f.reports.types();
    REQUIRE(types.size() == 3);
    CHECK(types[1] == ReportType::fill);
    CHECK(types[2] == ReportType::fill);
    CHECK(f.reports.all[1].fill->exec_id == TradeId{11});
    CHECK(f.reports.all[1].filled_quantity == "0.4"_qty);
    CHECK(f.reports.all[2].filled_quantity == "1"_qty);
    CHECK(f.reports.all[2].status == OrderStatus::filled);
    CHECK(f.gw->order(ClientOrderId{1})->is_done());
    CHECK(f.gw->order(ClientOrderId{1})->fees == "0.002"_ntl);
    CHECK(f.gw->stats().duplicates == 2);
    CHECK(f.gw->stats().sent == 1);
    CHECK(f.gw->stats().stream_events == 4);
    CHECK_FALSE(f.gw->cancel(ClientOrderId{1}).has_value());  // done
    CHECK_FALSE(f.gw->submit(f.limit(1, Side::buy, "3000", "1")).has_value());  // duplicate id
    CHECK_FALSE(f.gw->submit(f.limit(2, Side::buy, "3000.001", "1")).has_value());  // tick
}

TEST_CASE("BinanceGateway: venue rejection, cancel paths and late fill after cancel") {
    Fixture f;
    f.server.on("POST /api/v3/order", [](const test::FakeBinanceRest::Request& r) {
        if (r.params.at("newClientOrderId") == "tb9") {
            return test::FakeBinanceRest::Reply{400, R"({"code":-2010,"msg":"Account has insufficient balance"})"};
        }
        return test::FakeBinanceRest::Reply{200, R"({"orderId":600,"transactTime":1})"};
    });
    REQUIRE(f.gw->submit(f.limit(9, Side::buy, "3000", "1")).has_value());
    REQUIRE(f.pump_until([&] { return !f.reports.all.empty(); }));
    CHECK(f.reports.all[0].type == ReportType::rejected);
    CHECK(f.reports.all[0].reason.find("insufficient") != std::string::npos);
    CHECK(f.gw->order(ClientOrderId{9})->status == OrderStatus::rejected);

    // Order 10: accepted, cancel sent, but a fill arrives before the cancel confirms.
    REQUIRE(f.gw->submit(f.limit(10, Side::buy, "3000", "1")).has_value());
    REQUIRE(f.pump_until([&] { return f.reports.all.size() >= 2; }));
    f.server.on("DELETE /api/v3/order", test::FakeBinanceRest::Reply{200, R"({"orderId":600,"status":"CANCELED"})"});
    REQUIRE(f.gw->cancel(ClientOrderId{10}).has_value());
    CHECK(f.gw->order(ClientOrderId{10})->status == OrderStatus::pending_cancel);
    f.gw->on_stream_execution(f.stream(10, "TRADE", "PARTIALLY_FILLED", "0.3", "0.3", "3000", 21, 600));
    REQUIRE(f.pump_until([&] { return f.reports.all.size() >= 4; }));
    auto types = f.reports.types();
    // accepted(10), then fill and cancelled in some order, each once.
    int fills = 0, cancels = 0;
    for (std::size_t i = 2; i < types.size(); ++i) {
        fills += types[i] == ReportType::fill;
        cancels += types[i] == ReportType::cancelled;
    }
    CHECK(fills == 1);
    CHECK(cancels == 1);
    CHECK(f.gw->stats().late_fills == 1);
    const auto st = *f.gw->order(ClientOrderId{10});
    CHECK(st.status == OrderStatus::cancelled);
    CHECK(st.filled_quantity == "0.3"_qty);
    // Stream CANCELED afterwards is a duplicate.
    f.gw->on_stream_execution(f.stream(10, "CANCELED", "CANCELED", "0.3", "0", "0", -1, 600));
    f.pump_until([&] { return f.gw->stats().stream_events >= 2; });
    CHECK(f.reports.all.size() == 4);

    // Order 11: cancel of an already-gone order => cancel_rejected then query settles it as filled.
    REQUIRE(f.gw->submit(f.limit(11, Side::buy, "3000", "1")).has_value());
    REQUIRE(f.pump_until([&] { return f.reports.all.size() >= 5; }));
    f.server.on("DELETE /api/v3/order", test::FakeBinanceRest::Reply{400, R"({"code":-2011,"msg":"Unknown order sent."})"});
    f.server.on("GET /api/v3/order", test::FakeBinanceRest::Reply{200, R"({"orderId":600,"status":"FILLED","executedQty":"1","cummulativeQuoteQty":"3000.5"})"});
    REQUIRE(f.gw->cancel(ClientOrderId{11}).has_value());
    REQUIRE(f.pump_until([&] { return f.gw->order(ClientOrderId{11})->is_done(); }));
    types = f.reports.types();
    CHECK(types[types.size() - 2] == ReportType::cancel_rejected);
    CHECK(types.back() == ReportType::fill);
    CHECK(f.reports.all.back().fill->exec_id == TradeId{0});  // synthesized from the query
    CHECK(f.reports.all.back().fill->price == "3000.5"_px);
    CHECK(f.gw->order(ClientOrderId{11})->status == OrderStatus::filled);
}

TEST_CASE("BinanceGateway: a fill that lands after the cancel confirmation keeps the order cancelled") {
    // The REST cancel reply (worker thread) and a stream fill race for the
    // dispatch loop. Here the cancel confirmation lands first, so the order is
    // already terminal when the late fill arrives: the quantities update, the
    // status stays cancelled, and the fill is counted as late.
    Fixture f;
    f.server.on("POST /api/v3/order", test::FakeBinanceRest::Reply{200, R"({"orderId":700,"transactTime":1})"});
    f.server.on("DELETE /api/v3/order", test::FakeBinanceRest::Reply{200, R"({"orderId":700,"status":"CANCELED"})"});
    REQUIRE(f.gw->submit(f.limit(12, Side::buy, "3000", "1")).has_value());
    REQUIRE(f.pump_until([&] { return !f.reports.all.empty(); }));
    REQUIRE(f.gw->cancel(ClientOrderId{12}).has_value());
    REQUIRE(f.pump_until([&] { return f.gw->order(ClientOrderId{12})->status == OrderStatus::cancelled; }));
    REQUIRE(f.reports.all.size() == 2);
    CHECK(f.reports.all[1].type == ReportType::cancelled);

    f.gw->on_stream_execution(f.stream(12, "TRADE", "PARTIALLY_FILLED", "0.3", "0.3", "3000", 31, 700));
    REQUIRE(f.pump_until([&] { return f.reports.all.size() >= 3; }));
    const auto& late = f.reports.all.back();
    CHECK(late.type == ReportType::fill);
    CHECK(late.status == OrderStatus::cancelled);
    CHECK(late.filled_quantity == "0.3"_qty);
    CHECK(late.remaining_quantity == "0.7"_qty);
    CHECK(f.gw->stats().late_fills == 1);
    const auto st = *f.gw->order(ClientOrderId{12});
    CHECK(st.status == OrderStatus::cancelled);
    CHECK(st.is_done());
    CHECK(st.filled_quantity == "0.3"_qty);
    CHECK(st.fees == "0.001"_ntl);
    CHECK_FALSE(f.gw->cancel(ClientOrderId{12}).has_value());  // done

    // The stream's own CANCELED afterwards is a duplicate of the REST confirmation.
    f.gw->on_stream_execution(f.stream(12, "CANCELED", "CANCELED", "0.3", "0", "0", -1, 700));
    REQUIRE(f.pump_until([&] { return f.gw->stats().stream_events >= 2; }));
    CHECK(f.reports.all.size() == 3);
    CHECK(f.gw->stats().duplicates == 1);
    CHECK(f.gw->order(ClientOrderId{12})->status == OrderStatus::cancelled);
}

TEST_CASE("BinanceGateway: a cancel confirmation after a full fill keeps the order filled") {
    // First terminal state wins in the other direction too: the venue cannot
    // cancel what it has already filled, so a stale CANCELED never un-fills.
    Fixture f;
    f.server.on("POST /api/v3/order", test::FakeBinanceRest::Reply{200, R"({"orderId":701,"transactTime":1})"});
    REQUIRE(f.gw->submit(f.limit(13, Side::buy, "3000", "1")).has_value());
    REQUIRE(f.pump_until([&] { return !f.reports.all.empty(); }));
    f.gw->on_stream_execution(f.stream(13, "TRADE", "FILLED", "1", "1", "3000", 41, 701));
    REQUIRE(f.pump_until([&] { return f.gw->order(ClientOrderId{13})->status == OrderStatus::filled; }));
    f.gw->on_stream_execution(f.stream(13, "CANCELED", "CANCELED", "1", "0", "0", -1, 701));
    REQUIRE(f.pump_until([&] { return f.gw->stats().stream_events >= 2; }));
    CHECK(f.reports.all.size() == 3);
    CHECK(f.reports.all.back().type == ReportType::cancelled);
    CHECK(f.reports.all.back().status == OrderStatus::filled);
    CHECK(f.gw->order(ClientOrderId{13})->status == OrderStatus::filled);
    CHECK(f.gw->order(ClientOrderId{13})->filled_quantity == "1"_qty);
}

TEST_CASE("BinanceGateway: send failure is queried; unknown after failure => rejected") {
    Fixture f;
    // No POST route => 404 with a venue-style body => treated as a venue rejection.
    REQUIRE(f.gw->submit(f.limit(1, Side::buy, "3000", "1")).has_value());
    REQUIRE(f.pump_until([&] { return !f.reports.all.empty(); }));
    CHECK(f.reports.all[0].type == ReportType::rejected);

    // A transport failure (server closes without a body) leads to a query.
    Fixture g;
    g.server.on("POST /api/v3/order", [](const test::FakeBinanceRest::Request&) {
        return test::FakeBinanceRest::Reply{500, "not json"};
    });
    g.server.on("GET /api/v3/order", test::FakeBinanceRest::Reply{400, R"({"code":-2013,"msg":"Order does not exist."})"});
    REQUIRE(g.gw->submit(g.limit(2, Side::buy, "3000", "1")).has_value());
    REQUIRE(g.pump_until([&] { return !g.reports.all.empty(); }));
    CHECK(g.reports.all[0].type == ReportType::rejected);
    CHECK(g.gw->stats().send_failures == 1);
    CHECK(g.gw->stats().queries == 1);
    // ... and if the query finds it live, it is accepted.
    Fixture h;
    h.server.on("POST /api/v3/order", test::FakeBinanceRest::Reply{502, "gateway down"});
    h.server.on("GET /api/v3/order", test::FakeBinanceRest::Reply{200, R"({"orderId":700,"status":"NEW","executedQty":"0","cummulativeQuoteQty":"0"})"});
    REQUIRE(h.gw->submit(h.limit(3, Side::buy, "3000", "1")).has_value());
    REQUIRE(h.pump_until([&] { return !h.reports.all.empty(); }));
    CHECK(h.reports.all[0].type == ReportType::accepted);
    CHECK(h.reports.all[0].order_id == OrderId{700});
}

TEST_CASE("BinanceGateway: dry run never sends; reconciliation compares balances") {
    Fixture f({.dry_run = true});
    REQUIRE(f.gw->submit(f.limit(1, Side::buy, "3000", "1")).has_value());
    CHECK(f.reports.all.size() == 1);
    CHECK(f.reports.all[0].type == ReportType::rejected);
    CHECK(f.reports.all[0].reason.find("dry run") != std::string::npos);
    CHECK(f.server.requests().empty());

    portfolio::Portfolio pf("1000"_ntl);
    f.server.on("GET /api/v3/account", test::FakeBinanceRest::Reply{200, R"({"balances":[{"asset":"ETH","free":"0.5","locked":"0.25"},{"asset":"USDT","free":"990","locked":"10"}]})"});
    auto rec = f.gw->reconcile(pf, "0.001"_qty, "1"_ntl);
    REQUIRE_MESSAGE(rec.has_value(), rec.error().message);
    CHECK(rec->venue_base == "0.75"_qty);
    CHECK(rec->expected_base.is_zero());
    CHECK(rec->venue_quote == "1000"_ntl);
    CHECK(rec->base_difference == "0.75"_qty);
    CHECK(rec->quote_difference.is_zero());
    CHECK_FALSE(rec->within_tolerance);
    auto ok = f.gw->reconcile(pf, "1"_qty, "1"_ntl);
    REQUIRE(ok.has_value());
    CHECK(ok->within_tolerance);
}
