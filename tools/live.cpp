// tradebot-live: run strategies against the live market in one of four
// modes (paper, shadow, testnet, live). Runs until SIGINT/SIGTERM;
// artifacts and state land in runs/<mode>-<label>/ and a restart resumes
// from state.json. tradebot-paper is the same program pinned to paper mode.
//
//   tradebot-live --config FILE [--config OVERLAY]... [--mode M] [--label NAME] [--log-level L] [--check]
//
// --config may repeat: later files override earlier ones key by key (base,
// strategy, environment). --check runs the pre-flight checklist and exits
// without connecting.

#include "tradebot/core/config.hpp"
#include "tradebot/core/log.hpp"
#include "tradebot/live/runtime.hpp"
#include "tradebot/net/tls.hpp"
#include "tradebot/strategies/advanced.hpp"
#include "tradebot/strategies/baselines.hpp"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <string>
#include <vector>

namespace {

tradebot::live::TradingRuntime* g_runtime = nullptr;

void on_signal(int) {
    if (g_runtime != nullptr) {
        g_runtime->stop();
    }
}

int usage(const char* argv0) {
    std::fprintf(stderr,
                 "usage: %s --config FILE [--config OVERLAY]... [--mode paper|shadow|testnet|live] [--label NAME] "
                 "[--log-level L] [--check]\n",
                 argv0);
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace tradebot;
    std::vector<std::string> config_paths;
    std::string label_override, mode_override, log_level_text = "info";
    bool check_only = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--check") {
            check_only = true;
            continue;
        }
        if (i + 1 >= argc) return usage(argv[0]);
        const std::string val = argv[++i];
        if (arg == "--config") config_paths.push_back(val);
        else if (arg == "--label") label_override = val;
        else if (arg == "--mode") mode_override = val;
        else if (arg == "--log-level") log_level_text = val;
        else return usage(argv[0]);
    }
    if (config_paths.empty()) return usage(argv[0]);
    auto level = parse_log_level(log_level_text);
    if (!level) return usage(argv[0]);
    Logger log = Logger::stderr_logger("live", *level);

    auto cfg = Config::load_files(config_paths);
    if (!cfg) {
        log.error("{}", cfg.error().to_string());
        return 1;
    }
    cfg->apply_env_overrides();
    if (!label_override.empty()) cfg->set("paper.label", label_override);
    if (!mode_override.empty()) cfg->set("live.mode", mode_override);
#ifdef TRADEBOT_PAPER_ONLY
    cfg->set("live.mode", "paper");
#endif
    auto spec = live::parse_runtime_spec(*cfg);
    if (!spec) {
        log.error("{}", spec.error().to_string());
        return 1;
    }
    const auto violations = live::preflight_violations(*spec);
    if (check_only) {
        std::printf("mode: %s\n", std::string(live::to_string(spec->mode)).c_str());
        std::printf("venue: %s\n", spec->gateway.rest_base.c_str());
        std::printf("initial cash: %s\n", spec->initial_cash.to_string().c_str());
        std::printf("max order notional: %s\n", spec->limits.max_order_notional.to_string().c_str());
        std::printf("max position: %s\n", spec->limits.max_position.to_string().c_str());
        std::printf("max drawdown: %s  max daily loss: %s\n", spec->limits.max_drawdown.to_string().c_str(),
                    spec->limits.max_daily_loss.to_string().c_str());
        std::printf("credentials: %s\n", spec->credentials.present() ? "present" : "missing");
        if (violations.empty()) {
            std::printf("preflight: ok\n");
            return 0;
        }
        std::printf("preflight: %zu problem(s)\n", violations.size());
        for (const auto& v : violations) std::printf("  - %s\n", v.c_str());
        return 1;
    }
    if (!violations.empty()) {
        for (const auto& v : violations) log.error("preflight: {}", v);
        return 1;
    }
    std::string ca_file = cfg->get_string_or("tls.ca_file", "").value_or("");
    auto tls = net::TlsContext::create({.ca_file = ca_file});
    if (!tls) {
        log.error("{}", tls.error().to_string());
        return 1;
    }
    strategy::StrategyRegistry registry;
    strategies::register_baselines(registry);
    strategies::register_advanced(registry);

    if (spec->mode == live::TradingMode::live) {
        log.warn("LIVE MODE: real orders will be sent to {} with up to {} {}", spec->gateway.rest_base,
                 spec->gateway.max_capital.to_string(), spec->instrument.quote);
    }
    live::TradingRuntime runtime(*spec, registry, *tls, log);
    g_runtime = &runtime;
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    auto r = runtime.run();
    g_runtime = nullptr;
    if (!r) {
        log.error("{}", r.error().to_string());
        return 1;
    }
    return 0;
}
