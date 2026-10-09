// QT plan E2: desk settings from the database (strategy_config overlay).
//
// Ported by hand from PR #60 (tests/core/test_config_loader_db_overlay.cpp) with its fall-back
// assertions REVERSED (ruling 24: a saved settings change that cannot be applied refuses the run
// and never falls back to the files), and from gen-3 (codex/config-88-49,
// tests/core/test_live_config_override.cpp:35-72) as regression specs adapted to merge semantics:
// an unrelated edit keeps the limits, the credentials and the fractions; protected, unknown,
// structural and malformed overrides are refused; the settings snapshot is credential-free and
// parses back to the same config.
//
// The base config is the TRACKED config_template, read in place, so these assertions are about
// the books that ship.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

// Reach the private extract_config for the parse -> rebuild -> compare check, as
// test_config_loader.cpp does. Std headers are loaded before the macro flips.
#include <optional>
#include <variant>
#define private public
#include "trade_ngin/core/config_loader.hpp"
#undef private
#include "trade_ngin/live/live_settings.hpp"

using namespace trade_ngin;
using Json = nlohmann::json;

namespace {

std::filesystem::path repo_root() {
    namespace fs = std::filesystem;
    const fs::path from_source = fs::path(__FILE__).parent_path().parent_path().parent_path();
    if (fs::exists(from_source / "config_template" / "defaults.json")) return from_source;
    fs::path dir = fs::current_path();
    for (int i = 0; i < 8 && !dir.empty(); ++i) {
        if (fs::exists(dir / "config_template" / "defaults.json")) return dir;
        dir = dir.parent_path();
    }
    return {};
}

std::filesystem::path tmpl() { return repo_root() / "config_template"; }

std::string read_file(const std::filesystem::path& p) {
    std::ifstream in(p);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

Json file_json(const char* book, const char* file) {
    return Json::parse(read_file(tmpl() / "portfolios" / book / file));
}

/// load() with an overlay; returns the error text, empty on success.
std::string refusal(const Json& overlay, const char* book = "conservative") {
    Json snap;
    auto r = ConfigLoader::load(tmpl(), book, &overlay, &snap);
    return r.is_ok() ? std::string() : std::string(r.error()->what());
}

Json without(Json j, std::initializer_list<const char*> keys) {
    for (const char* k : keys) j.erase(k);
    return j;
}

class StrategyConfigOverlay : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_FALSE(repo_root().empty()) << "config_template/ not found; this test must not skip";
    }
};

}  // namespace

// ===== No row, or an empty row: the files, unchanged =====

TEST_F(StrategyConfigOverlay, NoOverlayIsExactlyTheFileConfig) {
    for (const char* book : {"conservative", "base", "equity_mr"}) {
        auto files = ConfigLoader::load(tmpl(), book);
        ASSERT_TRUE(files.is_ok()) << book << ": " << files.error()->what();
        Json snap;
        auto live = ConfigLoader::load(tmpl(), book, nullptr, &snap);
        ASSERT_TRUE(live.is_ok()) << book << ": " << live.error()->what();
        EXPECT_EQ(live.value().to_json(), files.value().to_json()) << book;
        EXPECT_FALSE(snap.empty()) << book << ": the settings used were not produced";
    }
}

TEST_F(StrategyConfigOverlay, AnEmptyObjectChangesNothing) {
    const Json overlay = Json::object();
    auto files = ConfigLoader::load(tmpl(), "conservative");
    auto live = ConfigLoader::load(tmpl(), "conservative", &overlay);
    ASSERT_TRUE(live.is_ok()) << live.error()->what();
    EXPECT_EQ(live.value().to_json(), files.value().to_json());
}

// ===== Ruling 3: any knob the files hold =====

