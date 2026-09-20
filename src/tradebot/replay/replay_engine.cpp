#include "tradebot/replay/replay_engine.hpp"

#include <algorithm>

namespace tradebot::replay {

using market_data::MarketEvent;

void EventBus::publish(const MarketEvent& event) const {
    for (auto* l : listeners_) {
        std::visit(
            [&](const auto& e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, market_data::Trade>) l->on_trade(e);
                else if constexpr (std::is_same_v<T, market_data::BookSnapshot>) l->on_book_snapshot(e);
                else if constexpr (std::is_same_v<T, market_data::BookDelta>) l->on_book_delta(e);
                else if constexpr (std::is_same_v<T, market_data::BookTicker>) l->on_book_ticker(e);
                else if constexpr (std::is_same_v<T, market_data::Candle>) l->on_candle(e);
                else l->on_heartbeat(e);
            },
            event);
    }
    for (const auto& h : handlers_) {
        h(event);
    }
}

ReplayEngine::ReplayEngine(EventSource& source, SimClock& clock, LatencyModel& latency,
                           Options opts)
    : source_(source), clock_(clock), latency_(latency), opts_(opts), rng_(opts.seed) {}

TimerId ReplayEngine::schedule_at(Timestamp at, TimerCallback cb) {
    timers_.push_back(Timer{std::move(cb), Duration{}, false});
    const TimerId id = timers_.size();
    pending_.push(Pending{std::max(at, clock_.now()), seq_++, kNoSlot, id});
    return id;
}

TimerId ReplayEngine::schedule_every(Duration interval, TimerCallback cb) {
    timers_.push_back(Timer{std::move(cb), interval, false});
    const TimerId id = timers_.size();
    const Timestamp first = clock_.now().floor_to(interval) + interval;
    pending_.push(Pending{first, seq_++, kNoSlot, id});
    return id;
}

void ReplayEngine::cancel(TimerId id) {
    if (id >= 1 && id <= timers_.size()) {
        timers_[id - 1].cancelled = true;
    }
}

Result<bool> ReplayEngine::read_ahead() {
    if (source_exhausted_) {
        return false;
    }
    MarketEvent ev;
    auto more = source_.next(ev);
    if (!more) {
        return tl::make_unexpected(more.error());
    }
    if (!*more) {
        source_exhausted_ = true;
        return false;
    }
    ++stats_.events_read;
    peeked_ = std::move(ev);
    return true;
}

std::size_t ReplayEngine::acquire_slot(MarketEvent&& event) {
    if (!free_slots_.empty()) {
        const std::size_t slot = free_slots_.back();
        free_slots_.pop_back();
        slots_[slot] = std::move(event);
        return slot;
    }
    slots_.push_back(std::move(event));
    return slots_.size() - 1;
}

void ReplayEngine::release_slot(std::size_t slot) noexcept { free_slots_.push_back(slot); }

void ReplayEngine::deliver(const Pending& p) {
    if (p.time > clock_.now()) {
        clock_.set(p.time);
    }
    if (p.slot != kNoSlot) {
        ++stats_.events_delivered;
        bus_.publish(slots_[p.slot]);
        release_slot(p.slot);
        return;
    }
    Timer& t = timers_[p.timer - 1];
    if (t.cancelled) {
        return;
    }
    ++stats_.timers_fired;
    t.cb(clock_.now());
    if (!t.interval.is_zero() && !t.cancelled && !source_exhausted_) {
        pending_.push(Pending{p.time + t.interval, seq_++, kNoSlot, p.timer});
    }
}

Result<void> ReplayEngine::run() {
    stop_requested_ = false;
    while (!stop_requested_) {
        // Read ahead until the next source event cannot precede the earliest
        // pending delivery.
        if (!peeked_ && !source_exhausted_) {
            auto r = read_ahead();
            if (!r) {
                return tl::make_unexpected(r.error());
            }
        }
        if (peeked_) {
            const Timestamp t = market_data::event_time(*peeked_);
            if (pending_.empty() || t <= pending_.top().time) {
                const Duration delay = latency_.market_data_delay(*peeked_, rng_);
                const std::size_t slot = acquire_slot(std::move(*peeked_));
                pending_.push(Pending{t + delay, seq_++, slot, 0});
                peeked_.reset();
                stats_.max_pending = std::max(stats_.max_pending, pending_.size());
                continue;
            }
        }
        if (pending_.empty()) {
            break;  // source exhausted and nothing left to deliver
        }
        if (opts_.end_time && pending_.top().time >= *opts_.end_time) {
            break;
        }
        const Pending p = pending_.top();
        pending_.pop();
        deliver(p);
    }
    return {};
}

}  // namespace tradebot::replay
