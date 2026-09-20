#pragma once

// Time-driven callback scheduling, implemented by the replay engine (on a
// simulated clock) and by the live/paper runtime (on the wall clock). The
// simulated exchange uses it to model latency: an order "arrives" at the
// venue a scheduled delay after submission.

#include "tradebot/core/clock.hpp"
#include "tradebot/core/time.hpp"

#include <cstdint>
#include <functional>

namespace tradebot {

using TimerId = std::uint64_t;
using TimerCallback = std::function<void(Timestamp)>;

class Scheduler {
public:
    virtual ~Scheduler() = default;
    [[nodiscard]] virtual Timestamp now() const noexcept = 0;
    // One-shot callback at `at` (or immediately, in order, if in the past).
    virtual TimerId schedule_at(Timestamp at, TimerCallback cb) = 0;
    // Periodic callback aligned to the interval from the epoch.
    virtual TimerId schedule_every(Duration interval, TimerCallback cb) = 0;
    virtual void cancel(TimerId id) = 0;
};

}  // namespace tradebot
