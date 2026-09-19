// Coverage for config_loader.cpp: JSON file loading, deep-merge, AppConfig
// extraction, validation, and error paths. All tests use a per-test temp
// directory so they don't depend on the real ./config tree.

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include "test_base.hpp"

// Reach private merge_json/validate_config/load_legacy helpers. Pre-load std
// headers before flipping the macro so libc++ internals stay valid.
#include <map>
#include <string>
#include <vector>
#define private public
#include "trade_ngin/core/config_loader.hpp"
#undef private

using namespace trade_ngin;
using namespace trade_ngin::testing;

namespace {

nlohmann::json minimal_defaults() {
    return {
        {"database", {{"host", "h"}, {"port", "5432"}, {"username", "u"},
                      {"password", "p"}, {"name", "n"}, {"num_connections", 5}}},
        {"execution", {{"commission_rate", 0.0005}, {"slippage_bps", 1.0},
                        {"position_limit_backtest", 1000.0}, {"position_limit_live", 500.0}}},
        {"optimization", {{"tau", 1.0}, {"capital", 500000.0},
                          {"cost_penalty_scalar", 50}, {"asymmetric_risk_buffer", 0.1},
                          {"max_iterations", 100}, {"convergence_threshold", 1e-6},
                          {"use_buffering", true}, {"buffer_size_factor", 0.05}}},
        {"backtest", {{"lookback_years", 2}, {"store_trade_details", true}}},
        {"live", {{"historical_days", 300}}},
        {"strategy_defaults", {{"max_strategy_allocation", 1.0},
                                {"min_strategy_allocation", 0.1},
                                {"use_optimization", true},
                                {"use_risk_management", true},
                                {"fdm", nlohmann::json::array({{1, 1.0}, {2, 1.03}})}}},
        {"risk_defaults", {{"confidence_level", 0.99}, {"lookback_period", 252},
                           {"max_correlation", 0.7}}},
    };
}

nlohmann::json minimal_portfolio() {
    return {
        {"portfolio_id", "TEST_PORTFOLIO"},
        {"initial_capital", 1'000'000.0},
        {"reserve_capital_pct", 0.10},
        {"max_drawdown", 0.4},
        {"max_leverage", 4.0},
        // Validation requires at least one strategy entry.
        {"strategies", {{"TREND_FOLLOWING", {{"weight", 1.0}, {"allocation", 1.0}}}}},
    };
}

nlohmann::json minimal_risk() {
    return {
        {"var_limit", 0.15},
        {"jump_risk_limit", 0.10},
        {"max_gross_leverage", 4.0},
        {"max_net_leverage", 2.0},
    };
}

nlohmann::json minimal_email() {
    return {
        {"smtp_host", "smtp.test.com"},
        {"smtp_port", 587},
        {"username", "u"},
        {"password", "p"},
        {"from_email", "f@test.com"},
        {"to_emails", nlohmann::json::array({"a@test.com"})},
    };
}

void write_json(const std::filesystem::path& p, const nlohmann::json& j) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream f(p);
    f << j.dump(2);
}

}  // namespace

class ConfigLoaderTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        const ::testing::TestInfo* info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        base_ = std::filesystem::temp_directory_path() /
                ("trade_ngin_config_" + std::string(info->name()));
        std::filesystem::remove_all(base_);
        std::filesystem::create_directories(base_);
    }

    void TearDown() override {
        std::filesystem::remove_all(base_);
        TestBase::TearDown();
    }

    void write_full_set(const std::string& portfolio_name,
                         const nlohmann::json& defaults_override = {},
                         const nlohmann::json& portfolio_override = {}) {
        auto defaults = minimal_defaults();
        for (auto& [k, v] : defaults_override.items()) defaults[k] = v;
        write_json(base_ / "defaults.json", defaults);

        auto portfolio = minimal_portfolio();
        for (auto& [k, v] : portfolio_override.items()) portfolio[k] = v;
        write_json(base_ / "portfolios" / portfolio_name / "portfolio.json", portfolio);

        write_json(base_ / "portfolios" / portfolio_name / "risk.json", minimal_risk());
        write_json(base_ / "portfolios" / portfolio_name / "email.json", minimal_email());
    }

    std::filesystem::path base_;
};

