#pragma once

// Incremental, stateful indicators shared by all strategies.
//
// Each accepts one value at a time and answers in O(1) (or amortized O(1)),
// so the same code serves replay at full speed and live trading. Values are
// doubles: indicators are analytics, not money. They report "not ready"
// until they have seen a full window.

#include "tradebot/market_data/events.hpp"

#include <cmath>
#include <deque>
#include <optional>

namespace tradebot::strategy {

class Sma {
public:
    explicit Sma(std::size_t period) : period_(period) {}
    void push(double v) {
        window_.push_back(v);
        sum_ += v;
        if (window_.size() > period_) {
            sum_ -= window_.front();
            window_.pop_front();
        }
    }
    [[nodiscard]] bool ready() const noexcept { return window_.size() >= period_; }
    [[nodiscard]] std::optional<double> value() const {
        if (!ready()) return std::nullopt;
        return sum_ / static_cast<double>(period_);
    }
    [[nodiscard]] std::size_t period() const noexcept { return period_; }

private:
    std::size_t period_;
    std::deque<double> window_;
    double sum_ = 0.0;
};

class Ema {
public:
    explicit Ema(std::size_t period) : period_(period), alpha_(2.0 / (static_cast<double>(period) + 1.0)) {}
    void push(double v) {
        ++count_;
        if (count_ == 1) {
            value_ = v;
        } else {
            value_ = alpha_ * v + (1.0 - alpha_) * value_;
        }
    }
    [[nodiscard]] bool ready() const noexcept { return count_ >= period_; }
    [[nodiscard]] std::optional<double> value() const {
        if (!ready()) return std::nullopt;
        return value_;
    }

private:
    std::size_t period_;
    double alpha_;
    double value_ = 0.0;
    std::size_t count_ = 0;
};

// Rolling mean and (population) standard deviation over a window.
class RollingStats {
public:
    explicit RollingStats(std::size_t period) : period_(period) {}
    void push(double v) {
        window_.push_back(v);
        sum_ += v;
        sum_sq_ += v * v;
        if (window_.size() > period_) {
            const double old = window_.front();
            sum_ -= old;
            sum_sq_ -= old * old;
            window_.pop_front();
        }
    }
    [[nodiscard]] bool ready() const noexcept { return window_.size() >= period_; }
    [[nodiscard]] std::optional<double> mean() const {
        if (!ready()) return std::nullopt;
        return sum_ / static_cast<double>(period_);
    }
    [[nodiscard]] std::optional<double> stddev() const {
        if (!ready()) return std::nullopt;
        const double n = static_cast<double>(period_);
        const double var = std::max(0.0, sum_sq_ / n - (sum_ / n) * (sum_ / n));
        return std::sqrt(var);
    }
    // (last - mean) / stddev; nullopt when not ready or stddev is zero.
    [[nodiscard]] std::optional<double> zscore() const {
        if (!ready() || window_.empty()) return std::nullopt;
        const double sd = *stddev();
        if (sd <= 0.0) return std::nullopt;
        return (window_.back() - *mean()) / sd;
    }

private:
    std::size_t period_;
    std::deque<double> window_;
    double sum_ = 0.0;
    double sum_sq_ = 0.0;
};

// Rolling maximum/minimum via monotonic deques; O(1) amortized.
class RollingExtrema {
public:
    explicit RollingExtrema(std::size_t period) : period_(period) {}
    void push(double v) {
        ++index_;
        while (!max_.empty() && max_.back().second <= v) max_.pop_back();
        max_.emplace_back(index_, v);
        while (!min_.empty() && min_.back().second >= v) min_.pop_back();
        min_.emplace_back(index_, v);
        const std::size_t oldest = index_ >= period_ ? index_ - period_ + 1 : 0;
        while (max_.front().first < oldest) max_.pop_front();
        while (min_.front().first < oldest) min_.pop_front();
        count_ = std::min(count_ + 1, period_);
    }
    [[nodiscard]] bool ready() const noexcept { return count_ >= period_; }
    [[nodiscard]] std::optional<double> max() const {
        if (!ready()) return std::nullopt;
        return max_.front().second;
    }
    [[nodiscard]] std::optional<double> min() const {
        if (!ready()) return std::nullopt;
        return min_.front().second;
    }

private:
    std::size_t period_;
    std::deque<std::pair<std::size_t, double>> max_;
    std::deque<std::pair<std::size_t, double>> min_;
    std::size_t index_ = 0;
    std::size_t count_ = 0;
};

// Average True Range over closed candles (Wilder smoothing).
class Atr {
public:
    explicit Atr(std::size_t period) : period_(period) {}
    void push(const market_data::Candle& c) {
        const double high = c.high.to_double();
        const double low = c.low.to_double();
        const double close = c.close.to_double();
        double tr = high - low;
        if (prev_close_) {
            tr = std::max({tr, std::fabs(high - *prev_close_), std::fabs(low - *prev_close_)});
        }
        prev_close_ = close;
        ++count_;
        if (count_ <= period_) {
            seed_sum_ += tr;
            if (count_ == period_) atr_ = seed_sum_ / static_cast<double>(period_);
        } else {
            atr_ = (atr_ * static_cast<double>(period_ - 1) + tr) / static_cast<double>(period_);
        }
    }
    [[nodiscard]] bool ready() const noexcept { return count_ >= period_; }
    [[nodiscard]] std::optional<double> value() const {
        if (!ready()) return std::nullopt;
        return atr_;
    }

private:
    std::size_t period_;
    std::optional<double> prev_close_;
    double seed_sum_ = 0.0;
    double atr_ = 0.0;
    std::size_t count_ = 0;
};

// Relative Strength Index (Wilder), on closes.
class Rsi {
public:
    explicit Rsi(std::size_t period) : period_(period) {}
    void push(double close) {
        if (!prev_) {
            prev_ = close;
            return;
        }
        const double change = close - *prev_;
        prev_ = close;
        const double gain = change > 0 ? change : 0.0;
        const double loss = change < 0 ? -change : 0.0;
        ++count_;
        if (count_ <= period_) {
            gain_sum_ += gain;
            loss_sum_ += loss;
            if (count_ == period_) {
                avg_gain_ = gain_sum_ / static_cast<double>(period_);
                avg_loss_ = loss_sum_ / static_cast<double>(period_);
            }
        } else {
            const double p = static_cast<double>(period_);
            avg_gain_ = (avg_gain_ * (p - 1) + gain) / p;
            avg_loss_ = (avg_loss_ * (p - 1) + loss) / p;
        }
    }
    [[nodiscard]] bool ready() const noexcept { return count_ >= period_; }
    [[nodiscard]] std::optional<double> value() const {
        if (!ready()) return std::nullopt;
        if (avg_loss_ <= 0.0) return 100.0;
        const double rs = avg_gain_ / avg_loss_;
        return 100.0 - 100.0 / (1.0 + rs);
    }

private:
    std::size_t period_;
    std::optional<double> prev_;
    double gain_sum_ = 0.0, loss_sum_ = 0.0;
    double avg_gain_ = 0.0, avg_loss_ = 0.0;
    std::size_t count_ = 0;
};

}  // namespace tradebot::strategy
