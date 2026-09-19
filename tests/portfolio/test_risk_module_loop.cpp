// The PortfolioManager's risk loop driven through risk modules.
//
// The PM evaluates every module on every lap and records each decision in
// last_risk_decisions(). The Carver module's decision is applied by the loop's
// original risk_exceeded block; every other module's decision is recorded as
// requested and applied NONE, because the PM applies decisions by action only
// from T-6a commit 6 (these tests are extended there to assert the application).

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/risk/basic_risk_modules.hpp"
#include "trade_ngin/strategy/base_strategy.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

namespace {

using Book = std::unordered_map<std::string, Position>;

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
    std::unordered_map<std::string, Position> get_target_positions() const override {
        if (calls_ == 0) return {};
        return script_[std::min(calls_, script_.size()) - 1];
    }

private:
    std::vector<Book> script_;
    size_t calls_{0};
};

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

// A module that records every call it gets and returns a scripted action.
class SpyModule : public RiskModule {
public:
    SpyModule(std::string id, RiskAction action, double scale = 1.0)
        : id_(std::move(id)), action_(action), scale_(scale) {}
    const std::string& id() const override { return id_; }
    const std::string& type() const override { return type_; }
    std::set<RiskTerm> terms() const override { return {RiskTerm::CUSTOM}; }
    std::set<RiskAction> capabilities() const override { return {action_}; }
    void on_bars(const std::vector<Bar>& bars, const RiskContext& ctx) override {
        (void)bars;
        events.push_back("on_bars:" + std::to_string(ctx.lap));
        contexts.push_back(ctx);
    }
    Result<RiskDecision> evaluate(const Book& book, const RiskContext& ctx) override {
        events.push_back("evaluate:" + std::to_string(ctx.lap));
        books.push_back(book);
        RiskDecision d;
        d.module_id = id_;
        d.action = action_;
        d.scale = scale_;
        d.reason = "spy " + std::to_string(++evaluations);
        if (action_ == RiskAction::REPLACE) d.book = replace_book;
        return Result<RiskDecision>(d);
    }
    nlohmann::json describe() const override { return {{"id", id_}, {"type", type_}}; }

    std::vector<std::string> events;
    std::vector<RiskContext> contexts;
    std::vector<Book> books;
    Book replace_book;
    int evaluations{0};

private:
    std::string id_;
    std::string type_{"spy"};
    RiskAction action_;
    double scale_;
};

// Test-local REPLACE module (no shipped module can replace).
class ReplaceModule : public RiskModule {
public:
    explicit ReplaceModule(Book book) : book_(std::move(book)) {}
    const std::string& id() const override { return id_; }
    const std::string& type() const override { return id_; }
    std::set<RiskTerm> terms() const override { return {RiskTerm::CUSTOM}; }
    std::set<RiskAction> capabilities() const override { return {RiskAction::REPLACE}; }
    Result<RiskDecision> evaluate(const Book& book, const RiskContext& ctx) override {
        (void)book;
        (void)ctx;
        RiskDecision d;
        d.module_id = id_;
        d.action = RiskAction::REPLACE;
        d.reason = "replacement book";
        d.book = book_;
        return Result<RiskDecision>(d);
    }
    nlohmann::json describe() const override { return {{"id", id_}, {"type", id_}}; }

private:
    std::string id_{"replace"};
    Book book_;
};

PortfolioConfig risk_config(bool allow_fractional) {
    PortfolioConfig pc{1000.0, 0.0, 1.0, 0.0, /*optimization=*/false, /*risk=*/true};
    pc.allow_fractional_positions = allow_fractional;
    pc.risk_config.capital = 1000.0;
    pc.risk_config.var_limit = 1e6;
    pc.risk_config.jump_risk_limit = 1e6;
    pc.risk_config.max_correlation = 1.0;
    pc.risk_config.max_gross_leverage = 0.29;
    pc.risk_config.max_net_leverage = 0.29;
    return pc;
}

std::vector<Bar> three_days() {
    return {make_bar("ZZA", 1, 100.0), make_bar("ZZA", 2, 102.0), make_bar("ZZA", 3, 99.0)};
}

}  // namespace

