// HD D-17: check_live_settings_overlay answers what a live run would answer, before AlgoLens
// saves a desk settings change. The base config is the tracked config_template, read in place.

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "trade_ngin/core/config_loader.hpp"
#include "trade_ngin/live/live_settings.hpp"

using namespace trade_ngin;
using Json = nlohmann::json;

namespace {

std::filesystem::path shipped_tmpl() {
    namespace fs = std::filesystem;
    fs::path dir = fs::path(__FILE__).parent_path();
    for (int i = 0; i < 8 && !dir.empty(); ++i) {
        if (fs::exists(dir / "config_template" / "defaults.json")) return dir / "config_template";
        dir = dir.parent_path();
    }
    dir = fs::current_path();
    for (int i = 0; i < 8 && !dir.empty(); ++i) {
        if (fs::exists(dir / "config_template" / "defaults.json")) return dir / "config_template";
        dir = dir.parent_path();
    }
    return {};
}

/// config_template copied once into a temporary directory, with the QT book given the
/// conservative book's email.json: the template ships none for qt_conservative (a deployment
/// adds its own), and a live run's loader requires one.
std::filesystem::path tmpl() {
    namespace fs = std::filesystem;
    static const fs::path copy = [] {
        const fs::path from = shipped_tmpl();
        if (from.empty()) return fs::path();
        const fs::path to = fs::temp_directory_path() / "qt_settings_check_config";
        fs::remove_all(to);
        fs::copy(from, to, fs::copy_options::recursive);
        const fs::path email = to / "portfolios" / "qt_conservative" / "email.json";
        if (!fs::exists(email)) {
            fs::copy_file(to / "portfolios" / "conservative" / "email.json", email);
        }
        return to;
    }();
    return copy;
}

/// The QT book's carver module as shipped, with `key` set to `value`.
Json gate_with(const std::string& key, const Json& value) {
    std::ifstream in(tmpl() / "portfolios" / "qt_conservative" / "risk.json");
    Json risk = Json::parse(in);
    Json modules = risk.at("modules");
    for (auto& m : modules) {
        if (m.value("type", "") == "carver") m[key] = value;
    }
    return modules;
}

std::string refusal(const Json& overrides) {
    auto r = check_live_settings_overlay(tmpl(), "qt_conservative", overrides);
    return r.is_ok() ? std::string() : std::string(r.error()->what());
}

}  // namespace

TEST(LiveSettingsCheck, NoChangeAndAPlainChangeAreAccepted) {
    if (tmpl().empty()) GTEST_SKIP() << "config_template not found";
    EXPECT_EQ(refusal(Json::object()), "");
    EXPECT_EQ(refusal(Json{{"risk", {{"max_drawdown", 0.25}}}}), "");
}

TEST(LiveSettingsCheck, AGateChangeWithItsMirrorIsAcceptedWithoutItRefused) {
    if (tmpl().empty()) GTEST_SKIP() << "config_template not found";
    const Json modules = gate_with("max_gross_leverage", 7.0);
    EXPECT_EQ(refusal(Json{{"risk", {{"modules", modules},
                                     {"risk_reporting", {{"max_gross_leverage", 7.0}}}}}}),
              "");
    EXPECT_NE(refusal(Json{{"risk", {{"modules", modules}}}}), "");
}

TEST(LiveSettingsCheck, ALimitOutOfItsBoundsIsRefusedWithTheRunsReason) {
    if (tmpl().empty()) GTEST_SKIP() << "config_template not found";
    const std::string why = refusal(Json{{"risk", {{"modules", gate_with("R_max", 1000)}}}});
    EXPECT_NE(why.find("R_max must be at most 3"), std::string::npos) << why;
    EXPECT_EQ(why.find("strategy_config version 0"), std::string::npos) << why;
}

TEST(LiveSettingsCheck, ASleeveSettingAStrategyRefusesIsRefusedBeforeItIsSaved) {
    // Seen in the simulator: risk_target 1.5 passed the config checks and stopped the model
    // run at strategy start-up ("Risk target must be between 0 and 1").
    if (tmpl().empty()) GTEST_SKIP() << "config_template not found";
    auto sleeve = [](const char* key, const Json& value) {
        Json overrides;
        overrides["strategies"]["TREND_FOLLOWING"]["config"][key] = value;
        return overrides;
    };
    const std::string why = refusal(sleeve("risk_target", 1.5));
    EXPECT_NE(why.find("Risk target must be between 0 and 1"), std::string::npos) << why;
    EXPECT_EQ(refusal(sleeve("risk_target", 0.18)), "");
    EXPECT_NE(refusal(sleeve("ema_windows", Json::array({Json::array({3, 9})}))), "");
}

TEST(LiveSettingsCheck, AnUnknownKeyAndAProtectedSectionAreRefused) {
    if (tmpl().empty()) GTEST_SKIP() << "config_template not found";
    EXPECT_NE(refusal(Json{{"risk", {{"no_such_limit", 1}}}}), "");
    EXPECT_NE(refusal(Json{{"database", {{"host", "x"}}}}), "");
}

