// tests/live/test_sleeve_risk_module_failure.cpp
//
// T-7b-2 C10b (HD 2026-09-24 ruling 18): a SLEEVE-scope risk module that cannot answer refuses
// its sleeve, as the portfolio rule (T-7a C5, HD 2026-09-21 option b) does for the book. The
// PortfolioManager holds that sleeve at its seeded T-1 book (no order for it) while the other
// sleeves trade; the live futures runners see it through sleeve_risk_module_failure(), log an
// ERROR, mark today's live_run_metadata row (risk_refusal, scope "sleeve") unless a portfolio
// refusal marked it, flag the email and exit kRiskModuleFailureExitCode, the same code and flag
// as a portfolio failure (a book the gate did not measure was stored).
//
// The runners are main()s: their blocks are read from the source (as test_risk_module_error_path
// does); the PM, the helpers and the email builder run.

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

const char* const kInjected = "sleeve gate cannot evaluate (injected)";

/// A SCALE-only sleeve module (no REFUSE capability: the shape a sleeve cutter would have) whose
/// evaluate throws on one rebalance and asks for nothing on the others.
class SleeveCutterThatThrowsOnOneDay final : public RiskModule {
public:
    explicit SleeveCutterThatThrowsOnOneDay(int throw_on) : throw_on_(throw_on) {}
    const std::string& id() const override { return id_; }
    const std::string& type() const override { return id_; }
    std::set<RiskTerm> terms() const override { return {RiskTerm::CUSTOM}; }
    std::set<RiskAction> capabilities() const override { return {RiskAction::SCALE}; }
    void begin_rebalance(const RiskContext& ctx) override {
        (void)ctx;
        ++rebalances_;
    }
    Result<RiskDecision> evaluate(const Book& book, const RiskContext& ctx) override {
        (void)book;
        (void)ctx;
        if (rebalances_ == throw_on_) throw std::runtime_error(kInjected);
        RiskDecision d;
        d.module_id = id_;
        return Result<RiskDecision>(d);
    }
    nlohmann::json describe() const override { return {{"id", id_}}; }

private:
    std::string id_{"sleeve_cutter"};
    int throw_on_;
    int rebalances_{0};
};

PortfolioConfig base_config() {
    PortfolioConfig pc{1000.0, 1.0, 0.0, /*optimization=*/false};
    pc.allow_fractional_positions = false;
    pc.risk_config.capital = 1000.0;
    pc.risk_config.var_limit = 1e6;
    pc.risk_config.jump_risk_limit = 1e6;
    pc.risk_config.max_correlation = 1.0;
    pc.risk_config.max_gross_leverage = 1e6;
    pc.risk_config.max_net_leverage = 1e6;
    pc.risk_modules = {test_carver_module(pc.risk_config)};
    return pc;
}

std::string read_source(const std::string& relative) {
    namespace fs = std::filesystem;
    fs::path dir = fs::current_path();
    for (int i = 0; i < 8 && !dir.empty(); ++i) {
        if (fs::exists(dir / relative)) {
            std::ifstream in(dir / relative);
            std::ostringstream ss;
            ss << in.rdbuf();
            return ss.str();
        }
        dir = dir.parent_path();
    }
    return {};
}

const char* const kFuturesRunners[] = {"apps/strategies/live_portfolio_conservative.cpp",
                                       "apps/strategies/live_portfolio.cpp"};

std::string between(const std::string& src, const std::string& from, const std::string& to) {
    const auto a = src.find(from);
    if (a == npos) return {};
    const auto b = src.find(to, a);
    if (b == npos) return {};
    return src.substr(a, b - a);
}

}  // namespace

class SleeveRiskModuleFailureTest : public TestBase {
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

    std::shared_ptr<ScriptedStrategy> make_strategy(const std::string& id, std::vector<Book> s) {
        StrategyConfig sc;
        sc.capital_allocation = 1000.0;
        sc.max_leverage = 10.0;
        sc.asset_classes = {AssetClass::FUTURES};
        sc.frequencies = {DataFrequency::DAILY};
        auto strat = std::make_shared<ScriptedStrategy>(id, sc, db_, std::move(s));
        EXPECT_TRUE(strat->initialize().is_ok());
        EXPECT_TRUE(strat->start().is_ok());
        return strat;
    }

    double qty(const std::string& sid) {
        return static_cast<double>(pm_->get_strategy_positions().at(sid).at("ZZA").quantity);
    }
    size_t execs(const std::string& sid) {
        const auto all = pm_->get_strategy_executions();
        auto it = all.find(sid);
        return it == all.end() ? 0 : it->second.size();
    }

