// tests/live/test_risk_module_error_path.cpp
//
// T-7a commit 5: the risk module error path (HD 2026-09-21, option b).
//
// A portfolio-scope risk module whose evaluate errors REFUSES the scope: the seeded T-1 book is
// held (every strategy pinned to its previous book, as a REFUSE does), no orders are generated,
// an ERROR is logged, the decision is recorded as a REFUSE carrying the error (so C1's metadata
// mark and risk_decisions_json() see it), the live futures runners exit non-zero after storing
// what a REFUSE day stores, and the email carries a visible flag.
//
// Before this commit the shipped shape failed OPEN: the lone Carver module's capabilities are
// {SCALE, WARN}, refuse_on_failed_gatekeeper refused only REFUSE-capable modules, so a Carver
// whose evaluate errored contributed NONE and the book shipped uncut with exit 0; a throw from a
// module's on_bars reached the loop, which logged "continuing without risk management" and
// shipped the book uncut too (T-6_BRANCH_REVIEW section 5 item 1, T-6b A-1r).
//
// The runners are main()s and cannot be linked here, so their placement of the exit path and
// the email flag is tested in the runner source (as test_live_run_refusal_arms.cpp does); the
// pieces that run are run: the PortfolioManager with a real Carver module that throws on one
// day, the helpers the runners call, and the real email builder's body.

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include "../risk/risk_module_test_helpers.hpp"
#include "trade_ngin/core/email_sender.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/live/risk_module_failure.hpp"
#include "trade_ngin/live/run_metadata_marks.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/risk/basic_risk_modules.hpp"
#include "trade_ngin/risk/carver_risk_module.hpp"
#include "trade_ngin/strategy/base_strategy.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

