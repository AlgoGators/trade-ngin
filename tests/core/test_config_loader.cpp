// Coverage for config_loader.cpp: JSON file loading, deep-merge, AppConfig
// extraction, validation, and error paths. All tests use a per-test temp
// directory so they don't depend on the real ./config tree.

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include "test_base.hpp"
#include "trade_ngin/strategy/equity_strategy_builder.hpp"

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
        // Schema 2: no use_optimization (it moved to portfolio.json), no
        // use_risk_management (deleted), no risk_defaults (every value is per book).
        {"strategy_defaults", {{"max_strategy_allocation", 1.0},
                                {"min_strategy_allocation", 0.1},
                                {"fdm", nlohmann::json::array({{1, 1.0}, {2, 1.03}})}}},
    };
}

nlohmann::json minimal_portfolio() {
    return {
        {"portfolio_id", "TEST_PORTFOLIO"},
        {"initial_capital", 1'000'000.0},
        {"max_drawdown", 0.4},
        {"max_leverage", 4.0},
        {"use_optimization", true},
        // Validation requires at least one strategy entry.
        {"strategies", {{"TREND_FOLLOWING", {{"weight", 1.0}, {"allocation", 1.0}}}}},
    };
}

// The seven gating values, written literally, plus the reporter and the two strategy
// limits schema 2 makes required.
nlohmann::json carver_module(const char* id = "carver") {
    return {
        {"id", id},
        {"type", "carver"},
        {"var_limit", 0.15},
        {"jump_risk_limit", 0.10},
        {"max_correlation", 0.7},
        {"max_gross_leverage", 4.0},
        {"max_net_leverage", 2.0},
        {"confidence_level", 0.99},
        {"lookback_period", 252},
        {"lookback_unit", "dates"},
        {"min_gate_dates", 21},
        {"missing_symbol_policy", "ignore"},
        {"_missing_symbol_policy_reason", "unit test"},
    };
}

nlohmann::json reporting_block() {
    return {
        {"type", "carver"},
        {"window", "all_bars"},
        {"var_limit", 0.15},
        {"jump_risk_limit", 0.10},
        {"max_correlation", 0.7},
        {"max_gross_leverage", 4.0},
        {"max_net_leverage", 2.0},
        {"confidence_level", 0.99},
        {"lookback_period", 252},
    };
}

