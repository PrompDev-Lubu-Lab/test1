#pragma once

// Flat, typed key/value configuration.
//
// Format is a deliberately tiny INI/TOML subset so there is no parser
// dependency:
//
//   # comment
//   [section]
//   key = value            -> "section.key"
//   name = "quoted string"
//
// Values are stored as strings and converted on access, so a wrong type is
// reported at the point of use with the key name in the error. Environment
// variables of the form TRADEBOT_SECTION_KEY override file values, which is
// how deployments inject secrets without writing them to disk.

#include "tradebot/core/error.hpp"
#include "tradebot/core/fixed_point.hpp"
#include "tradebot/core/time.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tradebot {

class Config {
public:
    Config() = default;

    [[nodiscard]] static Result<Config> parse(std::string_view text);
    [[nodiscard]] static Result<Config> load_file(const std::string& path);
    // Layered configuration: later files override earlier ones key by key
    // (base -> strategy -> environment), so a deployment never edits the
    // file that research produced.
    [[nodiscard]] static Result<Config> load_files(const std::vector<std::string>& paths);
    void merge(const Config& overrides);

    // Apply TRADEBOT_* environment overrides ("TRADEBOT_EXCHANGE_API_KEY"
    // sets "exchange.api_key"; only the first '_' splits section from key).
    void apply_env_overrides(std::string_view prefix = "TRADEBOT_");

    void set(std::string key, std::string value);
    [[nodiscard]] bool contains(std::string_view key) const noexcept;
    [[nodiscard]] std::vector<std::string> keys() const;

    // Typed getters. Missing key -> not_found; bad value -> parse_error.
    [[nodiscard]] Result<std::string> get_string(std::string_view key) const;
    [[nodiscard]] Result<std::int64_t> get_int(std::string_view key) const;
    [[nodiscard]] Result<double> get_double(std::string_view key) const;
    [[nodiscard]] Result<bool> get_bool(std::string_view key) const;
    [[nodiscard]] Result<Price> get_price(std::string_view key) const;
    [[nodiscard]] Result<Quantity> get_quantity(std::string_view key) const;
    [[nodiscard]] Result<Notional> get_notional(std::string_view key) const;
    // Durations accept a unit suffix: "500ms", "2s", "5m", "1h", "3d", "250us", "10ns".
    [[nodiscard]] Result<Duration> get_duration(std::string_view key) const;
    [[nodiscard]] Result<Timestamp> get_timestamp(std::string_view key) const;

    // Getters with defaults: missing key returns the default, but a present
    // key with an unparsable value is still an error.
    [[nodiscard]] Result<std::string> get_string_or(std::string_view key, std::string def) const;
    [[nodiscard]] Result<std::int64_t> get_int_or(std::string_view key, std::int64_t def) const;
    [[nodiscard]] Result<double> get_double_or(std::string_view key, double def) const;
    [[nodiscard]] Result<bool> get_bool_or(std::string_view key, bool def) const;
    [[nodiscard]] Result<Duration> get_duration_or(std::string_view key, Duration def) const;

    // A view restricted to one section, with the prefix stripped, so a
    // component can be handed only its own settings.
    [[nodiscard]] Config section(std::string_view name) const;

    [[nodiscard]] std::string to_string() const;

private:
    [[nodiscard]] const std::string* find(std::string_view key) const noexcept;

    std::map<std::string, std::string, std::less<>> values_;
};

[[nodiscard]] Result<Duration> parse_duration(std::string_view text);

}  // namespace tradebot