class RiskModuleLoopTest : public TestBase {
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

    std::shared_ptr<ScriptedStrategy> make_strategy(const std::string& id, std::vector<Book> script) {
        StrategyConfig sc;
        sc.capital_allocation = 1000.0;
        sc.max_leverage = 10.0;
        sc.asset_classes = {AssetClass::FUTURES};
        sc.frequencies = {DataFrequency::DAILY};
        auto s = std::make_shared<ScriptedStrategy>(id, sc, db_, std::move(script));
        EXPECT_TRUE(s->initialize().is_ok());
        EXPECT_TRUE(s->start().is_ok());
        return s;
    }

    void make_pm(bool allow_fractional, std::vector<Book> script) {
        static int n = 0;
        pm_ = std::make_unique<PortfolioManager>(risk_config(allow_fractional),
                                                 "PM_RML_" + std::to_string(++n));
        strategy_ = make_strategy("RML_S", std::move(script));
        ASSERT_TRUE(pm_->add_strategy(strategy_, 1.0, false).is_ok());
    }

    double quantity(const std::string& symbol) {
        auto positions = pm_->get_strategy_positions();
        return static_cast<double>(positions.at("RML_S").at(symbol).quantity);
    }

    std::shared_ptr<MockPostgresDatabase> db_;
    std::unique_ptr<PortfolioManager> pm_;
    std::shared_ptr<ScriptedStrategy> strategy_;
};

TEST_F(RiskModuleLoopTest, EveryDecisionTypeIsEvaluatedAndRecordedEachLap) {
    // A 2.5-lot target never becomes whole, so the loop runs all five laps.
    make_pm(false, {{{"ZZA", make_pos("ZZA", 2.5, 100.0)}}});
    ASSERT_TRUE(pm_->set_risk_modules(
                       {std::make_shared<ConstantScaleRiskModule>("scale", 0.5, /*every_lap=*/true),
                        std::make_shared<WarnRiskModule>(
                            "warn", RiskCondition{RiskCondition::Kind::ALWAYS, 0.0}, "always"),
                        std::make_shared<RefuseOnConditionRiskModule>(
                            "refuse", RiskCondition{RiskCondition::Kind::LAP_AT_LEAST, 2.0},
                            "from lap two"),
                        std::make_shared<ReplaceModule>(Book{{"ZZA", make_pos("ZZA", 1.0, 100.0)}})})
                    .is_ok());

    ::testing::internal::CaptureStdout();
    ASSERT_TRUE(pm_->process_market_data(three_days()).is_ok());
    const std::string out = ::testing::internal::GetCapturedStdout();

    const auto rows = pm_->last_risk_decisions();
    ASSERT_EQ(rows.size(), 20u) << "4 modules x 5 laps";
    for (int lap = 1; lap <= 5; ++lap) {
        const RiskDecisionRecord* r = &rows[static_cast<size_t>(4 * (lap - 1))];
        SCOPED_TRACE("lap " + std::to_string(lap));
        EXPECT_EQ(r[0].module_id, "scale");
        EXPECT_EQ(r[0].requested.action, RiskAction::SCALE);
        EXPECT_EQ(r[0].requested.scale, 0.5);
        EXPECT_EQ(r[1].module_id, "warn");
        EXPECT_EQ(r[1].requested.action, RiskAction::WARN);
        EXPECT_EQ(r[2].module_id, "refuse");
        EXPECT_EQ(r[2].requested.action, lap >= 2 ? RiskAction::REFUSE : RiskAction::NONE);
        EXPECT_EQ(r[3].module_id, "replace");
        EXPECT_EQ(r[3].requested.action, RiskAction::REPLACE);
        for (int k = 0; k < 4; ++k) {
            EXPECT_EQ(r[k].lap, lap);
            EXPECT_EQ(r[k].phase, RiskPhase::LAP);
            EXPECT_EQ(r[k].scope, RiskScope::PORTFOLIO);
            EXPECT_FALSE(r[k].empty_book);
            EXPECT_TRUE(r[k].error.empty());
            // Commit 4: a non-Carver decision is recorded, NOT applied (applied from commit 6).
            EXPECT_EQ(r[k].applied_action, RiskAction::NONE);
            EXPECT_EQ(r[k].applied_factor.raw_value(), Decimal(1.0).raw_value());
        }
    }
    // The book is untouched by the SCALE, the REFUSE and the REPLACE: 2.5 forced to 3.
    EXPECT_EQ(quantity("ZZA"), 3.0);
    // The WARN is logged once per lap.
    size_t warn_lines = 0;
    for (size_t at = out.find("Risk module warn warning on portfolio"); at != std::string::npos;
         at = out.find("Risk module warn warning on portfolio", at + 1)) {
        ++warn_lines;
    }
    EXPECT_EQ(warn_lines, 5u) << out;
    EXPECT_NE(out.find("[WARNING] [RiskManager] Risk module warn warning on portfolio " +
                       std::string("PM_RML_") ),
              std::string::npos);
    EXPECT_EQ(out.find("Risk limits exceeded"), std::string::npos);
    // One "Using risk manager" per lap, not one per module.
    size_t using_lines = 0;
    for (size_t at = out.find("Using risk manager"); at != std::string::npos;
         at = out.find("Using risk manager", at + 1)) {
        ++using_lines;
    }
    EXPECT_EQ(using_lines, 5u);

    auto j = pm_->risk_decisions_json();
    EXPECT_EQ(j["decisions"].size(), 20u);
    EXPECT_EQ(j["outcome"]["action"], "NONE");
    EXPECT_EQ(j["outcome"]["laps"], 5);
    EXPECT_EQ(j["modules"].size(), 4u);
    EXPECT_EQ(j["modules"][0]["scope"], "portfolio");
}