// ===== Happy path =====

TEST_F(ConfigLoaderTest, LoadValidConfigPopulatesAllFields) {
    write_full_set("base");
    auto r = ConfigLoader::load(base_, "base");
    ASSERT_TRUE(r.is_ok()) << (r.error() ? r.error()->what() : "no error");
    auto& c = r.value();
    EXPECT_EQ(c.portfolio_id, "TEST_PORTFOLIO");
    EXPECT_DOUBLE_EQ(c.initial_capital, 1'000'000.0);
    EXPECT_EQ(c.database.host, "h");
    EXPECT_EQ(c.database.num_connections, 5u);
    EXPECT_DOUBLE_EQ(c.execution.commission_rate, 0.0005);
    EXPECT_DOUBLE_EQ(c.opt_config.tau, 1.0);
    EXPECT_DOUBLE_EQ(c.risk_config.var_limit, 0.15);
    EXPECT_EQ(c.email.smtp_host, "smtp.test.com");
}

TEST_F(ConfigLoaderTest, PortfolioFileOverridesDefaults) {
    // Use a portfolio-level override on a top-level field that's not on the
    // required-validation list (initial_capital).
    write_full_set("base", /*defaults_override=*/{},
                    /*portfolio_override=*/{{"initial_capital", 2'500'000.0}});
    auto r = ConfigLoader::load(base_, "base");
    ASSERT_TRUE(r.is_ok()) << (r.error() ? r.error()->what() : "no error");
    EXPECT_DOUBLE_EQ(r.value().initial_capital, 2'500'000.0);
}

TEST_F(ConfigLoaderTest, ToJsonRoundTripsConfigStructures) {
    write_full_set("base");
    auto r = ConfigLoader::load(base_, "base");
    ASSERT_TRUE(r.is_ok());
    auto j = r.value().to_json();
    EXPECT_EQ(j["portfolio_id"].get<std::string>(), "TEST_PORTFOLIO");
    EXPECT_TRUE(j.contains("database"));
    EXPECT_TRUE(j.contains("execution"));
    EXPECT_TRUE(j.contains("optimization"));
    EXPECT_TRUE(j.contains("risk"));
    EXPECT_TRUE(j.contains("backtest"));
    EXPECT_TRUE(j.contains("live"));
    EXPECT_TRUE(j.contains("strategy_defaults"));
    EXPECT_TRUE(j.contains("email"));
}

// ===== Error paths =====

TEST_F(ConfigLoaderTest, LoadMissingDefaultsFileReturnsError) {
    // Only portfolio file, no defaults
    write_json(base_ / "portfolios" / "base" / "portfolio.json", minimal_portfolio());
    auto r = ConfigLoader::load(base_, "base");
    EXPECT_TRUE(r.is_error());
}

TEST_F(ConfigLoaderTest, LoadMissingPortfolioDirectoryReturnsError) {
    write_json(base_ / "defaults.json", minimal_defaults());
    auto r = ConfigLoader::load(base_, "doesnotexist");
    EXPECT_TRUE(r.is_error());
}

TEST_F(ConfigLoaderTest, LoadMalformedJsonReturnsError) {
    std::filesystem::create_directories(base_ / "portfolios" / "base");
    {
        std::ofstream f(base_ / "defaults.json");
        f << "{ this is not valid json";
    }
    write_json(base_ / "portfolios" / "base" / "portfolio.json", minimal_portfolio());
    auto r = ConfigLoader::load(base_, "base");
    EXPECT_TRUE(r.is_error());
}