// gen-3 UnrelatedEditPreservesLimitsCredentialsAndFractions, adapted: #60 re-extracted from
// to_json and so moved CONSERVATIVE's limits to the struct fallbacks (0.4 / 4.0) on ANY
// override. Here the overlay merges into the files' own JSON: one knob moves, nothing else.
TEST_F(StrategyConfigOverlay, AnUnrelatedEditMovesOnlyItsKnob) {
    auto files = ConfigLoader::load(tmpl(), "conservative");
    ASSERT_TRUE(files.is_ok());
    const Json overlay = {{"optimization", {{"cost_penalty_scalar", 12.75}}}};
    auto live = ConfigLoader::load(tmpl(), "conservative", &overlay);
    ASSERT_TRUE(live.is_ok()) << live.error()->what();
    const AppConfig& a = files.value();
    const AppConfig& b = live.value();
    EXPECT_EQ(b.opt_config.cost_penalty_scalar, 12.75);
    EXPECT_EQ(b.max_drawdown, a.max_drawdown);
    EXPECT_EQ(b.max_drawdown, 0.3) << "CONSERVATIVE's risk.json max_drawdown";
    EXPECT_EQ(b.max_leverage, a.max_leverage);
    EXPECT_EQ(b.risk_schema.to_json(), a.risk_schema.to_json());
    EXPECT_EQ(b.database.password, a.database.password);
    EXPECT_EQ(b.email.password, a.email.password);
    EXPECT_EQ(b.execution.to_json(), a.execution.to_json());
    Json ja = a.to_json(), jb = b.to_json();
    ja["optimization"].erase("cost_penalty_scalar");
    jb["optimization"].erase("cost_penalty_scalar");
    EXPECT_EQ(jb, ja) << "an edit of one knob moved another";
}

TEST_F(StrategyConfigOverlay, RiskStrategyAndTopLevelKnobsCanBeTurned) {
    // risk.json's own top-level limit
    {
        const Json overlay = {{"risk", {{"max_drawdown", 0.25}}}};
        auto r = ConfigLoader::load(tmpl(), "conservative", &overlay);
        ASSERT_TRUE(r.is_ok()) << r.error()->what();
        EXPECT_EQ(r.value().max_drawdown, 0.25);
        EXPECT_EQ(r.value().risk_schema.max_drawdown, 0.25);
    }
    // an array (the module list) is replaced whole, here with one limit changed
    {
        Json modules = file_json("conservative", "risk.json").at("modules");
        modules[0]["max_gross_leverage"] = 7.0;
        modules[0]["max_net_leverage"] = 5.0;
        Json reporting = {{"max_gross_leverage", 7.0}, {"max_net_leverage", 5.0}};
        const Json overlay = {{"risk", {{"modules", modules}, {"risk_reporting", reporting}}}};
        auto r = ConfigLoader::load(tmpl(), "conservative", &overlay);
        ASSERT_TRUE(r.is_ok()) << r.error()->what();
        EXPECT_EQ(r.value().risk_config.max_gross_leverage, 7.0);
        EXPECT_EQ(r.value().risk_config.max_net_leverage, 5.0);
    }
    // a strategy parameter
    {
        const Json overlay = {
            {"strategies", {{"TREND_FOLLOWING", {{"config", {{"risk_target", 0.15}}}}}}}};
        auto r = ConfigLoader::load(tmpl(), "conservative", &overlay);
        ASSERT_TRUE(r.is_ok()) << r.error()->what();
        EXPECT_EQ(r.value().strategies_config["TREND_FOLLOWING"]["config"]["risk_target"], 0.15);
        EXPECT_EQ(r.value().strategies_config["TREND_FOLLOWING"]["config"]["idm"], 2.5)
            << "a sibling key of the edited one moved";
    }
    // a top-level knob, and a whole number where the file has a fraction
    {
        const Json overlay = {{"covariance_stale_dates", 3}, {"live", {{"historical_days", 800}}}};
        auto r = ConfigLoader::load(tmpl(), "conservative", &overlay);
        ASSERT_TRUE(r.is_ok()) << r.error()->what();
        EXPECT_EQ(r.value().covariance_stale_dates, 3u);
        EXPECT_EQ(r.value().live.historical_days, 800);
    }
}

