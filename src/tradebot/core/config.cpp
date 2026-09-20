#include "tradebot/core/config.hpp"

#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

extern char** environ;

namespace tradebot {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

std::string to_lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

Error missing(std::string_view key) {
    return Error{ErrorCode::not_found, "missing config key '" + std::string(key) + "'"};
}

Error bad_value(std::string_view key, std::string_view value, std::string_view expected) {
    return Error{ErrorCode::parse_error, "config key '" + std::string(key) + "' has value '" +
                                             std::string(value) + "', expected " +
                                             std::string(expected)};
}

}  // namespace

Result<Config> Config::parse(std::string_view text) {
    Config cfg;
    std::string section;
    std::size_t line_no = 0;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t end = text.find('\n', pos);
        std::string_view line = text.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos);
        pos = end == std::string_view::npos ? text.size() + 1 : end + 1;
        ++line_no;

        line = trim(line);
        if (line.empty() || line.front() == '#' || line.front() == ';') {
            continue;
        }
        if (line.front() == '[') {
            if (line.back() != ']') {
                return make_error(ErrorCode::parse_error,
                                  "config line " + std::to_string(line_no) + ": unterminated section");
            }
            section = std::string(trim(line.substr(1, line.size() - 2)));
            if (section.empty()) {
                return make_error(ErrorCode::parse_error,
                                  "config line " + std::to_string(line_no) + ": empty section name");
            }
            continue;
        }
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            return make_error(ErrorCode::parse_error,
                              "config line " + std::to_string(line_no) + ": expected 'key = value'");
        }
        const std::string_view key = trim(line.substr(0, eq));
        std::string_view value = trim(line.substr(eq + 1));
        if (key.empty()) {
            return make_error(ErrorCode::parse_error,
                              "config line " + std::to_string(line_no) + ": empty key");
        }
        if (!value.empty() && value.front() == '"') {
            const std::size_t close = value.find('"', 1);
            if (close == std::string_view::npos) {
                return make_error(ErrorCode::parse_error,
                                  "config line " + std::to_string(line_no) + ": unterminated quote");
            }
            const std::string_view rest = trim(value.substr(close + 1));
            if (!rest.empty() && rest.front() != '#' && rest.front() != ';') {
                return make_error(ErrorCode::parse_error,
                                  "config line " + std::to_string(line_no) +
                                      ": unexpected text after closing quote");
            }
            value = value.substr(1, close - 1);
        } else {
            // Strip trailing comment on unquoted values.
            const std::size_t hash = value.find('#');
            if (hash != std::string_view::npos) {
                value = trim(value.substr(0, hash));
            }
        }
        std::string full_key = section.empty() ? std::string(key) : section + "." + std::string(key);
        cfg.values_[std::move(full_key)] = std::string(value);
    }
    return cfg;
}

Result<Config> Config::load_file(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        return make_error(ErrorCode::io_error, "cannot open config file '" + path + "'");
    }
    std::stringstream buf;
    buf << in.rdbuf();
    auto cfg = parse(buf.str());
    if (!cfg) {
        cfg.error().message = path + ": " + cfg.error().message;
    }
    return cfg;
}

Result<Config> Config::load_files(const std::vector<std::string>& paths) {
    if (paths.empty()) {
        return make_error(ErrorCode::invalid_argument, "no config file given");
    }
    Config merged;
    for (const auto& path : paths) {
        auto layer = load_file(path);
        if (!layer) {
            return tl::make_unexpected(layer.error());
        }
        merged.merge(*layer);
    }
    return merged;
}

void Config::merge(const Config& overrides) {
    for (const auto& [k, v] : overrides.values_) {
        values_[k] = v;
    }
}

void Config::apply_env_overrides(std::string_view prefix) {
    for (char** env = environ; env != nullptr && *env != nullptr; ++env) {
        std::string_view entry(*env);
        if (entry.substr(0, prefix.size()) != prefix) {
            continue;
        }
        const std::size_t eq = entry.find('=');
        if (eq == std::string_view::npos) {
            continue;
        }
        std::string_view name = entry.substr(prefix.size(), eq - prefix.size());
        if (name.empty()) {
            continue;
        }
        std::string key = to_lower(name);
        const std::size_t us = key.find('_');
        if (us != std::string::npos) {
            key[us] = '.';
        }
        values_[key] = std::string(entry.substr(eq + 1));
    }
}

void Config::set(std::string key, std::string value) {
    values_[std::move(key)] = std::move(value);
}

bool Config::contains(std::string_view key) const noexcept { return find(key) != nullptr; }

std::vector<std::string> Config::keys() const {
    std::vector<std::string> out;
    out.reserve(values_.size());
    for (const auto& [k, _] : values_) {
        out.push_back(k);
    }
    return out;
}

const std::string* Config::find(std::string_view key) const noexcept {
    auto it = values_.find(key);
    return it == values_.end() ? nullptr : &it->second;
}

Result<std::string> Config::get_string(std::string_view key) const {
    const std::string* v = find(key);
    if (!v) {
        return tl::make_unexpected(missing(key));
    }
    return *v;
}