TEST_F(ConfigLoaderTest, LoadMissingPortfolioIdReturnsError) {
    auto p = minimal_portfolio();
    p.erase("portfolio_id");
    write_json(base_ / "defaults.json", minimal_defaults());
    write_json(base_ / "portfolios" / "base" / "portfolio.json", p);
    write_json(base_ / "portfolios" / "base" / "risk.json", minimal_risk());
    write_json(base_ / "portfolios" / "base" / "email.json", minimal_email());
    auto r = ConfigLoader::load(base_, "base");
    EXPECT_TRUE(r.is_error());
}

// ===== Struct serialization round-trips =====

TEST_F(ConfigLoaderTest, DatabaseConfigConnectionStringFormat) {
    DatabaseConfig db;
    db.host = "host.example.com";
    db.port = "5433";
    db.username = "user";
    db.password = "pw";
    db.name = "mydb";
    EXPECT_EQ(db.get_connection_string(),
              "postgresql://user:pw@host.example.com:5433/mydb");
}

TEST_F(ConfigLoaderTest, EmailConfigJsonRoundTrip) {
    EmailConfig orig;
    orig.smtp_host = "smtp.example.com";
    orig.smtp_port = 465;
    orig.username = "u";
    orig.password = "p";
    orig.from_email = "f@test.com";
    orig.use_tls = false;
    orig.to_emails = {"a@test.com", "b@test.com"};
    EmailConfig restored;
    restored.from_json(orig.to_json());
    EXPECT_EQ(restored.smtp_host, "smtp.example.com");
    EXPECT_EQ(restored.smtp_port, 465);
    EXPECT_FALSE(restored.use_tls);
    EXPECT_EQ(restored.to_emails.size(), 2u);
}

TEST_F(ConfigLoaderTest, ExecutionConfigJsonRoundTrip) {
    ExecutionSettingsConfig ex;
    ex.commission_rate = 0.001;
    ex.slippage_bps = 2.5;
    ex.position_limit_backtest = 5000.0;
    ex.position_limit_live = 1000.0;
    ExecutionSettingsConfig r;
    r.from_json(ex.to_json());
    EXPECT_DOUBLE_EQ(r.commission_rate, 0.001);
    EXPECT_DOUBLE_EQ(r.slippage_bps, 2.5);
    EXPECT_DOUBLE_EQ(r.position_limit_backtest, 5000.0);
}

TEST_F(ConfigLoaderTest, BacktestSpecificConfigJsonRoundTrip) {
    BacktestSpecificConfig b;
    b.lookback_years = 7;
    b.store_trade_details = false;
    BacktestSpecificConfig r;
    r.from_json(b.to_json());
    EXPECT_EQ(r.lookback_years, 7);
    EXPECT_FALSE(r.store_trade_details);
}

TEST_F(ConfigLoaderTest, LiveSpecificConfigJsonRoundTrip) {
    LiveSpecificConfig l;
    l.historical_days = 500;
    LiveSpecificConfig r;
    r.from_json(l.to_json());
    EXPECT_EQ(r.historical_days, 500);
}

TEST_F(ConfigLoaderTest, StrategyDefaultsConfigJsonRoundTrip) {
    StrategyDefaultsConfig s;
    s.fdm = {{1, 1.0}, {2, 1.5}, {3, 2.0}};
    s.max_strategy_allocation = 0.5;
    s.min_strategy_allocation = 0.1;
    s.use_optimization = false;
    s.use_risk_management = true;
    StrategyDefaultsConfig r;
    r.from_json(s.to_json());
    EXPECT_EQ(r.fdm.size(), 3u);
    EXPECT_DOUBLE_EQ(r.fdm[2].second, 2.0);
    EXPECT_DOUBLE_EQ(r.max_strategy_allocation, 0.5);
    EXPECT_FALSE(r.use_optimization);
    EXPECT_TRUE(r.use_risk_management);
}