namespace {

using Book = std::unordered_map<std::string, Position>;
constexpr auto npos = std::string::npos;

Timestamp day(int d) {
    return Timestamp(std::chrono::seconds(1767225600LL + 86400LL * d));
}

Bar make_bar(const std::string& symbol, int d, double close) {
    Bar b;
    b.symbol = symbol;
    b.timestamp = day(d);
    b.open = b.high = b.low = b.close = Decimal(close);
    b.volume = 1000.0;
    return b;
}

Position make_pos(const std::string& symbol, double qty, double price) {
    Position p;
    p.symbol = symbol;
    p.quantity = Decimal(qty);
    p.average_price = Decimal(price);
    p.last_update = day(0);
    return p;
}

std::vector<Bar> bars_for_day(int d) {
    return {make_bar("ZZA", d, 100.0 + d), make_bar("ZZA", d + 1, 101.0 + d),
            make_bar("ZZA", d + 2, 99.0 + d)};
}

class ScriptedStrategy : public BaseStrategy {
public:
    ScriptedStrategy(std::string id, StrategyConfig config, std::shared_ptr<DatabaseInterface> db,
                     std::vector<Book> script)
        : BaseStrategy(std::move(id), std::move(config),
                       std::static_pointer_cast<trade_ngin::PostgresDatabase>(db)),
          script_(std::move(script)) {
        metadata_.name = "Scripted Strategy";
    }
    Result<void> on_data(const std::vector<Bar>& data) override {
        (void)data;
        ++calls_;
        return Result<void>();
    }
    Book get_target_positions() const override {
        if (calls_ == 0) return {};
        return script_[std::min(calls_, script_.size()) - 1];
    }

private:
    std::vector<Book> script_;
    size_t calls_{0};
};

const char* const kInjectedError =
    "Risk calculation failed: covariance matrix not positive definite (injected)";

/// The shipped module, a real CarverRiskModule, whose evaluate THROWS on one rebalance (one
/// process_market_data call, counted by begin_rebalance, which the PM calls exactly once per
/// call). Every other call is the unchanged Carver: same id, type, terms and capabilities
/// ({SCALE, WARN}: it cannot REFUSE), same window.
class CarverThatThrowsOnOneDay final : public RiskModule {
public:
    CarverThatThrowsOnOneDay(RiskConfig config, int throw_on_rebalance, bool throw_in_on_bars = false)
        : inner_("carver", std::move(config)),
          throw_on_(throw_on_rebalance),
          throw_in_on_bars_(throw_in_on_bars) {}
    const std::string& id() const override { return inner_.id(); }
    const std::string& type() const override { return inner_.type(); }
    std::set<RiskTerm> terms() const override { return inner_.terms(); }
    std::set<RiskAction> capabilities() const override { return inner_.capabilities(); }
    void begin_rebalance(const RiskContext& ctx) override {
        ++rebalances_;
        inner_.begin_rebalance(ctx);
    }
    void on_bars(const std::vector<Bar>& bars, const RiskContext& ctx) override {
        if (throw_in_on_bars_ && rebalances_ == throw_on_) throw std::runtime_error(kInjectedError);
        inner_.on_bars(bars, ctx);
    }
    Result<RiskDecision> evaluate(const Book& book, const RiskContext& ctx) override {
        if (!throw_in_on_bars_ && rebalances_ == throw_on_) {
            throw std::runtime_error(kInjectedError);
        }
        return inner_.evaluate(book, ctx);
    }
    nlohmann::json describe() const override { return inner_.describe(); }
    void on_applied(const RiskApplied& applied, const RiskContext& ctx) override {
        inner_.on_applied(applied, ctx);
    }

private:
    CarverRiskModule inner_;
    int throw_on_;
    bool throw_in_on_bars_;
    int rebalances_{0};
};

/// Limits no test book reaches: on a healthy day the Carver asks for nothing.
RiskConfig wide_limits() {
    RiskConfig rc;
    rc.capital = 1000.0;
    rc.var_limit = 1e6;
    rc.jump_risk_limit = 1e6;
    rc.max_correlation = 1.0;
    rc.max_gross_leverage = 1e6;
    rc.max_net_leverage = 1e6;
    return rc;
}

PortfolioConfig base_config() {
    PortfolioConfig pc{1000.0, 1.0, 0.0, /*optimization=*/false};
    pc.allow_fractional_positions = false;
    pc.risk_config = wide_limits();
    pc.risk_modules = {test_carver_module(pc.risk_config)};
    return pc;
}

std::filesystem::path find_repo_file(const std::string& relative) {
    namespace fs = std::filesystem;
    fs::path dir = fs::current_path();
    for (int i = 0; i < 8 && !dir.empty(); ++i) {
        if (fs::exists(dir / relative)) return dir / relative;
        dir = dir.parent_path();
    }
    return {};
}

std::string read_source(const std::string& relative) {
    auto path = find_repo_file(relative);
    if (path.empty()) return {};
    std::ifstream in(path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

const char* const kFuturesRunners[] = {"apps/strategies/live_portfolio_conservative.cpp",
                                       "apps/strategies/live_portfolio.cpp"};

/// The text of `src` from `from` up to (not including) `to`, or "" when either is missing.
std::string between(const std::string& src, const std::string& from, const std::string& to) {
    const auto a = src.find(from);
    if (a == npos) return {};
    const auto b = src.find(to, a);
    if (b == npos) return {};
    return src.substr(a, b - a);
}

}  // namespace

class RiskModuleErrorPathTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        db_ = std::make_shared<MockPostgresDatabase>("mock://testdb");
        ASSERT_TRUE(db_->connect().is_ok());
        LoggerConfig lc;
        lc.destination = LogDestination::CONSOLE;
        lc.min_level = LogLevel::INFO;
        lc.include_timestamp = false;
        Logger::instance().initialize(lc);
    }
    void TearDown() override {
        pm_.reset();
        db_.reset();
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        TestBase::TearDown();
    }

    /// One strategy "EP_S" with one target per process_market_data call and a lone Carver that
    /// throws on call `throw_on`.
    void make_pm(std::vector<Book> script, int throw_on, bool throw_in_on_bars = false) {
        static int n = 0;
        pm_id_ = "PM_EP_" + std::to_string(++n);
        pm_ = std::make_unique<PortfolioManager>(base_config(), pm_id_);
        StrategyConfig sc;
        sc.capital_allocation = 1000.0;
        sc.max_leverage = 10.0;
        sc.asset_classes = {AssetClass::FUTURES};
        sc.frequencies = {DataFrequency::DAILY};
        strategy_ = std::make_shared<ScriptedStrategy>("EP_S", sc, db_, std::move(script));
        ASSERT_TRUE(strategy_->initialize().is_ok());
        ASSERT_TRUE(strategy_->start().is_ok());
        ASSERT_TRUE(pm_->add_strategy(strategy_, 1.0, false).is_ok());
        ASSERT_TRUE(pm_->set_risk_modules({std::make_shared<CarverThatThrowsOnOneDay>(
                                              wide_limits(), throw_on, throw_in_on_bars)})
                        .is_ok());
    }