namespace {

/// A copy of tmpl() whose QT book's portfolio.json has been changed by `edit`.
std::filesystem::path tmpl_with(const std::string& tag, void (*edit)(Json&)) {
    namespace fs = std::filesystem;
    const fs::path to = fs::temp_directory_path() / ("qt_settings_check_" + tag);
    fs::remove_all(to);
    fs::copy(tmpl(), to, fs::copy_options::recursive);
    const fs::path file = to / "portfolios" / "qt_conservative" / "portfolio.json";
    Json portfolio;
    {
        std::ifstream in(file);
        portfolio = Json::parse(in);
    }
    edit(portfolio);
    std::ofstream(file) << portfolio.dump(2);
    return to;
}

std::string sleeve_refusal(const char* key, const Json& value) {
    Json overrides;
    overrides["strategies"]["TREND_FOLLOWING"]["config"][key] = value;
    return refusal(overrides);
}

}  // namespace

TEST(LiveSettingsCheck, ABookWhoseFilesDoNotLoadIsRefused) {
    if (tmpl().empty()) GTEST_SKIP() << "config_template not found";
    auto r = check_live_settings_overlay(tmpl(), "no_such_portfolio", Json::object());
    ASSERT_TRUE(r.is_error());
    EXPECT_NE(std::string(r.error()->what()).find("the config files do not load"),
              std::string::npos)
        << r.error()->what();
}

TEST(LiveSettingsCheck, ABookWithoutTheLoopKeysIsRefusedAsARunWouldBe) {
    if (tmpl().empty()) GTEST_SKIP() << "config_template not found";
    const auto dir = tmpl_with("no_slow_rule", [](Json& p) { p.erase("equity_slow_rule"); });
    auto r = check_live_settings_overlay(dir, "qt_conservative", Json::object());
    ASSERT_TRUE(r.is_error());
    EXPECT_NE(std::string(r.error()->what()).find("equity_slow_rule"), std::string::npos)
        << r.error()->what();
}

TEST(LiveSettingsCheck, ASleeveThatIsNotATrendStrategyIsNotCheckedAsOne) {
    if (tmpl().empty()) GTEST_SKIP() << "config_template not found";
    const auto dir = tmpl_with("other_sleeve", [](Json& p) {
        Json other = p["strategies"]["TREND_FOLLOWING"];
        other["type"] = "SomeOtherStrategy";
        other["config"] = Json::object();  // no sleeve keys: a trend check would refuse it
        other["default_allocation"] = 0.0;
        p["strategies"]["OTHER"] = other;
        // A book of two sleeves carries no trading_rule_removals (the loader refuses the list
        // outside a book of one trend sleeve, as BASE's two sleeves have none).
        p.erase("trading_rule_removals");
    });
    auto r = check_live_settings_overlay(dir, "qt_conservative", Json::object());
    EXPECT_TRUE(r.is_ok()) << r.error()->what();
}

TEST(LiveSettingsCheck, ASleeveKeyOrWindowTheRunRefusesIsRefused) {
    if (tmpl().empty()) GTEST_SKIP() << "config_template not found";
    std::string why = sleeve_refusal("idm", 0);
    EXPECT_NE(why.find("idm must be a positive number"), std::string::npos) << why;
    for (const Json& windows : {Json::array({Json::array({8})}),
                                Json::array({Json::array({2.5, 8})}),
                                Json::array({Json::array({2, "8"})}), Json::array({8})}) {
        why = sleeve_refusal("ema_windows", windows);
        EXPECT_NE(why.find("ema_windows must hold [fast, slow] pairs of whole numbers"),
                  std::string::npos)
            << windows.dump() << ": " << why;
    }
}

TEST(LiveSettings, FilesThatNameAnotherPortfolioAreRefused) {
    if (tmpl().empty()) GTEST_SKIP() << "config_template not found";
    auto r = resolve_live_settings(tmpl(), "qt_conservative", "SOME_OTHER_PORTFOLIO",
                                   Result<std::optional<StrategyConfigRow>>(std::optional<StrategyConfigRow>{}));
    ASSERT_TRUE(r.is_error());
    EXPECT_NE(std::string(r.error()->what()).find("the config files now name portfolio"),
              std::string::npos)
        << r.error()->what();
}

// ===== D-60 (the property test asked for in #88): every setting the desk can save is read =====
//
// For every number and true/false the QT book's settings_used carries outside the blocks the desk
// never sees (database, email, qt, backtest keys, comments) and the read-only mirrors, a changed
// value saved as the desk saves it (the whole risk module list for a gate limit, with its
// risk_reporting mirror) must (1) be accepted by a live run's start-up checks and (2) change the
// config the run parses (AppConfig), so the knob is read, not merely stored.

