#pragma once

// Latency models used by replay and the simulated exchange.
//
// A live system never sees an event at the instant the venue stamped it,
// and its orders never reach the venue instantly. Replay applies a
// market-data delay before delivering each event to subscribers; the
// simulated exchange applies order and acknowledgement delays. All draws
// come from an engine-owned seeded generator, so runs are reproducible.

#include "tradebot/core/time.hpp"
#include "tradebot/market_data/events.hpp"

#include <memory>
#include <random>

namespace tradebot::replay {

using Rng = std::mt19937_64;

class LatencyModel {
public:
    virtual ~LatencyModel() = default;
    // Delay between an event's time and the moment a subscriber sees it.
    [[nodiscard]] virtual Duration market_data_delay(const market_data::MarketEvent& event,
                                                     Rng& rng) = 0;
    // One-way delay from us to the venue (order submit/cancel).
    [[nodiscard]] virtual Duration order_delay(Rng& rng) = 0;
    // One-way delay from the venue back to us (acks, fills).
    [[nodiscard]] virtual Duration ack_delay(Rng& rng) = 0;
};

class ZeroLatency final : public LatencyModel {
public:
    [[nodiscard]] Duration market_data_delay(const market_data::MarketEvent&, Rng&) override {
        return {};
    }
    [[nodiscard]] Duration order_delay(Rng&) override { return {}; }
    [[nodiscard]] Duration ack_delay(Rng&) override { return {}; }
};

class ConstantLatency final : public LatencyModel {
public:
    ConstantLatency(Duration market_data, Duration order, Duration ack)
        : md_(market_data), order_(order), ack_(ack) {}
    [[nodiscard]] Duration market_data_delay(const market_data::MarketEvent&, Rng&) override {
        return md_;
    }
    [[nodiscard]] Duration order_delay(Rng&) override { return order_; }
    [[nodiscard]] Duration ack_delay(Rng&) override { return ack_; }

private:
    Duration md_, order_, ack_;
};

// base + uniform jitter in [0, jitter]; a reasonable stand-in for a
// co-located or nearby client until measured distributions are available.
class JitterLatency final : public LatencyModel {
public:
    struct Params {
        Duration market_data_base = Duration::millis(5);
        Duration market_data_jitter = Duration::millis(10);
        Duration order_base = Duration::millis(5);
        Duration order_jitter = Duration::millis(10);
        Duration ack_base = Duration::millis(5);
        Duration ack_jitter = Duration::millis(10);
    };
    explicit JitterLatency(Params p) : p_(p) {}
    [[nodiscard]] Duration market_data_delay(const market_data::MarketEvent&, Rng& rng) override {
        return draw(p_.market_data_base, p_.market_data_jitter, rng);
    }
    [[nodiscard]] Duration order_delay(Rng& rng) override {
        return draw(p_.order_base, p_.order_jitter, rng);
    }
    [[nodiscard]] Duration ack_delay(Rng& rng) override {
        return draw(p_.ack_base, p_.ack_jitter, rng);
    }

private:
    static Duration draw(Duration base, Duration jitter, Rng& rng) {
        if (jitter.count_nanos() <= 0) {
            return base;
        }
        std::uniform_int_distribution<std::int64_t> dist(0, jitter.count_nanos());
        return base + Duration::nanos(dist(rng));
    }
    Params p_;
};

}  // namespace tradebot::replay
