#include "tradebot/research/research_json.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

using namespace tradebot;
using namespace tradebot::research;
using namespace tradebot::literals;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        std::random_device rd;
        path = fs::temp_directory_path() / ("tradebot-rj-" + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~TempDir() { fs::remove_all(path); }
};

analytics::PerformanceReport sample_report() {
    analytics::PerformanceReport r;
    r.run_id = "ma_2024-03-01_2024-03-08_deadbeef";
    r.returns.samples = 3;
    r.returns.total_return = 0.05;
    r.returns.sharpe = 1.5;
    r.returns.max_drawdown = 0.02;
    r.trades.round_trips = 4;
    r.trades.wins = 3;
    r.trades.losses = 1;
    r.trades.win_rate = 0.75;
    r.trades.profit_factor = 2.0;
    r.trades.net_pnl = "12.5"_ntl;
    r.trades.total_fees = "0.75"_ntl;
    return r;
}

std::vector<std::string> keys_of(const nlohmann::json& j) {
    std::vector<std::string> out;
    for (auto it = j.begin(); it != j.end(); ++it) out.push_back(it.key());
    return out;
}

}  // namespace

TEST_CASE("research json: field names mirror the structs") {
    MonteCarloResult mc{2000, 4, -0.01, 0.04, 0.09, 0.02, 0.05, 0.1};
    const auto j = to_json(mc);
    CHECK(keys_of(j) == std::vector<std::string>{"drawdown_p50", "drawdown_p95", "probability_negative", "return_p05",
                                                 "return_p50", "return_p95", "samples", "trips"});
    CHECK(j["samples"] == 2000);
    CHECK(j["return_p05"] == doctest::Approx(-0.01));

    GridResult g;
    g.points = {{"fee=0", 0.05, 1.2, 0.02, 4}, {"fee=40", -0.01, std::nan(""), 0.05, 4}};
    g.fraction_positive = 0.5;
    g.median_sharpe = 0.6;
    g.min_sharpe = std::nan("");
    g.max_sharpe = 1.2;
    const auto gj = to_json(g);
    CHECK(gj["points"].size() == 2);
    CHECK(gj["points"][1]["sharpe"] == 0.0);  // non-finite becomes 0.0, never NaN in the file
    CHECK(gj["min_sharpe"] == 0.0);
    CHECK(gj["points"][0]["label"] == "fee=0");

    KillCriteria c;
    CHECK(to_json(c)["min_round_trips"] == 30);
    CHECK(to_json(c)["must_beat_benchmark"] == true);

    Verdict v;
    v.pass = false;
    v.failures = {"round trips 4 < 30"};
    CHECK(to_json(v)["failures"][0] == "round trips 4 < 30");

    GoNoGo g2;
    g2.go = false;
    g2.checks = {{"kill criteria", false, "1 failure"}, {"monte carlo p05 return > 0", true, "p05 0.01"}};
    const auto gg = to_json(g2);
    CHECK(gg["go"] == false);
    CHECK(gg["checks"][1]["name"] == "monte carlo p05 return > 0");
    CHECK(gg["checks"][1]["pass"] == true);

    ConsistencyResult cr;
    cr.from = *Timestamp::parse_iso8601("2024-03-01");
    cr.to = *Timestamp::parse_iso8601("2024-03-08");
    cr.consistent = true;
    CHECK(to_json(cr)["from"] == "2024-03-01T00:00:00.000000000Z");
    CHECK(to_json(cr)["consistent"] == true);

    RegimeResult rr;
    rr.segments = {{cr.from, cr.to, 0.4, true, 0.02, 0.01}};
    rr.high_vol_return = 0.02;
    CHECK(to_json(rr)["segments"][0]["high_vol"] == true);
    CHECK(to_json(rr)["segments"][0]["to"] == "2024-03-08T00:00:00.000000000Z");
}

TEST_CASE("research json: walk-forward windows carry chosen params and the out-of-sample report") {
    WalkForwardResult wf;
    WindowResult w;
    w.window = {*Timestamp::parse_iso8601("2024-03-01"), *Timestamp::parse_iso8601("2024-03-05"),
                *Timestamp::parse_iso8601("2024-03-07")};
    w.chosen_label = "ma_fast=5_slow=20";
    w.chosen_params = *Config::parse("fast = 5\nslow = 20");
    w.in_sample_metric = 1.1;
    w.out_of_sample = sample_report();
    wf.windows.push_back(w);
    wf.oos_total_return = 0.05;
    wf.oos_mean_sharpe = 1.5;
    wf.oos_positive_fraction = 1.0;
    wf.oos_round_trips = 4;
    const auto j = to_json(wf);
    CHECK(j["windows"].size() == 1);
    CHECK(j["windows"][0]["chosen_params"]["fast"] == "5");
    CHECK(j["windows"][0]["chosen_params"]["slow"] == "20");
    CHECK(j["windows"][0]["train_to"] == "2024-03-05T00:00:00.000000000Z");
    CHECK(j["windows"][0]["out_of_sample"]["trades"]["net_pnl"] == "12.5");  // money stays a string
    CHECK(j["oos_round_trips"] == 4);
}

TEST_CASE("research json: ranked comparison") {
    std::vector<Ranked> rows;
    rows.push_back({"a", sample_report()});
    auto b = sample_report();
    b.returns.sharpe = 0.5;
    rows.push_back({"b", b});
    const auto j = to_json(rank(rows, SelectMetric::sharpe), SelectMetric::sharpe);
    CHECK(j["sorted_by"] == "sharpe");
    CHECK(j["rows"][0]["label"] == "a");
    CHECK(j["rows"][0]["rank"] == 1);
    CHECK(j["rows"][1]["metric"] == doctest::Approx(0.5));
}

TEST_CASE("research json: validation report omits sections that did not run, and write_json is atomic") {
    TempDir tmp;
    ValidationReport v;
    v.run_id = "r1";
    v.report = sample_report();
    v.verdict = evaluate(v.report, v.criteria);
    v.monte_carlo = MonteCarloResult{100, 4, 0.01, 0.04, 0.09, 0.02, 0.05, 0.0};
    v.go_no_go.go = false;
    const auto j = to_json(v);
    CHECK(j.contains("monte_carlo"));
    CHECK_FALSE(j.contains("costs"));
    CHECK_FALSE(j.contains("stability"));
    CHECK_FALSE(j.contains("walk_forward"));
    CHECK_FALSE(j.contains("consistency"));
    CHECK_FALSE(j.contains("regimes"));
    CHECK(j["report"]["run_id"] == "ma_2024-03-01_2024-03-08_deadbeef");
    CHECK(j["verdict"]["pass"] == false);  // 4 round trips < 30

    const fs::path file = tmp.path / "nested" / "validation.json";
    REQUIRE(write_json(file, j).has_value());
    CHECK_FALSE(fs::exists(tmp.path / "nested" / "validation.json.tmp"));
    std::ifstream in(file);
    const auto back = nlohmann::json::parse(in);
    CHECK(back == j);
}
