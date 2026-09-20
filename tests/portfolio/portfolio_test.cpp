#include "tradebot/portfolio/portfolio.hpp"

#include <doctest/doctest.h>

using namespace tradebot;
using namespace tradebot::execution;
using namespace tradebot::portfolio;
using namespace tradebot::literals;

namespace {

const InstrumentId kEth{1};
const StrategyId kA{1};
const StrategyId kB{2};
const Timestamp kT0 = *Timestamp::parse_iso8601("2024-03-15T10:00:00Z");

std::uint64_t g_exec_id = 1000;  // every fill gets a distinct execution id

ExecutionReport fill_report(std::uint64_t cid, StrategyId strategy, Side side, const char* price,
                            const char* qty, const char* fee, Quantity remaining = Quantity{}) {
    ExecutionReport r;
    r.type = ReportType::fill;
    r.client_id = ClientOrderId{cid};
    r.order_id = OrderId{cid};
    r.instrument = kEth;
    r.strategy = strategy;
    r.side = side;
    r.time = kT0;
    r.status = remaining.is_zero() ? OrderStatus::filled : OrderStatus::partially_filled;
    r.remaining_quantity = remaining;
    r.fill = Fill{*Price::parse(price), *Quantity::parse(qty), *Notional::parse(fee), Liquidity::taker, TradeId{g_exec_id++}};
    return r;
}

ExecutionReport accepted(std::uint64_t cid, StrategyId strategy, Side side, OrderType type,
                         const char* price, const char* qty) {
    ExecutionReport r;
    r.type = ReportType::accepted;
    r.client_id = ClientOrderId{cid};
    r.order_id = OrderId{cid};
    r.instrument = kEth;
    r.strategy = strategy;
    r.side = side;
    r.order_type = type;
    r.price = *Price::parse(price);
    r.time = kT0;
    r.status = OrderStatus::open;
    r.remaining_quantity = *Quantity::parse(qty);
    return r;
}

ExecutionReport terminal(std::uint64_t cid, StrategyId strategy, ReportType type, OrderStatus status) {
    ExecutionReport r;
    r.type = type;
    r.client_id = ClientOrderId{cid};
    r.strategy = strategy;
    r.instrument = kEth;
    r.status = status;
    return r;
}

}  // namespace

TEST_CASE("Portfolio: buy, average up, partial sell realizes, fees netted, equity marks") {
    Portfolio pf("10000"_ntl);
    CHECK(pf.cash() == "10000"_ntl);
    CHECK(pf.equity() == "10000"_ntl);

    pf.on_execution_report(fill_report(1, kA, Side::buy, "3000", "1", "3"));
    CHECK(pf.cash() == "6997"_ntl);
    CHECK(pf.position(kEth) == "1"_qty);
    CHECK(pf.account().position(kEth)->average_entry == "3000"_px);
    CHECK(pf.account().fees == "3"_ntl);
    // No mark yet: position valued at cost.
    CHECK(pf.equity() == "9997"_ntl);

    pf.on_execution_report(fill_report(2, kA, Side::buy, "3100", "1", "3.1"));
    CHECK(pf.position(kEth) == "2"_qty);
    CHECK(pf.account().position(kEth)->average_entry == "3050"_px);
    CHECK(pf.cash() == "3893.9"_ntl);

    pf.set_mark(kEth, "3200"_px);
    CHECK(pf.unrealized_pnl() == "300"_ntl);  // (3200 - 3050) * 2
    CHECK(pf.equity() == "10293.9"_ntl);  // 3893.9 + 6400
    CHECK(pf.peak_equity() == "10293.9"_ntl);

    pf.on_execution_report(fill_report(3, kA, Side::sell, "3200", "0.5", "1.6"));
    CHECK(pf.position(kEth) == "1.5"_qty);
    CHECK(pf.account().position(kEth)->average_entry == "3050"_px);  // unchanged on reduce
    CHECK(pf.account().realized_pnl == "75"_ntl);  // (3200 - 3050) * 0.5
    CHECK(pf.account().fees == "7.7"_ntl);
    CHECK(pf.realized_pnl_net() == "67.3"_ntl);
    CHECK(pf.cash() == "5492.3"_ntl);  // 3893.9 + 1600 - 1.6
    CHECK(pf.unrealized_pnl() == "225"_ntl);
    CHECK(pf.equity() == "10292.3"_ntl);  // dropped by the fee only
    CHECK(pf.drawdown() == "1.6"_ntl);

    // Mark drops: drawdown grows, peak stays.
    pf.on_trade(market_data::Trade{kEth, kT0, kT0, TradeId{1}, "3000"_px, "1"_qty, Side::sell});
    CHECK(*pf.mark(kEth) == "3000"_px);
    CHECK(pf.equity() == "9992.3"_ntl);
    CHECK(pf.drawdown() == "301.6"_ntl);
    CHECK(pf.stats().fills == 3);
    CHECK(pf.stats().volume == "2.5"_qty);
    CHECK(pf.stats().turnover == "7700"_ntl);
}

