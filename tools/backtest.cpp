// tradebot-backtest: run one backtest (or a parameter sweep) from a config.
//
//   tradebot-backtest --config configs/backtest.conf [--out runs] [--threads N]
//                     [--param key=value ...] [--from D] [--to D] [--strategy NAME]
//
// Artifacts are written to <out>/<run_id>/. A [sweep] section in the
// config expands into one run per parameter combination.

#include "tradebot/backtest/backtest.hpp"
#include "tradebot/core/config.hpp"
#include "tradebot/core/log.hpp"
#include "tradebot/strategies/advanced.hpp"
#include "tradebot/strategies/baselines.hpp"

#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

int usage(const char* argv0) {
    std::fprintf(stderr,
                 "usage: %s --config FILE [--out DIR] [--threads N] [--param k=v]... "
                 "[--from YYYY-MM-DD] [--to YYYY-MM-DD] [--strategy NAME] [--log-level L]\n",
                 argv0);
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace tradebot;
    using namespace tradebot::backtest;

    std::string config_path, out_dir = "runs", log_level_text = "info";
    unsigned threads = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::pair<std::string, std::string>> overrides;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (i + 1 >= argc) return usage(argv[0]);
        const std::string val = argv[++i];
        if (arg == "--config") config_path = val;
        else if (arg == "--out") out_dir = val;
        else if (arg == "--threads") threads = static_cast<unsigned>(std::stoul(val));
        else if (arg == "--log-level") log_level_text = val;
        else if (arg == "--from") overrides.emplace_back("backtest.from", val);
        else if (arg == "--to") overrides.emplace_back("backtest.to", val);
        else if (arg == "--strategy") overrides.emplace_back("strategy.name", val);
        else if (arg == "--param") {
            const std::size_t eq = val.find('=');
            if (eq == std::string::npos) return usage(argv[0]);
            overrides.emplace_back("strategy.params." + val.substr(0, eq), val.substr(eq + 1));
        } else return usage(argv[0]);
    }
    if (config_path.empty()) return usage(argv[0]);

    auto level = parse_log_level(log_level_text);
    if (!level) return usage(argv[0]);
    Logger log = Logger::stderr_logger("backtest", *level);

    auto cfg = Config::load_file(config_path);
    if (!cfg) {
        log.error("{}", cfg.error().to_string());
        return 1;
    }
    cfg->apply_env_overrides();
    for (const auto& [k, v] : overrides) cfg->set(k, v);

    auto spec = parse_backtest_spec(*cfg);
    if (!spec) {
        log.error("{}", spec.error().to_string());
        return 1;
    }
    strategy::StrategyRegistry registry;
    strategies::register_baselines(registry);
    strategies::register_advanced(registry);

    std::vector<BacktestSpec> specs;
    const Config sweep = cfg->section("sweep");
    if (!sweep.keys().empty()) {
        auto expanded = expand_sweep(*spec, sweep);
        if (!expanded) {
            log.error("{}", expanded.error().to_string());
            return 1;
        }
        specs = std::move(*expanded);
        log.info("sweep: {} runs on {} threads", specs.size(), threads);
    } else {
        specs.push_back(*spec);
    }

    auto results = run_batch(specs, registry, log, threads);
    int failures = 0;
    std::printf("%-60s %14s %9s %9s %6s %6s\n", "run_id", "final_equity", "return%", "max_dd%", "fills", "orders");
    for (const auto& r : results) {
        if (!r) {
            ++failures;
            log.error("run failed: {}", r.error().to_string());
            continue;
        }
        if (auto w = write_artifacts(*r, std::filesystem::path(out_dir) / r->spec.run_id); !w) {
            ++failures;
            log.error("{}", w.error().to_string());
            continue;
        }
        const auto& s = r->summary;
        std::printf("%-60s %14s %9.3f %9.3f %6llu %6llu\n", r->spec.run_id.c_str(),
                    s.final_equity.to_string().c_str(), s.total_return * 100.0,
                    s.max_drawdown_fraction * 100.0, static_cast<unsigned long long>(s.fills),
                    static_cast<unsigned long long>(s.orders));
    }
    return failures == 0 ? 0 : 1;
}
