#pragma once

// BinanceGateway: the real exchange behind the ExecutionVenue interface.
//
// Strategies, the risk gate and the portfolio do not change. The gateway
// sends orders and cancels over signed REST from a worker thread (never
// blocking the dispatch thread) and receives outcomes from the user data
// stream. An order state machine reconciles the two sources: REST
// acknowledgements and stream events can arrive in either order, fills may
// arrive after a cancel was sent, and a cancel may find the order already
// gone. Every report delivered downstream is deduplicated by (order,
// execution type, trade id) and delivered in venue time order per order.
//
// dry_run mode (shadow trading) validates and records orders but never
// sends them, answering with a synthesized rejection so the strategy sees
// a consistent, if pessimistic, world.

#include "tradebot/core/clock.hpp"
#include "tradebot/core/instrument.hpp"
#include "tradebot/core/log.hpp"
#include "tradebot/execution/types.hpp"
#include "tradebot/gateway/binance_rest.hpp"
#include "tradebot/gateway/user_stream.hpp"
#include "tradebot/portfolio/portfolio.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <unordered_map>

namespace tradebot::gateway {

// Marshals a callback onto the dispatch thread (LiveScheduler::post).
using Poster = std::function<void(std::function<void()>)>;

struct GatewayOptions {
    bool dry_run = false;  // shadow mode: never send
    Duration query_after = Duration::seconds(5);  // query an order with no ack/stream event after this
};

class BinanceGateway final : public execution::ExecutionVenue {
public:
    BinanceGateway(BinanceRestClient& rest, Instrument instrument, const Clock& clock, Poster post,
                   GatewayOptions opts, Logger log);
    ~BinanceGateway() override;

    // --- ExecutionVenue ------------------------------------------------------
    void set_listener(execution::ExecutionListener* listener) override { listener_ = listener; }
    [[nodiscard]] Result<void> submit(const execution::OrderRequest& request) override;
    [[nodiscard]] Result<void> cancel(ClientOrderId client_id) override;
    [[nodiscard]] std::optional<execution::OrderState> order(ClientOrderId client_id) const override;

    // --- inputs from the user stream (any thread; marshalled via post) -----------
    void on_stream_execution(const ParsedExecution& exec);

    // --- reconciliation (dispatch thread) ----------------------------------------
    struct Reconciliation {
        Quantity venue_base;  // free + locked base asset
        Quantity expected_base;  // portfolio position
        Notional venue_quote;
        Notional expected_quote;  // portfolio cash
        Quantity base_difference;  // venue - expected
        Notional quote_difference;
        bool within_tolerance = false;
    };
    [[nodiscard]] Result<Reconciliation> reconcile(const portfolio::Portfolio& portfolio, Quantity base_tolerance,
                                                   Notional quote_tolerance);

    // Periodic housekeeping: queries orders that have been silent too long.
    void poll_silent_orders();

    struct Stats {
        std::uint64_t submitted = 0;
        std::uint64_t sent = 0;
        std::uint64_t send_failures = 0;
        std::uint64_t cancels_sent = 0;
        std::uint64_t stream_events = 0;
        std::uint64_t duplicates = 0;
        std::uint64_t late_fills = 0;  // fills after a cancel request
        std::uint64_t queries = 0;
    };
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
    [[nodiscard]] std::size_t pending_jobs() const;
    void stop();

private:
    struct Tracked {
        execution::OrderState state;
        bool cancel_requested = false;
        bool acked = false;
        std::set<std::string> seen;  // dedupe keys
        Timestamp last_activity;
    };
    void worker();
    void enqueue(std::function<void()> job);
    void apply(Tracked& t, execution::ExecutionReport report, const std::string& dedupe_key);
    void deliver(const execution::ExecutionReport& report);
    void handle_rest_ack(ClientOrderId id, Result<nlohmann::json> response);
    void handle_rest_cancel(ClientOrderId id, Result<nlohmann::json> response);
    void handle_query(ClientOrderId id, Result<nlohmann::json> response);

    BinanceRestClient& rest_;
    Instrument instrument_;
    const Clock& clock_;
    Poster post_;
    GatewayOptions opts_;
    Logger log_;
    execution::ExecutionListener* listener_ = nullptr;
    std::unordered_map<ClientOrderId, Tracked> orders_;
    Stats stats_;

    // Worker thread for blocking REST calls.
    std::thread worker_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> jobs_;
    std::atomic<bool> stop_{false};
};

}  // namespace tradebot::gateway