TEST_F(RiskModuleLoopTest, CarverStillScalesExactlyAsToday) {
    // The constructor's Carver module: 7 lots at 100 on 1000 capital is 0.7x gross against a
    // 0.29x cap. Fractional positions converge on lap 1, so the scale is applied once.
    make_pm(true, {{{"ZZA", make_pos("ZZA", 7.0, 100.0)}}});
    const auto bars = three_days();
    ASSERT_TRUE(pm_->process_market_data(bars).is_ok());

    RiskManager bare(risk_config(true).risk_config);
    auto ref = bare.process_positions({{"ZZA", make_pos("ZZA", 7.0, 100.0)}},
                                      bare.create_market_data(bars), {});
    ASSERT_TRUE(ref.is_ok());
    ASSERT_TRUE(ref.value().risk_exceeded);
    Decimal expected(7.0);
    expected *= ref.value().recommended_scale;  // the in-place multiply the loop has always made
    auto positions = pm_->get_strategy_positions();
    EXPECT_EQ(positions.at("RML_S").at("ZZA").quantity.raw_value(), expected.raw_value());

    const auto rows = pm_->last_risk_decisions();
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].module_id, "carver");
    EXPECT_EQ(rows[0].requested.action, RiskAction::SCALE);
    EXPECT_EQ(rows[0].requested.scale, ref.value().recommended_scale);
    ASSERT_TRUE(rows[0].requested.metrics.has_value());
    EXPECT_EQ(rows[0].applied_action, RiskAction::SCALE);
    EXPECT_EQ(rows[0].applied_factor.raw_value(),
              Decimal(ref.value().recommended_scale).raw_value());
}

TEST_F(RiskModuleLoopTest, DecisionsClearedAtEachProcessMarketData) {
    make_pm(true, {{{"ZZA", make_pos("ZZA", 1.0, 100.0)}}});
    auto spy = std::make_shared<SpyModule>("spy", RiskAction::NONE);
    ASSERT_TRUE(pm_->set_risk_modules({spy}).is_ok());
    ASSERT_TRUE(pm_->process_market_data(three_days()).is_ok());
    ASSERT_EQ(pm_->last_risk_decisions().size(), 1u);
    EXPECT_EQ(pm_->last_risk_decisions()[0].requested.reason, "spy 1");
    ASSERT_TRUE(pm_->process_market_data({make_bar("ZZA", 4, 100.0)}).is_ok());
    const auto rows = pm_->last_risk_decisions();
    ASSERT_EQ(rows.size(), 1u) << "the first call's row must be gone";
    EXPECT_EQ(rows[0].requested.reason, "spy 2");
}

