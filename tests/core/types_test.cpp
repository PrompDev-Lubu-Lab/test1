#include "tradebot/core/types.hpp"

#include <doctest/doctest.h>

#include <unordered_set>

using namespace tradebot;

TEST_CASE("Enums: to_string / parse") {
    CHECK(to_string(Side::buy) == "buy");
    CHECK(opposite(Side::buy) == Side::sell);
    CHECK(opposite(Side::sell) == Side::buy);
    CHECK(*parse_side("buy") == Side::buy);
    CHECK(*parse_side("SELL") == Side::sell);
    CHECK_FALSE(parse_side("hold").has_value());

    CHECK(*parse_order_type("limit") == OrderType::limit);
    CHECK(*parse_order_type("MARKET") == OrderType::market);
    CHECK_FALSE(parse_order_type("stop").has_value());

    CHECK(*parse_time_in_force("ioc") == TimeInForce::ioc);
    CHECK(*parse_time_in_force("post_only") == TimeInForce::post_only);
    CHECK(to_string(TimeInForce::fok) == "fok");
    CHECK_FALSE(parse_time_in_force("gtd").has_value());

    CHECK(to_string(OrderStatus::partially_filled) == "partially_filled");
    CHECK(to_string(Liquidity::maker) == "maker");
}

TEST_CASE("OrderStatus: terminal states") {
    CHECK(is_terminal(OrderStatus::filled));
    CHECK(is_terminal(OrderStatus::cancelled));
    CHECK(is_terminal(OrderStatus::rejected));
    CHECK(is_terminal(OrderStatus::expired));
    CHECK_FALSE(is_terminal(OrderStatus::pending_new));
    CHECK_FALSE(is_terminal(OrderStatus::open));
    CHECK_FALSE(is_terminal(OrderStatus::partially_filled));
    CHECK_FALSE(is_terminal(OrderStatus::pending_cancel));
}

TEST_CASE("StrongId: distinct types, validity, hashing") {
    OrderId a{42};
    OrderId b{42};
    OrderId c{43};
    CHECK(a == b);
    CHECK(a != c);
    CHECK(a < c);
    CHECK(a.is_valid());
    CHECK_FALSE(OrderId{}.is_valid());
    CHECK(OrderId::invalid() == OrderId{});
    static_assert(!std::is_convertible_v<OrderId, TradeId>);
    static_assert(!std::is_constructible_v<OrderId, TradeId>);

    std::unordered_set<ClientOrderId> ids;
    ids.insert(ClientOrderId{1});
    ids.insert(ClientOrderId{1});
    ids.insert(ClientOrderId{2});
    CHECK(ids.size() == 2);
}

TEST_CASE("IdGenerator: monotonic starting at 1") {
    IdGenerator<ClientOrderId> gen;
    CHECK(gen.next() == ClientOrderId{1});
    CHECK(gen.next() == ClientOrderId{2});
    IdGenerator<TradeId> gen2(100);
    CHECK(gen2.next() == TradeId{100});
}