Result<std::int64_t> Config::get_int(std::string_view key) const {
    const std::string* v = find(key);
    if (!v) {
        return tl::make_unexpected(missing(key));
    }
    std::string_view s = trim(*v);
    std::int64_t out = 0;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    if (ec != std::errc{} || ptr != s.data() + s.size()) {
        return tl::make_unexpected(bad_value(key, *v, "an integer"));
    }
    return out;
}

Result<double> Config::get_double(std::string_view key) const {
    const std::string* v = find(key);
    if (!v) {
        return tl::make_unexpected(missing(key));
    }
    std::string_view s = trim(*v);
    double out = 0;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    if (ec != std::errc{} || ptr != s.data() + s.size()) {
        return tl::make_unexpected(bad_value(key, *v, "a number"));
    }
    return out;
}

Result<bool> Config::get_bool(std::string_view key) const {
    const std::string* v = find(key);
    if (!v) {
        return tl::make_unexpected(missing(key));
    }
    const std::string s = to_lower(trim(*v));
    if (s == "true" || s == "yes" || s == "on" || s == "1") return true;
    if (s == "false" || s == "no" || s == "off" || s == "0") return false;
    return tl::make_unexpected(bad_value(key, *v, "a boolean"));
}

Result<Price> Config::get_price(std::string_view key) const {
    const std::string* v = find(key);
    if (!v) {
        return tl::make_unexpected(missing(key));
    }
    auto r = Price::parse(trim(*v));
    if (!r) {
        return tl::make_unexpected(bad_value(key, *v, "a decimal price"));
    }
    return *r;
}

Result<Quantity> Config::get_quantity(std::string_view key) const {
    const std::string* v = find(key);
    if (!v) {
        return tl::make_unexpected(missing(key));
    }
    auto r = Quantity::parse(trim(*v));
    if (!r) {
        return tl::make_unexpected(bad_value(key, *v, "a decimal quantity"));
    }
    return *r;
}

Result<Notional> Config::get_notional(std::string_view key) const {
    const std::string* v = find(key);
    if (!v) {
        return tl::make_unexpected(missing(key));
    }
    auto r = Notional::parse(trim(*v));
    if (!r) {
        return tl::make_unexpected(bad_value(key, *v, "a decimal notional"));
    }
    return *r;
}

Result<Duration> Config::get_duration(std::string_view key) const {
    const std::string* v = find(key);
    if (!v) {
        return tl::make_unexpected(missing(key));
    }
    auto r = parse_duration(trim(*v));
    if (!r) {
        return tl::make_unexpected(bad_value(key, *v, "a duration such as 500ms, 2s, 5m, 1h"));
    }
    return *r;
}

Result<Timestamp> Config::get_timestamp(std::string_view key) const {
    const std::string* v = find(key);
    if (!v) {
        return tl::make_unexpected(missing(key));
    }
    auto r = Timestamp::parse_iso8601(trim(*v));
    if (!r) {
        return tl::make_unexpected(bad_value(key, *v, "an ISO-8601 UTC timestamp"));
    }
    return *r;
}

Result<std::string> Config::get_string_or(std::string_view key, std::string def) const {
    return contains(key) ? get_string(key) : Result<std::string>(std::move(def));
}
Result<std::int64_t> Config::get_int_or(std::string_view key, std::int64_t def) const {
    return contains(key) ? get_int(key) : Result<std::int64_t>(def);
}
Result<double> Config::get_double_or(std::string_view key, double def) const {
    return contains(key) ? get_double(key) : Result<double>(def);
}
Result<bool> Config::get_bool_or(std::string_view key, bool def) const {
    return contains(key) ? get_bool(key) : Result<bool>(def);
}
Result<Duration> Config::get_duration_or(std::string_view key, Duration def) const {
    return contains(key) ? get_duration(key) : Result<Duration>(def);
}

Config Config::section(std::string_view name) const {
    Config out;
    const std::string prefix = std::string(name) + ".";
    for (const auto& [k, v] : values_) {
        if (k.size() > prefix.size() && k.compare(0, prefix.size(), prefix) == 0) {
            out.values_[k.substr(prefix.size())] = v;
        }
    }
    return out;
}

std::string Config::to_string() const {
    std::string out;
    for (const auto& [k, v] : values_) {
        out += k;
        out += " = ";
        out += v;
        out += '\n';
    }
    return out;
}

Result<Duration> parse_duration(std::string_view text) {
    text = trim(text);
    if (text.empty()) {
        return make_error(ErrorCode::parse_error, "empty duration");
    }
    std::size_t i = 0;
    while (i < text.size() && (text[i] >= '0' && text[i] <= '9')) {
        ++i;
    }
    if (i == 0) {
        return make_error(ErrorCode::parse_error, "duration must start with digits");
    }
    std::int64_t n = 0;
    std::from_chars(text.data(), text.data() + i, n);
    const std::string_view unit = trim(text.substr(i));
    if (unit == "ns") return Duration::nanos(n);
    if (unit == "us") return Duration::micros(n);
    if (unit == "ms") return Duration::millis(n);
    if (unit == "s") return Duration::seconds(n);
    if (unit == "m") return Duration::minutes(n);
    if (unit == "h") return Duration::hours(n);
    if (unit == "d") return Duration::days(n);
    return make_error(ErrorCode::parse_error,
                      "unknown duration unit '" + std::string(unit) + "'");
}

}  // namespace tradebot
