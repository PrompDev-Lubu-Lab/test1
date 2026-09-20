#pragma once

// Simulated exchange: a matching venue driven by replayed (or live) market
// data.
//
// What it models
//   - Latency: an order or cancel reaches the venue after order_delay; every
//     report reaches the client after ack_delay. Both come from the shared
//     LatencyModel and Rng, so runs are reproducible.
//   - Venue rules: tick size, lot size, minimum quantity and notional, exactly
//     as the Instrument specifies; violations are rejected at arrival.
//   - Aggressive orders (market, marketable limit): walk the venue's book
//     level by level, paying the taker fee. Consumed liquidity is removed
//     from the venue book until the feed's next update restores it, which
//     approximates transient impact.
//   - Resting limit orders: join the back of the queue at their price. They
//     fill when the market trades through the price, or when trades at the
//     price exhaust the quantity queued ahead, or when the opposite side of
//     the book crosses the price. Book reductions at the price shrink the
//     queue ahead (assumed cancellations). Fills pay the maker fee.
//   - Time in force: GTC rests, IOC fills what it can and expires the rest,
//     FOK fills entirely or not at all, post-only rejects if it would take.
//   - Self-trade prevention: our resting orders are not part of the venue
//     book, so our own aggressive orders never match them (equivalent to
//     Binance's EXPIRE_MAKER? No: equivalent to ignoring them; documented).
//
// What it does not model (yet): partial rejections under rate limits,
// venue outages, and fee tiers by volume.

#include "tradebot/core/clock.hpp"
#include "tradebot/core/instrument.hpp"
#include "tradebot/core/scheduler.hpp"
#include "tradebot/execution/types.hpp"
#include "tradebot/market_data/book_synchronizer.hpp"
#include "tradebot/market_data/events.hpp"
#include "tradebot/replay/latency.hpp"
#include "tradebot/replay/replay_engine.hpp"

#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace tradebot::execution {

enum class QueueModel : std::uint8_t {
    optimistic,  // fill as soon as the market trades at our price
    queue,  // fill after the quantity ahead of us at our price has traded
    pessimistic,  // fill only when the market trades through our price
};

struct SimulatedExchangeOptions {
    FeeSchedule fees;
    QueueModel queue_model = QueueModel::queue;
    bool consume_liquidity = true;  // aggressive fills deplete the venue book
    bool allow_partial_market_fills = true;  // market order on a thin book
    bool require_synced_book = true;  // reject aggressive orders while resyncing
};

class SimulatedExchange final : public ExecutionVenue, public replay::MarketDataListener {
public:
    SimulatedExchange(Instrument instrument, Scheduler& scheduler, replay::LatencyModel& latency,
                      replay::Rng& rng, SimulatedExchangeOptions opts = SimulatedExchangeOptions{});

    // --- ExecutionVenue -------------------------------------------------
    void set_listener(ExecutionListener* listener) override { listener_ = listener; }
    [[nodiscard]] Result<void> submit(const OrderRequest& request) override;
    [[nodiscard]] Result<void> cancel(ClientOrderId client_id) override;
    [[nodiscard]] std::optional<OrderState> order(ClientOrderId client_id) const override;

    // --- MarketDataListener (subscribe on the venue bus) ----------------
    void on_book_snapshot(const market_data::BookSnapshot& s) override;
    void on_book_delta(const market_data::BookDelta& d) override;
    void on_trade(const market_data::Trade& t) override;

    [[nodiscard]] const market_data::OrderBook& book() const noexcept { return sync_.book(); }
    [[nodiscard]] const Instrument& instrument() const noexcept { return instrument_; }
    [[nodiscard]] std::vector<OrderState> open_orders() const;

    struct Stats {
        std::uint64_t submitted = 0;
        std::uint64_t accepted = 0;
        std::uint64_t rejected = 0;
        std::uint64_t fills = 0;
        std::uint64_t cancelled = 0;
        std::uint64_t expired = 0;
        Notional fees_charged;
        Quantity taker_volume;
        Quantity maker_volume;
    };
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

private:
    struct Resting {
        ClientOrderId client_id;
        Quantity queue_ahead;  // quantity at our price that was there before us
    };
    using PriceLevelQueue = std::vector<Resting>;  // FIFO per price

    void on_order_arrival(OrderRequest request, Timestamp arrival);
    void on_cancel_arrival(ClientOrderId client_id, Timestamp arrival);
    [[nodiscard]] Result<void> validate(const OrderRequest& request) const;

    // Aggressive execution against the venue book; returns filled quantity.
    Quantity take(OrderState& order, std::optional<Price> limit, Timestamp now);
    void rest(OrderState& order);
    void unrest(const OrderState& order);
    void fill(OrderState& order, Price price, Quantity quantity, Liquidity liquidity, Timestamp now);
    void finish(OrderState& order, ReportType type, OrderStatus status, Timestamp now,
                std::string reason = {});
    void report(const OrderState& order, ReportType type, Timestamp now,
                std::optional<Fill> fill = std::nullopt, std::string reason = {});

    // Maker fills driven by market activity.
    void match_resting_against_trade(const market_data::Trade& t);
    void match_resting_against_book();
    void shrink_queues_from_book();

    Instrument instrument_;
    Scheduler& scheduler_;
    replay::LatencyModel& latency_;
    replay::Rng& rng_;
    SimulatedExchangeOptions opts_;
    ExecutionListener* listener_ = nullptr;

    market_data::BookSynchronizer sync_;
    std::unordered_map<ClientOrderId, OrderState> orders_;
    std::map<Price, PriceLevelQueue> resting_bids_;  // iterate from best: rbegin
    std::map<Price, PriceLevelQueue> resting_asks_;  // iterate from best: begin
    IdGenerator<OrderId> order_ids_;
    IdGenerator<TradeId> exec_ids_;
    Stats stats_;
};

}  // namespace tradebot::execution