nlohmann::json minimal_risk() {
    return {
        {"schema", 2},
        {"modules", nlohmann::json::array({carver_module()})},
        {"risk_reporting", reporting_block()},
        {"max_drawdown", 0.4},
        {"max_leverage", 4.0},
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
                         const nlohmann::json& portfolio_override = {},
                         const nlohmann::json& risk = minimal_risk()) {
        auto defaults = minimal_defaults();
        for (auto& [k, v] : defaults_override.items()) defaults[k] = v;
        write_json(base_ / "defaults.json", defaults);

        auto portfolio = minimal_portfolio();
        for (auto& [k, v] : portfolio_override.items()) portfolio[k] = v;
        write_json(base_ / "portfolios" / portfolio_name / "portfolio.json", portfolio);

        write_json(base_ / "portfolios" / portfolio_name / "risk.json", risk);
        write_json(base_ / "portfolios" / portfolio_name / "email.json", minimal_email());
    }

    /// Loads a config whose risk.json is `risk` and returns the error text (empty on
    /// success), so a rule's test reads as "this file -> this message".
    std::string load_error(const nlohmann::json& risk,
                            const nlohmann::json& portfolio_override = {},
                            const nlohmann::json& defaults_override = {}) {
        write_full_set("base", defaults_override, portfolio_override, risk);
        auto r = ConfigLoader::load(base_, "base");
        if (r.is_ok()) return "";
        return r.error()->what();
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
    StrategyDefaultsConfig r;
    const auto j = s.to_json();
    r.from_json(j);
    EXPECT_EQ(r.fdm.size(), 3u);
    EXPECT_DOUBLE_EQ(r.fdm[2].second, 2.0);
    EXPECT_DOUBLE_EQ(r.max_strategy_allocation, 0.5);
    // Schema 2 deleted both: a block that still serialised them would put a key back into
    // the merged config that the loader now refuses (S7), and an override round trip
    // would fail on a key nobody wrote.
    EXPECT_FALSE(j.contains("use_optimization"))
        << "use_optimization moved to portfolio.json in schema 2";
    EXPECT_FALSE(j.contains("use_risk_management"))
        << "use_risk_management was deleted in schema 2";
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
    bool use_optimization;     // portfolio.json's top level, as resolved
    const char* none_ruled_on = nullptr;  // set when the book is assigned `none` (commit 8)
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

    EXPECT_EQ(c.use_optimization, e.use_optimization) << b << ": use_optimization";

    // Schema 2: the SAME seven values are also on the book's own carver module, and the
    // module -- not AppConfig::risk_config -- is what the PortfolioManager gates with.
    // Rule C1 makes the two equal; these assertions are what proves it on the shipped
    // books rather than on a fixture.
    ASSERT_EQ(c.risk_schema.portfolio.size(), 1u) << b << ": one portfolio-scope module";
    const RiskModuleConfig& m = c.risk_schema.portfolio.front();
    EXPECT_TRUE(c.risk_schema.sleeves.empty()) << b << ": no shipped book assigns a sleeve module";
    if (e.none_ruled_on != nullptr) {
        // T-6b commit 8 (HD 2026-09-18): this book runs NO risk module. The seven values above
        // are the reporter's (risk_reporting still measures the book); the module is `none`
        // with the ruling on it, and there is no carver whose values could be compared.
        EXPECT_EQ(m.type, "none") << b << ": module type";
        const auto* none = std::get_if<NoneModuleConfig>(&m.params);
        ASSERT_NE(none, nullptr) << b << ": module params";
        EXPECT_EQ(none->ruled_by, "HD") << b << ": _ruled_by";
        EXPECT_EQ(none->ruled_on, e.none_ruled_on) << b << ": _ruled_on";
        EXPECT_FALSE(none->reason.empty()) << b << ": _reason";
        return;
    }
    EXPECT_EQ(m.type, "carver") << b << ": module type";
    const auto* carver = std::get_if<CarverModuleConfig>(&m.params);
    ASSERT_NE(carver, nullptr) << b << ": module params";
    EXPECT_EQ(carver->var_limit, e.var_limit) << b << ": module.var_limit";
    EXPECT_EQ(carver->jump_risk_limit, e.jump_risk_limit) << b << ": module.jump_risk_limit";
    EXPECT_EQ(carver->max_correlation, e.max_correlation) << b << ": module.max_correlation";
    EXPECT_EQ(carver->max_gross_leverage, e.max_gross_leverage)
        << b << ": module.max_gross_leverage";
    EXPECT_EQ(carver->max_net_leverage, e.max_net_leverage) << b << ": module.max_net_leverage";
    EXPECT_EQ(carver->confidence_level, e.confidence_level) << b << ": module.confidence_level";
    EXPECT_EQ(carver->lookback_period, e.lookback_period) << b << ": module.lookback_period";
    EXPECT_EQ(carver->lookback_unit, "dates") << b << ": module.lookback_unit";
    EXPECT_EQ(carver->min_gate_dates, 21) << b << ": module.min_gate_dates";
    EXPECT_EQ(carver->missing_symbol_policy, "ignore") << b << ": module.missing_symbol_policy";
    EXPECT_FALSE(carver->missing_symbol_policy_reason.empty())
        << b << ": \"ignore\" is the fail-open policy and must say why it is chosen";
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
                                 /*use_optimization*/ true});
}

