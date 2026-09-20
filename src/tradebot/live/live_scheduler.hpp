#pragma once

// LiveScheduler: the wall-clock counterpart of ReplayEngine.
//
// One dispatch thread owns every strategy, the portfolio, the risk gate
// and the venue. Producer threads (the market-data feed) post events into
// a queue; the dispatch loop interleaves those events with due timers in
// time order. Market data is delivered on the venue bus and the client
// bus in one step, since live data already carries real latency. Order
// and acknowledgement delays for the simulated venue still come from the
// latency model through schedule_at, exactly as in replay.

#include "tradebot/core/clock.hpp"
#include "tradebot/core/scheduler.hpp"
#include "tradebot/market_data/events.hpp"
#include "tradebot/replay/replay_engine.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <queue>
#include <unordered_map>

namespace tradebot::live {

class LiveScheduler final : public Scheduler {
public:
    explicit LiveScheduler(const Clock& clock);

    // --- Scheduler (dispatch thread only) --------------------------------
    [[nodiscard]] Timestamp now() const noexcept override { return clock_.now(); }
    TimerId schedule_at(Timestamp at, TimerCallback cb) override;
    TimerId schedule_every(Duration interval, TimerCallback cb) override;
    void cancel(TimerId id) override;

    // --- producers (any thread) -------------------------------------------
    void post_event(market_data::MarketEvent event);
    // Runs `fn` on the dispatch thread as soon as possible.
    void post(std::function<void()> fn);
    void stop() noexcept;

    // --- dispatch thread ----------------------------------------------------
    [[nodiscard]] replay::EventBus& venue_bus() noexcept { return venue_bus_; }
    [[nodiscard]] replay::EventBus& bus() noexcept { return bus_; }
    // Blocks until stop() is called; processes events and timers.
    void run();
    // Processes everything currently due and returns (for tests / stepping).
    void run_once(Duration max_wait);

    struct Stats {
        std::uint64_t events = 0;
        std::uint64_t timers_fired = 0;
        std::uint64_t posts = 0;
        std::size_t max_queue = 0;
    };
    [[nodiscard]] Stats stats() const;

private:
    struct Timer {
        TimerCallback cb;
        Duration interval;
        bool cancelled = false;
    };
    struct Pending {
        Timestamp time;
        std::uint64_t seq;
        TimerId timer;
        bool operator>(const Pending& o) const noexcept {
            if (time != o.time) return time > o.time;
            return seq > o.seq;
        }
    };
    struct Incoming {
        std::optional<market_data::MarketEvent> event;
        std::function<void()> fn;
    };

    void fire(const Pending& p);
    [[nodiscard]] std::optional<Timestamp> next_timer_time() const;

    const Clock& clock_;
    replay::EventBus venue_bus_;
    replay::EventBus bus_;

    // Timers: dispatch thread only.
    std::priority_queue<Pending, std::vector<Pending>, std::greater<>> timers_due_;
    std::unordered_map<TimerId, Timer> timers_;
    TimerId next_timer_id_ = 1;
    std::uint64_t seq_ = 0;

    // Producer queue.
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Incoming> incoming_;
    std::atomic<bool> stop_{false};
    Stats stats_;
};

}  // namespace tradebot::live