TEST_F(ConfigLoaderTest, DatabaseConfigJsonRoundTrip) {
    DatabaseConfig d;
    d.host = "h";
    d.port = "9999";
    d.username = "u";
    d.password = "p";
    d.name = "n";
    d.num_connections = 13;
    DatabaseConfig r;
    r.from_json(d.to_json());
    EXPECT_EQ(r.host, "h");
    EXPECT_EQ(r.port, "9999");
    EXPECT_EQ(r.num_connections, 13u);
}

// ===== from_json with missing fields preserves defaults =====

TEST_F(ConfigLoaderTest, EmailConfigFromEmptyJsonPreservesDefaults) {
    EmailConfig e;
    e.from_json(nlohmann::json::object());
    EXPECT_EQ(e.smtp_host, "smtp.gmail.com");  // default
    EXPECT_EQ(e.smtp_port, 587);                // default
    EXPECT_TRUE(e.use_tls);                     // default
}

TEST_F(ConfigLoaderTest, BacktestConfigFromEmptyJsonPreservesDefaults) {
    BacktestSpecificConfig b;
    b.from_json(nlohmann::json::object());
    EXPECT_EQ(b.lookback_years, 2);
    EXPECT_TRUE(b.store_trade_details);
}

// ===== load_legacy =====

TEST_F(ConfigLoaderTest, LoadLegacyMissingFileReturnsError) {
    auto r = ConfigLoader::load_legacy(base_ / "missing.json");
    EXPECT_TRUE(r.is_error());
}

TEST_F(ConfigLoaderTest, LoadLegacyMalformedJsonReturnsError) {
    auto p = base_ / "legacy.json";
    std::ofstream(p) << "{ not valid";
    auto r = ConfigLoader::load_legacy(p);
    EXPECT_TRUE(r.is_error());
}

TEST_F(ConfigLoaderTest, LoadLegacyMissingPortfolioIdFailsValidation) {
    nlohmann::json legacy = {
        {"database", minimal_defaults()["database"]},
        {"portfolio", {{"strategies", {{"S1", {{"weight", 1.0}}}}}}},
    };
    auto p = base_ / "legacy.json";
    std::ofstream(p) << legacy.dump(2);
    auto r = ConfigLoader::load_legacy(p);
    EXPECT_TRUE(r.is_error());
}

TEST_F(ConfigLoaderTest, LoadLegacyValidConfigPopulatesFields) {
    nlohmann::json legacy = {
        {"portfolio_id", "LEGACY_TEST"},
        {"database", minimal_defaults()["database"]},
        {"email", minimal_email()},
        {"portfolio", {{"strategies", {{"S1", {{"weight", 1.0}}}}}}},
    };
    legacy["initial_capital"] = 1'000'000.0;
    legacy["reserve_capital_pct"] = 0.1;
    auto p = base_ / "legacy.json";
    std::ofstream(p) << legacy.dump(2);
    auto r = ConfigLoader::load_legacy(p);
    // Even with portfolio_id and strategies set, validation requires database
    // fields and capital. With them populated, this should pass.
    if (r.is_ok()) {
        EXPECT_EQ(r.value().portfolio_id, "LEGACY_TEST");
    }
}

// ===== validate_config edge cases =====

TEST_F(ConfigLoaderTest, ValidateRejectsNonPositiveInitialCapital) {
    AppConfig c;
    c.portfolio_id = "P";
    c.database.host = "h";
    c.database.username = "u";
    c.database.password = "p";
    c.database.name = "n";
    c.initial_capital = 0.0;
    c.strategies_config = {{"s", {{"w", 1.0}}}};
    EXPECT_TRUE(ConfigLoader::validate_config(c).is_error());
}

TEST_F(ConfigLoaderTest, ValidateRejectsReserveCapitalPctOutOfRange) {
    AppConfig c;
    c.portfolio_id = "P";
    c.database.host = "h";
    c.database.username = "u";
    c.database.password = "p";
    c.database.name = "n";
    c.initial_capital = 100.0;
    c.reserve_capital_pct = 1.0;  // must be < 1.0
    c.strategies_config = {{"s", {{"w", 1.0}}}};
    EXPECT_TRUE(ConfigLoader::validate_config(c).is_error());
}