// ===== Refusals (ruling 24): each one refuses, none falls back =====

TEST_F(StrategyConfigOverlay, ANonObjectIsRefused) {
    for (const Json& overlay : {Json(nullptr), Json::array(), Json::array({1}), Json(1), Json("x"),
                                Json(true)}) {
        const std::string why = refusal(overlay);
        EXPECT_NE(why, "") << overlay.dump() << " was applied";
        EXPECT_NE(why.find("object"), std::string::npos) << why;
    }
}

// #60: {"risk": null} reset the risk limits to the compiled defaults.
TEST_F(StrategyConfigOverlay, ANullAnywhereIsRefused) {
    for (const Json& overlay :
         {Json{{"risk", nullptr}}, Json{{"optimization", {{"cost_penalty_scalar", nullptr}}}},
          Json{{"strategies",
                {{"TREND_FOLLOWING",
                  {{"config", {{"ema_windows", Json::array({Json::array({2, 8}), nullptr})}}}}}}}},
          Json{{"risk", {{"modules", Json::array({Json{{"max_gross_leverage", nullptr}}})}}}}}) {
        const std::string why = refusal(overlay);
        EXPECT_NE(why, "") << overlay.dump() << " was applied";
        EXPECT_NE(why.find("null"), std::string::npos) << why;
    }
}

TEST_F(StrategyConfigOverlay, TheDatabaseAndEmailSectionsAndThePortfolioIdAreRefused) {
    for (const Json& overlay :
         {Json{{"database", {{"host", "elsewhere"}}}}, Json{{"database", {{"password", "pw"}}}},
          Json{{"database", Json::object()}}, Json{{"email", {{"password", "pw"}}}},
          Json{{"email", {{"to_emails", Json::array({"someone@example.com"})}}}},
          Json{{"email", Json::object()}}, Json{{"portfolio_id", "BASE_PORTFOLIO"}}}) {
        const std::string why = refusal(overlay);
        EXPECT_NE(why, "") << overlay.dump() << " was applied";
        const std::string key = overlay.begin().key();
        EXPECT_NE(why.find(key), std::string::npos) << why;
    }
}

// Review E#12: the runner reads qt.desk_editable (and the rest of the qt block) from portfolio.json
// only, so an override of it would be recorded in settings_used and ignored by the run. Refused,
// on a portfolio whose files hold the block and on one whose files do not.
TEST_F(StrategyConfigOverlay, TheQtBlockIsRefused) {
    const std::vector<Json> overlays = {Json{{"qt", {{"desk_editable", false}}}},
                                        Json{{"qt", {{"desk_editable", true}}}},
                                        Json{{"qt", Json::object()}}};
    for (const Json& overlay : overlays) {
        const std::string why = refusal(overlay, "conservative");
        EXPECT_NE(why.find("qt (the desk settings"), std::string::npos) << why;
    }
    // Against a desk-editable portfolio's own portfolio.json, which holds the very same key and
    // kind (so the shape check alone would let it through).
    for (const char* book : {"qt_e2e", "qt_conservative"}) {
        const Json files = file_json(book, "portfolio.json");
        ASSERT_TRUE(files.contains("qt")) << book;
        for (const Json& overlay : overlays) {
            auto checked = ConfigLoader::check_overlay(overlay, files);
            ASSERT_TRUE(checked.is_error()) << book << ": " << overlay.dump() << " was applied";
            EXPECT_NE(std::string(checked.error()->what()).find("portfolio.json only"),
                      std::string::npos)
                << checked.error()->what();
        }
    }
    // Another key of the same file still overrides.
    EXPECT_TRUE(ConfigLoader::check_overlay(Json{{"initial_capital", 1.0}},
                                            Json{{"initial_capital", 2.0}, {"qt", Json::object()}})
                    .is_ok());
}

