// tradebot-monitor: watch a running strategy and decide whether it is still
// the strategy research approved.
//
//   tradebot-monitor status RUN_DIR [--max-age 45s]
//       Prints status.json (written by the runtime on the heartbeat cadence)
//       and exits 0 if the heartbeat is fresh, 1 if stale, 2 if missing.
//   tradebot-monitor drift --run DIR --reference DIR [--min-samples N]
//       Compares the run with the reference (the backtest or paper run that
//       passed go/no-go); writes drift.txt/drift.json into the run dir.
//       Exit 0 ok, 1 warn, 2 alarm.
//   tradebot-monitor revalidate --run DIR --reference DIR [--config FILE] [--min-trades N]
//       Drift plus kill criteria plus, with --config, a backtest of the
//       same spec over the run's window (consistency). Writes
//       revalidation.txt; exit 0 keep, 1 watch, 2 retire.

#include "tradebot/analytics/analytics.hpp"
#include "tradebot/backtest/backtest.hpp"
#include "tradebot/core/config.hpp"
#include "tradebot/core/log.hpp"
#include "tradebot/live/feed_health.hpp"
#include "tradebot/research/drift.hpp"
#include "tradebot/research/validation.hpp"
#include "tradebot/strategies/advanced.hpp"
#include "tradebot/strategies/baselines.hpp"

#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

namespace {

int usage(const char* argv0) {
    std::fprintf(stderr,
                 "usage:\n"
                 "  %s status RUN_DIR [--max-age 45s]\n"
                 "  %s drift --run DIR --reference DIR [--min-samples N]\n"
                 "  %s revalidate --run DIR --reference DIR [--config FILE] [--min-trades N]\n",
                 argv0, argv0, argv0);
    return 3;
}

std::map<std::string, std::string> parse_flags(int argc, char** argv, int start) {
    std::map<std::string, std::string> flags;
    for (int i = start; i + 1 < argc; i += 2) {
        flags[argv[i]] = argv[i + 1];
    }
    return flags;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace tradebot;
    if (argc < 3) return usage(argv[0]);
    const std::string mode = argv[1];

    if (mode == "status") {
        const std::filesystem::path dir = argv[2];
        auto flags = parse_flags(argc, argv, 3);
        Duration max_age = Duration::seconds(45);
        if (flags.contains("--max-age")) {
            auto d = parse_duration(flags["--max-age"]);
            if (!d) return usage(argv[0]);
            max_age = *d;
        }
        std::ifstream in(dir / "status.json");
        if (in) {
            std::stringstream ss;
            ss << in.rdbuf();
            std::fputs(ss.str().c_str(), stdout);
        }
        WallClock clock;
        auto age = live::heartbeat_age(dir / "heartbeat", clock.now());
        if (!age) {
            std::printf("heartbeat: missing\n");
            return 2;
        }
        const bool fresh = *age <= max_age;
        std::printf("heartbeat: %s (age %s)\n", fresh ? "fresh" : "STALE", age->to_string().c_str());
        return fresh ? 0 : 1;
    }

    auto flags = parse_flags(argc, argv, 2);
    if (!flags.contains("--run") || !flags.contains("--reference")) return usage(argv[0]);
    const std::filesystem::path run = flags["--run"];
    const std::filesystem::path reference = flags["--reference"];
    research::DriftThresholds thresholds;
    if (flags.contains("--min-samples")) thresholds.min_samples = std::stoul(flags["--min-samples"]);

    auto drift = research::detect_drift_dirs(reference, run, thresholds);
    if (!drift) {
        std::fprintf(stderr, "%s\n", drift.error().to_string().c_str());
        return 3;
    }

    if (mode == "drift") {
        std::fputs(research::format_drift(*drift).c_str(), stdout);
        if (auto w = research::write_drift(*drift, run); !w) {
            std::fprintf(stderr, "%s\n", w.error().to_string().c_str());
            return 3;
        }
        return static_cast<int>(drift->status);
    }

    if (mode == "revalidate") {
        auto observed = analytics::analyze_run_dir(run);
        if (!observed) {
            std::fprintf(stderr, "%s\n", observed.error().to_string().c_str());
            return 3;
        }
        research::KillCriteria criteria;
        if (flags.contains("--min-trades")) criteria.min_round_trips = std::stoul(flags["--min-trades"]);
        std::optional<research::ConsistencyResult> consistency;
        if (flags.contains("--config")) {
            auto cfg = Config::load_file(flags["--config"]);
            if (!cfg) {
                std::fprintf(stderr, "%s\n", cfg.error().to_string().c_str());
                return 3;
            }
            cfg->apply_env_overrides();
            if (!cfg->contains("backtest.from")) cfg->set("backtest.from", "2000-01-01");
            if (!cfg->contains("backtest.to")) cfg->set("backtest.to", "2100-01-01");
            if (!cfg->contains("backtest.initial_cash") && cfg->contains("paper.initial_cash")) {
                cfg->set("backtest.initial_cash", *cfg->get_string("paper.initial_cash"));
            }
            if (!cfg->contains("backtest.symbol") && cfg->contains("paper.symbol")) {
                cfg->set("backtest.symbol", *cfg->get_string("paper.symbol"));
            }
            auto spec = backtest::parse_backtest_spec(*cfg);
            if (!spec) {
                std::fprintf(stderr, "%s\n", spec.error().to_string().c_str());
                return 3;
            }
            strategy::StrategyRegistry registry;
            strategies::register_baselines(registry);
            strategies::register_advanced(registry);
            auto c = research::backtest_paper_consistency(*spec, run, registry, Logger::stderr_logger("revalidate", LogLevel::warn));
            if (!c) {
                std::fprintf(stderr, "consistency check failed: %s\n", c.error().to_string().c_str());
                return 3;
            }
            consistency = *c;
        }
        auto rv = research::revalidate(*drift, *observed, criteria, consistency);
        std::fputs(research::format_revalidation(rv).c_str(), stdout);
        if (auto w = research::write_revalidation(rv, run); !w) {
            std::fprintf(stderr, "%s\n", w.error().to_string().c_str());
            return 3;
        }
        return static_cast<int>(rv.decision);
    }
    return usage(argv[0]);
}
