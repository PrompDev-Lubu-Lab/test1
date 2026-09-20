#pragma once

// Execution journal: an append-only, fsynced record of every execution
// report, written before anything else reacts to it. If state.json is
// lost or corrupt, the portfolio is rebuilt by replaying the journal; in
// live trading it is also the audit trail that reconciliation checks the
// venue's own history against.

#include "tradebot/core/error.hpp"
#include "tradebot/execution/types.hpp"

#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>

namespace tradebot::live {

[[nodiscard]] std::string encode_report(const execution::ExecutionReport& report);
[[nodiscard]] Result<execution::ExecutionReport> decode_report(std::string_view line);

class Journal {
public:
    Journal() = default;
    ~Journal();
    Journal(const Journal&) = delete;
    Journal& operator=(const Journal&) = delete;

    // Opens for append (creates if missing).
    [[nodiscard]] Result<void> open(const std::filesystem::path& path);
    // Appends one report and flushes it to the OS; `sync` also fsyncs.
    [[nodiscard]] Result<void> append(const execution::ExecutionReport& report, bool sync = true);
    [[nodiscard]] Result<void> close();
    [[nodiscard]] std::uint64_t appended() const noexcept { return appended_; }

    // Replays every report in the file in order. Corrupt lines are skipped
    // and counted (returned), never fatal.
    [[nodiscard]] static Result<std::uint64_t> replay(
        const std::filesystem::path& path,
        const std::function<void(const execution::ExecutionReport&)>& handler);

private:
    std::FILE* file_ = nullptr;
    std::filesystem::path path_;
    std::uint64_t appended_ = 0;
};

}  // namespace tradebot::live