    size_t executions_now() {
        size_t n = 0;
        for (const auto& [sid, reports] : pm_->get_strategy_executions()) {
            (void)sid;
            n += reports.size();
        }
        return n;
    }

    double quantity(const std::string& symbol) {
        return static_cast<double>(pm_->get_strategy_positions().at("EP_S").at(symbol).quantity);
    }

    /// Day 1 healthy (the manager trades into the seeded 2-lot book), day 2 the Carver throws
    /// while the strategy asks for 5 lots. Returns day 2's captured stdout.
    std::string run_two_days(bool throw_in_on_bars = false) {
        make_pm({{{"ZZA", make_pos("ZZA", 2.0, 100.0)}}, {{"ZZA", make_pos("ZZA", 5.0, 100.0)}}},
                /*throw_on=*/2, throw_in_on_bars);
        // Seeded, as both futures runners seed yesterday's stored book before the call.
        EXPECT_TRUE(
            pm_->update_strategy_position("EP_S", "ZZA", make_pos("ZZA", 2.0, 100.0)).is_ok());
        ::testing::internal::CaptureStdout();
        const auto day1 = pm_->process_market_data(bars_for_day(1));
        const std::string out1 = ::testing::internal::GetCapturedStdout();
        EXPECT_TRUE(day1.is_ok()) << out1;
        EXPECT_EQ(quantity("ZZA"), 2.0) << "day 1 is healthy: the strategy's own 2 lots";
        EXPECT_FALSE(portfolio_risk_module_failure(pm_->last_risk_decisions()).has_value())
            << "day 1 has no module failure";
        executions_before_day2_ = executions_now();

        ::testing::internal::CaptureStdout();
        day2_result_ok_ = pm_->process_market_data(bars_for_day(2)).is_ok();
        return ::testing::internal::GetCapturedStdout();
    }

    std::shared_ptr<MockPostgresDatabase> db_;
    std::unique_ptr<PortfolioManager> pm_;
    std::shared_ptr<ScriptedStrategy> strategy_;
    std::string pm_id_;
    size_t executions_before_day2_{0};
    bool day2_result_ok_{false};
};

// =============================================================================================
// The PortfolioManager: a lone Carver whose evaluate throws on one day holds the seeded book.
// =============================================================================================

TEST_F(RiskModuleErrorPathTest, ALoneCarverWhoseEvaluateThrowsOnOneDayHoldsTheSeededBook) {
    const std::string out = run_two_days();
    ASSERT_TRUE(day2_result_ok_) << "a seeded refusal is not a run error\n" << out;
    EXPECT_EQ(quantity("ZZA"), 2.0)
        << "the book must be HELD at the seeded 2 lots; 5 means the uncut book shipped\n"
        << out;
    EXPECT_EQ(executions_now() - executions_before_day2_, 0u)
        << "a held book sends no order";
    EXPECT_NE(out.find(std::string("Risk management calculation failed: ") + kInjectedError), npos)
        << out;
    EXPECT_NE(out.find("[ERROR]"), npos) << out;
    EXPECT_NE(out.find("Risk module carver failed on portfolio " + pm_id_ +
                       " in iteration 1: " + kInjectedError +
                       "; a portfolio-scope module that cannot answer refuses the scope"),
              npos)
        << out;
    EXPECT_NE(out.find("Risk refusal: every strategy pinned to its previous positions after "
                       "iteration 1; leaving the loop"),
              npos)
        << out;
}

