#include "tradebot/live/live_scheduler.hpp"

#include <algorithm>

namespace tradebot::live {

LiveScheduler::LiveScheduler(const Clock& clock) : clock_(clock) {}

TimerId LiveScheduler::schedule_at(Timestamp at, TimerCallback cb) {
    const TimerId id = next_timer_id_++;
    timers_.emplace(id, Timer{std::move(cb), Duration{}, false});
    timers_due_.push(Pending{std::max(at, now()), seq_++, id});
    return id;
}

TimerId LiveScheduler::schedule_every(Duration interval, TimerCallback cb) {
    const TimerId id = next_timer_id_++;
    timers_.emplace(id, Timer{std::move(cb), interval, false});
    timers_due_.push(Pending{now().floor_to(interval) + interval, seq_++, id});
    return id;
}

void LiveScheduler::cancel(TimerId id) {
    if (auto it = timers_.find(id); it != timers_.end()) {
        it->second.cancelled = true;
    }
}

void LiveScheduler::post_event(market_data::MarketEvent event) {
    {
        std::lock_guard lock(mutex_);
        incoming_.push_back(Incoming{std::move(event), {}});
        stats_.max_queue = std::max(stats_.max_queue, incoming_.size());
    }
    cv_.notify_one();
}

void LiveScheduler::post(std::function<void()> fn) {
    {
        std::lock_guard lock(mutex_);
        incoming_.push_back(Incoming{std::nullopt, std::move(fn)});
        stats_.max_queue = std::max(stats_.max_queue, incoming_.size());
    }
    cv_.notify_one();
}

void LiveScheduler::stop() noexcept {
    stop_.store(true);
    cv_.notify_all();
}

std::optional<Timestamp> LiveScheduler::next_timer_time() const {
    if (timers_due_.empty()) return std::nullopt;
    return timers_due_.top().time;
}

void LiveScheduler::fire(const Pending& p) {
    auto it = timers_.find(p.timer);
    if (it == timers_.end()) return;
    if (it->second.cancelled) {
        timers_.erase(it);
        return;
    }
    {
        std::lock_guard lock(mutex_);
        ++stats_.timers_fired;
    }
    if (it->second.interval.is_zero()) {
        TimerCallback cb = std::move(it->second.cb);
        timers_.erase(it);
        cb(now());
        return;
    }
    Timer& t = it->second;
    const Duration interval = t.interval;
    t.cb(now());
    if (!t.cancelled) {
        // Next boundary strictly after the scheduled time, skipping any we
        // missed while busy.
        Timestamp next = p.time + interval;
        const Timestamp n = now();
        while (next <= n) next += interval;
        timers_due_.push(Pending{next, seq_++, p.timer});
    } else {
        timers_.erase(p.timer);
    }
}

void LiveScheduler::run_once(Duration max_wait) {
    // 1. Drain incoming events/posts.
    std::deque<Incoming> batch;
    {
        std::unique_lock lock(mutex_);
        if (incoming_.empty()) {
            auto deadline = std::chrono::steady_clock::now() + max_wait.to_chrono();
            if (auto nt = next_timer_time()) {
                const Duration until = *nt - now();
                if (until.count_nanos() < max_wait.count_nanos()) {
                    deadline = std::chrono::steady_clock::now() +
                               std::chrono::nanoseconds(std::max<std::int64_t>(0, until.count_nanos()));
                }
            }
            cv_.wait_until(lock, deadline, [&] { return !incoming_.empty() || stop_.load(); });
        }
        batch.swap(incoming_);
    }
    for (auto& in : batch) {
        if (in.event) {
            // Timers due before this event's arrival fire first.
            while (!timers_due_.empty() && timers_due_.top().time <= now()) {
                const Pending p = timers_due_.top();
                timers_due_.pop();
                fire(p);
            }
            {
                std::lock_guard lock(mutex_);
                ++stats_.events;
            }
            venue_bus_.publish(*in.event);
            bus_.publish(*in.event);
        } else if (in.fn) {
            {
                std::lock_guard lock(mutex_);
                ++stats_.posts;
            }
            in.fn();
        }
    }
    // 2. Fire timers that are due now.
    while (!timers_due_.empty() && timers_due_.top().time <= now()) {
        const Pending p = timers_due_.top();
        timers_due_.pop();
        fire(p);
    }
}

void LiveScheduler::run() {
    stop_.store(false);
    while (!stop_.load()) {
        run_once(Duration::millis(100));
    }
    // Drain anything posted right before stop so state is consistent.
    run_once(Duration{});
}

LiveScheduler::Stats LiveScheduler::stats() const {
    std::lock_guard lock(mutex_);
    return stats_;
}

}  // namespace tradebot::live
