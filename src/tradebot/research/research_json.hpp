#pragma once

// JSON forms of the research results, so that what tradebot-research prints
// can also be persisted and read by tools that never see stdout (the
// dashboard's Research tab, tradebot-monitor). Field names are the struct
// field names; timestamps are ISO-8601; durations are seconds; fractions
// and scores are numbers; nothing monetary appears here except through the
// embedded analytics report, where it is a decimal string.

#include "tradebot/research/research.hpp"
#include "tradebot/research/validation.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>

namespace tradebot::research {

[[nodiscard]] nlohmann::json to_json(const KillCriteria& c);
[[nodiscard]] nlohmann::json to_json(const Verdict& v);
[[nodiscard]] nlohmann::json to_json(const MonteCarloResult& r);
[[nodiscard]] nlohmann::json to_json(const GridResult& r);
[[nodiscard]] nlohmann::json to_json(const RegimeResult& r);
[[nodiscard]] nlohmann::json to_json(const ConsistencyResult& r);
[[nodiscard]] nlohmann::json to_json(const WalkForwardResult& r);
[[nodiscard]] nlohmann::json to_json(const GoNoGo& g);
// A ranked comparison (sweep output): rows in rank order with the metric
// they were sorted by.
[[nodiscard]] nlohmann::json to_json(const std::vector<Ranked>& rows, SelectMetric by);

// Everything `tradebot-research validate` computes for one run. Sections
// that were not run are absent from the JSON, not null.
struct ValidationReport {
    std::string run_id;
    analytics::PerformanceReport report;
    KillCriteria criteria;
    Verdict verdict;
    std::optional<MonteCarloResult> monte_carlo;
    std::optional<GridResult> costs;
    std::optional<GridResult> stability;
    std::optional<RegimeResult> regimes;
    std::optional<WalkForwardResult> walk_forward;
    std::optional<ConsistencyResult> consistency;
    GoNoGo go_no_go;
};

[[nodiscard]] nlohmann::json to_json(const ValidationReport& v);

// Writes `j.dump(2)` atomically (temp file + rename), creating the parent
// directory if needed.
[[nodiscard]] Result<void> write_json(const std::filesystem::path& file, const nlohmann::json& j);

}  // namespace tradebot::research
