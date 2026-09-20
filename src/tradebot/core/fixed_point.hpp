#pragma once

// Fixed-point decimal arithmetic for prices, quantities and notionals.
//
// All monetary values are stored as a signed 64-bit integer count of
// 1e-8 units. This gives exact decimal arithmetic (no binary floating point
// rounding), a range of roughly +/- 92 billion, and 8 decimal places, which
// covers every major crypto venue's precision.
//
// Price, Quantity and Notional are distinct types so that meaningless
// operations (adding a price to a quantity) fail to compile, while the
// meaningful cross-type products are provided explicitly.

#include "tradebot/core/error.hpp"

#include <charconv>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace tradebot {

enum class RoundingMode { down, up, nearest };

namespace detail {

__extension__ typedef __int128 int128;

constexpr std::int64_t kFixedScale = 100'000'000;  // 1e-8
constexpr int kFixedDigits = 8;

[[noreturn]] inline void throw_overflow(const char* what) {
    throw std::overflow_error(what);
}

constexpr std::int64_t checked_add(std::int64_t a, std::int64_t b) {
    std::int64_t out = 0;
    if (__builtin_add_overflow(a, b, &out)) {
        throw_overflow("fixed-point addition overflow");
    }
    return out;
}

constexpr std::int64_t checked_sub(std::int64_t a, std::int64_t b) {
    std::int64_t out = 0;
    if (__builtin_sub_overflow(a, b, &out)) {
        throw_overflow("fixed-point subtraction overflow");
    }
    return out;
}

constexpr std::int64_t checked_mul(std::int64_t a, std::int64_t b) {
    std::int64_t out = 0;
    if (__builtin_mul_overflow(a, b, &out)) {
        throw_overflow("fixed-point multiplication overflow");
    }
    return out;
}

constexpr std::int64_t narrow(int128 v) {
    if (v > std::numeric_limits<std::int64_t>::max() ||
        v < std::numeric_limits<std::int64_t>::min()) {
        throw_overflow("fixed-point result out of range");
    }
    return static_cast<std::int64_t>(v);
}

// Divide with explicit rounding mode. Rounding semantics are symmetric
// around zero for `nearest` (half away from zero) and directional for
// `down` (toward -inf) and `up` (toward +inf).
constexpr int128 div_round(int128 num, int128 den, RoundingMode mode) {
    if (den == 0) {
        throw std::domain_error("fixed-point division by zero");
    }
    int128 q = num / den;
    int128 r = num % den;
    if (r == 0) {
        return q;
    }
    const bool negative = (num < 0) != (den < 0);
    switch (mode) {
        case RoundingMode::down:
            return negative ? q - 1 : q;
        case RoundingMode::up:
            return negative ? q : q + 1;
        case RoundingMode::nearest: {
            int128 abs_r = r < 0 ? -r : r;
            int128 abs_den = den < 0 ? -den : den;
            if (abs_r * 2 >= abs_den) {
                return negative ? q - 1 : q + 1;
            }
            return q;
        }
    }
    return q;
}

// (a * b) / scale
constexpr std::int64_t mul_scaled(std::int64_t a, std::int64_t b, RoundingMode mode) {
    return narrow(div_round(static_cast<int128>(a) * b, kFixedScale, mode));
}

// (a * scale) / b
constexpr std::int64_t div_scaled(std::int64_t a, std::int64_t b, RoundingMode mode) {
    return narrow(div_round(static_cast<int128>(a) * kFixedScale, b, mode));
}

Result<std::int64_t> parse_fixed_raw(std::string_view text);
std::string format_fixed_raw(std::int64_t raw);

}  // namespace detail

template <class Tag>
class Fixed {
public:
    using raw_type = std::int64_t;
    static constexpr raw_type scale = detail::kFixedScale;
    static constexpr int decimals = detail::kFixedDigits;

    constexpr Fixed() noexcept = default;

    [[nodiscard]] static constexpr Fixed from_raw(raw_type raw) noexcept {
        Fixed f;
        f.raw_ = raw;
        return f;
    }

    [[nodiscard]] static constexpr Fixed from_int(std::int64_t whole) {
        return from_raw(detail::checked_mul(whole, scale));
    }

