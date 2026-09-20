#include "tradebot/core/log.hpp"

#include <cstdio>
#include <vector>

namespace tradebot {

void StderrSink::write(const LogRecord& record) {
    const std::string line = std::format("{} {:<5} [{}] {}\n", record.time.to_iso8601(),
                                         to_string(record.level), record.component,
                                         record.message);
    std::lock_guard lock(mutex_);
    std::fputs(line.c_str(), stderr);
}

void StderrSink::flush() {
    std::lock_guard lock(mutex_);
    std::fflush(stderr);
}

void MemorySink::write(const LogRecord& record) {
    std::lock_guard lock(mutex_);
    entries_.push_back(Entry{record.time, record.level, std::string(record.component),
                             std::string(record.message)});
}

std::vector<MemorySink::Entry> MemorySink::entries() const {
    std::lock_guard lock(mutex_);
    return entries_;
}

void MemorySink::clear() {
    std::lock_guard lock(mutex_);
    entries_.clear();
}

void Logger::emit(LogLevel level, const std::string& message) const {
    const Timestamp now = config_->clock ? config_->clock->now() : Timestamp::epoch();
    config_->sink->write(LogRecord{now, level, component_, message});
}

Logger Logger::stderr_logger(std::string component, LogLevel level) {
    static const WallClock wall_clock;
    static const auto sink = std::make_shared<StderrSink>();
    return make(std::move(component), sink, wall_clock, level);
}

Result<LogLevel> parse_log_level(std::string_view text) {
    if (text == "trace") return LogLevel::trace;
    if (text == "debug") return LogLevel::debug;
    if (text == "info") return LogLevel::info;
    if (text == "warn" || text == "warning") return LogLevel::warn;
    if (text == "error") return LogLevel::error;
    if (text == "off") return LogLevel::off;
    return make_error(ErrorCode::parse_error, "invalid log level: '" + std::string(text) + "'");
}

}  // namespace tradebot