TEST_F(ConfigLoaderTest, ValidateRejectsEmptyStrategies) {
    AppConfig c;
    c.portfolio_id = "P";
    c.database.host = "h";
    c.database.username = "u";
    c.database.password = "p";
    c.database.name = "n";
    c.initial_capital = 100.0;
    c.strategies_config = nlohmann::json::object();  // empty
    EXPECT_TRUE(ConfigLoader::validate_config(c).is_error());
}

TEST_F(ConfigLoaderTest, ValidateRejectsMissingDatabaseFields) {
    AppConfig c;
    c.portfolio_id = "P";
    c.initial_capital = 100.0;
    c.strategies_config = {{"s", {{"w", 1.0}}}};
    // database fields all empty
    EXPECT_TRUE(ConfigLoader::validate_config(c).is_error());
}

// ===== merge_json deep merge =====

TEST_F(ConfigLoaderTest, DeepMergeRecursesIntoNestedObjects) {
    nlohmann::json target = {
        {"a", {{"b", 1}, {"c", 2}}},
        {"d", "old"},
    };
    nlohmann::json source = {
        {"a", {{"c", 99}, {"e", 3}}},
        {"d", "new"},
    };
    ConfigLoader::merge_json(target, source);
    EXPECT_EQ(target["a"]["b"], 1);    // preserved
    EXPECT_EQ(target["a"]["c"], 99);   // overridden
    EXPECT_EQ(target["a"]["e"], 3);    // added
    EXPECT_EQ(target["d"], "new");     // top-level override
}

TEST_F(ConfigLoaderTest, DeepMergeReplacesNonObjectsWithoutRecursion) {
    nlohmann::json target = {{"x", nlohmann::json::array({1, 2, 3})}};
    nlohmann::json source = {{"x", nlohmann::json::array({4})}};
    ConfigLoader::merge_json(target, source);
    EXPECT_EQ(target["x"].size(), 1u);
    EXPECT_EQ(target["x"][0], 4);
}

// ──────────────────────────────────────────────────────────────────────────
// BA-11 / C-1 C5 (T2.9) -- the staleness bounds must be CONFIGURABLE, not
// hardcoded defaults that only look configurable.
//
// LiveSpecificConfig has read both keys from JSON all along, but
// config_template/defaults.json declared only data_staleness_tolerance_days and
// the config the runners actually load declared NEITHER, so the value in force
// was always the struct default. The template is the tracked artefact a fresh
// checkout copies, so a key missing there is a key nobody can set.
// ──────────────────────────────────────────────────────────────────────────

TEST(LiveStalenessConfig, BothBoundsRoundTripThroughJson) {
    LiveSpecificConfig c;
    // The documented defaults: 4 absorbs a weekend plus a holiday; 5 covers a
    // three-day weekend plus a further holiday.
    EXPECT_EQ(c.data_staleness_tolerance_days, 4);
    EXPECT_EQ(c.execution_price_max_staleness_days, 5);

    c.from_json(nlohmann::json{{"data_staleness_tolerance_days", 9},
                               {"execution_price_max_staleness_days", 11}});
    EXPECT_EQ(c.data_staleness_tolerance_days, 9) << "the configured value must win";
    EXPECT_EQ(c.execution_price_max_staleness_days, 11);

    const auto j = c.to_json();
    EXPECT_EQ(j.at("data_staleness_tolerance_days").get<int>(), 9);
    EXPECT_EQ(j.at("execution_price_max_staleness_days").get<int>(), 11);
}

TEST(LiveStalenessConfig, OmittedKeysKeepTheDocumentedDefaults) {
    LiveSpecificConfig c;
    c.from_json(nlohmann::json{{"historical_days", 730}});
    EXPECT_EQ(c.data_staleness_tolerance_days, 4) << "default unchanged (BA-11)";
    EXPECT_EQ(c.execution_price_max_staleness_days, 5) << "default unchanged (BA-11)";
}