namespace {

bool desk_never_sees(const std::vector<std::string>& path) {
    static const std::vector<std::string> hidden_sections = {"database", "email", "qt"};
    if (std::find(hidden_sections.begin(), hidden_sections.end(), path.front()) !=
        hidden_sections.end())
        return true;
    for (const auto& k : path) {
        if (k.empty() || k[0] == '_' || k == "portfolio_id" ||
            k.find("backtest") != std::string::npos)
            return true;
    }
    // Read-only in AlgoLens (one legal value, or a mirror of the gate; D-16), and
    // starting_capital, which follows initial_capital (the engine requires the two equal).
    if (path.size() >= 2 && path[0] == "risk" &&
        (path[1] == "schema" || path[1] == "risk_reporting"))
        return true;
    if (path.size() == 1 && path[0] == "starting_capital") return true;
    return false;
}

struct Leaf {
    std::vector<std::string> path;  // a risk module's key: {"risk", "modules", <id>, key}
    Json value;
};

void leaves(const Json& node, std::vector<std::string> prefix, std::vector<Leaf>& out) {
    for (auto it = node.begin(); it != node.end(); ++it) {
        auto path = prefix;
        path.push_back(it.key());
        if (desk_never_sees(path)) continue;
        if (it->is_object() && !it->empty()) {
            leaves(*it, path, out);
        } else if (it->is_array() && !it->empty() && (*it)[0].is_object() &&
                   (*it)[0].contains("id")) {
            for (const auto& item : *it) {
                auto at = path;
                at.push_back(item.at("id").get<std::string>());
                leaves(item, at, out);
            }
        } else if (it->is_number() || it->is_boolean()) {
            out.push_back({path, *it});
        }
    }
}

Json changed(const Json& v) {
    if (v.is_boolean()) return !v.get<bool>();
    if (v.is_number_integer()) {
        const auto n = v.get<long long>();
        return n == 0 ? 1LL : n + std::max<long long>(1, n / 10);
    }
    const double x = v.get<double>();
    return x == 0.0 ? 0.01 : x * 0.9;  // smaller: stays inside an upper bound
}

/// The overrides the desk's save makes for `leaf` set to `value` (AlgoLens settings.py).
Json desk_overrides(const Json& files, const Leaf& leaf, const Json& value) {
    Json out = Json::object();
    if (leaf.path.size() == 4 && leaf.path[0] == "risk" && leaf.path[1] == "modules") {
        Json modules = files.at("risk").at("modules");
        for (auto& m : modules) {
            if (m.at("id") == leaf.path[2]) m[leaf.path[3]] = value;
        }
        out["risk"]["modules"] = modules;
        const Json& rep = files.at("risk").at("risk_reporting");
        if (rep.contains(leaf.path[3])) out["risk"]["risk_reporting"][leaf.path[3]] = value;
        return out;
    }
    Json* node = &out;
    for (size_t i = 0; i + 1 < leaf.path.size(); ++i) node = &(*node)[leaf.path[i]];
    (*node)[leaf.path.back()] = value;
    // initial_capital moves starting_capital with it (AlgoLens saves them together).
    if (leaf.path.size() == 1 && leaf.path[0] == "initial_capital" &&
        files.contains("starting_capital"))
        out["starting_capital"] = value;
    return out;
}

}  // namespace

TEST(LiveSettingsCheck, EverySettingTheDeskCanSaveIsReadByALiveRun) {
    if (tmpl().empty()) GTEST_SKIP() << "config_template not found";
    Json files;
    auto base = ConfigLoader::load(tmpl(), "qt_conservative", nullptr, &files);
    ASSERT_TRUE(base.is_ok()) << base.error()->what();
    const Json base_config = base.value().to_json();

    std::vector<Leaf> all;
    leaves(files, {}, all);
    ASSERT_GT(all.size(), 20u);

    std::vector<std::string> problems;
    for (const auto& leaf : all) {
        std::string name;
        for (const auto& k : leaf.path) name += (name.empty() ? "" : ".") + k;
        const Json value = changed(leaf.value);
        const Json overrides = desk_overrides(files, leaf, value);
        auto checked = check_live_settings_overlay(tmpl(), "qt_conservative", overrides);
        if (checked.is_error()) {
            problems.push_back(name + " = " + value.dump() + ": refused: " +
                               checked.error()->what());
            continue;
        }
        auto live = ConfigLoader::load(tmpl(), "qt_conservative", &overrides);
        ASSERT_TRUE(live.is_ok()) << name;
        // Two optimisation knobs are read into AppConfig members its to_json does not write.
        const bool member_moved =
            (name == "optimization.sign_close_band" &&
             live.value().sign_close_band != base.value().sign_close_band) ||
            (name == "optimization.b_sigma_floor" &&
             live.value().b_sigma_floor != base.value().b_sigma_floor);
        if (live.value().to_json() == base_config && !member_moved) {
            problems.push_back(name + " = " + value.dump() +
                               ": saved but not read (no change in the parsed config)");
        }
    }
    std::string report;
    for (const auto& p : problems) report += "\n  " + p;
    EXPECT_TRUE(problems.empty()) << problems.size() << " of " << all.size()
                                  << " settings fail:" << report;
}