TEST_F(StrategyConfigOverlay, ACredentialLikeKeyAnywhereIsRefusedAndItsValueNeverEchoed) {
    for (const Json& overlay :
         {Json{{"strategies", {{"TREND_FOLLOWING", {{"config", {{"api_token", "sk-SENSITIVE"}}}}}}}},
          Json{{"optimization", {{"client_secret", "sk-SENSITIVE"}}}},
          Json{{"live", {{"smtp_relay", "sk-SENSITIVE"}}}},
          Json{{"execution", {{"broker_credentials", {{"user", "sk-SENSITIVE"}}}}}},
          Json{{"database", {{"password", "sk-SENSITIVE"}}}}}) {
        const std::string why = refusal(overlay);
        EXPECT_NE(why, "") << overlay.dump() << " was applied";
        EXPECT_EQ(why.find("sk-SENSITIVE"), std::string::npos) << "a value was echoed: " << why;
    }
}

// Typo protection (ruling 3 covers knobs the files HOLD): a key the files do not have is refused.
TEST_F(StrategyConfigOverlay, AKeyTheFilesDoNotHoldIsRefused) {
    for (const Json& overlay :
         {Json{{"optimisation", {{"cost_penalty_scalar", 1.0}}}},
          Json{{"risk", {{"max_drawdwn", 0.2}}}}, Json{{"new", 1}},
          Json{{"risk", {{"max_leverage", 2.0}}}},  // retired on CONSERVATIVE: not in its files
          Json{{"max_leverage", 2.0}},              // #60 to_json's top-level copy: never read
          Json{{"strategies", {{"TREND_FOLOWING", {{"config", {{"risk_target", 0.1}}}}}}}}}) {
        const std::string why = refusal(overlay);
        EXPECT_NE(why, "") << overlay.dump() << " was applied";
        EXPECT_NE(why.find("not a key of the config files"), std::string::npos) << why;
    }
}

TEST_F(StrategyConfigOverlay, AChangeOfShapeOrTypeIsRefused) {
    for (const Json& overlay :
         {Json{{"risk", 1}}, Json{{"optimization", {{"cost_penalty_scalar", {{"a", 1}}}}}},
          Json{{"use_optimization", "yes"}}, Json{{"initial_capital", "500000"}},
          Json{{"strategies", {{"TREND_FOLLOWING", {{"config", {{"ema_windows", 8}}}}}}}},
          Json{{"live", {{"historical_days", true}}}}}) {
        const std::string why = refusal(overlay);
        EXPECT_NE(why, "") << overlay.dump() << " was applied";
    }
}

// The merged config goes through the same extract and validate as the files alone.
TEST_F(StrategyConfigOverlay, AMergedConfigTheLoaderRejectsIsRefused) {
    // starting_capital must equal initial_capital (extract_config)
    EXPECT_NE(refusal({{"starting_capital", 600000.0}}), "");
    // risk.max_drawdown in (0, 1] (parse_risk_schema)
    EXPECT_NE(refusal({{"risk", {{"max_drawdown", 1.5}}}}), "");
    // sizing_mode has one value (extract_config)
    EXPECT_NE(refusal({{"sizing_mode", "full_compounding"}}), "");
    // initial_capital positive (validate_config) on a book without starting_capital
    const std::string why = refusal({{"initial_capital", -5.0}}, "equity_mr");
    EXPECT_NE(why.find("initial_capital must be positive"), std::string::npos) << why;
}

TEST_F(StrategyConfigOverlay, ARefusalNamesTheStrategyConfigOverlay) {
    const std::string why = refusal({{"risk", nullptr}});
    EXPECT_NE(why.find("strategy_config"), std::string::npos) << why;
}

// ===== The settings used: credential-free, and it parses back to the same config =====