TEST_F(RiskModuleErrorPathTest, TheFailureIsRecordedAsARefuseCarryingTheError) {
    run_two_days();
    ASSERT_TRUE(day2_result_ok_);
    bool found = false;
    for (const auto& rec : pm_->last_risk_decisions()) {
        if (rec.scope != RiskScope::PORTFOLIO || rec.phase != RiskPhase::LAP) continue;
        found = true;
        EXPECT_EQ(rec.module_id, "carver");
        EXPECT_EQ(rec.applied_action, RiskAction::REFUSE)
            << "the failed module's row IS the refusal; NONE hides it from the mark";
        EXPECT_EQ(rec.error, kInjectedError);
    }
    EXPECT_TRUE(found);

    // C1's metadata mark reads it.
    const auto refusal = portfolio_risk_refusal(pm_->last_risk_decisions());
    ASSERT_TRUE(refusal.has_value()) << "C1's Q2 mark does not see the module failure";
    EXPECT_EQ((*refusal)["action"], "REFUSE");
    EXPECT_EQ((*refusal)["module"], "carver");
    EXPECT_EQ((*refusal)["scope_id"], pm_id_);
    EXPECT_EQ((*refusal)["reason"], kInjectedError);
    EXPECT_EQ((*refusal)["error"], kInjectedError);

    // risk_decisions_json(): the outcome is a refusal and the row carries the error.
    const auto j = pm_->risk_decisions_json();
    EXPECT_EQ(j["outcome"]["action"], "REFUSE");
    EXPECT_EQ(j["outcome"]["refused"], true);
    EXPECT_EQ(j["outcome"]["pinned_scopes"], nlohmann::json::array({pm_id_}));
    bool row = false;
    for (const auto& d : j["decisions"]) {
        if (d["module"] != "carver" || d["phase"] != "lap") continue;
        row = true;
        EXPECT_EQ(d["applied"]["action"], "REFUSE");
        EXPECT_EQ(d["error"], kInjectedError);
    }
    EXPECT_TRUE(row);
}

// A throw from the module's on_bars used to reach the loop, which logged "continuing without risk
// management" and shipped the book uncut. It is the portfolio risk step failing: refused, held,
// recorded as a REFUSE row of kRiskStepModuleId carrying the error.
TEST_F(RiskModuleErrorPathTest, AnOnBarsThrowRefusesThePortfolioScopeToo) {
    const std::string out = run_two_days(/*throw_in_on_bars=*/true);
    ASSERT_TRUE(day2_result_ok_) << out;
    EXPECT_EQ(quantity("ZZA"), 2.0) << "held, not the uncut 5 lots\n" << out;
    EXPECT_EQ(executions_now() - executions_before_day2_, 0u);
    EXPECT_EQ(out.find("continuing without risk management"), npos) << out;
    EXPECT_NE(out.find("the portfolio risk step could not answer, so the scope is refused"), npos)
        << out;
    const auto failure = portfolio_risk_module_failure(pm_->last_risk_decisions());
    ASSERT_TRUE(failure.has_value());
    EXPECT_EQ((*failure)["module"], kRiskStepModuleId);
    EXPECT_EQ((*failure)["applied"], "REFUSE");
    EXPECT_NE((*failure)["error"].get<std::string>().find(kInjectedError), npos);
    const auto refusal = portfolio_risk_refusal(pm_->last_risk_decisions());
    ASSERT_TRUE(refusal.has_value());
    EXPECT_EQ(pm_->risk_decisions_json()["outcome"]["refused"], true);
}

// The refuse_unseeded rule still applies: a live scope nobody seeded has no previous book to hold,
// so pinning would ship a FLAT book. The call is a run error (the runners exit 1).
TEST_F(RiskModuleErrorPathTest, AModuleFailureOnANeverSeededLiveScopeIsARunError) {
    make_pm({{{"ZZA", make_pos("ZZA", 5.0, 100.0)}}}, /*throw_on=*/1);
    ::testing::internal::CaptureStdout();
    const auto result = pm_->process_market_data(bars_for_day(1));
    const std::string out = ::testing::internal::GetCapturedStdout();
    ASSERT_TRUE(result.is_error()) << "a flat book must not ship as a refusal\n" << out;
    EXPECT_NE(std::string(result.error()->what()).find("never seeded"), npos)
        << result.error()->what();
    EXPECT_NE(out.find("pinning would ship a FLAT book"), npos) << out;
    // Recorded as a REFUSE too, so the runner marks the row before it exits 1.
    EXPECT_TRUE(portfolio_risk_refusal(pm_->last_risk_decisions()).has_value());
}

TEST(RiskModuleErrorPathSource, ThePortfolioManagerNoLongerContinuesWithoutRiskManagement) {
    const std::string src = read_source("src/portfolio/portfolio_manager.cpp");
    if (src.empty()) GTEST_SKIP() << "PM source not found from the test working directory";
    EXPECT_EQ(src.find("continuing without risk management"), npos)
        << "the loop still has a path that ships the book ungated when the risk step fails";
    EXPECT_EQ(src.find("return Result<void>();  // Don't fail the entire operation"), npos)
        << "an exception inside the portfolio risk step still returns OK";
}