    /// BASE's shape: two sleeves, the module on TF only. Day 1 healthy (TF 2 lots, FAST 1), day 2
    /// the module throws while TF asks 5 and FAST asks 3. Both sleeves seeded as the runners seed
    /// them (T-7a C3). Returns day 2's stdout.
    std::string run_two_days() {
        static int n = 0;
        pm_ = std::make_unique<PortfolioManager>(base_config(), "PM_SRMF_" + std::to_string(++n));
        tf_ = make_strategy("TF", {{{"ZZA", make_pos("ZZA", 2.0, 100.0)}},
                                   {{"ZZA", make_pos("ZZA", 5.0, 100.0)}}});
        fast_ = make_strategy("FAST", {{{"ZZA", make_pos("ZZA", 1.0, 100.0)}},
                                       {{"ZZA", make_pos("ZZA", 3.0, 100.0)}}});
        EXPECT_TRUE(pm_->add_strategy(tf_, 0.5, false).is_ok());
        EXPECT_TRUE(pm_->add_strategy(fast_, 0.5, false).is_ok());
        EXPECT_TRUE(pm_->set_risk_modules(
                           {}, {{"TF", {std::make_shared<SleeveCutterThatThrowsOnOneDay>(2)}}})
                        .is_ok());
        EXPECT_TRUE(pm_->update_strategy_position("TF", "ZZA", make_pos("ZZA", 2.0, 100.0)).is_ok());
        EXPECT_TRUE(
            pm_->update_strategy_position("FAST", "ZZA", make_pos("ZZA", 1.0, 100.0)).is_ok());
        ::testing::internal::CaptureStdout();
        const bool day1 = pm_->process_market_data(bars_for_day(1)).is_ok();
        const std::string out1 = ::testing::internal::GetCapturedStdout();
        EXPECT_TRUE(day1) << out1;
        EXPECT_FALSE(sleeve_risk_module_failure(pm_->last_risk_decisions()).has_value())
            << "day 1 is healthy";
        tf_before_ = execs("TF");
        fast_before_ = execs("FAST");
        ::testing::internal::CaptureStdout();
        day2_ok_ = pm_->process_market_data(bars_for_day(2)).is_ok();
        return ::testing::internal::GetCapturedStdout();
    }

    std::shared_ptr<MockPostgresDatabase> db_;
    std::unique_ptr<PortfolioManager> pm_;
    std::shared_ptr<ScriptedStrategy> tf_, fast_;
    size_t tf_before_{0}, fast_before_{0};
    bool day2_ok_{false};
};

TEST_F(SleeveRiskModuleFailureTest, TheFailedSleeveIsHeldAndTheOtherSleeveTradesThroughTheLoop) {
    const std::string out = run_two_days();
    ASSERT_TRUE(day2_ok_) << out;
    EXPECT_EQ(qty("TF"), 2.0) << "TF is held at its seeded 2 lots; 5 means it shipped ungated\n"
                              << out;
    EXPECT_EQ(execs("TF") - tf_before_, 0u) << "a held sleeve sends no order";
    EXPECT_EQ(qty("FAST"), 3.0) << "FAST trades to its 3 lots\n" << out;
    EXPECT_GT(execs("FAST") - fast_before_, 0u);
    EXPECT_NE(out.find(std::string("Risk module sleeve_cutter failed on sleeve TF before the "
                                   "loop: ") +
                       kInjected + "; a sleeve-scope module that cannot answer refuses its sleeve"),
              npos)
        << out;
}

TEST_F(SleeveRiskModuleFailureTest, TheRunnerSeesTheSleeveFailureAndExitsThree) {
    run_two_days();
    ASSERT_TRUE(day2_ok_);
    EXPECT_FALSE(portfolio_risk_module_failure(pm_->last_risk_decisions()).has_value())
        << "the portfolio module answered; only the sleeve failed";
    EXPECT_FALSE(portfolio_risk_refusal(pm_->last_risk_decisions()).has_value());
    const auto failure = sleeve_risk_module_failure(pm_->last_risk_decisions());
    ASSERT_TRUE(failure.has_value()) << "the runner does not see the sleeve failure";
    EXPECT_EQ((*failure)["scope"], "sleeve");
    EXPECT_EQ((*failure)["module"], "sleeve_cutter");
    EXPECT_EQ((*failure)["scope_id"], "TF");
    EXPECT_EQ((*failure)["error"], kInjected);
    EXPECT_EQ((*failure)["applied"], "REFUSE");
    EXPECT_EQ((*failure)["action"], "REFUSE");
    EXPECT_EQ(live_run_exit_code(failure), kRiskModuleFailureExitCode);
    // The metadata mark the runner writes with it: the watchdog reads the risk_refusal key.
    const auto marked =
        mark_risk_refusal(nlohmann::json{{"kept", 1}}, *failure, pm_->risk_decisions_json());
    EXPECT_EQ(marked["kept"], 1);
    EXPECT_EQ(marked["risk_refusal"]["scope"], "sleeve");
    EXPECT_EQ(marked["risk_refusal"]["scope_id"], "TF");
}