TEST_F(StrategyConfigOverlay, TheSettingsUsedCarryNoCredentialAndReproduceTheConfig) {
    for (const char* book : {"conservative", "base", "equity_mr"}) {
        Json snap;
        auto r = ConfigLoader::load(tmpl(), book, nullptr, &snap);
        ASSERT_TRUE(r.is_ok()) << book << ": " << r.error()->what();
        EXPECT_FALSE(snap.contains("database")) << book;
        EXPECT_FALSE(snap.contains("email")) << book;
        std::string where;
        EXPECT_FALSE(ConfigLoader::find_secret_key(snap, "", &where)) << book << ": " << where;
        const std::string text = snap.dump();
        EXPECT_EQ(text.find("YOUR_DB_PASSWORD"), std::string::npos) << book;
        EXPECT_EQ(text.find("YOUR_SMTP_APP_PASSWORD"), std::string::npos) << book;
        // parse -> rebuild -> compare (gen-3's guard): the snapshot is the config.
        auto again = ConfigLoader::extract_config(snap);
        ASSERT_TRUE(again.is_ok()) << book << ": " << again.error()->what();
        EXPECT_EQ(without(again.value().to_json(), {"database", "email"}),
                  without(r.value().to_json(), {"database", "email"}))
            << book;
        EXPECT_TRUE(snap.contains("risk") && snap.contains("strategies")) << book;
    }
}

TEST_F(StrategyConfigOverlay, TheSettingsUsedShowTheOverride) {
    const Json overlay = {{"optimization", {{"cost_penalty_scalar", 12.75}}}};
    Json snap;
    auto r = ConfigLoader::load(tmpl(), "conservative", &overlay, &snap);
    ASSERT_TRUE(r.is_ok()) << r.error()->what();
    EXPECT_EQ(snap["optimization"]["cost_penalty_scalar"], 12.75);
}

TEST(SecretKeyScan, NamesCredentialLikeKeysByPathOnly) {
    std::string where;
    EXPECT_TRUE(ConfigLoader::find_secret_key({{"a", {{"db_password", 1}}}}, "", &where));
    EXPECT_EQ(where, "a.db_password");
    for (const char* key : {"password", "PASSWORD", "api_token", "Client_Secret", "credentials",
                            "smtp_host", "database", "Email"}) {
        EXPECT_TRUE(ConfigLoader::find_secret_key({{"x", {{key, "v"}}}}, "", &where)) << key;
    }
    EXPECT_TRUE(ConfigLoader::find_secret_key(
        {{"risk", {{"modules", Json::array({Json::object(), Json{{"secret", 1}}})}}}}, "", &where));
    EXPECT_EQ(where, "risk.modules[1].secret");
    // Values are not keys: a note that mentions a password is not a credential.
    EXPECT_FALSE(ConfigLoader::find_secret_key(
        {{"_note", "the password lives in email.json"}, {"risk", {{"max_drawdown", 0.3}}}}, "",
        &where));
}

// A credential-like key in the files themselves (outside database/email) makes the live load
// refuse to produce the settings used; a backtest's plain load is untouched (ruling 4).
TEST(StrategyConfigOverlayFiles, ACredentialLikeKeyInTheFilesRefusesTheSettingsUsed) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "trade_ngin_e2_secret_in_files";
    fs::remove_all(dir);
    fs::create_directories(dir / "portfolios");
    fs::copy(tmpl() / "defaults.json", dir / "defaults.json");
    fs::copy(tmpl() / "portfolios" / "conservative", dir / "portfolios" / "conservative");
    Json portfolio = file_json("conservative", "portfolio.json");
    portfolio["strategies"]["TREND_FOLLOWING"]["config"]["auth_token"] = "sk-SENSITIVE";
    {
        std::ofstream out(dir / "portfolios" / "conservative" / "portfolio.json");
        out << portfolio.dump(2);
    }
    EXPECT_TRUE(ConfigLoader::load(dir, "conservative").is_ok());
    Json snap;
    auto r = ConfigLoader::load(dir, "conservative", nullptr, &snap);
    ASSERT_TRUE(r.is_error());
    EXPECT_NE(std::string(r.error()->what()).find("auth_token"), std::string::npos);
    EXPECT_EQ(std::string(r.error()->what()).find("sk-SENSITIVE"), std::string::npos);
    fs::remove_all(dir);
}