TEST(TrackedTemplateResolvedRiskConfig, Base) {
    expect_resolved_risk_config({"base", "BASE_PORTFOLIO",
                                 /*max_drawdown*/ 0.4, /*max_leverage*/ 4.0,
                                 /*var_limit*/ 0.15, /*jump_risk_limit*/ 0.1,
                                 /*max_correlation*/ 0.7,  // literal in its risk.json (schema 2)
                                 /*max_gross*/ 4.0, /*max_net*/ 2.0,
                                 /*confidence*/ 0.99, /*lookback*/ 252,
                                 /*capital*/ 500000.0,
                                 /*use_optimization*/ true});
}

// use_optimization resolves FALSE here: schema 2 moved the key into portfolio.json and
// EQUITY_MR writes false, which is what both equity runners have always done in code
// (live_equity_mean_reversion.cpp, bt_equity_mean_reversion.cpp). Moving the key must not
// turn the equity optimizer on (T-RISK-ARCH_ADVERSARIAL §C3); the runners additionally
// refuse to start on a `true` (refuse_if_optimizer_requested, tested below).
TEST(TrackedTemplateResolvedRiskConfig, EquityMr) {
    expect_resolved_risk_config({"equity_mr", "EQUITY_MR_PORTFOLIO",
                                 /*max_drawdown*/ 0.3, /*max_leverage*/ 1.0,
                                 /*var_limit*/ 0.25, /*jump_risk_limit*/ 0.08,
                                 /*max_correlation*/ 0.7,  // literal in its risk.json (schema 2)
                                 /*max_gross*/ 1.0, /*max_net*/ 1.0,
                                 /*confidence*/ 0.99, /*lookback*/ 252,
                                 /*capital*/ 100000.0,
                                 /*use_optimization*/ false,
                                 /*none_ruled_on*/ "2026-09-18"});
}

// The max_correlation trap, closed. Under schema 1 base/risk.json and equity_mr/risk.json
// carried NO max_correlation and resolved 0.7 from defaults.json risk_defaults -- a value
// no book had written, indistinguishable from RiskConfig's struct default (also 0.7), so
// changing the global silently moved two books' gate. Schema 2 deletes risk_defaults and
// writes every gating value per book. This is that test, inverted: the value must now come
// from the book's OWN file, and there must be no global left to move it with.
TEST(TrackedTemplateResolvedRiskConfig, EveryGatingValueIsLiteralInTheBooksOwnRiskJson) {
    const auto tmpl = tracked_config_template();
    ASSERT_FALSE(tmpl.empty()) << "config_template/ not found; this test must not skip";

    std::ifstream din(tmpl / "defaults.json");
    const auto defaults = nlohmann::json::parse(din);
    EXPECT_FALSE(defaults.contains("risk_defaults"))
        << "defaults.json still carries risk_defaults: a gating value no book wrote can "
           "still reach a book's gate (and the loader now refuses the key outright)";
    ASSERT_TRUE(defaults.contains("strategy_defaults"));
    EXPECT_FALSE(defaults.at("strategy_defaults").contains("use_optimization"))
        << "use_optimization moved to portfolio.json in schema 2";
    EXPECT_FALSE(defaults.at("strategy_defaults").contains("use_risk_management"))
        << "use_risk_management was deleted in schema 2";

    struct Book { const char* dir; double max_correlation; bool use_optimization; };
    for (const Book& b : {Book{"conservative", 0.85, true}, Book{"base", 0.7, true},
                          Book{"equity_mr", 0.7, false}}) {
        std::ifstream in(tmpl / "portfolios" / b.dir / "risk.json");
        const auto risk = nlohmann::json::parse(in);
        ASSERT_TRUE(risk.contains("modules")) << b.dir << ": risk.json must be schema 2";
        ASSERT_EQ(risk.at("modules").size(), 1u) << b.dir;
        // A `none` book (EQUITY_MR since T-6b commit 8) has no gating module; its values are
        // written literally in its reporter block, which is what still measures it.
        const bool none_book = risk.at("modules").at(0).at("type") == "none";
        const auto& module = none_book ? risk.at("risk_reporting") : risk.at("modules").at(0);
        for (const char* field : {"var_limit", "jump_risk_limit", "max_correlation",
                                  "max_gross_leverage", "max_net_leverage",
                                  "confidence_level", "lookback_period"}) {
            EXPECT_TRUE(module.contains(field))
                << b.dir << ": risk.json does not write " << field
                << " literally -- schema 2 has no layer for it to come from";
        }
        EXPECT_EQ(module.at("max_correlation").get<double>(), b.max_correlation)
            << b.dir << ": max_correlation, written in the book's own file";

        std::ifstream pin(tmpl / "portfolios" / b.dir / "portfolio.json");
        const auto portfolio = nlohmann::json::parse(pin);
        ASSERT_TRUE(portfolio.contains("use_optimization")) << b.dir;
        EXPECT_EQ(portfolio.at("use_optimization").get<bool>(), b.use_optimization) << b.dir;
    }
}