TEST_CASE("Portfolio: closing and flipping through zero") {
    Portfolio pf("10000"_ntl);
    pf.on_execution_report(fill_report(1, kA, Side::buy, "100", "2", "0"));
    // Sell 3: closes 2 (realizes (90-100)*2 = -20), opens short 1 @ 90.
    pf.on_execution_report(fill_report(2, kA, Side::sell, "90", "3", "0"));
    const Position& p = *pf.account().position(kEth);
    CHECK(p.quantity == "-1"_qty);
    CHECK(p.average_entry == "90"_px);
    CHECK(p.realized_pnl == "-20"_ntl);
    pf.set_mark(kEth, "80"_px);
    CHECK(pf.unrealized_pnl() == "10"_ntl);  // short 1 from 90, now 80
    // Buy back 1 @ 80: realize +10, flat.
    pf.on_execution_report(fill_report(3, kA, Side::buy, "80", "1", "0"));
    CHECK(pf.position(kEth).is_zero());
    CHECK(pf.account().position(kEth)->average_entry.is_zero());
    CHECK(pf.account().realized_pnl == "-10"_ntl);
    CHECK(pf.cash() == "9990"_ntl);
    CHECK(pf.equity() == "9990"_ntl);
    CHECK(pf.unrealized_pnl().is_zero());
}

TEST_CASE("Portfolio: per-strategy ledgers sum to the account") {
    Portfolio pf("10000"_ntl);
    pf.on_execution_report(fill_report(1, kA, Side::buy, "100", "1", "0.1"));
    pf.on_execution_report(fill_report(2, kB, Side::buy, "110", "2", "0.2"));
    pf.on_execution_report(fill_report(3, kB, Side::sell, "120", "1", "0.1"));
    pf.set_mark(kEth, "130"_px);

    CHECK(pf.position(kA, kEth) == "1"_qty);
    CHECK(pf.position(kB, kEth) == "1"_qty);
    CHECK(pf.position(kEth) == "2"_qty);
    CHECK(pf.ledger(kA).cash == "-100.1"_ntl);  // strategy ledgers start at zero cash
    CHECK(pf.ledger(kB).cash == "-100.3"_ntl);
    CHECK(pf.ledger(kB).realized_pnl == "10"_ntl);
    CHECK(pf.account().realized_pnl == "10"_ntl);
    CHECK(pf.unrealized_pnl(kA) == "30"_ntl);
    CHECK(pf.unrealized_pnl(kB) == "20"_ntl);
    CHECK(pf.unrealized_pnl() == "50"_ntl);
    CHECK(pf.equity(kA) == "29.9"_ntl);  // -100.1 + 130
    CHECK(pf.equity(kB) == "29.7"_ntl);  // -100.3 + 130
    CHECK(pf.equity() == "10059.6"_ntl);  // 10000 + 29.9 + 29.7
    CHECK(pf.ledger(StrategyId{99}).cash.is_zero());
    CHECK(pf.position(StrategyId{99}, kEth).is_zero());
}

