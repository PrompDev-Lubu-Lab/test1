#include "tradebot/gateway/binance_rest.hpp"

#include "support/fake_binance_rest.hpp"

#include <doctest/doctest.h>

using namespace tradebot;
using namespace tradebot::gateway;
using namespace tradebot::literals;

namespace {

struct Fixture {
    test::FakeBinanceRest server;
    net::HttpClient http = *net::HttpClient::create(nullptr, {.timeout = Duration::seconds(5), .proxy = net::ProxyConfig{}});
    SimClock clock{Timestamp::from_millis(1700000000000)};
    BinanceRestClient rest{http, BinanceCredentials{"key", "secret"}, clock, {.base_url = server.base_url()}};
};

execution::OrderRequest limit_order() {
    return execution::OrderRequest{ClientOrderId{42}, InstrumentId{1}, StrategyId{1}, Side::buy, OrderType::limit,
                                   TimeInForce::gtc, "3000.5"_px, "0.25"_qty};
}

}  // namespace

TEST_CASE("BinanceRestClient: signed new order carries key, signature, timestamp and fields") {
    Fixture f;
    f.server.on("POST /api/v3/order", test::FakeBinanceRest::Reply{200, R"({"symbol":"ETHUSDT","orderId":12345,"clientOrderId":"tb42","transactTime":1700000000123})"});
    auto r = f.rest.new_order("ETHUSDT", limit_order());
    REQUIRE_MESSAGE(r.has_value(), r.error().message);
    CHECK((*r)["orderId"] == 12345);
    auto reqs = f.server.requests();
    REQUIRE(reqs.size() == 1);
    CHECK(reqs[0].key_ok);
    CHECK(reqs[0].signature_ok);
    CHECK(reqs[0].params["symbol"] == "ETHUSDT");
    CHECK(reqs[0].params["side"] == "BUY");
    CHECK(reqs[0].params["type"] == "LIMIT");
    CHECK(reqs[0].params["timeInForce"] == "GTC");
    CHECK(reqs[0].params["quantity"] == "0.25");
    CHECK(reqs[0].params["price"] == "3000.5");
    CHECK(reqs[0].params["newClientOrderId"] == "tb42");
    CHECK(reqs[0].params["timestamp"] == "1700000000000");
    CHECK(reqs[0].params["recvWindow"] == "5000");

    auto po = limit_order();
    po.time_in_force = TimeInForce::post_only;
    REQUIRE(f.rest.new_order("ETHUSDT", po).has_value());
    CHECK(f.server.requests()[1].params["type"] == "LIMIT_MAKER");
    CHECK_FALSE(f.server.requests()[1].params.contains("timeInForce"));
    auto mk = limit_order();
    mk.type = OrderType::market;
    REQUIRE(f.rest.new_order("ETHUSDT", mk).has_value());
    CHECK(f.server.requests()[2].params["type"] == "MARKET");
    CHECK_FALSE(f.server.requests()[2].params.contains("price"));
}

TEST_CASE("BinanceRestClient: venue errors are typed; unknown order detection") {
    Fixture f;
    f.server.on("DELETE /api/v3/order", test::FakeBinanceRest::Reply{400, R"({"code":-2011,"msg":"Unknown order sent."})"});
    auto r = f.rest.cancel_order("ETHUSDT", ClientOrderId{7});
    REQUIRE_FALSE(r.has_value());
    auto ve = parse_venue_error(r.error());
    REQUIRE(ve.has_value());
    CHECK(ve->http_status == 400);
    CHECK(ve->code == -2011);
    CHECK(ve->message == "Unknown order sent.");
    CHECK(is_unknown_order(r.error()));
    CHECK(f.server.requests()[0].params["origClientOrderId"] == "tb7");

    f.server.on("GET /api/v3/order", test::FakeBinanceRest::Reply{429, R"({"code":-1003,"msg":"Too many requests."})"});
    auto q = f.rest.query_order("ETHUSDT", ClientOrderId{7});
    REQUIRE_FALSE(q.has_value());
    CHECK(q.error().code == ErrorCode::invalid_state);
    CHECK_FALSE(is_unknown_order(q.error()));
    CHECK_FALSE(parse_venue_error(Error{ErrorCode::io_error, "disk"}).has_value());
    CHECK(*BinanceRestClient::parse_client_id("tb99") == ClientOrderId{99});
    CHECK_FALSE(BinanceRestClient::parse_client_id("x99").has_value());
    CHECK_FALSE(BinanceRestClient::parse_client_id("tb9x").has_value());
}

TEST_CASE("BinanceRestClient: balances, listen key, server time, missing credentials") {
    Fixture f;
    f.server.on("GET /api/v3/account", test::FakeBinanceRest::Reply{200, R"({"balances":[{"asset":"ETH","free":"1.5","locked":"0.5"},{"asset":"USDT","free":"100.25","locked":"0"}]})"});
    auto b = f.rest.balances();
    REQUIRE_MESSAGE(b.has_value(), b.error().message);
    REQUIRE(b->size() == 2);
    CHECK((*b)[0].asset == "ETH");
    CHECK((*b)[0].free == "1.5"_qty);
    CHECK((*b)[0].locked == "0.5"_qty);
    CHECK(f.server.requests().back().params["omitZeroBalances"] == "true");

    f.server.on("POST /api/v3/userDataStream", test::FakeBinanceRest::Reply{200, R"({"listenKey":"abc123"})"});
    auto k = f.rest.create_listen_key();
    REQUIRE(k.has_value());
    CHECK(*k == "abc123");
    CHECK(f.server.requests().back().key_ok);
    CHECK_FALSE(f.server.requests().back().signature_ok);  // keyed, not signed
    f.server.on("PUT /api/v3/userDataStream", test::FakeBinanceRest::Reply{200, "{}"});
    CHECK(f.rest.keepalive_listen_key("abc123").has_value());
    CHECK(f.server.requests().back().params["listenKey"] == "abc123");
    f.server.on("GET /api/v3/time", test::FakeBinanceRest::Reply{200, R"({"serverTime":1700000000500})"});
    CHECK(*f.rest.server_time_ms() == 1700000000500);

    BinanceRestClient anon{f.http, BinanceCredentials{}, f.clock, {.base_url = f.server.base_url()}};
    auto r = anon.new_order("ETHUSDT", limit_order());
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == ErrorCode::invalid_state);
    setenv("TRADEBOT_BINANCE_API_KEY", "k", 1);
    setenv("TRADEBOT_BINANCE_API_SECRET", "s", 1);
    CHECK(credentials_from_env().present());
    unsetenv("TRADEBOT_BINANCE_API_KEY");
    unsetenv("TRADEBOT_BINANCE_API_SECRET");
    CHECK_FALSE(credentials_from_env().present());
}