TEST_F(RiskModuleLoopTest, EmptyBookRunsOnBarsButNotEvaluateAndRecordsOneRowPerModule) {
    make_pm(true, {Book{}});
    auto a = std::make_shared<SpyModule>("a", RiskAction::NONE);
    auto b = std::make_shared<SpyModule>("b", RiskAction::WARN);
    ASSERT_TRUE(pm_->set_risk_modules({a, b}).is_ok());
    ASSERT_TRUE(pm_->process_market_data(three_days()).is_ok());
    EXPECT_EQ(a->events, (std::vector<std::string>{"on_bars:1"}));
    EXPECT_EQ(b->events, (std::vector<std::string>{"on_bars:1"}));
    const auto rows = pm_->last_risk_decisions();
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_TRUE(rows[0].empty_book);
    EXPECT_TRUE(rows[1].empty_book);
    EXPECT_EQ(rows[0].module_id, "a");
    EXPECT_EQ(rows[1].module_id, "b");
    EXPECT_EQ(rows[1].requested.action, RiskAction::NONE);
}

TEST_F(RiskModuleLoopTest, ContextCarriesLapScopeCapitalAndBacktestFlag) {
    make_pm(false, {{{"ZZA", make_pos("ZZA", 2.5, 100.0)}}});
    auto spy = std::make_shared<SpyModule>("spy", RiskAction::NONE);
    ASSERT_TRUE(pm_->set_risk_modules({spy}).is_ok());
    pm_->set_backtest_mode(true);
    const auto bars = three_days();
    const Timestamp as_of = day(3);
    ASSERT_TRUE(pm_->process_market_data(bars, /*skip_execution_generation=*/true, as_of).is_ok());
    ASSERT_EQ(spy->contexts.size(), 5u);
    for (size_t k = 0; k < spy->contexts.size(); ++k) {
        const RiskContext& c = spy->contexts[k];
        EXPECT_EQ(c.phase, RiskPhase::LAP);
        EXPECT_EQ(c.lap, static_cast<int>(k + 1));
        EXPECT_EQ(c.scope, RiskScope::PORTFOLIO);
        EXPECT_EQ(c.scope_id, c.portfolio_id);
        EXPECT_EQ(c.portfolio_id.rfind("PM_RML_", 0), 0u);
        EXPECT_EQ(c.capital, Decimal(1000.0));
        EXPECT_TRUE(c.is_backtest);
        EXPECT_TRUE(c.is_warmup);
        ASSERT_TRUE(c.as_of.has_value());
        EXPECT_EQ(*c.as_of, as_of);
        EXPECT_TRUE(c.applied.empty());
    }
    // The spy saw the aggregated book on every lap.
    ASSERT_EQ(spy->books.size(), 5u);
    EXPECT_EQ(spy->books[0].at("ZZA").quantity, Decimal(2.5));
}

TEST_F(RiskModuleLoopTest, SetRiskModulesRejectsNullAndDuplicateIds) {
    make_pm(true, {{{"ZZA", make_pos("ZZA", 1.0, 100.0)}}});
    auto a = std::make_shared<SpyModule>("a", RiskAction::NONE);
    EXPECT_TRUE(pm_->set_risk_modules({a, nullptr}).is_error());
    EXPECT_TRUE(
        pm_->set_risk_modules({a, std::make_shared<SpyModule>("a", RiskAction::WARN)}).is_error());
    // A rejected call leaves the constructor's Carver module in place.
    ASSERT_TRUE(pm_->process_market_data(three_days()).is_ok());
    const auto rows = pm_->last_risk_decisions();
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].module_id, "carver");
    EXPECT_TRUE(a->events.empty());
}