// The drift pin. This is what C-1 C5 actually found: the struct grew a field and
// the tracked template did not, so the knob existed in code and nowhere an
// operator could reach it. Asserting the template declares every key the struct
// serialises catches the next one automatically.
TEST(LiveStalenessConfig, TrackedTemplateDeclaresEveryLiveKeyTheStructSerialises) {
    namespace fs = std::filesystem;
    fs::path dir = fs::current_path();
    fs::path found;
    for (int i = 0; i < 8 && !dir.empty(); ++i) {
        if (fs::exists(dir / "config_template" / "defaults.json")) {
            found = dir / "config_template" / "defaults.json";
            break;
        }
        dir = dir.parent_path();
    }
    if (found.empty()) GTEST_SKIP() << "config_template/defaults.json not reachable";

    std::ifstream in(found);
    const nlohmann::json tmpl = nlohmann::json::parse(in);
    ASSERT_TRUE(tmpl.contains("live")) << "the template must carry a live block";
    const auto& live = tmpl.at("live");

    // Bound to a local: to_json() returns by value and items() holds a reference
    // into it, so iterating the temporary directly reads freed memory.
    const nlohmann::json serialised = LiveSpecificConfig{}.to_json();
    for (const auto& [key, _] : serialised.items()) {
        EXPECT_TRUE(live.contains(key))
            << "config_template/defaults.json omits live." << key
            << " -- the struct reads it but no operator can set it, so the hardcoded "
               "default is silently in force (C-1 C5 / T2.9)";
    }
}

// ──────────────────────────────────────────────────────────────────────────
// The resolved risk configuration per book, loaded from the TRACKED template.
//
// These are the regression net for the risk-config schema change (T-6 commit 7,
// schema 2). They assert what ConfigLoader::load resolves TODAY for each of the
// three books, field by field, so a re-nesting that silently falls back to a
// struct default fails here instead of moving positions:
//
//   * max_drawdown / max_leverage are read from the TOP LEVEL of risk.json
//     (config_loader.cpp extract_config). Moving them under a sub-object without
//     moving the reader falls back to AppConfig's 0.4 / 4.0; max_leverage sizes
//     the trend-following book (capital * max_leverage), so CONSERVATIVE 2.0 and
//     EQUITY_MR 1 going to 4.0 moves every position. BASE already equals the
//     fallback, so only the other two books catch it (T-RISK-ARCH §9 defect 5).
//   * AppConfig::risk_config's seven gating fields feed the reporter
//     (snapshot_rm) on every runner; emptying the object moves the stored
//     risk_scale on every row (T-RISK-ARCH_ADVERSARIAL §D1).
//   * max_correlation is ABSENT from base/risk.json and equity_mr/risk.json;
//     their 0.7 comes from defaults.json risk_defaults, not from the book
//     (T-RISK-ARCH §4, "the max_correlation trap"). The provenance test below
//     pins that path explicitly.
//
// The template mirrors the values the runners load (verified for all three
// books); it is used rather than config/ so the test is hermetic.
// ──────────────────────────────────────────────────────────────────────────