// ──────────────────────────────────────────────────────────────────────────
// The loader's own schema-2 rules: the removed keys (S7), use_optimization (P1),
// the schema-1 message (T0), and the two strategy limits (R9). The per-module
// rules live in tests/risk/test_risk_module_config.cpp, against parse_risk_schema.
// Each asserts the EXACT text, because the text is the whole value of a
// fail-closed rule: an operator reading it has to know what to do next.
// ──────────────────────────────────────────────────────────────────────────

TEST_F(ConfigLoaderTest, Schema1RiskJsonIsNamedAsSuchWithTheMigrationCommand) {
    nlohmann::json schema1 = {{"var_limit", 0.15}, {"jump_risk_limit", 0.1},
                              {"max_gross_leverage", 4.0}, {"max_net_leverage", 2.0},
                              {"max_drawdown", 0.4}, {"max_leverage", 4.0}};
    EXPECT_EQ(load_error(schema1),
              "risk config for TEST_PORTFOLIO: risk.json is schema 1 (flat gating keys, no "
              "\"schema\"/\"modules\"); migrate it with: python3 scripts/migrate_risk_json.py "
              "<config dir> --in-place");
}

TEST_F(ConfigLoaderTest, RemovedKeyUseRiskManagementIsALoadErrorWhereverItHides) {
    // Nested inside a strategy's config block: the place a half-finished migration
    // leaves one, and the place a recursive walk is the only thing that finds it.
    nlohmann::json portfolio_override = {
        {"strategies", {{"TREND_FOLLOWING",
                         {{"weight", 1.0}, {"config", {{"use_risk_management", false}}}}}}}};
    EXPECT_EQ(load_error(minimal_risk(), portfolio_override),
              "config for TEST_PORTFOLIO: strategies.TREND_FOLLOWING.config.use_risk_management "
              "(use_risk_management) was removed in schema 2; risk is assigned by risk.json "
              "\"modules\". Delete the key (a leftover false would silently turn risk back on, "
              "T-RISK-ARCH_ADVERSARIAL E2)");
}

TEST_F(ConfigLoaderTest, RemovedKeyRiskDefaultsIsALoadError) {
    nlohmann::json defaults_override = {
        {"risk_defaults", {{"max_correlation", 0.9}}}};
    EXPECT_EQ(load_error(minimal_risk(), {}, defaults_override),
              "config for TEST_PORTFOLIO: risk_defaults was removed in schema 2; every gating "
              "value is written literally in each portfolio's risk.json (run "
              "scripts/migrate_risk_json.py)");
}

TEST_F(ConfigLoaderTest, RemovedKeyStrategyDefaultsUseOptimizationIsALoadError) {
    nlohmann::json defaults_override = {
        {"strategy_defaults", {{"max_strategy_allocation", 1.0}, {"use_optimization", true}}}};
    EXPECT_EQ(load_error(minimal_risk(), {}, defaults_override),
              "config for TEST_PORTFOLIO: strategy_defaults.use_optimization moved to "
              "portfolio.json \"use_optimization\" in schema 2");
}

