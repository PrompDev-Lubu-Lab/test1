#pragma once

// Feed health monitor and watchdog heartbeat.
//
// The monitor watches when market data last arrived and whether the venue
// book is synchronized. When the feed goes quiet for longer than the stale
// threshold the runtime trips the kill switch (uncertainty means "go
// safe", never "guess"); when data resumes it can re-arm automatically.
// The watchdog writes a heartbeat file from the dispatch thread so an
// external supervisor can tell a live process from a hung one.

#include "tradebot/core/time.hpp"
#include "tradebot/market_data/events.hpp"
#include "tradebot/replay/replay_engine.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace tradebot::live {

enum class FeedState : std::uint8_t { waiting, healthy, stale };

[[nodiscard]] constexpr std::string_view to_string(FeedState s) noexcept {
    switch (s) {
        case FeedState::waiting: return "waiting";
        case FeedState::healthy: return "healthy";
        case FeedState::stale: return "stale";
    }
    return "?";
}

class FeedHealthMonitor final : public replay::MarketDataListener {
public:
    struct Options {
        Duration stale_after = Duration::seconds(15);
    };

    explicit FeedHealthMonitor(Options opts) : opts_(opts) {}

    // MarketDataListener: any event counts as life.
    void on_trade(const market_data::Trade& t) override { seen(t.recv_time, true); }
    void on_book_snapshot(const market_data::BookSnapshot& s) override { seen(s.recv_time, false); }
    void on_book_delta(const market_data::BookDelta& d) override { seen(d.recv_time, false); }
    void on_book_ticker(const market_data::BookTicker& bt) override { seen(bt.recv_time, false); }
    void on_candle(const market_data::Candle& c) override { seen(c.recv_time, false); }
    void on_heartbeat(const market_data::Heartbeat& h) override { seen(h.recv_time, false); }

    // Evaluates the state at `now`. Returns true when the state changed.
    bool check(Timestamp now);

    [[nodiscard]] FeedState state() const noexcept { return state_; }
    [[nodiscard]] std::optional<Timestamp> last_event() const noexcept { return last_event_; }
    [[nodiscard]] std::optional<Timestamp> last_trade() const noexcept { return last_trade_; }
    [[nodiscard]] std::uint64_t events() const noexcept { return events_; }
    [[nodiscard]] std::uint64_t stale_episodes() const noexcept { return stale_episodes_; }
    [[nodiscard]] Duration silence(Timestamp now) const noexcept {
        return last_event_ ? now - *last_event_ : Duration{};
    }

private:
    void seen(Timestamp t, bool trade);

    Options opts_;
    FeedState state_ = FeedState::waiting;
    std::optional<Timestamp> last_event_;
    std::optional<Timestamp> last_trade_;
    std::uint64_t events_ = 0;
    std::uint64_t stale_episodes_ = 0;
};

// Writes "<iso time> <status>\n" atomically to the heartbeat file.
[[nodiscard]] Result<void> write_heartbeat(const std::filesystem::path& file, Timestamp now,
                                           std::string_view status);
// Age of the heartbeat, or nullopt if missing/unreadable.
[[nodiscard]] std::optional<Duration> heartbeat_age(const std::filesystem::path& file, Timestamp now);

}  // namespace tradebot::live
