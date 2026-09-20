// tradebot-analyze: compute performance metrics for finished runs.
//
//   tradebot-analyze runs/<run_id> [runs/<other_run_id> ...]
//
// Prints a report for each run and writes metrics.json, report.txt and
// round_trips.csv into the run directory.

#include "tradebot/analytics/analytics.hpp"

#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    using namespace tradebot;
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s RUN_DIR [RUN_DIR ...]\n", argv[0]);
        return 2;
    }
    int failures = 0;
    for (int i = 1; i < argc; ++i) {
        const std::filesystem::path dir = argv[i];
        auto report = analytics::analyze_run_dir(dir);
        if (!report) {
            std::fprintf(stderr, "%s: %s\n", argv[i], report.error().to_string().c_str());
            ++failures;
            continue;
        }
        std::fputs(analytics::format_report(*report).c_str(), stdout);
        std::fputs("\n", stdout);
        if (auto w = analytics::write_report(*report, dir); !w) {
            std::fprintf(stderr, "%s: %s\n", argv[i], w.error().to_string().c_str());
            ++failures;
        }
    }
    return failures == 0 ? 0 : 1;
}