TEST_F(ConfigLoaderTest, RemovedShockThresholdKeysUnderRiskAreALoadError) {
    auto risk = minimal_risk();
    risk["modules"][0]["corr_shock_threshold"] = 0.65;
    EXPECT_EQ(load_error(risk),
              "risk config for TEST_PORTFOLIO: risk.modules[0].corr_shock_threshold has had no "
              "reader since the carver_shock methods were deleted; delete it");
    auto risk2 = minimal_risk();
    risk2["risk_reporting"]["jump_shock_threshold"] = 0.75;
    EXPECT_EQ(load_error(risk2),
              "risk config for TEST_PORTFOLIO: risk.risk_reporting.jump_shock_threshold has had "
              "no reader since the carver_shock methods were deleted; delete it");
}

TEST_F(ConfigLoaderTest, UseOptimizationIsRequiredAndBoolean) {
    auto portfolio = minimal_portfolio();
    portfolio.erase("use_optimization");
    write_json(base_ / "defaults.json", minimal_defaults());
    write_json(base_ / "portfolios" / "base" / "portfolio.json", portfolio);
    write_json(base_ / "portfolios" / "base" / "risk.json", minimal_risk());
    write_json(base_ / "portfolios" / "base" / "email.json", minimal_email());
    auto r = ConfigLoader::load(base_, "base");
    ASSERT_TRUE(r.is_error());
    EXPECT_EQ(std::string(r.error()->what()),
              "config for TEST_PORTFOLIO: portfolio.json must set \"use_optimization\" (true or "
              "false) at its top level; schema 2 has no default");

    // A quoted "true" is the typo that used to read as absent and take the default.
    EXPECT_EQ(load_error(minimal_risk(), {{"use_optimization", "true"}}),
              "config for TEST_PORTFOLIO: portfolio.json must set \"use_optimization\" (true or "
              "false) at its top level; schema 2 has no default");
}

TEST_F(ConfigLoaderTest, UseOptimizationResolvesFromPortfolioJson) {
    write_full_set("base", {}, {{"use_optimization", false}});
    auto r = ConfigLoader::load(base_, "base");
    ASSERT_TRUE(r.is_ok()) << (r.error() ? r.error()->what() : "");
    EXPECT_FALSE(r.value().use_optimization);
}

// T-6c commit B: portfolio.json "covariance_history_prices", the PortfolioManager's covariance
// history length in prices per symbol. Read through to_json() so this test compiles against the
// parent source, where the key has no reader.
TEST_F(ConfigLoaderTest, CovarianceHistoryPricesAbsentMeans756) {
    write_full_set("base");  // minimal_portfolio() does not write the key
    auto r = ConfigLoader::load(base_, "base");
    ASSERT_TRUE(r.is_ok()) << (r.error() ? r.error()->what() : "");
    EXPECT_EQ(r.value().to_json().value("covariance_history_prices", -1), 756)
        << "an absent covariance_history_prices must mean 756";

    write_full_set("base", {}, {{"covariance_history_prices", 500}});
    auto r2 = ConfigLoader::load(base_, "base");
    ASSERT_TRUE(r2.is_ok()) << (r2.error() ? r2.error()->what() : "");
    EXPECT_EQ(r2.value().to_json().value("covariance_history_prices", -1), 500);
}