    // Lossy: use only at the boundary with external systems (e.g. JSON that
    // delivers numbers as doubles). Rounds to nearest unit.
    [[nodiscard]] static Fixed from_double(double v) {
        const long double scaled = static_cast<long double>(v) * static_cast<long double>(scale);
        if (scaled >= static_cast<long double>(std::numeric_limits<raw_type>::max()) ||
            scaled <= static_cast<long double>(std::numeric_limits<raw_type>::min())) {
            detail::throw_overflow("double out of fixed-point range");
        }
        const long double rounded = scaled < 0 ? scaled - 0.5L : scaled + 0.5L;
        return from_raw(static_cast<raw_type>(rounded));
    }

    // Exact: parses decimal text such as "1234.56789012". Rejects more than
    // 8 fractional digits rather than silently truncating.
    [[nodiscard]] static Result<Fixed> parse(std::string_view text) {
        auto raw = detail::parse_fixed_raw(text);
        if (!raw) {
            return tl::make_unexpected(raw.error());
        }
        return from_raw(*raw);
    }

    [[nodiscard]] constexpr raw_type raw() const noexcept { return raw_; }
    [[nodiscard]] double to_double() const noexcept {
        return static_cast<double>(raw_) / static_cast<double>(scale);
    }
    [[nodiscard]] std::string to_string() const { return detail::format_fixed_raw(raw_); }

    [[nodiscard]] constexpr bool is_zero() const noexcept { return raw_ == 0; }
    [[nodiscard]] constexpr bool is_positive() const noexcept { return raw_ > 0; }
    [[nodiscard]] constexpr bool is_negative() const noexcept { return raw_ < 0; }
    [[nodiscard]] constexpr Fixed abs() const {
        if (raw_ == std::numeric_limits<raw_type>::min()) {
            detail::throw_overflow("abs overflow");
        }
        return from_raw(raw_ < 0 ? -raw_ : raw_);
    }

    // Round to a multiple of `step` (e.g. tick size or lot size).
    [[nodiscard]] constexpr Fixed round_to(Fixed step, RoundingMode mode) const {
        if (step.raw_ <= 0) {
            throw std::invalid_argument("round_to step must be positive");
        }
        const auto units = detail::div_round(raw_, step.raw_, mode);
        return from_raw(detail::narrow(units * step.raw_));
    }

    [[nodiscard]] constexpr bool is_multiple_of(Fixed step) const noexcept {
        return step.raw_ > 0 && raw_ % step.raw_ == 0;
    }

    constexpr Fixed operator-() const {
        if (raw_ == std::numeric_limits<raw_type>::min()) {
            detail::throw_overflow("negation overflow");
        }
        return from_raw(-raw_);
    }

    constexpr Fixed& operator+=(Fixed o) {
        raw_ = detail::checked_add(raw_, o.raw_);
        return *this;
    }
    constexpr Fixed& operator-=(Fixed o) {
        raw_ = detail::checked_sub(raw_, o.raw_);
        return *this;
    }

    friend constexpr Fixed operator+(Fixed a, Fixed b) { return a += b; }
    friend constexpr Fixed operator-(Fixed a, Fixed b) { return a -= b; }

    // Scale by an integer factor (e.g. number of contracts).
    friend constexpr Fixed operator*(Fixed a, std::int64_t k) {
        return from_raw(detail::checked_mul(a.raw_, k));
    }
    friend constexpr Fixed operator*(std::int64_t k, Fixed a) { return a * k; }

    // Scale by a rational factor with explicit rounding, e.g. fee rate 0.001
    // expressed as mul_ratio(1, 1000). Never goes through floating point.
    [[nodiscard]] constexpr Fixed mul_ratio(std::int64_t num, std::int64_t den,
                                            RoundingMode mode = RoundingMode::nearest) const {
        return from_raw(detail::narrow(
            detail::div_round(static_cast<detail::int128>(raw_) * num, den, mode)));
    }

    // Ratio of two like quantities as a plain double (for analytics only).
    friend constexpr double ratio(Fixed a, Fixed b) {
        return static_cast<double>(a.raw_) / static_cast<double>(b.raw_);
    }

    friend constexpr auto operator<=>(Fixed, Fixed) noexcept = default;
    friend constexpr bool operator==(Fixed, Fixed) noexcept = default;

private:
    raw_type raw_ = 0;
};

struct PriceTag {};
struct QuantityTag {};
struct NotionalTag {};

using Price = Fixed<PriceTag>;
using Quantity = Fixed<QuantityTag>;
using Notional = Fixed<NotionalTag>;

// Cross-type arithmetic. Each product/quotient names its rounding mode
// explicitly so callers cannot accidentally round in the venue's favour or
// their own.
[[nodiscard]] constexpr Notional notional(Price p, Quantity q,
                                          RoundingMode mode = RoundingMode::nearest) {
    return Notional::from_raw(detail::mul_scaled(p.raw(), q.raw(), mode));
}

