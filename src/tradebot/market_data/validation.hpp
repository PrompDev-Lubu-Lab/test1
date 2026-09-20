#pragma once

// Stream sanity checks. A feed that silently repeats, skips or corrupts
// records poisons every result downstream, so every ingested stream passes
// through here and every anomaly is counted and reported.

#include "tradebot/core/time.hpp"
#include "tradebot/market_data/events.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace tradebot::market_data {

enum class IssueKind : std::uint8_t {
    duplicate,  // same or older id seen again (dropped)
    gap,  // ids skipped (kept, flagged)
    time_regression,  // exchange_time went backwards
    invalid_value,  // non-positive price/quantity, crossed ticker (dropped)
};

[[nodiscard]] constexpr std::string_view to_string(IssueKind k) noexcept {
    switch (k) {
        case IssueKind::duplicate: return "duplicate";
        case IssueKind::gap: return "gap";
        case IssueKind::time_regression: return "time_regression";
        case IssueKind::invalid_value: return "invalid_value";
    }
    return "?";
}

struct ValidationIssue {
    IssueKind kind;
    Timestamp time;  // recv_time of the offending event
    std::string detail;
};

struct ValidationStats {
    std::uint64_t accepted = 0;
    std::uint64_t dropped = 0;
    std::uint64_t duplicates = 0;
    std::uint64_t gaps = 0;
    std::uint64_t time_regressions = 0;
    std::uint64_t invalid_values = 0;
};

// Validates one instrument's trade stream: strictly increasing trade ids,
// non-decreasing exchange time, positive price and quantity.
class TradeValidator {
public:
    using IssueHandler = std::function<void(const ValidationIssue&)>;

    explicit TradeValidator(IssueHandler on_issue = {}) : on_issue_(std::move(on_issue)) {}

    // Returns true if the trade should be kept.
    [[nodiscard]] bool check(const Trade& trade);

    // Forget sequence state, e.g. after a reconnect where ids may restart.
    void reset();

    [[nodiscard]] const ValidationStats& stats() const noexcept { return stats_; }

private:
    void report(IssueKind kind, Timestamp time, std::string detail);

    IssueHandler on_issue_;
    ValidationStats stats_;
    std::optional<std::uint64_t> last_id_;
    std::optional<Timestamp> last_time_;
};

// Validates a top-of-book stream: increasing update ids, positive sizes,
// bid strictly below ask.
class BookTickerValidator {
public:
    using IssueHandler = std::function<void(const ValidationIssue&)>;

    explicit BookTickerValidator(IssueHandler on_issue = {}) : on_issue_(std::move(on_issue)) {}

    [[nodiscard]] bool check(const BookTicker& ticker);
    void reset();
    [[nodiscard]] const ValidationStats& stats() const noexcept { return stats_; }

private:
    void report(IssueKind kind, Timestamp time, std::string detail);

    IssueHandler on_issue_;
    ValidationStats stats_;
    std::optional<std::int64_t> last_id_;
};

}  // namespace tradebot::market_data