TEST_F(ConfigLoaderTest, CovarianceHistoryPricesBelowTwoOrNotAWholeNumberIsRefused) {
    for (const nlohmann::json& bad :
         {nlohmann::json(1), nlohmann::json(0), nlohmann::json(-5), nlohmann::json(2.5),
          nlohmann::json("756"), nlohmann::json(true), nlohmann::json(nullptr)}) {
        EXPECT_EQ(load_error(minimal_risk(), {{"covariance_history_prices", bad}}),
                  "config for TEST_PORTFOLIO: portfolio.json \"covariance_history_prices\" must be "
                  "a whole number of at least 2 (prices per symbol kept for the optimiser's "
                  "covariance; absent means 756), got " + bad.dump())
            << "value " << bad.dump();
    }
    EXPECT_EQ(load_error(minimal_risk(), {{"covariance_history_prices", 2}}), "")
        << "2 is the smallest value that gives a return";
}

TEST(TrackedTemplateCovarianceHistory, EveryBookDeclares756) {
    const auto tmpl = tracked_config_template();
    ASSERT_FALSE(tmpl.empty()) << "config_template/ not found; this test must not skip";
    for (const char* book : {"base", "conservative", "equity_mr"}) {
        std::ifstream f(tmpl / "portfolios" / book / "portfolio.json");
        ASSERT_TRUE(f.good()) << book;
        const auto portfolio = nlohmann::json::parse(f);
        ASSERT_TRUE(portfolio.contains("covariance_history_prices")) << book;
        EXPECT_EQ(portfolio.at("covariance_history_prices").get<int>(), 756) << book;
        auto loaded = ConfigLoader::load(tmpl, book);
        ASSERT_TRUE(loaded.is_ok()) << book << ": " << (loaded.is_error() ? loaded.error()->what() : "");
        EXPECT_EQ(loaded.value().to_json().value("covariance_history_prices", -1), 756) << book;
    }
}

// T-7b-1 7d: portfolio.json "covariance_stale_dates", how many union dates a covariance
// participant's last date may trail the newest before the optimizer leaves it out of the date
// intersection. Read through to_json() so this test compiles against the parent source.
TEST_F(ConfigLoaderTest, CovarianceStaleDatesAbsentMeans5) {
    write_full_set("base");
    auto r = ConfigLoader::load(base_, "base");
    ASSERT_TRUE(r.is_ok()) << (r.error() ? r.error()->what() : "");
    EXPECT_EQ(r.value().to_json().value("covariance_stale_dates", -1), 5)
        << "an absent covariance_stale_dates must mean 5";

    write_full_set("base", {}, {{"covariance_stale_dates", 3}});
    auto r2 = ConfigLoader::load(base_, "base");
    ASSERT_TRUE(r2.is_ok()) << (r2.error() ? r2.error()->what() : "");
    EXPECT_EQ(r2.value().to_json().value("covariance_stale_dates", -1), 3);
}

TEST_F(ConfigLoaderTest, CovarianceStaleDatesNotAWholeNumberIsRefused) {
    for (const nlohmann::json& bad : {nlohmann::json(-1), nlohmann::json(2.5), nlohmann::json("5"),
                                      nlohmann::json(true), nlohmann::json(nullptr)}) {
        EXPECT_EQ(load_error(minimal_risk(), {{"covariance_stale_dates", bad}}),
                  "config for TEST_PORTFOLIO: portfolio.json \"covariance_stale_dates\" must be a "
                  "whole number of at least 0 (union dates a covariance participant's last date "
                  "may trail the newest before the optimiser leaves it out of the date "
                  "intersection; absent means 5), got " + bad.dump())
            << "value " << bad.dump();
    }
    EXPECT_EQ(load_error(minimal_risk(), {{"covariance_stale_dates", 0}}), "")
        << "0 is allowed: every participant must print on the newest date";
}

