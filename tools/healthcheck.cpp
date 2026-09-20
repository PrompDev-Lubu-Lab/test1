// tradebot-healthcheck: exit 0 if a runtime's heartbeat file is fresh.
//
//   tradebot-healthcheck FILE [--max-age 30s]
//
// Exit codes: 0 healthy, 1 stale or stopped, 2 missing/unreadable, 3 usage.
// Meant for container HEALTHCHECKs and watchdog timers; prints the age and
// the status word the runtime wrote ("paper healthy armed", "stopped", ...).

#include "tradebot/core/clock.hpp"
#include "tradebot/core/config.hpp"
#include "tradebot/live/feed_health.hpp"

#include <cstdio>
#include <fstream>
#include <string>

int main(int argc, char** argv) {
    using namespace tradebot;
    std::string file;
    Duration max_age = Duration::seconds(30);
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--max-age") {
            if (i + 1 >= argc) return 3;
            auto d = parse_duration(argv[++i]);
            if (!d) {
                std::fprintf(stderr, "bad --max-age: %s\n", d.error().message.c_str());
                return 3;
            }
            max_age = *d;
        } else if (file.empty()) {
            file = arg;
        } else {
            std::fprintf(stderr, "usage: %s FILE [--max-age 30s]\n", argv[0]);
            return 3;
        }
    }
    if (file.empty()) {
        std::fprintf(stderr, "usage: %s FILE [--max-age 30s]\n", argv[0]);
        return 3;
    }
    WallClock clock;
    auto age = live::heartbeat_age(file, clock.now());
    if (!age) {
        std::printf("missing: %s\n", file.c_str());
        return 2;
    }
    std::string status;
    {
        std::ifstream in(file);
        std::string iso;
        in >> iso;
        std::getline(in, status);
        if (!status.empty() && status.front() == ' ') status.erase(0, 1);
    }
    const bool stopped = status.rfind("stopped", 0) == 0;
    const bool fresh = *age <= max_age;
    std::printf("%s: age %s (max %s) status '%s'\n", stopped ? "stopped" : fresh ? "healthy" : "stale",
                age->to_string().c_str(), max_age.to_string().c_str(), status.c_str());
    return fresh && !stopped ? 0 : 1;
}
