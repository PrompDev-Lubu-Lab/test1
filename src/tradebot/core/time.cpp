#include "tradebot/core/time.hpp"

#include <charconv>
#include <cstdio>
#include <ctime>
#include <format>

namespace tradebot {

namespace {

constexpr std::int64_t kNanosPerSecond = 1'000'000'000;

// Days from civil date, algorithm by Howard Hinnant (public domain).
constexpr std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) noexcept {
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

constexpr void civil_from_days(std::int64_t z, std::int64_t& y, unsigned& m,
                               unsigned& d) noexcept {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const auto doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y += (m <= 2);
}

bool parse_uint(std::string_view text, std::size_t& pos, std::size_t digits, std::int64_t& out) {
    if (pos + digits > text.size()) {
        return false;
    }
    std::int64_t v = 0;
    for (std::size_t i = 0; i < digits; ++i) {
        const char c = text[pos + i];
        if (c < '0' || c > '9') {
            return false;
        }
        v = v * 10 + (c - '0');
    }
    pos += digits;
    out = v;
    return true;
}

bool expect_char(std::string_view text, std::size_t& pos, char c) {
    if (pos < text.size() && text[pos] == c) {
        ++pos;
        return true;
    }
    return false;
}

}  // namespace

std::string Duration::to_string() const {
    char buf[64];
    const std::int64_t abs_ns = ns_ < 0 ? -ns_ : ns_;
    const char* sign = ns_ < 0 ? "-" : "";
    if (abs_ns >= 86400LL * kNanosPerSecond) {
        std::snprintf(buf, sizeof buf, "%s%.2fd", sign, static_cast<double>(abs_ns) / 86400e9);
    } else if (abs_ns >= 3600LL * kNanosPerSecond) {
        std::snprintf(buf, sizeof buf, "%s%.2fh", sign, static_cast<double>(abs_ns) / 3600e9);
    } else if (abs_ns >= 60LL * kNanosPerSecond) {
        std::snprintf(buf, sizeof buf, "%s%.2fm", sign, static_cast<double>(abs_ns) / 60e9);
    } else if (abs_ns >= kNanosPerSecond) {
        std::snprintf(buf, sizeof buf, "%s%.3fs", sign, static_cast<double>(abs_ns) / 1e9);
    } else if (abs_ns >= 1'000'000) {
        std::snprintf(buf, sizeof buf, "%s%.3fms", sign, static_cast<double>(abs_ns) / 1e6);
    } else if (abs_ns >= 1'000) {
        std::snprintf(buf, sizeof buf, "%s%.3fus", sign, static_cast<double>(abs_ns) / 1e3);
    } else {
        std::snprintf(buf, sizeof buf, "%s%lldns", sign, static_cast<long long>(abs_ns));
    }
    return buf;
}

Result<Timestamp> Timestamp::parse_iso8601(std::string_view text) {
    auto fail = [&] {
        return make_error(ErrorCode::parse_error,
                          "invalid ISO-8601 UTC timestamp: '" + std::string(text) + "'");
    };

    std::size_t pos = 0;
    std::int64_t year = 0, month = 0, day = 0;
    if (!parse_uint(text, pos, 4, year) || !expect_char(text, pos, '-') ||
        !parse_uint(text, pos, 2, month) || !expect_char(text, pos, '-') ||
        !parse_uint(text, pos, 2, day)) {
        return fail();
    }
    if (month < 1 || month > 12 || day < 1 || day > 31) {
        return fail();
    }

    std::int64_t hour = 0, minute = 0, second = 0, frac_ns = 0;
    if (pos != text.size()) {
        if (!expect_char(text, pos, 'T') || !parse_uint(text, pos, 2, hour) ||
            !expect_char(text, pos, ':') || !parse_uint(text, pos, 2, minute) ||
            !expect_char(text, pos, ':') || !parse_uint(text, pos, 2, second)) {
            return fail();
        }
        if (hour > 23 || minute > 59 || second > 60) {
            return fail();
        }
        if (expect_char(text, pos, '.')) {
            int digits = 0;
            while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
                if (digits < 9) {
                    frac_ns = frac_ns * 10 + (text[pos] - '0');
                    ++digits;
                }
                ++pos;
            }
            if (digits == 0) {
                return fail();
            }
            for (; digits < 9; ++digits) {
                frac_ns *= 10;
            }
        }
        if (!expect_char(text, pos, 'Z') || pos != text.size()) {
            return fail();
        }
    }

    const std::int64_t days =
        days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
    const std::int64_t secs = days * 86400 + hour * 3600 + minute * 60 + second;
    return Timestamp::from_nanos(secs * kNanosPerSecond + frac_ns);
}

std::string Timestamp::to_iso8601() const {
    std::int64_t secs = ns_ / kNanosPerSecond;
    std::int64_t frac = ns_ % kNanosPerSecond;
    if (frac < 0) {
        frac += kNanosPerSecond;
        --secs;
    }
    std::int64_t days = secs / 86400;
    std::int64_t sod = secs % 86400;
    if (sod < 0) {
        sod += 86400;
        --days;
    }
    std::int64_t year = 0;
    unsigned month = 0, day = 0;
    civil_from_days(days, year, month, day);

    return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:09}Z", year, month, day, sod / 3600,
                       (sod / 60) % 60, sod % 60, frac);
}

}  // namespace tradebot