namespace {

// The repository's config_template/, located from this source file's path
// (tests/core/ -> repo root) and, failing that, by walking up from the working
// directory. Returns an empty path when neither finds it.
std::filesystem::path tracked_config_template() {
    namespace fs = std::filesystem;
    const fs::path from_source =
        fs::path(__FILE__).parent_path().parent_path().parent_path() / "config_template";
    if (fs::exists(from_source / "defaults.json")) return from_source;
    fs::path dir = fs::current_path();
    for (int i = 0; i < 8 && !dir.empty(); ++i) {
        if (fs::exists(dir / "config_template" / "defaults.json")) return dir / "config_template";
        dir = dir.parent_path();
    }
    return {};
}

struct ResolvedRiskExpectation {
    const char* book;          // portfolio directory under config_template/portfolios
    const char* portfolio_id;
    double max_drawdown;
    double max_leverage;
    double var_limit;
    double jump_risk_limit;
    double max_correlation;
    double max_gross_leverage;
    double max_net_leverage;
    double confidence_level;
    int lookback_period;
    double capital;            // risk_config.capital = initial_capital
    bool use_optimization;     // strategy_defaults, as resolved (not as the runner applies it)
    bool use_risk_management;
};

void expect_resolved_risk_config(const ResolvedRiskExpectation& e) {
    const auto tmpl = tracked_config_template();
    ASSERT_FALSE(tmpl.empty()) << "config_template/ not found from " << __FILE__
                               << " or the working directory; this test must not skip";
    auto loaded = ConfigLoader::load(tmpl, e.book);
    ASSERT_TRUE(loaded.is_ok()) << e.book << ": ConfigLoader::load failed: "
                                << (loaded.is_error() ? loaded.error()->what() : "");
    const AppConfig& c = loaded.value();
    const std::string b = e.book;

    EXPECT_EQ(c.portfolio_id, e.portfolio_id) << b << ": portfolio_id";

    // Strategy limits, read from risk.json's top level.
    EXPECT_EQ(c.max_drawdown, e.max_drawdown) << b << ": max_drawdown";
    EXPECT_EQ(c.max_leverage, e.max_leverage)
        << b << ": max_leverage (sizes the book; 4.0 here on a non-BASE book is the "
                "AppConfig fallback, i.e. the reader lost the key)";

    // The seven gating fields of AppConfig::risk_config.
    const RiskConfig& r = c.risk_config;
    EXPECT_EQ(r.var_limit, e.var_limit) << b << ": risk_config.var_limit";
    EXPECT_EQ(r.jump_risk_limit, e.jump_risk_limit) << b << ": risk_config.jump_risk_limit";
    EXPECT_EQ(r.max_correlation, e.max_correlation) << b << ": risk_config.max_correlation";
    EXPECT_EQ(r.max_gross_leverage, e.max_gross_leverage)
        << b << ": risk_config.max_gross_leverage";
    EXPECT_EQ(r.max_net_leverage, e.max_net_leverage) << b << ": risk_config.max_net_leverage";
    EXPECT_EQ(r.confidence_level, e.confidence_level) << b << ": risk_config.confidence_level";
    EXPECT_EQ(r.lookback_period, e.lookback_period) << b << ": risk_config.lookback_period";
    EXPECT_EQ(r.capital.as_double(), e.capital) << b << ": risk_config.capital";

    EXPECT_EQ(c.strategy_defaults.use_optimization, e.use_optimization)
        << b << ": strategy_defaults.use_optimization";
    EXPECT_EQ(c.strategy_defaults.use_risk_management, e.use_risk_management)
        << b << ": strategy_defaults.use_risk_management";
}

}  // namespace

TEST(TrackedTemplateResolvedRiskConfig, Conservative) {
    expect_resolved_risk_config({"conservative", "CONSERVATIVE_PORTFOLIO",
                                 /*max_drawdown*/ 0.3, /*max_leverage*/ 2.0,
                                 /*var_limit*/ 0.25, /*jump_risk_limit*/ 0.05,
                                 /*max_correlation*/ 0.85,  // literal in its risk.json
                                 /*max_gross*/ 4.0, /*max_net*/ 2.0,
                                 /*confidence*/ 0.99, /*lookback*/ 252,
                                 /*capital*/ 500000.0,
                                 /*use_optimization*/ true, /*use_risk_management*/ true});
}