// =============================================================================================
// The runner exit-code path (the helpers both futures runners call, and their placement).
// =============================================================================================

TEST_F(RiskModuleErrorPathTest, TheRunnerExitCodePathReturnsNonZeroOnAHeldDay) {
    run_two_days();
    ASSERT_TRUE(day2_result_ok_);
    const auto failure = portfolio_risk_module_failure(pm_->last_risk_decisions());
    ASSERT_TRUE(failure.has_value()) << "the runner does not see the module failure";
    EXPECT_EQ((*failure)["module"], "carver");
    EXPECT_EQ((*failure)["error"], kInjectedError);
    EXPECT_EQ((*failure)["applied"], "REFUSE") << "the failure must have held the book";
    EXPECT_NE(live_run_exit_code(failure), 0) << "a held day must not exit 0";
    EXPECT_EQ(live_run_exit_code(failure), kRiskModuleFailureExitCode);
    EXPECT_NE(kRiskModuleFailureExitCode, 1) << "1 is the refusal to START (nothing stored)";
    EXPECT_EQ(live_run_exit_code(std::nullopt), 0) << "a healthy day still exits 0";
}

TEST(RiskModuleErrorPathSource, BothTwinsReadTheFailureAfterTheMarkAndBeforeTheErrorCheck) {
    for (const char* runner : kFuturesRunners) {
        SCOPED_TRACE(runner);
        const std::string src = read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found from the test working directory";
        const auto process = src.find("portfolio->process_market_data(");
        ASSERT_NE(process, npos);
        const auto mark = src.find("mark_risk_refusal(", process);
        const auto detect = src.find(
            "risk_module_failure = portfolio_risk_module_failure(portfolio->last_risk_decisions());",
            process);
        const auto error_check = src.find("if (port_process_result.is_error())", process);
        ASSERT_NE(mark, npos);
        ASSERT_NE(error_check, npos);
        ASSERT_NE(detect, npos) << "the runner never asks whether a portfolio risk module failed";
        EXPECT_LT(mark, detect) << "the metadata row is marked first";
        EXPECT_LT(detect, error_check)
            << "read before the error check, so an unseeded failure (exit 1) is logged too";
        EXPECT_NE(src.find("ERROR(\"RISK_MODULE_FAILURE portfolio risk module \"", detect), npos);
    }
}

TEST(RiskModuleErrorPathSource, BothTwinsExitNonZeroAfterStoringTheDay) {
    for (const char* runner : kFuturesRunners) {
        SCOPED_TRACE(runner);
        const std::string src = read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found from the test working directory";
        const auto ret = src.find("return live_run_exit_code(risk_module_failure);");
        ASSERT_NE(ret, npos) << "main() still ends with a bare exit 0 on a held day";
        EXPECT_EQ(src.find("\n        return 0;\n"), npos)
            << "a bare `return 0;` is left at the end of main()";
        // After everything a REFUSE day stores: positions, live_results, the email.
        const auto positions = src.find("store_positions");
        const auto results_insert = src.find("INSERT INTO trading.live_results");
        const auto send = src.find("email_sender->send_email(subject, email_body");
        ASSERT_NE(send, npos);
        EXPECT_LT(send, ret);
        if (positions != npos) EXPECT_LT(positions, ret);
        if (results_insert != npos) EXPECT_LT(results_insert, ret);
        const auto declared = src.find("std::optional<nlohmann::json> risk_module_failure;");
        ASSERT_NE(declared, npos);
        EXPECT_NE(src.find("if (!skip_strategy_processing) {", declared), npos)
            << "declared at main() scope, before the strategy-processing block";
    }
}

// =============================================================================================
// The email flag.
// =============================================================================================