[[nodiscard]] constexpr Quantity quantity_for(Notional n, Price p,
                                              RoundingMode mode = RoundingMode::down) {
    return Quantity::from_raw(detail::div_scaled(n.raw(), p.raw(), mode));
}

[[nodiscard]] constexpr Price price_for(Notional n, Quantity q,
                                        RoundingMode mode = RoundingMode::nearest) {
    return Price::from_raw(detail::div_scaled(n.raw(), q.raw(), mode));
}

// Literal helpers for tests and configuration: 1.5_px, 0.25_qty
namespace literals {
inline Price operator""_px(const char* text, std::size_t len) {
    auto r = Price::parse(std::string_view(text, len));
    if (!r) {
        throw std::invalid_argument(r.error().to_string());
    }
    return *r;
}
inline Quantity operator""_qty(const char* text, std::size_t len) {
    auto r = Quantity::parse(std::string_view(text, len));
    if (!r) {
        throw std::invalid_argument(r.error().to_string());
    }
    return *r;
}
inline Notional operator""_ntl(const char* text, std::size_t len) {
    auto r = Notional::parse(std::string_view(text, len));
    if (!r) {
        throw std::invalid_argument(r.error().to_string());
    }
    return *r;
}
}  // namespace literals

// --- inline implementations -------------------------------------------------

namespace detail {

inline Result<std::int64_t> parse_fixed_raw(std::string_view text) {
    if (text.empty()) {
        return make_error(ErrorCode::parse_error, "empty decimal string");
    }
    std::size_t pos = 0;
    bool negative = false;
    if (text[pos] == '-' || text[pos] == '+') {
        negative = text[pos] == '-';
        ++pos;
    }
    if (pos >= text.size()) {
        return make_error(ErrorCode::parse_error, "decimal string has no digits");
    }

    int128 whole = 0;
    bool any_digit = false;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
        whole = whole * 10 + (text[pos] - '0');
        if (whole > std::numeric_limits<std::int64_t>::max() / kFixedScale) {
            return make_error(ErrorCode::overflow, "decimal string too large");
        }
        any_digit = true;
        ++pos;
    }

    int128 frac = 0;
    int frac_digits = 0;
    if (pos < text.size() && text[pos] == '.') {
        ++pos;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
            if (frac_digits == kFixedDigits) {
                // Allow trailing zeros beyond precision, reject anything else.
                if (text[pos] != '0') {
                    return make_error(ErrorCode::parse_error,
                                      "more than 8 fractional digits in decimal string");
                }
            } else {
                frac = frac * 10 + (text[pos] - '0');
                ++frac_digits;
            }
            any_digit = true;
            ++pos;
        }
    }

    if (!any_digit || pos != text.size()) {
        return make_error(ErrorCode::parse_error,
                          "invalid decimal string: '" + std::string(text) + "'");
    }
    for (int i = frac_digits; i < kFixedDigits; ++i) {
        frac *= 10;
    }
    int128 raw = whole * kFixedScale + frac;
    if (negative) {
        raw = -raw;
    }
    if (raw > std::numeric_limits<std::int64_t>::max() ||
        raw < std::numeric_limits<std::int64_t>::min()) {
        return make_error(ErrorCode::overflow, "decimal string out of range");
    }
    return static_cast<std::int64_t>(raw);
}

inline std::string format_fixed_raw(std::int64_t raw) {
    // Work in unsigned magnitude to handle INT64_MIN.
    const bool negative = raw < 0;
    std::uint64_t mag = negative ? (~static_cast<std::uint64_t>(raw) + 1u)
                                 : static_cast<std::uint64_t>(raw);
    const auto uscale = static_cast<std::uint64_t>(kFixedScale);
    std::uint64_t whole = mag / uscale;
    std::uint64_t frac = mag % uscale;

    std::string out;
    if (negative) {
        out.push_back('-');
    }
    out += std::to_string(whole);
    if (frac != 0) {
        char buf[kFixedDigits + 1];
        for (int i = kFixedDigits - 1; i >= 0; --i) {
            buf[i] = static_cast<char>('0' + frac % 10);
            frac /= 10;
        }
        buf[kFixedDigits] = '\0';
        std::string_view digits(buf, kFixedDigits);
        while (!digits.empty() && digits.back() == '0') {
            digits.remove_suffix(1);
        }
        out.push_back('.');
        out.append(digits);
    }
    return out;
}

}  // namespace detail

}  // namespace tradebot
