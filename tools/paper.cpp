// tradebot-paper: run strategies against the live market with simulated
// fills. Runs until SIGINT/SIGTERM; artifacts and state land in
// runs/paper-<label>/ and a restart resumes from state.json.
//
//   tradebot-paper --config configs/paper.conf [--label NAME] [--log-level L]

#include "tradebot/core/config.hpp"
#include "tradebot/core/log.hpp"
#include "tradebot/live/paper_runtime.hpp"
#include "tradebot/net/tls.hpp"
#include "tradebot/strategies/baselines.hpp"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <string>

namespace {

tradebot::live::PaperRuntime* g_runtime = nullptr;

void on_signal(int) {
    if (g_runtime != nullptr) {
        g_runtime->stop();
    }
}

int usage(const char* argv0) {
    std::fprintf(stderr, "usage: %s --config FILE [--label NAME] [--log-level L]\n", argv0);
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace tradebot;
    std::string config_path, label_override, log_level_text = "info";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (i + 1 >= argc) return usage(argv[0]);
        const std::string val = argv[++i];
        if (arg == "--config") config_path = val;
        else if (arg == "--label") label_override = val;
        else if (arg == "--log-level") log_level_text = val;
        else return usage(argv[0]);
    }
    if (config_path.empty()) return usage(argv[0]);
    auto level = parse_log_level(log_level_text);
    if (!level) return usage(argv[0]);
    Logger log = Logger::stderr_logger("paper", *level);

    auto cfg = Config::load_file(config_path);
    if (!cfg) {
        log.error("{}", cfg.error().to_string());
        return 1;
    }
    cfg->apply_env_overrides();
    if (!label_override.empty()) cfg->set("paper.label", label_override);
    auto spec = live::parse_paper_spec(*cfg);
    if (!spec) {
        log.error("{}", spec.error().to_string());
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

    live::PaperRuntime runtime(*spec, registry, *tls, log);
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