TEST_F(RiskModuleErrorPathTest, TheEmailIsFlaggedOnAHeldDay) {
    run_two_days();
    ASSERT_TRUE(day2_result_ok_);
    const auto failure = portfolio_risk_module_failure(pm_->last_risk_decisions());
    ASSERT_TRUE(failure.has_value());

    // The body the runners send, from the real builder (the overload both futures runners call;
    // no positions, since the test symbol has no instrument in the registry the builder reads).
    EmailSender sender{EmailSenderConfig{}};
    const std::string body = sender.generate_trading_report_body(
        StrategyPositionsMap{}, std::unordered_map<std::string, Position>{}, std::nullopt, {},
        StrategyExecutionsMap{}, "2026-05-01", "CONSERVATIVE_PORTFOLIO");
    ASSERT_NE(body.find("<div class=\"container\">\n"), npos) << "the builder's layout moved";

    const std::string flagged = flag_email_body_for_risk_module_failure(body, *failure);
    const std::string subject = risk_module_failure_email_subject("Daily Trading Report - 2026-05-01");

    EXPECT_EQ(subject, "[RISK MODULE FAILED - BOOK HELD] Daily Trading Report - 2026-05-01");
    const auto flag = flagged.find("<strong>RISK MODULE FAILED - BOOK HELD:</strong>");
    ASSERT_NE(flag, npos) << "no visible flag in the body";
    EXPECT_NE(flagged.find("the portfolio risk module carver could not evaluate today's book (" +
                           std::string(kInjectedError) + ")"),
              npos);
    EXPECT_NE(flagged.find("Every strategy is held at the previous day's positions and no orders "
                           "were generated. The run exited with code 3."),
              npos);
    EXPECT_LT(flag, flagged.find("Daily Trading Report</h1>"))
        << "the flag opens the report, above the header";
    EXPECT_GT(flag, flagged.find("<div class=\"container\">")) << "inside the report container";
    // Nothing else of the body is touched.
    EXPECT_EQ(flagged.size(),
              body.size() + risk_module_failure_email_banner(*failure).size());
}

TEST(RiskModuleErrorPathEmail, TheFlagEscapesTheErrorText) {
    nlohmann::json failure = {{"module", "carver"}, {"error", "x < y & \"z\""}};
    const std::string banner = risk_module_failure_email_banner(failure);
    EXPECT_NE(banner.find("(x &lt; y &amp; &quot;z&quot;)"), npos) << banner;
    EXPECT_EQ(banner.find("x < y"), npos);
    // A body without the builder's container still carries the flag, first.
    EXPECT_EQ(flag_email_body_for_risk_module_failure("<p>plain</p>", failure).find(banner), 0u);
}

TEST(RiskModuleErrorPathSource, BothTwinsFlagTheEmailBeforeSendingIt) {
    for (const char* runner : kFuturesRunners) {
        SCOPED_TRACE(runner);
        const std::string src = read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found from the test working directory";
        const auto body = src.find("std::string email_body = email_sender->generate_trading_report_body(");
        const auto subject_flag = src.find("subject = risk_module_failure_email_subject(subject);");
        const auto body_flag = src.find("email_body = flag_email_body_for_risk_module_failure(");
        const auto send = src.find("email_sender->send_email(subject, email_body");
        ASSERT_NE(body, npos);
        ASSERT_NE(send, npos);
        ASSERT_NE(subject_flag, npos) << "the subject is never flagged";
        ASSERT_NE(body_flag, npos) << "the body is never flagged";
        EXPECT_LT(body, subject_flag);
        EXPECT_LT(body, body_flag);
        EXPECT_LT(subject_flag, send);
        EXPECT_LT(body_flag, send);
    }
}

// The twins carry the C5 hunks byte-identically.
TEST(RiskModuleErrorPathSource, TheTwinsCarryTheSameErrorPathBlocks) {
    const std::string a = read_source(kFuturesRunners[0]);
    const std::string b = read_source(kFuturesRunners[1]);
    if (a.empty() || b.empty()) GTEST_SKIP() << "runner source not found";
    const std::pair<const char*, const char*> blocks[] = {
        {"        // Set when a portfolio risk module could not answer",
         "        // NORMAL TRADING DAY PROCESSING"},
        {"            // T-7a C5 (HD 2026-09-21, option b)",
         "            if (port_process_result.is_error())"},
        {"                    // T-7a C5: a day the portfolio risk module", "auto send_result"},
        {"        if (risk_module_failure) {\n            ERROR(\"RISK_MODULE_FAILURE the day",
         "    } catch (const std::exception& e) {"},
    };
    for (const auto& [from, to] : blocks) {
        SCOPED_TRACE(from);
        const std::string ba = between(a, from, to);
        const std::string bb = between(b, from, to);
        ASSERT_FALSE(ba.empty()) << "block missing in " << kFuturesRunners[0];
        EXPECT_EQ(ba, bb);
    }
}
