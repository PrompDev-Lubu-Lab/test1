#pragma once

// Clock abstraction. Every component that needs "now" takes a Clock&, so the
// same code runs under a simulated clock during replay/backtests and the
// wall clock in paper/live trading. Reading std::chrono::system_clock
// directly anywhere outside WallClock is a bug.

#include "tradebot/core/time.hpp"

#include <chrono>
#include <stdexcept>

namespace tradebot {

class Clock {
public:
    virtual ~Clock() = default;
    [[nodiscard]] virtual Timestamp now() const noexcept = 0;
};

class WallClock final : public Clock {
public:
    [[nodiscard]] Timestamp now() const noexcept override {
        return Timestamp::from_chrono(std::chrono::system_clock::now());
    }
};

// Manually driven clock for replay and tests. Time only moves forward; an
// attempt to move it backwards is a logic error and throws, because it would
// mean the event stream feeding it is out of order.
class SimClock final : public Clock {
public:
    SimClock() = default;
    explicit SimClock(Timestamp start) noexcept : now_(start) {}

    [[nodiscard]] Timestamp now() const noexcept override { return now_; }

    void set(Timestamp t) {
        if (t < now_) {
            throw std::logic_error("SimClock cannot move backwards");
        }
        now_ = t;
    }

    void advance(Duration d) {
        if (d.is_negative()) {
            throw std::logic_error("SimClock cannot advance by a negative duration");
        }
        now_ += d;
    }

private:
    Timestamp now_;
};

}  // namespace tradebot
