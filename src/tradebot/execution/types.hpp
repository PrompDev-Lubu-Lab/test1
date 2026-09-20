#pragma once

// The execution interface every venue implements: the simulated exchange
// now, paper trading and the real exchange gateway later. Strategies and
// the portfolio talk only to these types, which is what lets one strategy
// binary run against any of them.
//
// Semantics are asynchronous everywhere: submit() and cancel() return once
// the request is accepted for transmission; outcomes arrive later as
// ExecutionReports on the listener, in venue time order.

#include "tradebot/core/error.hpp"
#include "tradebot/core/fixed_point.hpp"
#include "tradebot/core/time.hpp"
#include "tradebot/core/types.hpp"

#include <optional>
#include <string>

namespace tradebot::execution {

struct OrderRequest {
    ClientOrderId client_id;  // caller-assigned, unique per venue session
    InstrumentId instrument;
    StrategyId strategy;
    Side side;
    OrderType type = OrderType::limit;
    TimeInForce time_in_force = TimeInForce::gtc;
    Price price;  // limit orders only
    Quantity quantity;
};

struct Fill {
    Price price;
    Quantity quantity;
    Notional fee;  // in quote currency; negative would be a rebate
    Liquidity liquidity;
    TradeId exec_id;  // venue execution id
};

enum class ReportType : std::uint8_t {
    accepted,  // order is live at the venue
    rejected,  // never became live
    fill,  // partial or full
    cancelled,
    cancel_rejected,  // cancel arrived after the order was already done
    expired,  // IOC/FOK remainder, or venue-side expiry
};

[[nodiscard]] constexpr std::string_view to_string(ReportType t) noexcept {
    switch (t) {
        case ReportType::accepted: return "accepted";
        case ReportType::rejected: return "rejected";
        case ReportType::fill: return "fill";
        case ReportType::cancelled: return "cancelled";
        case ReportType::cancel_rejected: return "cancel_rejected";
        case ReportType::expired: return "expired";
    }
    return "?";
}

struct ExecutionReport {
    ReportType type;
    ClientOrderId client_id;
    OrderId order_id;  // venue-assigned, invalid on rejection
    InstrumentId instrument;
    StrategyId strategy;
    Side side;
    OrderType order_type = OrderType::limit;
    Price price;  // limit price (zero for market orders)
    Timestamp time;  // venue time of the event
    OrderStatus status;  // order status after this event
    Quantity filled_quantity;  // cumulative
    Quantity remaining_quantity;
    std::optional<Fill> fill;  // set for type == fill
    std::string reason;  // rejections and cancel rejections
};

class ExecutionListener {
public:
    virtual ~ExecutionListener() = default;
    virtual void on_execution_report(const ExecutionReport& report) = 0;
};

// Venue-side view of an order.
struct OrderState {
    OrderRequest request;
    OrderId order_id;
    OrderStatus status = OrderStatus::pending_new;
    Quantity filled_quantity;
    Notional filled_notional;
    Notional fees;
    Timestamp created;
    Timestamp updated;

    [[nodiscard]] Quantity remaining() const { return request.quantity - filled_quantity; }
    [[nodiscard]] bool is_done() const noexcept { return is_terminal(status); }
};

class ExecutionVenue {
public:
    virtual ~ExecutionVenue() = default;
    virtual void set_listener(ExecutionListener* listener) = 0;
    // Errors here mean the request never left (malformed, duplicate id);
    // venue-side rejections come back as reports.
    [[nodiscard]] virtual Result<void> submit(const OrderRequest& request) = 0;
    [[nodiscard]] virtual Result<void> cancel(ClientOrderId client_id) = 0;
    [[nodiscard]] virtual std::optional<OrderState> order(ClientOrderId client_id) const = 0;
};

// Fee as an exact rational rate, e.g. 0.1% = FeeRate{1, 1000}.
struct FeeRate {
    std::int64_t numerator = 0;
    std::int64_t denominator = 1;

    [[nodiscard]] static constexpr FeeRate bps(std::int64_t basis_points) noexcept {
        return FeeRate{basis_points, 10'000};
    }
    [[nodiscard]] static constexpr FeeRate zero() noexcept { return FeeRate{0, 1}; }
    // Fees round up: the venue never rounds in the client's favour.
    [[nodiscard]] Notional apply(Notional notional) const {
        return notional.mul_ratio(numerator, denominator, RoundingMode::up);
    }
};

struct FeeSchedule {
    FeeRate maker = FeeRate::bps(10);
    FeeRate taker = FeeRate::bps(10);
    [[nodiscard]] Notional fee_for(Liquidity liquidity, Notional notional) const {
        return (liquidity == Liquidity::maker ? maker : taker).apply(notional);
    }
};

}  // namespace tradebot::execution
