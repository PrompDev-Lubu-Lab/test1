#include "tradebot/market_data/binance/raw_processor.hpp"

#include <doctest/doctest.h>

using namespace tradebot;
using namespace tradebot::market_data;
using namespace tradebot::market_data::binance;
using namespace tradebot::literals;

TEST_CASE("RawRecordProcessor: routes records by stream name") {
    RawRecordProcessor proc("ETHUSDT", InstrumentId{3}, VenueId{1});
    const Timestamp t0 = *Timestamp::parse_iso8601("2024-03-15T10:00:00Z");

    auto r = proc.process({t0, "exchange_info",
                           R"({"symbols":[{"symbol":"ETHUSDT","baseAsset":"ETH","quoteAsset":"USDT","filters":[
                               {"filterType":"PRICE_FILTER","tickSize":"0.01"},
                               {"filterType":"LOT_SIZE","minQty":"0.0001","stepSize":"0.0001"},
                               {"filterType":"NOTIONAL","minNotional":"5"}]}]})"});
    REQUIRE_MESSAGE(r.has_value(), r.error().message);
    CHECK_FALSE(r->has_value());
    REQUIRE(proc.instrument().has_value());
    CHECK(proc.instrument()->tick_size == "0.01"_px);

    r = proc.process({t0, "depth_snapshot", R"({"lastUpdateId":10,"bids":[["1","1"]],"asks":[["2","1"]]})"});
    REQUIRE(r.has_value());
    REQUIRE(r->has_value());
    CHECK(std::holds_alternative<BookSnapshot>(**r));
    CHECK(event_instrument(**r) == InstrumentId{3});
    CHECK(event_time(**r) == t0);

    r = proc.process({t0, "ethusdt@aggTrade",
                      R"({"stream":"ethusdt@aggTrade","data":{"e":"aggTrade","E":1,"s":"ETHUSDT","a":1,"p":"1","q":"1","f":1,"l":1,"T":1,"m":false,"M":true}})"});
    REQUIRE(r.has_value());
    REQUIRE(r->has_value());
    CHECK(std::holds_alternative<Trade>(**r));

    // Unsupported stream types are ignored, not errors.
    r = proc.process({t0, "ethusdt@ticker", R"({"stream":"ethusdt@ticker","data":{"e":"24hrTicker"}})"});
    REQUIRE(r.has_value());
    CHECK_FALSE(r->has_value());

    // Malformed payload is an error, counted.
    r = proc.process({t0, "ethusdt@aggTrade", R"({"stream":"ethusdt@aggTrade","data":{"e":"aggTrade"}})"});
    CHECK_FALSE(r.has_value());
    r = proc.process({t0, "exchange_info", "{}"});
    CHECK_FALSE(r.has_value());

    const auto& s = proc.stats();
    CHECK(s.records == 6);
    CHECK(s.events == 2);
    CHECK(s.ignored == 2);
    CHECK(s.errors == 2);
}