// ===== resolve_live_settings: the runners' one entry point =====

namespace {
Result<std::optional<StrategyConfigRow>> no_row() {
    return Result<std::optional<StrategyConfigRow>>(std::optional<StrategyConfigRow>{});
}
Result<std::optional<StrategyConfigRow>> a_row(int version, Json overrides,
                                               std::string portfolio = "CONSERVATIVE_PORTFOLIO") {
    StrategyConfigRow row;
    row.portfolio_id = std::move(portfolio);
    row.version = version;
    row.overrides = std::move(overrides);
    row.reason = "desk test";
    row.created_by = "dom";
    return Result<std::optional<StrategyConfigRow>>(std::optional<StrategyConfigRow>(row));
}
}  // namespace

TEST_F(StrategyConfigOverlay, ALookupErrorRefusesTheRunAndNeverFallsBack) {
    auto failed = make_error<std::optional<StrategyConfigRow>>(ErrorCode::DATABASE_ERROR,
                                                               "connection lost", "test");
    auto r = resolve_live_settings(tmpl(), "conservative", "CONSERVATIVE_PORTFOLIO", failed);
    ASSERT_TRUE(r.is_error()) << "a failed lookup ran on the files (#60's fall-back)";
    EXPECT_NE(std::string(r.error()->what()).find("connection lost"), std::string::npos);
}

TEST_F(StrategyConfigOverlay, NoActiveRowRunsOnTheFilesAndRecordsANullVersion) {
    auto r = resolve_live_settings(tmpl(), "conservative", "CONSERVATIVE_PORTFOLIO", no_row());
    ASSERT_TRUE(r.is_ok()) << r.error()->what();
    EXPECT_FALSE(r.value().strategy_config_version.has_value());
    EXPECT_TRUE(r.value().settings_used.at("strategy_config_version").is_null());
    EXPECT_EQ(r.value().config.to_json(),
              ConfigLoader::load(tmpl(), "conservative").value().to_json());
    EXPECT_FALSE(r.value().settings_used.at("config").contains("database"));
}

TEST_F(StrategyConfigOverlay, AnActiveRowIsAppliedAndItsVersionRecorded) {
    auto r = resolve_live_settings(tmpl(), "conservative", "CONSERVATIVE_PORTFOLIO",
                                   a_row(7, {{"risk", {{"max_drawdown", 0.25}}}}));
    ASSERT_TRUE(r.is_ok()) << r.error()->what();
    EXPECT_EQ(r.value().strategy_config_version, 7);
    EXPECT_EQ(r.value().settings_used.at("strategy_config_version"), 7);
    EXPECT_EQ(r.value().settings_used.at("config").at("risk").at("max_drawdown"), 0.25);
    EXPECT_EQ(r.value().config.max_drawdown, 0.25);
}

TEST_F(StrategyConfigOverlay, ARowThatCannotBeAppliedRefusesAndNamesItsVersion) {
    auto r = resolve_live_settings(tmpl(), "conservative", "CONSERVATIVE_PORTFOLIO",
                                   a_row(9, {{"risk", nullptr}}));
    ASSERT_TRUE(r.is_error());
    EXPECT_NE(std::string(r.error()->what()).find("version 9"), std::string::npos)
        << r.error()->what();
    const std::string line = live_settings_refusal_line("CONSERVATIVE_PORTFOLIO", r.error()->what());
    EXPECT_NE(line.find("STRATEGY_CONFIG refused"), std::string::npos);
}

TEST_F(StrategyConfigOverlay, ARowOfAnotherPortfolioIsRefused) {
    auto r = resolve_live_settings(tmpl(), "conservative", "CONSERVATIVE_PORTFOLIO",
                                   a_row(1, Json::object(), "BASE_PORTFOLIO"));
    ASSERT_TRUE(r.is_error());
}