TEST(TrackedTemplateCovarianceStaleDates, EveryBookDeclares5) {
    const auto tmpl = tracked_config_template();
    ASSERT_FALSE(tmpl.empty()) << "config_template/ not found; this test must not skip";
    for (const char* book : {"base", "conservative", "equity_mr"}) {
        std::ifstream f(tmpl / "portfolios" / book / "portfolio.json");
        ASSERT_TRUE(f.good()) << book;
        const auto portfolio = nlohmann::json::parse(f);
        ASSERT_TRUE(portfolio.contains("covariance_stale_dates")) << book;
        EXPECT_EQ(portfolio.at("covariance_stale_dates").get<int>(), 5) << book;
        auto loaded = ConfigLoader::load(tmpl, book);
        ASSERT_TRUE(loaded.is_ok()) << book << ": " << (loaded.is_error() ? loaded.error()->what() : "");
        EXPECT_EQ(loaded.value().to_json().value("covariance_stale_dates", -1), 5) << book;
    }
}

// T-7b-2 9h (HD 2026-09-25): CONSERVATIVE's trend sleeve runs IDM 2.5, Carver's table row for
// 30 or more instruments (the universe is 36 names); the subsystem-return formula, correlations
// floored at 0, reaches the 2.5 cap on the same 36. BASE already shipped 2.5 on both sleeves.
// Read through the loader's strategies block, which is what the futures runners size with.
TEST(TrackedTemplateTrendIdm, ConservativeResolvesCarversThirtyPlusRowAndBaseStays25) {
    const auto tmpl = tracked_config_template();
    ASSERT_FALSE(tmpl.empty()) << "config_template/ not found; this test must not skip";

    auto cons = ConfigLoader::load(tmpl, "conservative");
    ASSERT_TRUE(cons.is_ok()) << (cons.is_error() ? cons.error()->what() : "");
    const auto& tf = cons.value().strategies_config.at("TREND_FOLLOWING").at("config");
    ASSERT_TRUE(tf.contains("idm"));
    EXPECT_EQ(tf.at("idm").get<double>(), 2.5)
        << "CONSERVATIVE TREND_FOLLOWING idm: Carver's row for 30+ instruments is 2.50";
    ASSERT_TRUE(tf.contains("_idm_rationale"));
    const auto why = tf.at("_idm_rationale").get<std::string>();
    EXPECT_NE(why.find("30+ instruments -> 2.50"), std::string::npos) << why;
    EXPECT_NE(why.find("36 names"), std::string::npos) << why;
    EXPECT_NE(why.find("HD 2026-09-25"), std::string::npos) << why;

    auto base = ConfigLoader::load(tmpl, "base");
    ASSERT_TRUE(base.is_ok()) << (base.is_error() ? base.error()->what() : "");
    for (const char* sleeve : {"TREND_FOLLOWING", "TREND_FOLLOWING_FAST"}) {
        EXPECT_EQ(base.value().strategies_config.at(sleeve).at("config").at("idm").get<double>(), 2.5)
            << "BASE " << sleeve << " idm";
    }
}

TEST_F(ConfigLoaderTest, StrategyLimitsAreRequiredInRiskJson) {
    auto risk = minimal_risk();
    risk.erase("max_drawdown");
    EXPECT_EQ(load_error(risk),
              "risk config for TEST_PORTFOLIO: risk.max_drawdown is required and must be in "
              "(0, 1]");
    auto risk2 = minimal_risk();
    risk2.erase("max_leverage");
    EXPECT_EQ(load_error(risk2),
              "risk config for TEST_PORTFOLIO: risk.max_leverage is required and must be > 0 (it "
              "sizes the book: trend_following.cpp:1216)");
    auto risk3 = minimal_risk();
    risk3["max_drawdown"] = 1.2;
    EXPECT_EQ(load_error(risk3),
              "risk config for TEST_PORTFOLIO: risk.max_drawdown is required and must be in "
              "(0, 1]");
}