TEST_CASE("Portfolio: open-order exposure tracks accepted, partial fills and terminal reports") {
    Portfolio pf("10000"_ntl);
    pf.set_mark(kEth, "100"_px);
    pf.on_execution_report(accepted(1, kA, Side::buy, OrderType::limit, "95", "2"));
    pf.on_execution_report(accepted(2, kA, Side::sell, OrderType::limit, "105", "1"));
    pf.on_execution_report(accepted(3, kB, Side::buy, OrderType::market, "0", "1"));  // uses mark
    auto ea = pf.open_exposure(kA);
    CHECK(ea.open_orders == 2);
    CHECK(ea.buy_notional == "190"_ntl);
    CHECK(ea.sell_quantity == "1"_qty);
    CHECK(pf.open_exposure(kB).buy_notional == "100"_ntl);
    CHECK(pf.open_exposure().open_orders == 3);

    // Partial fill of order 1 reduces remaining exposure.
    pf.on_execution_report(fill_report(1, kA, Side::buy, "95", "0.5", "0", "1.5"_qty));
    CHECK(pf.open_exposure(kA).buy_notional == "142.5"_ntl);
    CHECK(pf.open_exposure(kA).open_orders == 2);
    // Cancel order 2, fill order 1 fully.
    pf.on_execution_report(terminal(2, kA, ReportType::cancelled, OrderStatus::cancelled));
    pf.on_execution_report(fill_report(1, kA, Side::buy, "95", "1.5", "0"));
    CHECK(pf.open_exposure(kA).open_orders == 0);
    CHECK(pf.open_exposure(kA).buy_notional.is_zero());
    CHECK(pf.open_exposure(kA).sell_quantity.is_zero());
    pf.on_execution_report(terminal(3, kB, ReportType::rejected, OrderStatus::rejected));
    CHECK(pf.open_exposure().open_orders == 0);
    CHECK(pf.position(kEth) == "2"_qty);
}

TEST_CASE("Portfolio: equity curve samples and marks from tickers/candles") {
    Portfolio pf("1000"_ntl);
    pf.sample(kT0);
    pf.on_execution_report(fill_report(1, kA, Side::buy, "100", "5", "0.5"));
    pf.on_book_ticker(market_data::BookTicker{kEth, kT0, 1, "101"_px, "1"_qty, "103"_px, "1"_qty});
    CHECK(*pf.mark(kEth) == "102"_px);
    pf.sample(kT0 + Duration::minutes(1));
    market_data::Candle c;
    c.instrument = kEth;
    c.close = "110"_px;
    c.closed = true;
    pf.on_candle(c);
    c.close = "999"_px;
    c.closed = false;
    pf.on_candle(c);  // forming candles do not mark
    CHECK(*pf.mark(kEth) == "110"_px);
    pf.sample(kT0 + Duration::minutes(2));

    const auto& curve = pf.equity_curve();
    REQUIRE(curve.size() == 3);
    CHECK(curve[0].equity == "1000"_ntl);
    CHECK(curve[0].position.is_zero());
    CHECK(curve[1].equity == "1009.5"_ntl);  // 499.5 cash + 510
    CHECK(curve[1].unrealized_pnl == "10"_ntl);
    CHECK(curve[1].fees == "0.5"_ntl);
    CHECK(curve[1].position == "5"_qty);
    CHECK(curve[1].mark == "102"_px);
    CHECK(curve[2].equity == "1049.5"_ntl);
    CHECK(curve[2].time == kT0 + Duration::minutes(2));
    CHECK(pf.peak_equity() == "1049.5"_ntl);
}

TEST_CASE("Portfolio: redelivered fills are applied once") {
    Portfolio pf("1000"_ntl);
    auto r = fill_report(1, kA, Side::buy, "100", "1", "0.1");
    pf.on_execution_report(r);
    pf.on_execution_report(r);  // same client id + exec id
    CHECK(pf.position(kEth) == "1"_qty);
    CHECK(pf.stats().fills == 1);
    CHECK(pf.stats().duplicate_fills == 1);
    r.fill->exec_id = TradeId{555};  // a genuinely new execution
    pf.on_execution_report(r);
    CHECK(pf.position(kEth) == "2"_qty);
    CHECK(pf.stats().fills == 2);
}
