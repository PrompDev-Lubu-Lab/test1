// tradebot-research: sweeps, walk-forward evaluation, the experiment index
// and kill-criteria verdicts.
//
//   tradebot-research sweep        --config FILE [--out runs] [--metric sharpe] [--threads N]
//   tradebot-research walk-forward --config FILE --train 30d --test 7d [--step 7d] [--metric sharpe]
//   tradebot-research judge        RUN_DIR [--min-trades N] [--min-sharpe X] [--max-dd F] [--min-pf X]
//   tradebot-research index        [--out runs]

#include "tradebot/analytics/analytics.hpp"
#include "tradebot/backtest/backtest.hpp"
#include "tradebot/core/config.hpp"
#include "tradebot/core/log.hpp"
#include "tradebot/research/research.hpp"
#include "tradebot/strategies/advanced.hpp"
#include "tradebot/strategies/baselines.hpp"

#include <cstdio>
#include <map>
#include <string>
#include <thread>

namespace {

int usage(const char* argv0) {
    std::fprintf(stderr,
                 "usage:\n"
                 "  %s sweep --config FILE [--out DIR] [--metric M] [--threads N]\n"
                 "  %s walk-forward --config FILE --train DUR --test DUR [--step DUR] [--metric M] [--threads N]\n"
                 "  %s judge RUN_DIR [--min-trades N] [--min-sharpe X] [--max-dd F] [--min-pf X] [--ignore-benchmark]\n"
                 "  %s index [--out DIR]\n",
                 argv0, argv0, argv0, argv0);
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace tradebot;
    using namespace tradebot::research;

    if (argc < 2) return usage(argv[0]);
    const std::string mode = argv[1];
    std::map<std::string, std::string> opt;
    std::vector<std::string> positional;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a.rfind("--", 0) == 0) {
            if (a == "--ignore-benchmark") {
                opt[a] = "1";
            } else if (i + 1 < argc) {
                opt[a] = argv[++i];
            } else {
                return usage(argv[0]);
            }
        } else {
            positional.push_back(a);
        }
    }
    auto get = [&](const char* k, const std::string& def) {
        auto it = opt.find(k);
        return it == opt.end() ? def : it->second;
    };
    Logger log = Logger::stderr_logger("research", parse_log_level(get("--log-level", "info")).value_or(LogLevel::info));
    const std::string out_dir = get("--out", "runs");
    const unsigned threads = static_cast<unsigned>(std::stoul(get("--threads", std::to_string(std::max(1u, std::thread::hardware_concurrency())))));
    auto metric = parse_select_metric(get("--metric", "sharpe"));
    if (!metric) {
        log.error("{}", metric.error().to_string());
        return 1;
    }

    if (mode == "index") {
        auto rows = read_index(std::filesystem::path(out_dir) / "index.csv");
        if (!rows) {
            log.error("{}", rows.error().to_string());
            return 1;
        }
        std::printf("%-20s %-50s %9s %8s %8s %6s %9s\n", "recorded", "run_id", "return%", "sharpe", "max_dd%", "trips", "bench%");
        for (const auto& r : *rows) {
            std::printf("%-20s %-50s %9.2f %8.2f %8.2f %6zu %9.2f\n", r.recorded_at.substr(0, 19).c_str(),
                        r.run_id.substr(0, 50).c_str(), r.total_return * 100.0, r.sharpe, r.max_drawdown * 100.0,
                        r.round_trips, r.benchmark_return * 100.0);
        }
        return 0;
    }

    if (mode == "judge") {
        if (positional.empty()) return usage(argv[0]);
        KillCriteria c;
        c.min_round_trips = std::stoul(get("--min-trades", "30"));
        c.min_sharpe = std::stod(get("--min-sharpe", "0.5"));
        c.max_drawdown = std::stod(get("--max-dd", "0.25"));
        c.min_profit_factor = std::stod(get("--min-pf", "1.1"));
        c.must_beat_benchmark = !opt.contains("--ignore-benchmark");
        int failures = 0;
        for (const auto& dir : positional) {
            auto report = analytics::analyze_run_dir(dir);
            if (!report) {
                log.error("{}: {}", dir, report.error().to_string());
                ++failures;
                continue;
            }
            const Verdict v = evaluate(*report, c);
            std::printf("%s\n%s", dir.c_str(), format_verdict(v).c_str());
            if (!v.pass) ++failures;
        }
        return failures == 0 ? 0 : 1;
    }

    // sweep and walk-forward need a config.
    const std::string config_path = get("--config", "");
    if (config_path.empty()) return usage(argv[0]);
    auto cfg = Config::load_file(config_path);
    if (!cfg) {
        log.error("{}", cfg.error().to_string());
        return 1;
    }
    cfg->apply_env_overrides();
    auto base = backtest::parse_backtest_spec(*cfg);
    if (!base) {
        log.error("{}", base.error().to_string());
        return 1;
    }
    const Config sweep = cfg->section("sweep");
    strategy::StrategyRegistry registry;
    strategies::register_baselines(registry);
    strategies::register_advanced(registry);

    if (mode == "sweep") {
        std::vector<backtest::BacktestSpec> specs;
        if (sweep.keys().empty()) {
            specs.push_back(*base);
        } else {
            auto expanded = backtest::expand_sweep(*base, sweep);
            if (!expanded) {
                log.error("{}", expanded.error().to_string());
                return 1;
            }
            specs = std::move(*expanded);
        }
        auto results = backtest::run_batch(specs, registry, log, threads);
        std::vector<Ranked> ranked;
        for (const auto& r : results) {
            if (!r) {
                log.error("run failed: {}", r.error().to_string());
                continue;
            }
            auto report = analytics::analyze(*r);
            const auto dir = std::filesystem::path(out_dir) / r->spec.run_id;
            if (auto w = backtest::write_artifacts(*r, dir); !w) log.error("{}", w.error().to_string());
            if (auto w = analytics::write_report(report, dir); !w) log.error("{}", w.error().to_string());
            if (auto a = append_to_index(std::filesystem::path(out_dir) / "index.csv", *r, report); !a) {
                log.error("{}", a.error().to_string());
            }
            ranked.push_back(Ranked{r->spec.strategies.front().label, std::move(report)});
        }
        std::fputs(format_comparison(rank(std::move(ranked), *metric), *metric).c_str(), stdout);
        return 0;
    }

    if (mode == "walk-forward") {
        auto train = parse_duration(get("--train", ""));
        auto test = parse_duration(get("--test", ""));
        if (!train || !test) {
            log.error("--train and --test durations are required (e.g. 30d, 7d)");
            return 1;
        }
        auto step = parse_duration(get("--step", get("--test", "")));
        if (!step) {
            log.error("bad --step");
            return 1;
        }
        const auto windows = walk_forward_windows(base->from, base->to, *train, *test, *step);
        if (windows.empty()) {
            log.error("range too short for train={} test={}", train->to_string(), test->to_string());
            return 1;
        }
        log.info("walk-forward: {} windows, {} candidates", windows.size(),
                 sweep.keys().empty() ? 1 : backtest::expand_sweep(*base, sweep).value_or(std::vector<backtest::BacktestSpec>{}).size());
        auto result = run_walk_forward(*base, sweep, windows, *metric, registry, log, threads);
        if (!result) {
            log.error("{}", result.error().to_string());
            return 1;
        }
        std::fputs(format_walk_forward(*result).c_str(), stdout);
        return 0;
    }
    return usage(argv[0]);
}
