#include "tradebot/net/url.hpp"

#include <doctest/doctest.h>

using namespace tradebot;
using namespace tradebot::net;

TEST_CASE("Url: parse full and minimal forms") {
    auto u = Url::parse("https://api.binance.com/api/v3/depth?symbol=ETHUSDT&limit=1000");
    REQUIRE(u.has_value());
    CHECK(u->scheme == "https");
    CHECK(u->host == "api.binance.com");
    CHECK(u->port == 443);
    CHECK(u->path == "/api/v3/depth");
    CHECK(u->query == "symbol=ETHUSDT&limit=1000");
    CHECK(u->is_secure());
    CHECK(u->host_header() == "api.binance.com");
    CHECK(u->path_and_query() == "/api/v3/depth?symbol=ETHUSDT&limit=1000");
    CHECK(u->to_string() == "https://api.binance.com/api/v3/depth?symbol=ETHUSDT&limit=1000");

    auto w = Url::parse("wss://stream.binance.com:9443/stream?streams=ethusdt@aggTrade");
    REQUIRE(w.has_value());
    CHECK(w->port == 9443);
    CHECK(w->host_header() == "stream.binance.com:9443");
    CHECK(w->path == "/stream");

    auto m = Url::parse("http://localhost");
    REQUIRE(m.has_value());
    CHECK(m->port == 80);
    CHECK(m->path == "/");
    CHECK_FALSE(m->is_secure());

    auto q = Url::parse("ws://127.0.0.1:8080?x=1");
    REQUIRE(q.has_value());
    CHECK(q->path == "/");
    CHECK(q->query == "x=1");
}

TEST_CASE("Url: rejects malformed input") {
    CHECK_FALSE(Url::parse("api.binance.com/x").has_value());
    CHECK_FALSE(Url::parse("ftp://host/").has_value());
    CHECK_FALSE(Url::parse("https:///path").has_value());
    CHECK_FALSE(Url::parse("https://host:abc/").has_value());
    CHECK_FALSE(Url::parse("https://host:0/").has_value());
    CHECK_FALSE(Url::parse("https://host:70000/").has_value());
    CHECK_FALSE(Url::parse("https://user:pw@host/").has_value());
}
