#pragma once

// Minimal structured logger.
//
// Design goals: no third-party dependency, std::format for messages, a
// pluggable sink so tests can capture output and the backtester can route
// logs into run artifacts, and an injected Clock so log lines in a backtest
// carry simulated time (which is what you want when debugging a strategy).

#include "tradebot/core/clock.hpp"

#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace tradebot {

enum class LogLevel : std::uint8_t { trace, debug, info, warn, error, off };

[[nodiscard]] constexpr std::string_view to_string(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::trace: return "TRACE";
        case LogLevel::debug: return "DEBUG";
        case LogLevel::info: return "INFO";
        case LogLevel::warn: return "WARN";
        case LogLevel::error: return "ERROR";
        case LogLevel::off: return "OFF";
    }
    return "?";
}

struct LogRecord {
    Timestamp time;
    LogLevel level;
    std::string_view component;
    std::string_view message;
};

class LogSink {
public:
    virtual ~LogSink() = default;
    virtual void write(const LogRecord& record) = 0;
    virtual void flush() {}
};

// Writes "<iso-time> <LEVEL> [component] message\n" to stderr.
class StderrSink final : public LogSink {
public:
    void write(const LogRecord& record) override;
    void flush() override;

private:
    std::mutex mutex_;
};

// Collects records in memory; used by tests and by run artifacts.
class MemorySink final : public LogSink {
public:
    struct Entry {
        Timestamp time;
        LogLevel level;
        std::string component;
        std::string message;
    };

    void write(const LogRecord& record) override;
    [[nodiscard]] std::vector<Entry> entries() const;
    void clear();

private:
    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
};

// A Logger is cheap to copy: it is a component name plus a shared handle to
// the sink/clock/level configuration.
class Logger {
public:
    struct Config {
        std::shared_ptr<LogSink> sink;
        const Clock* clock = nullptr;  // non-owning; must outlive the logger
        LogLevel level = LogLevel::info;
    };

    Logger() = default;
    Logger(std::string component, std::shared_ptr<Config> config)
        : component_(std::move(component)), config_(std::move(config)) {}

    // Derive a logger for a sub-component sharing the same configuration.
    [[nodiscard]] Logger child(std::string_view name) const {
        return Logger(component_ + "." + std::string(name), config_);
    }

    [[nodiscard]] bool enabled(LogLevel level) const noexcept {
        return config_ && config_->sink && level >= config_->level && level != LogLevel::off;
    }

    void set_level(LogLevel level) noexcept {
        if (config_) {
            config_->level = level;
        }
    }

    template <class... Args>
    void log(LogLevel level, std::format_string<Args...> fmt, Args&&... args) const {
        if (!enabled(level)) {
            return;
        }
        emit(level, std::format(fmt, std::forward<Args>(args)...));
    }

    template <class... Args>
    void trace(std::format_string<Args...> fmt, Args&&... args) const {
        log(LogLevel::trace, fmt, std::forward<Args>(args)...);
    }
    template <class... Args>
    void debug(std::format_string<Args...> fmt, Args&&... args) const {
        log(LogLevel::debug, fmt, std::forward<Args>(args)...);
    }
    template <class... Args>
    void info(std::format_string<Args...> fmt, Args&&... args) const {
        log(LogLevel::info, fmt, std::forward<Args>(args)...);
    }
    template <class... Args>
    void warn(std::format_string<Args...> fmt, Args&&... args) const {
        log(LogLevel::warn, fmt, std::forward<Args>(args)...);
    }
    template <class... Args>
    void error(std::format_string<Args...> fmt, Args&&... args) const {
        log(LogLevel::error, fmt, std::forward<Args>(args)...);
    }

    void flush() const {
        if (config_ && config_->sink) {
            config_->sink->flush();
        }
    }

    [[nodiscard]] const std::string& component() const noexcept { return component_; }

    // Convenience constructors.
    [[nodiscard]] static Logger make(std::string component, std::shared_ptr<LogSink> sink,
                                     const Clock& clock, LogLevel level = LogLevel::info) {
        auto cfg = std::make_shared<Config>();
        cfg->sink = std::move(sink);
        cfg->clock = &clock;
        cfg->level = level;
        return Logger(std::move(component), std::move(cfg));
    }

    // Stderr logger on the wall clock; the default for tools and live processes.
    [[nodiscard]] static Logger stderr_logger(std::string component,
                                              LogLevel level = LogLevel::info);

private:
    void emit(LogLevel level, const std::string& message) const;

    std::string component_;
    std::shared_ptr<Config> config_;
};

[[nodiscard]] Result<LogLevel> parse_log_level(std::string_view text);

}  // namespace tradebot