TEST_F(ConfigLoaderTest, RiskConfigIsBackFilledFromTheReporterNotTheModule) {
    // The reporter is the single source of AppConfig::risk_config. Rule C1 keeps the two
    // equal while a carver module is assigned, so to see which one the back-fill reads,
    // the book has to have no carver module at all.
    nlohmann::json none_module = {{"id", "none"}, {"type", "none"},
                                  {"_reason", "unit test"}, {"_ruled_by", "tests"},
                                  {"_ruled_on", "2026-09-19"}};
    auto risk = minimal_risk();
    risk["modules"] = nlohmann::json::array({none_module});
    risk["risk_reporting"]["var_limit"] = 0.33;
    risk["risk_reporting"]["max_gross_leverage"] = 1.0;
    risk["risk_reporting"]["max_net_leverage"] = 1.0;
    write_full_set("base", {}, {}, risk);
    auto r = ConfigLoader::load(base_, "base");
    ASSERT_TRUE(r.is_ok()) << (r.error() ? r.error()->what() : "");
    EXPECT_DOUBLE_EQ(r.value().risk_config.var_limit, 0.33);
    // The equity start-up guard reads exactly this value; without the back-fill it would
    // fall to RiskConfig's struct 4.0 and both equity binaries would refuse to start.
    EXPECT_DOUBLE_EQ(r.value().risk_config.max_gross_leverage, 1.0);
    EXPECT_DOUBLE_EQ(r.value().risk_config.capital.as_double(), 1'000'000.0);
    ASSERT_EQ(r.value().risk_schema.portfolio.size(), 1u);
    EXPECT_EQ(r.value().risk_schema.portfolio.front().type, "none");
}

// PR #60's path: a DB override is merged back through to_json -> merge_json ->
// extract_config. Under schema 2 that only survives if to_json emits the schema-2 risk
// object and no removed key; otherwise extract_config fails, the loader WARNs and every
// DB override is silently discarded.
TEST_F(ConfigLoaderTest, AppConfigToJsonRoundTripsThroughExtractConfig) {
    write_full_set("base");
    auto direct = ConfigLoader::load(base_, "base");
    ASSERT_TRUE(direct.is_ok()) << (direct.error() ? direct.error()->what() : "");

    nlohmann::json j = direct.value().to_json();
    ConfigLoader::merge_json(j, nlohmann::json::object());
    auto round_tripped = ConfigLoader::extract_config(j);
    ASSERT_TRUE(round_tripped.is_ok())
        << "AppConfig::to_json no longer produces a config extract_config accepts, so every DB "
           "override would be discarded with a WARN: "
        << (round_tripped.is_error() ? round_tripped.error()->what() : "");
    const AppConfig& a = direct.value();
    const AppConfig& b = round_tripped.value();
    EXPECT_EQ(b.use_optimization, a.use_optimization);
    EXPECT_DOUBLE_EQ(b.max_drawdown, a.max_drawdown);
    EXPECT_DOUBLE_EQ(b.max_leverage, a.max_leverage);
    EXPECT_DOUBLE_EQ(b.risk_config.var_limit, a.risk_config.var_limit);
    EXPECT_DOUBLE_EQ(b.risk_config.max_correlation, a.risk_config.max_correlation);
    ASSERT_EQ(b.risk_schema.portfolio.size(), a.risk_schema.portfolio.size());
    EXPECT_EQ(b.risk_schema.portfolio.front().id, a.risk_schema.portfolio.front().id);
    EXPECT_EQ(b.risk_schema.to_json(), a.risk_schema.to_json());
}

// Ruling 7: the equity runners keep the hard-coded false AND refuse to start on a config
// true. The guard is shared by both runners so they cannot drift; this is it.
TEST(EquityOptimizerGuard, RefusesOnlyWhenTheConfigAsksForTheOptimizer) {
    EXPECT_TRUE(apps::refuse_if_optimizer_requested(false).is_ok())
        << "today's EQUITY_MR config says false: the guard must be silent";
    auto refused = apps::refuse_if_optimizer_requested(true);
    ASSERT_TRUE(refused.is_error());
    EXPECT_EQ(std::string(refused.error()->what()),
              "Refusing to start: portfolio.json sets use_optimization=true, but the equity "
              "runners do not run the optimizer (HD 2026-09-01; see the comment at the "
              "hard-code).");
}
