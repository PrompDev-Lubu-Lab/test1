#pragma once

// The replay engine: drives a SimClock from an EventSource, delays each
// event by the latency model, and delivers events and timers to
// subscribers in strict time order.
//
// Ordering guarantee: a subscriber never observes time going backwards.
// Events with delivery time T are dispatched only once every source event
// that could be delivered before T has been read, which the loop ensures
// by reading ahead until the next source event's own time exceeds the
// earliest queued delivery.
//
// This is the same dispatch loop paper trading and live trading use with a
// wall-clock source; strategies subscribe through EventBus either way.

#include "tradebot/core/clock.hpp"
#include "tradebot/core/error.hpp"
#include "tradebot/core/scheduler.hpp"
#include "tradebot/market_data/events.hpp"
#include "tradebot/replay/event_source.hpp"
#include "tradebot/replay/latency.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <queue>
#include <unordered_map>
#include <vector>

namespace tradebot::replay {

// Typed subscriber interface; override what you need.
class MarketDataListener {
public:
    virtual ~MarketDataListener() = default;
    virtual void on_trade(const market_data::Trade&) {}
    virtual void on_book_snapshot(const market_data::BookSnapshot&) {}
    virtual void on_book_delta(const market_data::BookDelta&) {}
    virtual void on_book_ticker(const market_data::BookTicker&) {}
    virtual void on_candle(const market_data::Candle&) {}
    virtual void on_heartbeat(const market_data::Heartbeat&) {}
};

class EventBus {
public:
    using EventHandler = std::function<void(const market_data::MarketEvent&)>;

    void subscribe(MarketDataListener& listener) { listeners_.push_back(&listener); }
    void subscribe(EventHandler handler) { handlers_.push_back(std::move(handler)); }
    void publish(const market_data::MarketEvent& event) const;

private:
    std::vector<MarketDataListener*> listeners_;
    std::vector<EventHandler> handlers_;
};

struct ReplayOptions {
    std::uint64_t seed = 1;
    std::optional<Timestamp> end_time;  // stop once the clock would pass this
};

// Two delivery points for every event:
//   venue_bus   at the event's own time, undelayed: what the venue itself
//               knows (the simulated exchange subscribes here)
//   bus         after the market-data latency: what a client sees
//               (strategies, portfolio marks, analytics subscribe here)
class ReplayEngine final : public Scheduler {
public:
    using Options = ReplayOptions;

    ReplayEngine(EventSource& source, SimClock& clock, LatencyModel& latency,
                 Options opts = Options{});

    [[nodiscard]] EventBus& bus() noexcept { return bus_; }
    [[nodiscard]] EventBus& venue_bus() noexcept { return venue_bus_; }
    [[nodiscard]] const Clock& clock() const noexcept { return clock_; }
    [[nodiscard]] Timestamp now() const noexcept override { return clock_.now(); }
    [[nodiscard]] Rng& rng() noexcept { return rng_; }
    [[nodiscard]] LatencyModel& latency() noexcept { return latency_; }

    // One-shot timer. A time in the past fires at the current time.
    TimerId schedule_at(Timestamp at, TimerCallback cb) override;
    // Periodic timer aligned to the interval from the epoch (first fire at
    // the next boundary), stops when the source is exhausted.
    TimerId schedule_every(Duration interval, TimerCallback cb) override;
    void cancel(TimerId id) override;

    // Runs until the source is exhausted and all deliveries are done, until
    // end_time, or until stop() is called from a handler.
    [[nodiscard]] Result<void> run();
    void stop() noexcept { stop_requested_ = true; }

    struct Stats {
        std::uint64_t events_read = 0;
        std::uint64_t events_delivered = 0;
        std::uint64_t timers_fired = 0;
        std::size_t max_pending = 0;
    };
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

private:
    static constexpr std::size_t kNoSlot = static_cast<std::size_t>(-1);
    // Heap entries are small; queued events live in a slot pool so heap
    // operations never move MarketEvent payloads.
    struct Pending {
        Timestamp time;
        std::uint64_t seq;
        std::size_t slot = kNoSlot;  // event slot, or kNoSlot for a timer
        TimerId timer = 0;
        bool venue_side = false;  // deliver on venue_bus instead of bus
        bool operator>(const Pending& o) const noexcept {
            if (time != o.time) return time > o.time;
            return seq > o.seq;
        }
    };
    struct Timer {
        TimerCallback cb;
        Duration interval;  // zero for one-shot
        bool cancelled = false;
    };

    [[nodiscard]] Result<bool> read_ahead();  // reads one source event into pending
    void deliver(const Pending& p);
    std::size_t acquire_slot(market_data::MarketEvent&& event);
    void release_slot(std::size_t slot) noexcept;

    EventSource& source_;
    SimClock& clock_;
    LatencyModel& latency_;
    Options opts_;
    EventBus bus_;
    EventBus venue_bus_;
    Rng rng_;
    std::priority_queue<Pending, std::vector<Pending>, std::greater<>> pending_;
    std::vector<market_data::MarketEvent> slots_;
    std::vector<int> slot_uses_;  // deliveries still owed per slot
    std::vector<std::size_t> free_slots_;
    std::optional<market_data::MarketEvent> peeked_;
    bool source_exhausted_ = false;
    bool stop_requested_ = false;
    std::uint64_t seq_ = 0;
    std::unordered_map<TimerId, Timer> timers_;  // one-shots erased after firing
    TimerId next_timer_id_ = 1;
    Stats stats_;
};

}  // namespace tradebot::replay