TEST(SleeveRiskModuleFailureHelper, AHealthyOrADeliberateSleeveRefusalIsNotAFailure) {
    RiskDecisionRecord deliberate;
    deliberate.scope = RiskScope::SLEEVE;
    deliberate.scope_id = "TF";
    deliberate.module_id = "stop";
    deliberate.applied_action = RiskAction::REFUSE;  // a REFUSE the module returned: no error
    EXPECT_FALSE(sleeve_risk_module_failure({deliberate}).has_value());
    RiskDecisionRecord portfolio_failure = deliberate;
    portfolio_failure.scope = RiskScope::PORTFOLIO;
    portfolio_failure.error = "boom";
    EXPECT_FALSE(sleeve_risk_module_failure({portfolio_failure}).has_value())
        << "a portfolio failure is portfolio_risk_module_failure's";
    RiskDecisionRecord step = deliberate;
    step.module_id = kRiskStepModuleId;
    step.error = "on_bars blew up";
    const auto f = sleeve_risk_module_failure({deliberate, step});
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ((*f)["module"], kRiskStepModuleId);
}

TEST_F(SleeveRiskModuleFailureTest, TheEmailFlagNamesTheSleeveAndSaysTheOthersTraded) {
    run_two_days();
    const auto failure = sleeve_risk_module_failure(pm_->last_risk_decisions());
    ASSERT_TRUE(failure.has_value());
    EmailSender sender{EmailSenderConfig{}};
    const std::string body = sender.generate_trading_report_body(
        StrategyPositionsMap{}, std::unordered_map<std::string, Position>{}, std::nullopt, {},
        StrategyExecutionsMap{}, "2026-05-01", "BASE_PORTFOLIO");
    const std::string flagged = flag_email_body_for_risk_module_failure(body, *failure);
    EXPECT_EQ(risk_module_failure_email_subject("Daily Trading Report - 2026-05-01"),
              "[RISK MODULE FAILED - BOOK HELD] Daily Trading Report - 2026-05-01");
    EXPECT_NE(flagged.find(std::string("the sleeve risk module sleeve_cutter could not evaluate "
                                       "sleeve TF's book (") +
                           kInjected +
                           "). That sleeve is held at the previous day's positions and sent no "
                           "orders; the other sleeves traded. The run exited with code 3."),
              npos)
        << flagged;
    EXPECT_EQ(flagged.find("Every strategy is held"), npos) << "the portfolio wording";
}

TEST(SleeveRiskModuleFailureSource, BothTwinsFlagMarkAndExitOnASleeveFailure) {
    std::string blocks[2];
    int i = 0;
    for (const char* runner : kFuturesRunners) {
        SCOPED_TRACE(runner);
        const std::string src = read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found from the test working directory";
        const auto portfolio_detect = src.find(
            "risk_module_failure = portfolio_risk_module_failure(portfolio->last_risk_decisions());");
        const auto sleeve_detect =
            src.find("sleeve_risk_module_failure(portfolio->last_risk_decisions());");
        const auto error_check = src.find("if (port_process_result.is_error())", portfolio_detect);
        ASSERT_NE(portfolio_detect, npos);
        ASSERT_NE(sleeve_detect, npos) << "the runner never asks whether a sleeve module failed";
        ASSERT_NE(error_check, npos);
        EXPECT_LT(portfolio_detect, sleeve_detect) << "a portfolio failure takes precedence";
        EXPECT_LT(sleeve_detect, error_check)
            << "read before the error check, so an unseeded sleeve's failure (exit 1) is logged";
        const std::string block =
            between(src, "            // T-7b-2 C10b (HD 2026-09-24 ruling 18)",
                    "            if (port_process_result.is_error())");
        ASSERT_FALSE(block.empty());
        EXPECT_NE(block.find("if (!risk_module_failure) {"), npos);
        EXPECT_NE(block.find("ERROR(\"RISK_MODULE_FAILURE sleeve risk module \""), npos);
        EXPECT_NE(block.find("if (!portfolio_risk_refusal(portfolio->last_risk_decisions()))"),
                  npos)
            << "a portfolio refusal's mark must not be overwritten";
        EXPECT_NE(block.find("mark_risk_refusal(portfolio_config_json, *risk_module_failure,"),
                  npos);
        // The exit code and the email flag are the portfolio rule's, driven by the same variable.
        EXPECT_NE(src.find("return live_run_exit_code(risk_module_failure);"), npos);
        EXPECT_NE(src.find("subject = risk_module_failure_email_subject(subject);"), npos);
        blocks[i++] = block;
    }
    EXPECT_EQ(blocks[0], blocks[1]) << "the twins' C10b blocks differ";
}