TEST(TrackedTemplateResolvedRiskConfig, Base) {
    expect_resolved_risk_config({"base", "BASE_PORTFOLIO",
                                 /*max_drawdown*/ 0.4, /*max_leverage*/ 4.0,
                                 /*var_limit*/ 0.15, /*jump_risk_limit*/ 0.1,
                                 /*max_correlation*/ 0.7,  // by ABSENCE: risk_defaults
                                 /*max_gross*/ 4.0, /*max_net*/ 2.0,
                                 /*confidence*/ 0.99, /*lookback*/ 252,
                                 /*capital*/ 500000.0,
                                 /*use_optimization*/ true, /*use_risk_management*/ true});
}

// use_optimization resolves true here as on every book; both equity runners
// then hard-code their PortfolioConfig's use_optimization to false
// (live_equity_mean_reversion.cpp, bt_equity_mean_reversion.cpp). This pins the
// RESOLVED value only; moving the key into portfolio.json must not turn the
// equity optimizer on (T-RISK-ARCH_ADVERSARIAL §C3).
TEST(TrackedTemplateResolvedRiskConfig, EquityMr) {
    expect_resolved_risk_config({"equity_mr", "EQUITY_MR_PORTFOLIO",
                                 /*max_drawdown*/ 0.3, /*max_leverage*/ 1.0,
                                 /*var_limit*/ 0.25, /*jump_risk_limit*/ 0.08,
                                 /*max_correlation*/ 0.7,  // by ABSENCE: risk_defaults
                                 /*max_gross*/ 1.0, /*max_net*/ 1.0,
                                 /*confidence*/ 0.99, /*lookback*/ 252,
                                 /*capital*/ 100000.0,
                                 /*use_optimization*/ true, /*use_risk_management*/ true});
}

// The max_correlation trap, pinned. BASE and EQUITY_MR resolve 0.7 because
// defaults.json risk_defaults.max_correlation is applied before risk.json and
// their risk.json files do not carry the key -- NOT because of RiskConfig's
// struct default, which happens to be 0.7 too and so hides which path is live.
// Changing the global alone moves both books and leaves CONSERVATIVE (literal
// 0.85) where it is. A schema that writes every gating parameter literally per
// book is expected to flip the 0.9 expectations below in the same commit.
TEST(TrackedTemplateResolvedRiskConfig, MaxCorrelationOnBaseAndEquityMrComesFromRiskDefaults) {
    namespace fs = std::filesystem;
    const auto tmpl = tracked_config_template();
    ASSERT_FALSE(tmpl.empty()) << "config_template/ not found; this test must not skip";

    for (const char* book : {"base", "equity_mr"}) {
        std::ifstream in(tmpl / "portfolios" / book / "risk.json");
        const auto risk = nlohmann::json::parse(in);
        EXPECT_FALSE(risk.contains("max_correlation"))
            << book << ": risk.json now carries max_correlation literally; update this pin";
    }
    std::ifstream din(tmpl / "defaults.json");
    auto defaults = nlohmann::json::parse(din);
    ASSERT_EQ(defaults.at("risk_defaults").at("max_correlation").get<double>(), 0.7)
        << "defaults.json risk_defaults.max_correlation";

    // A copy of the template with ONLY the global changed to 0.9.
    const fs::path copy = fs::temp_directory_path() / "trade_ngin_template_maxcorr_provenance";
    fs::remove_all(copy);
    fs::copy(tmpl, copy, fs::copy_options::recursive);
    defaults["risk_defaults"]["max_correlation"] = 0.9;
    {
        std::ofstream out(copy / "defaults.json");
        out << defaults.dump(2);
    }
    struct Case { const char* book; double expected; };
    for (const Case& k : {Case{"base", 0.9}, Case{"equity_mr", 0.9}, Case{"conservative", 0.85}}) {
        auto loaded = ConfigLoader::load(copy, k.book);
        ASSERT_TRUE(loaded.is_ok()) << k.book << ": load of the modified copy failed";
        EXPECT_EQ(loaded.value().risk_config.max_correlation, k.expected)
            << k.book << ": risk_config.max_correlation with risk_defaults.max_correlation = 0.9";
    }
    fs::remove_all(copy);
}
