#pragma once

// Time representation.
//
// The whole system uses a single convention: Timestamp is nanoseconds since
// the Unix epoch, UTC, as a signed 64-bit integer. Nothing else reads the
// wall clock directly; components take a Clock (see clock.hpp) so that the
// same code runs against historical replay and live feeds.

#include "tradebot/core/error.hpp"

#include <chrono>
#include <compare>
#include <cstdint>
#include <string>
#include <string_view>

namespace tradebot {

class Duration {
public:
    constexpr Duration() noexcept = default;
    [[nodiscard]] static constexpr Duration nanos(std::int64_t v) noexcept { return Duration{v}; }
    [[nodiscard]] static constexpr Duration micros(std::int64_t v) noexcept {
        return Duration{v * 1'000};
    }
    [[nodiscard]] static constexpr Duration millis(std::int64_t v) noexcept {
        return Duration{v * 1'000'000};
    }
    [[nodiscard]] static constexpr Duration seconds(std::int64_t v) noexcept {
        return Duration{v * 1'000'000'000};
    }
    [[nodiscard]] static constexpr Duration minutes(std::int64_t v) noexcept {
        return seconds(v * 60);
    }
    [[nodiscard]] static constexpr Duration hours(std::int64_t v) noexcept {
        return minutes(v * 60);
    }
    [[nodiscard]] static constexpr Duration days(std::int64_t v) noexcept { return hours(v * 24); }

    template <class Rep, class Period>
    [[nodiscard]] static constexpr Duration from_chrono(std::chrono::duration<Rep, Period> d) {
        return Duration{std::chrono::duration_cast<std::chrono::nanoseconds>(d).count()};
    }

    [[nodiscard]] constexpr std::int64_t count_nanos() const noexcept { return ns_; }
    [[nodiscard]] constexpr std::int64_t count_micros() const noexcept { return ns_ / 1'000; }
    [[nodiscard]] constexpr std::int64_t count_millis() const noexcept { return ns_ / 1'000'000; }
    [[nodiscard]] constexpr std::int64_t count_seconds() const noexcept {
        return ns_ / 1'000'000'000;
    }
    [[nodiscard]] constexpr double as_seconds() const noexcept {
        return static_cast<double>(ns_) / 1e9;
    }
    [[nodiscard]] constexpr std::chrono::nanoseconds to_chrono() const noexcept {
        return std::chrono::nanoseconds{ns_};
    }

    [[nodiscard]] constexpr bool is_zero() const noexcept { return ns_ == 0; }
    [[nodiscard]] constexpr bool is_negative() const noexcept { return ns_ < 0; }

    constexpr Duration operator-() const noexcept { return Duration{-ns_}; }
    constexpr Duration& operator+=(Duration o) noexcept {
        ns_ += o.ns_;
        return *this;
    }
    constexpr Duration& operator-=(Duration o) noexcept {
        ns_ -= o.ns_;
        return *this;
    }
    friend constexpr Duration operator+(Duration a, Duration b) noexcept { return a += b; }
    friend constexpr Duration operator-(Duration a, Duration b) noexcept { return a -= b; }
    friend constexpr Duration operator*(Duration a, std::int64_t k) noexcept {
        return Duration{a.ns_ * k};
    }
    friend constexpr Duration operator/(Duration a, std::int64_t k) noexcept {
        return Duration{a.ns_ / k};
    }
    friend constexpr std::int64_t operator/(Duration a, Duration b) noexcept {
        return a.ns_ / b.ns_;
    }

    friend constexpr auto operator<=>(Duration, Duration) noexcept = default;
    friend constexpr bool operator==(Duration, Duration) noexcept = default;

    [[nodiscard]] std::string to_string() const;

private:
    constexpr explicit Duration(std::int64_t ns) noexcept : ns_(ns) {}
    std::int64_t ns_ = 0;
};

class Timestamp {
public:
    constexpr Timestamp() noexcept = default;

    [[nodiscard]] static constexpr Timestamp epoch() noexcept { return Timestamp{0}; }
    [[nodiscard]] static constexpr Timestamp from_nanos(std::int64_t ns) noexcept {
        return Timestamp{ns};
    }
    [[nodiscard]] static constexpr Timestamp from_micros(std::int64_t us) noexcept {
        return Timestamp{us * 1'000};
    }
    [[nodiscard]] static constexpr Timestamp from_millis(std::int64_t ms) noexcept {
        return Timestamp{ms * 1'000'000};
    }
    [[nodiscard]] static constexpr Timestamp from_seconds(std::int64_t s) noexcept {
        return Timestamp{s * 1'000'000'000};
    }
    [[nodiscard]] static Timestamp from_chrono(std::chrono::system_clock::time_point tp) {
        return Timestamp{std::chrono::duration_cast<std::chrono::nanoseconds>(
                             tp.time_since_epoch())
                             .count()};
    }

    // Parses ISO-8601 UTC: "YYYY-MM-DDTHH:MM:SS[.fffffffff]Z" or a date-only
    // "YYYY-MM-DD". Anything else is a parse error; timezone offsets other
    // than 'Z' are deliberately unsupported so all stored data is UTC.
    [[nodiscard]] static Result<Timestamp> parse_iso8601(std::string_view text);

    [[nodiscard]] constexpr std::int64_t nanos_since_epoch() const noexcept { return ns_; }
    [[nodiscard]] constexpr std::int64_t micros_since_epoch() const noexcept {
        return ns_ / 1'000;
    }
    [[nodiscard]] constexpr std::int64_t millis_since_epoch() const noexcept {
        return ns_ / 1'000'000;
    }
    [[nodiscard]] constexpr std::int64_t seconds_since_epoch() const noexcept {
        return ns_ / 1'000'000'000;
    }
    [[nodiscard]] std::chrono::system_clock::time_point to_chrono() const {
        return std::chrono::system_clock::time_point{
            std::chrono::duration_cast<std::chrono::system_clock::duration>(
                std::chrono::nanoseconds{ns_})};
    }

    // Truncate to the start of a bucket (e.g. candle open time).
    [[nodiscard]] constexpr Timestamp floor_to(Duration bucket) const noexcept {
        const auto b = bucket.count_nanos();
        std::int64_t q = ns_ / b;
        if (ns_ < 0 && ns_ % b != 0) {
            --q;
        }
        return Timestamp{q * b};
    }

    // "YYYY-MM-DDTHH:MM:SS.fffffffffZ" (always 9 fractional digits).
    [[nodiscard]] std::string to_iso8601() const;

    constexpr Timestamp& operator+=(Duration d) noexcept {
        ns_ += d.count_nanos();
        return *this;
    }
    constexpr Timestamp& operator-=(Duration d) noexcept {
        ns_ -= d.count_nanos();
        return *this;
    }
    friend constexpr Timestamp operator+(Timestamp t, Duration d) noexcept { return t += d; }
    friend constexpr Timestamp operator+(Duration d, Timestamp t) noexcept { return t += d; }
    friend constexpr Timestamp operator-(Timestamp t, Duration d) noexcept { return t -= d; }
    friend constexpr Duration operator-(Timestamp a, Timestamp b) noexcept {
        return Duration::nanos(a.ns_ - b.ns_);
    }

    friend constexpr auto operator<=>(Timestamp, Timestamp) noexcept = default;
    friend constexpr bool operator==(Timestamp, Timestamp) noexcept = default;

private:
    constexpr explicit Timestamp(std::int64_t ns) noexcept : ns_(ns) {}
    std::int64_t ns_ = 0;
};

}  // namespace tradebot
