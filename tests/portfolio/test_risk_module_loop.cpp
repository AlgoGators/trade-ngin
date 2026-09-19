// The PortfolioManager's risk loop driven through risk modules.
//
// The PM evaluates every module on every lap, applies the scope's combined
// decision by action (REFUSE > REPLACE > SCALE > WARN > NONE; the SCALE applied is
// the minimum, as the double operand of the in-place multiply) and records each
// decision in last_risk_decisions(), with the sleeve scope before the loop and the
// post-rounding point after it.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
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
    std::unordered_map<std::string, std::vector<double>> get_price_history() const override {
        return history_;
    }
    void set_history(std::unordered_map<std::string, std::vector<double>> h) {
        history_ = std::move(h);
    }

private:
    std::vector<Book> script_;
    size_t calls_{0};
    std::unordered_map<std::string, std::vector<double>> history_;
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
    void begin_rebalance(const RiskContext& ctx) override {
        events.push_back("begin_rebalance");
        rebalance_contexts.push_back(ctx);
    }
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
    void on_applied(const RiskApplied& a, const RiskContext& ctx) override {
        (void)ctx;
        applied.push_back(a);
    }

    std::vector<std::string> events;
    std::vector<RiskApplied> applied;
    std::vector<RiskContext> contexts;
    std::vector<RiskContext> rebalance_contexts;
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
    explicit ReplaceModule(Book book, int from_lap = 1) : book_(std::move(book)), from_lap_(from_lap) {}
    const std::string& id() const override { return id_; }
    const std::string& type() const override { return id_; }
    std::set<RiskTerm> terms() const override { return {RiskTerm::CUSTOM}; }
    std::set<RiskAction> capabilities() const override { return {RiskAction::REPLACE}; }
    Result<RiskDecision> evaluate(const Book& book, const RiskContext& ctx) override {
        (void)book;
        RiskDecision d;
        d.module_id = id_;
        if (ctx.phase == RiskPhase::LAP && ctx.lap < from_lap_) return Result<RiskDecision>(d);
        d.action = RiskAction::REPLACE;
        d.reason = "replacement book";
        d.book = book_;
        return Result<RiskDecision>(d);
    }
    nlohmann::json describe() const override { return {{"id", id_}, {"type", id_}}; }

private:
    std::string id_{"replace"};
    Book book_;
    int from_lap_;
};

std::vector<RiskDecisionRecord> rows_of(const std::vector<RiskDecisionRecord>& all, RiskPhase phase) {
    std::vector<RiskDecisionRecord> out;
    for (const auto& r : all) {
        if (r.phase == phase) out.push_back(r);
    }
    return out;
}

size_t count_of(const std::string& text, const std::string& needle) {
    size_t n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++n;
    return n;
}

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
    // 2.5 lots never become whole, so a lap 2 happens; there the REFUSE beats the SCALE and the
    // REPLACE, pins the strategy to its previous positions and ends the loop.
    make_pm(false, {{{"ZZA", make_pos("ZZA", 2.5, 100.0)}}});
    ASSERT_TRUE(pm_->update_strategy_position("RML_S", "ZZA", make_pos("ZZA", 1.0, 100.0)).is_ok());
    ASSERT_TRUE(pm_->set_risk_modules(
                       {std::make_shared<ConstantScaleRiskModule>("scale", 0.5, /*every_lap=*/true),
                        std::make_shared<WarnRiskModule>(
                            "warn", RiskCondition{RiskCondition::Kind::ALWAYS, 0.0}, "always"),
                        std::make_shared<RefuseOnConditionRiskModule>(
                            "refuse", RiskCondition{RiskCondition::Kind::LAP_AT_LEAST, 2.0},
                            "from lap two"),
                        std::make_shared<ReplaceModule>(Book{{"ZZA", make_pos("ZZA", 9.0, 100.0)}},
                                                        /*from_lap=*/2)})
                    .is_ok());

    ::testing::internal::CaptureStdout();
    ASSERT_TRUE(pm_->process_market_data(three_days()).is_ok());
    const std::string out = ::testing::internal::GetCapturedStdout();

    const auto rows = rows_of(pm_->last_risk_decisions(), RiskPhase::LAP);
    ASSERT_EQ(rows.size(), 8u) << "4 modules x 2 laps";
    const std::vector<RiskAction> requested_1 = {RiskAction::SCALE, RiskAction::WARN,
                                                 RiskAction::NONE, RiskAction::NONE};
    const std::vector<RiskAction> applied_1 = {RiskAction::SCALE, RiskAction::WARN,
                                               RiskAction::NONE, RiskAction::NONE};
    const std::vector<RiskAction> requested_2 = {RiskAction::SCALE, RiskAction::WARN,
                                                 RiskAction::REFUSE, RiskAction::REPLACE};
    const std::vector<RiskAction> applied_2 = {RiskAction::NONE, RiskAction::WARN,
                                               RiskAction::REFUSE, RiskAction::NONE};
    const std::vector<std::string> ids = {"scale", "warn", "refuse", "replace"};
    for (int k = 0; k < 4; ++k) {
        SCOPED_TRACE(ids[k]);
        EXPECT_EQ(rows[k].module_id, ids[k]);
        EXPECT_EQ(rows[k].lap, 1);
        EXPECT_EQ(rows[k].requested.action, requested_1[k]);
        EXPECT_EQ(rows[k].applied_action, applied_1[k]);
        EXPECT_EQ(rows[4 + k].module_id, ids[k]);
        EXPECT_EQ(rows[4 + k].lap, 2);
        EXPECT_EQ(rows[4 + k].requested.action, requested_2[k]);
        EXPECT_EQ(rows[4 + k].applied_action, applied_2[k]);
    }
    EXPECT_EQ(rows[0].applied_factor.raw_value(), Decimal(0.5).raw_value());
    EXPECT_EQ(rows[4].applied_factor.raw_value(), Decimal(1.0).raw_value());
    // Lap 1 halved the book (2.5 -> 1.25); lap 2 pinned it to the previous positions.
    EXPECT_EQ(quantity("ZZA"), 1.0);
    EXPECT_EQ(count_of(out, "Risk module warn warning on portfolio"), 2u) << out;
    EXPECT_EQ(count_of(out, "Using risk manager"), 2u);
    EXPECT_EQ(count_of(out, "Risk limits exceeded, scaling positions by 0.500000"), 1u);
    EXPECT_EQ(count_of(out, "Risk module refuse refused portfolio"), 1u);
    EXPECT_EQ(count_of(out, "Risk limits not exceeded"), 0u);

    auto j = pm_->risk_decisions_json();
    EXPECT_EQ(j["decisions"].size(), 12u) << "8 lap rows and 4 post-rounding rows";
    EXPECT_EQ(j["outcome"]["action"], "REFUSE");
    EXPECT_EQ(j["outcome"]["laps"], 2);
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

    const auto rows = rows_of(pm_->last_risk_decisions(), RiskPhase::LAP);
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
    ASSERT_EQ(rows_of(pm_->last_risk_decisions(), RiskPhase::LAP).size(), 1u);
    EXPECT_EQ(rows_of(pm_->last_risk_decisions(), RiskPhase::LAP)[0].requested.reason, "spy 1");
    ASSERT_TRUE(pm_->process_market_data({make_bar("ZZA", 4, 100.0)}).is_ok());
    const auto rows = rows_of(pm_->last_risk_decisions(), RiskPhase::LAP);
    ASSERT_EQ(rows.size(), 1u) << "the first call's row must be gone";
    EXPECT_EQ(rows[0].requested.reason, "spy 2");
}

TEST_F(RiskModuleLoopTest, EmptyBookRunsOnBarsButNotEvaluateAndRecordsOneRowPerModule) {
    make_pm(true, {Book{}});
    auto a = std::make_shared<SpyModule>("a", RiskAction::NONE);
    auto b = std::make_shared<SpyModule>("b", RiskAction::WARN);
    ASSERT_TRUE(pm_->set_risk_modules({a, b}).is_ok());
    ASSERT_TRUE(pm_->process_market_data(three_days()).is_ok());
    EXPECT_EQ(a->events, (std::vector<std::string>{"begin_rebalance", "on_bars:1"}));
    EXPECT_EQ(b->events, (std::vector<std::string>{"begin_rebalance", "on_bars:1"}));
    const auto rows = rows_of(pm_->last_risk_decisions(), RiskPhase::LAP);
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
    const auto rows = rows_of(pm_->last_risk_decisions(), RiskPhase::LAP);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].module_id, "carver");
    EXPECT_TRUE(a->events.empty());
}

TEST_F(RiskModuleLoopTest, BeginRebalanceOncePerCallBeforeAnyLap) {
    // 2.5 lots never become whole: five laps per call.
    make_pm(false, {{{"ZZA", make_pos("ZZA", 2.5, 100.0)}}});
    auto spy = std::make_shared<SpyModule>("spy", RiskAction::NONE);
    ASSERT_TRUE(pm_->set_risk_modules({spy}).is_ok());
    const std::vector<std::string> one_call = {
        "begin_rebalance", "on_bars:1", "evaluate:1", "on_bars:2", "evaluate:2", "on_bars:3",
        "evaluate:3",      "on_bars:4", "evaluate:4", "on_bars:5", "evaluate:5"};
    ASSERT_TRUE(pm_->process_market_data(three_days()).is_ok());
    EXPECT_EQ(spy->events, one_call);
    spy->events.clear();
    ASSERT_TRUE(pm_->process_market_data({make_bar("ZZA", 4, 100.0)}).is_ok());
    EXPECT_EQ(spy->events, one_call);
    ASSERT_EQ(spy->rebalance_contexts.size(), 2u);
    EXPECT_EQ(spy->rebalance_contexts[0].phase, RiskPhase::REBALANCE_START);
    EXPECT_EQ(spy->rebalance_contexts[0].lap, 0);
    EXPECT_EQ(spy->rebalance_contexts[0].scope, RiskScope::PORTFOLIO);
}

// ===== The Carver module's window (was PortfolioManager::risk_history_) =====

class CarverWindowTest : public RiskModuleLoopTest {
protected:
    static void expect_same_bars(const std::vector<Bar>& got, const std::vector<Bar>& want,
                                 const std::string& where) {
        ASSERT_EQ(got.size(), want.size()) << where;
        for (size_t k = 0; k < got.size(); ++k) {
            EXPECT_EQ(got[k].symbol, want[k].symbol) << where << " bar " << k;
            EXPECT_EQ(got[k].timestamp, want[k].timestamp) << where << " bar " << k;
            EXPECT_EQ(got[k].close.raw_value(), want[k].close.raw_value()) << where << " bar " << k;
        }
    }
};

TEST_F(CarverWindowTest, MatchesTodaysPushTrimIncludingAnEmptyBookLap) {
    // Fractional positions: every call converges on lap 1, so each call appends its bars once.
    make_pm(true, {Book{}, {{"ZZA", make_pos("ZZA", 1.0, 100.0)}},
                   {{"ZZA", make_pos("ZZA", 1.0, 100.0)}}});
    RiskConfig cfg = risk_config(true).risk_config;
    cfg.lookback_period = 1000;  // no trim
    auto carver = std::make_shared<CarverRiskModule>("carver", cfg);
    ASSERT_TRUE(pm_->set_risk_modules({carver}).is_ok());

    const std::vector<Bar> b1 = {make_bar("ZZA", 1, 100.0), make_bar("ZZB", 1, 50.0),
                                 make_bar("ZZA", 2, 101.0)};
    const std::vector<Bar> b2 = {make_bar("ZZA", 3, 102.0), make_bar("ZZB", 3, 51.0),
                                 make_bar("ZZA", 4, 99.0), make_bar("ZZB", 4, 49.0)};
    const std::vector<Bar> b3 = {make_bar("ZZA", 5, 98.0), make_bar("ZZB", 5, 48.0)};

    ASSERT_TRUE(pm_->process_market_data(b1).is_ok());
    // Call 1's book is empty: the bars still went into the window (append before the return).
    ASSERT_EQ(rows_of(pm_->last_risk_decisions(), RiskPhase::LAP).size(), 1u);
    EXPECT_TRUE(rows_of(pm_->last_risk_decisions(), RiskPhase::LAP)[0].empty_book);
    expect_same_bars(carver->window(), b1, "after the empty-book call");

    ASSERT_TRUE(pm_->process_market_data(b2).is_ok());
    ASSERT_TRUE(pm_->process_market_data(b3).is_ok());
    std::vector<Bar> all = b1;
    all.insert(all.end(), b2.begin(), b2.end());
    all.insert(all.end(), b3.begin(), b3.end());
    expect_same_bars(carver->window(), all, "after call 3");
}

TEST_F(CarverWindowTest, TrimsToLookbackBarsAcrossMultipleLaps) {
    // Whole contracts and a binding leverage cap: 7 lots become 2.9, never whole, so every
    // call runs five laps and appends its bars five times.
    make_pm(false, {{{"ZZA", make_pos("ZZA", 7.0, 100.0)}}});
    RiskConfig cfg = risk_config(false).risk_config;
    cfg.lookback_period = 5;
    auto carver = std::make_shared<CarverRiskModule>("carver", cfg);
    auto spy = std::make_shared<SpyModule>("spy", RiskAction::NONE);
    ASSERT_TRUE(pm_->set_risk_modules({carver, spy}).is_ok());

    const std::vector<std::vector<Bar>> calls = {
        {make_bar("ZZA", 1, 100.0), make_bar("ZZA", 2, 102.0), make_bar("ZZA", 3, 99.0)},
        {make_bar("ZZA", 4, 101.0), make_bar("ZZA", 5, 97.0)},
        {make_bar("ZZA", 6, 103.0), make_bar("ZZA", 7, 100.0), make_bar("ZZA", 8, 98.0),
         make_bar("ZZA", 9, 104.0)}};

    // The PortfolioManager's append and trim, transcribed.
    std::vector<Bar> ref;
    for (size_t c = 0; c < calls.size(); ++c) {
        spy->evaluations = 0;
        ASSERT_TRUE(pm_->process_market_data(calls[c]).is_ok());
        ASSERT_EQ(spy->evaluations, 5) << "call " << c;
        for (int lap = 0; lap < 5; ++lap) {
            ref.insert(ref.end(), calls[c].begin(), calls[c].end());
            if (ref.size() > 5) ref.erase(ref.begin(), ref.end() - 5);
        }
        expect_same_bars(carver->window(), ref, "after call " + std::to_string(c));
    }
}

// ===== Commit 6: the PM applies decisions by action =====

namespace {

// A module whose finalize() returns a scripted action and records the book it saw.
class FinalizeSpy : public RiskModule {
public:
    FinalizeSpy(std::string id, RiskAction at_finalize) : id_(std::move(id)), action_(at_finalize) {}
    const std::string& id() const override { return id_; }
    const std::string& type() const override { return id_; }
    std::set<RiskTerm> terms() const override { return {RiskTerm::CUSTOM}; }
    std::set<RiskAction> capabilities() const override { return {action_}; }
    Result<RiskDecision> evaluate(const Book& book, const RiskContext& ctx) override {
        (void)book;
        (void)ctx;
        RiskDecision d;
        d.module_id = id_;
        return Result<RiskDecision>(d);
    }
    Result<RiskDecision> finalize(const Book& book, const RiskContext& ctx) override {
        finalized.push_back(book);
        contexts.push_back(ctx);
        RiskDecision d;
        d.module_id = id_;
        d.action = action_;
        d.scale = action_ == RiskAction::SCALE ? 0.5 : 1.0;
        d.reason = "at the post-rounding point";
        return Result<RiskDecision>(d);
    }
    nlohmann::json describe() const override { return {{"id", id_}}; }

    std::vector<Book> finalized;
    std::vector<RiskContext> contexts;

private:
    std::string id_;
    RiskAction action_;
};

}  // namespace

// A two-sleeve manager: strategy ids SA and SB at 0.5 each.
class RiskScopeTest : public RiskModuleLoopTest {
protected:
    void make_two_sleeves(bool allow_fractional, bool use_optimization, Book a, Book b) {
        static int n = 0;
        PortfolioConfig pc = risk_config(allow_fractional);
        pc.use_optimization = use_optimization;
        pm_ = std::make_unique<PortfolioManager>(pc, "PM_SCOPE_" + std::to_string(++n));
        sa_ = make_strategy("SA", {std::move(a)});
        sb_ = make_strategy("SB", {std::move(b)});
        ASSERT_TRUE(pm_->add_strategy(sa_, 0.5, use_optimization).is_ok());
        ASSERT_TRUE(pm_->add_strategy(sb_, 0.5, use_optimization).is_ok());
    }
    double qty(const std::string& sid, const std::string& symbol) {
        return static_cast<double>(pm_->get_strategy_positions().at(sid).at(symbol).quantity);
    }
    std::shared_ptr<ScriptedStrategy> sa_, sb_;
};

class RiskApplyTest : public RiskModuleLoopTest {};
class RiskRefuseTest : public RiskScopeTest {};
class RiskSleeveTest : public RiskScopeTest {};
class RiskValidationTest : public RiskScopeTest {};
class RiskReplaceTest : public RiskScopeTest {};
class RiskPostRoundingTest : public RiskScopeTest {};

TEST_F(RiskApplyTest, ScaleIsBitwiseTheInPlaceMultiply) {
    const std::vector<std::pair<std::string, double>> in = {{"Q1", 2.42425398},
                                                            {"Q2", 7.12345678},
                                                            {"Q3", -3.33333333},
                                                            {"Q4", 13.0},
                                                            {"Q5", 5.55555555}};
    Book book;
    for (const auto& [symbol, q] : in) book[symbol] = make_pos(symbol, q, 100.0);
    make_pm(true, {book});
    const double s = 0.8894680636;
    ASSERT_TRUE(pm_->set_risk_modules({std::make_shared<ConstantScaleRiskModule>("c", s)}).is_ok());
    ASSERT_TRUE(pm_->process_market_data(three_days()).is_ok());

    // Decimal q(x); q *= s: the factor rounds to 88946806 raw, the product truncates.
    const std::vector<int64_t> expected = {215629648, 633608728, -296489353, 1156308478, 494148921};
    // The broken path Decimal(double(q) * s) differs on every input, so the inputs discriminate.
    const std::vector<int64_t> broken = {215629649, 633608731, -296489354, 1156308483, 494148924};
    auto positions = pm_->get_strategy_positions().at("RML_S");
    for (size_t k = 0; k < in.size(); ++k) {
        Decimal ref(in[k].second);
        ref *= s;
        EXPECT_EQ(ref.raw_value(), expected[k]);
        EXPECT_EQ(Decimal(in[k].second * s).raw_value(), broken[k]);
        EXPECT_EQ(positions.at(in[k].first).quantity.raw_value(), expected[k]) << in[k].first;
    }
}

TEST_F(RiskApplyTest, ScaleOneIsNoneAndPrintsNotExceeded) {
    make_pm(true, {{{"ZZA", make_pos("ZZA", 3.0, 100.0)}}});
    auto scale_one = std::make_shared<SpyModule>("scale_one", RiskAction::SCALE, 1.0);
    ASSERT_TRUE(pm_->set_risk_modules({std::make_shared<ConstantScaleRiskModule>("one", 1.0),
                                       scale_one})
                    .is_ok());
    ::testing::internal::CaptureStdout();
    ASSERT_TRUE(pm_->process_market_data(three_days()).is_ok());
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_EQ(quantity("ZZA"), 3.0);
    EXPECT_EQ(out.find("Risk limits exceeded"), std::string::npos) << out;
    EXPECT_NE(out.find("Risk limits not exceeded, no scaling needed"), std::string::npos);
    EXPECT_EQ(count_of(out, "[ERROR] [RiskManager] Risk module scale_one returned SCALE 1.000000; "
                            "not applied"),
              1u)
        << out;
    EXPECT_EQ(count_of(out, "Risk module one returned"), 0u);
    const auto lap = rows_of(pm_->last_risk_decisions(), RiskPhase::LAP);
    ASSERT_EQ(lap.size(), 2u);
    EXPECT_EQ(lap[0].requested.action, RiskAction::NONE);  // ConstantScale(1.0) never asks
    EXPECT_EQ(lap[1].requested.action, RiskAction::SCALE);
    EXPECT_EQ(lap[1].applied_action, RiskAction::NONE);
}

TEST_F(RiskApplyTest, CallbackCarriesTheQuantisedFactor) {
    // 2.5 lots never become whole: the spy's SCALE is applied on every lap.
    make_pm(false, {{{"ZZA", make_pos("ZZA", 2.5, 100.0)}}});
    auto spy = std::make_shared<SpyModule>("spy", RiskAction::SCALE, 0.1234567891);
    ASSERT_TRUE(pm_->set_risk_modules({spy}).is_ok());
    ASSERT_TRUE(pm_->process_market_data(three_days()).is_ok());
    ASSERT_GE(spy->applied.size(), 2u);
    EXPECT_EQ(spy->applied[0].action, RiskAction::SCALE);
    EXPECT_EQ(spy->applied[0].factor.raw_value(), 12345679);  // round half away, not truncation
    EXPECT_EQ(spy->applied[0].requested_scale, 0.1234567891);
    EXPECT_TRUE(spy->applied[0].won);
    EXPECT_FALSE(spy->applied[0].pinned);
    // Lap 2 sees what lap 1 applied, quantised.
    const std::string pm_id = spy->contexts[1].portfolio_id;
    ASSERT_TRUE(spy->contexts[1].applied.count(pm_id));
    EXPECT_EQ(spy->contexts[1].applied.at(pm_id), 0.12345679);
    EXPECT_TRUE(spy->contexts[0].applied.empty());
    const auto lap = rows_of(pm_->last_risk_decisions(), RiskPhase::LAP);
    EXPECT_EQ(lap[0].applied_factor.raw_value(), 12345679);
}

TEST_F(RiskApplyTest, CarverAppliedLevelIsTheQuantisedFactor) {
    make_pm(true, {{{"ZZA", make_pos("ZZA", 7.0, 100.0)}}});
    auto carver = std::make_shared<CarverRiskModule>("carver", risk_config(true).risk_config);
    ASSERT_TRUE(pm_->set_risk_modules({carver}).is_ok());
    ASSERT_TRUE(pm_->process_market_data(three_days()).is_ok());
    const auto lap = rows_of(pm_->last_risk_decisions(), RiskPhase::LAP);
    ASSERT_EQ(lap.size(), 1u);
    ASSERT_EQ(lap[0].applied_action, RiskAction::SCALE);
    const double s = lap[0].requested.scale;
    EXPECT_EQ(carver->applied_level(), static_cast<double>(Decimal(s)));
    EXPECT_NE(carver->applied_level(), s);  // the level is the quantised factor, not the request
}

TEST_F(RiskApplyTest, MinWithinALevelNotProduct) {
    make_pm(true, {{{"ZZA", make_pos("ZZA", 10.0, 100.0)}}});
    auto a = std::make_shared<SpyModule>("a", RiskAction::SCALE, 0.9);
    auto b = std::make_shared<SpyModule>("b", RiskAction::SCALE, 0.7);
    ASSERT_TRUE(pm_->set_risk_modules({a, b}).is_ok());
    ASSERT_TRUE(pm_->process_market_data(three_days()).is_ok());
    EXPECT_EQ(quantity("ZZA"), 7.0);
    ASSERT_GE(a->applied.size(), 1u);  // lap 1 (then the post-rounding point's NONE)
    EXPECT_FALSE(a->applied[0].won);
    EXPECT_TRUE(b->applied[0].won);
    EXPECT_EQ(a->applied[0].factor.raw_value(), Decimal(0.7).raw_value());
    const auto lap = rows_of(pm_->last_risk_decisions(), RiskPhase::LAP);
    EXPECT_EQ(lap[0].applied_action, RiskAction::NONE);
    EXPECT_EQ(lap[1].applied_action, RiskAction::SCALE);
}

TEST_F(RiskRefuseTest, PortfolioRefuseBreaksTheLoopAndPinsPreviousPositions) {
    make_pm(false, {{{"ZZA", make_pos("ZZA", 5.0, 100.0)}}});
    ASSERT_TRUE(pm_->update_strategy_position("RML_S", "ZZA", make_pos("ZZA", 1.0, 100.0)).is_ok());
    // Lap 1: 5 x 0.85 = 4.25, fractional, so a lap 2 happens, where the refusal fires.
    ASSERT_TRUE(pm_->set_risk_modules(
                       {std::make_shared<ConstantScaleRiskModule>("cut", 0.85, /*every_lap=*/true),
                        std::make_shared<RefuseOnConditionRiskModule>(
                            "stop", RiskCondition{RiskCondition::Kind::LAP_AT_LEAST, 2.0},
                            "lap two")})
                    .is_ok());
    ::testing::internal::CaptureStdout();
    const auto result = pm_->process_market_data(three_days());
    const std::string out = ::testing::internal::GetCapturedStdout();
    ASSERT_TRUE(result.is_ok());

    const auto lap = rows_of(pm_->last_risk_decisions(), RiskPhase::LAP);
    int max_lap = 0;
    for (const auto& r : lap) max_lap = std::max(max_lap, r.lap);
    EXPECT_EQ(max_lap, 2);
    EXPECT_EQ(quantity("ZZA"), 1.0) << "pinned to the previous positions";
    EXPECT_EQ(out.find("Max iterations reached"), std::string::npos);
    EXPECT_NE(out.find("Risk module stop refused portfolio"), std::string::npos) << out;
    EXPECT_NE(out.find("Risk refusal: every strategy pinned to its previous positions after "
                       "iteration 2; leaving the loop"),
              std::string::npos);
    EXPECT_NE(out.find("Final positions pinned by a risk refusal after 2 iterations; rounding "
                       "skipped."),
              std::string::npos);
    // The strategy's own targets (its signals) are untouched.
    EXPECT_EQ(static_cast<double>(strategy_->get_target_positions().at("ZZA").quantity), 5.0);
    // Lap 2: the REFUSE won over the SCALE, which is recorded and not applied.
    EXPECT_EQ(lap[2].requested.action, RiskAction::SCALE);
    EXPECT_EQ(lap[2].applied_action, RiskAction::NONE);
    EXPECT_EQ(lap[3].applied_action, RiskAction::REFUSE);
    auto j = pm_->risk_decisions_json();
    EXPECT_EQ(j["outcome"]["action"], "REFUSE");
    EXPECT_EQ(j["outcome"]["refused"], true);
}

TEST_F(RiskRefuseTest, PinnedScopeIsSkippedByForcedRoundingAndFinalCheck) {
    make_two_sleeves(false, false, {{"AAA", make_pos("AAA", 3.0, 100.0)}},
                     {{"BBB", make_pos("BBB", 5.0, 100.0)}});
    ASSERT_TRUE(pm_->update_strategy_position("SA", "AAA", make_pos("AAA", 1.5, 100.0)).is_ok());
    ASSERT_TRUE(pm_->set_risk_modules(
                       {std::make_shared<ConstantScaleRiskModule>("cut", 0.85, /*every_lap=*/true)},
                       {{"SA",
                         {std::make_shared<RefuseOnConditionRiskModule>(
                             "stop_a", RiskCondition{RiskCondition::Kind::ALWAYS, 0.0}, "sleeve")}}})
                    .is_ok());
    ::testing::internal::CaptureStdout();
    ASSERT_TRUE(pm_->process_market_data(three_days()).is_ok());
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_EQ(qty("SA", "AAA"), 1.5);  // pinned, fractional, left alone
    EXPECT_EQ(qty("SB", "BBB"), std::round(5.0 * 0.85 * 0.85 * 0.85 * 0.85 * 0.85));
    EXPECT_NE(out.find("Max iterations reached (5)"), std::string::npos);
    EXPECT_NE(out.find("Final forced rounding for BBB"), std::string::npos) << out;
    EXPECT_EQ(out.find("Final forced rounding for AAA"), std::string::npos) << out;
    EXPECT_EQ(out.find("FINAL CHECK"), std::string::npos) << out;
    EXPECT_EQ(out.find("Fractional contract detected in iteration 1: AAA"), std::string::npos);
}

TEST_F(RiskRefuseTest, PerScopeApplySkipsPinnedSleeves) {
    make_two_sleeves(true, false, {{"AAA", make_pos("AAA", 4.0, 100.0)}},
                     {{"BBB", make_pos("BBB", 6.0, 100.0)}});
    ASSERT_TRUE(pm_->update_strategy_position("SA", "AAA", make_pos("AAA", 2.0, 100.0)).is_ok());
    ASSERT_TRUE(pm_->set_risk_modules(
                       {std::make_shared<ConstantScaleRiskModule>("half", 0.5)},
                       {{"SA",
                         {std::make_shared<RefuseOnConditionRiskModule>(
                             "stop_a", RiskCondition{RiskCondition::Kind::ALWAYS, 0.0}, "sleeve")}}})
                    .is_ok());
    ASSERT_TRUE(pm_->process_market_data(three_days()).is_ok());
    EXPECT_EQ(qty("SA", "AAA"), 2.0);  // the seed, not 4 and not 4 x 0.5
    EXPECT_EQ(qty("SB", "BBB"), 3.0);  // halved
    const auto sleeve = rows_of(pm_->last_risk_decisions(), RiskPhase::SLEEVE);
    ASSERT_EQ(sleeve.size(), 1u);
    EXPECT_EQ(sleeve[0].scope, RiskScope::SLEEVE);
    EXPECT_EQ(sleeve[0].scope_id, "SA");
    EXPECT_EQ(sleeve[0].applied_action, RiskAction::REFUSE);
    EXPECT_EQ(pm_->risk_decisions_json()["outcome"]["pinned_scopes"], nlohmann::json({"SA"}));
}

TEST_F(RiskRefuseTest, PinnedSleeveIsExcludedFromTheOptimiser) {
    // The optimiser runs only on symbols with >= 20 returns, so both strategies supply a
    // 40-price history.
    make_two_sleeves(false, true, {{"AAA", make_pos("AAA", 9.0, 100.0)}, {"BBB", make_pos("BBB", 4.0, 100.0)}},
                     {{"AAA", make_pos("AAA", 7.0, 100.0)}, {"BBB", make_pos("BBB", 5.0, 100.0)}});
    std::unordered_map<std::string, std::vector<double>> history;
    for (int d = 0; d < 40; ++d) {
        history["AAA"].push_back(100.0 * (1.0 + 0.03 * std::sin(0.7 * d)));
        history["BBB"].push_back(100.0 * (1.0 + 0.02 * std::cos(1.3 * d)));
    }
    sa_->set_history(history);
    sb_->set_history(history);
    ASSERT_TRUE(pm_->update_strategy_position("SA", "AAA", make_pos("AAA", 2.0, 100.0)).is_ok());
    ASSERT_TRUE(pm_->update_strategy_position("SA", "BBB", make_pos("BBB", 1.0, 100.0)).is_ok());
    ASSERT_TRUE(pm_->set_risk_modules(
                       {}, {{"SA",
                             {std::make_shared<RefuseOnConditionRiskModule>(
                                 "stop_a", RiskCondition{RiskCondition::Kind::ALWAYS, 0.0},
                                 "sleeve")}}})
                    .is_ok());
    ::testing::internal::CaptureStdout();
    ASSERT_TRUE(pm_->process_market_data(three_days()).is_ok());
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_EQ(qty("SA", "AAA"), 2.0) << out;
    EXPECT_EQ(qty("SA", "BBB"), 1.0);
    // SB went through the optimiser: its output is not its strategy target.
    const bool sb_changed = qty("SB", "AAA") != 7.0 || qty("SB", "BBB") != 5.0;
    EXPECT_TRUE(sb_changed) << "SB AAA=" << qty("SB", "AAA") << " BBB=" << qty("SB", "BBB");
}

TEST_F(RiskSleeveTest, SleeveModulesRunOncePreLoopOnOwnTargetsWithSleeveCapital) {
    make_two_sleeves(false, false, {{"AAA", make_pos("AAA", 2.5, 100.0)}},
                     {{"BBB", make_pos("BBB", 4.0, 100.0)}});
    auto spy = std::make_shared<SpyModule>("sleeve_spy", RiskAction::NONE);
    ASSERT_TRUE(pm_->set_risk_modules({}, {{"SA", {spy}}}).is_ok());
    ASSERT_TRUE(pm_->process_market_data(three_days()).is_ok());
    // Five laps (2.5 never becomes whole) and still one sleeve evaluation.
    EXPECT_EQ(spy->events, (std::vector<std::string>{"begin_rebalance", "on_bars:0", "evaluate:0"}));
    ASSERT_EQ(spy->books.size(), 1u);
    ASSERT_EQ(spy->books[0].size(), 1u);
    EXPECT_EQ(spy->books[0].at("AAA").quantity, Decimal(2.5));
    const RiskContext& c = spy->contexts[0];
    EXPECT_EQ(c.phase, RiskPhase::SLEEVE);
    EXPECT_EQ(c.lap, 0);
    EXPECT_EQ(c.scope, RiskScope::SLEEVE);
    EXPECT_EQ(c.scope_id, "SA");
    EXPECT_EQ(c.capital, Decimal(1000.0 * 0.5));
    ASSERT_EQ(spy->rebalance_contexts.size(), 1u);
    EXPECT_EQ(spy->rebalance_contexts[0].scope, RiskScope::SLEEVE);
    EXPECT_EQ(spy->rebalance_contexts[0].scope_id, "SA");
}

TEST_F(RiskSleeveTest, SleeveScaleIsAppliedToThatSleeveOnlyAndSeenByThePortfolio) {
    make_two_sleeves(true, false, {{"AAA", make_pos("AAA", 4.0, 100.0)}},
                     {{"BBB", make_pos("BBB", 6.0, 100.0)}});
    auto portfolio = std::make_shared<SpyModule>("portfolio_spy", RiskAction::NONE);
    ASSERT_TRUE(pm_->set_risk_modules(
                       {portfolio},
                       {{"SA", {std::make_shared<ConstantScaleRiskModule>("half_a", 0.5)}}})
                    .is_ok());
    ASSERT_TRUE(pm_->process_market_data(three_days()).is_ok());
    EXPECT_EQ(qty("SA", "AAA"), 2.0);
    EXPECT_EQ(qty("SB", "BBB"), 6.0);
    // The portfolio module measures the sleeve-scaled book and sees what the sleeve applied.
    ASSERT_EQ(portfolio->books.size(), 1u);
    EXPECT_EQ(portfolio->books[0].at("AAA").quantity, Decimal(2.0));
    EXPECT_EQ(portfolio->contexts[0].applied.at("SA"), 0.5);
}

TEST_F(RiskValidationTest, CompositionTermOnlyOncePerChain) {
    make_two_sleeves(true, false, {{"AAA", make_pos("AAA", 1.0, 100.0)}},
                     {{"BBB", make_pos("BBB", 1.0, 100.0)}});
    const RiskConfig rc = risk_config(true).risk_config;
    auto portfolio_carver = std::make_shared<CarverRiskModule>("carver", rc);
    // A Carver at the portfolio and another on a sleeve: correlation/VaR/jump counted twice.
    auto r = pm_->set_risk_modules({portfolio_carver},
                                   {{"SA", {std::make_shared<CarverRiskModule>("carver_a", rc)}}});
    ASSERT_TRUE(r.is_error());
    EXPECT_NE(std::string(r.error()->what()).find("COMPOSITION"), std::string::npos);
    // Two at the portfolio scope.
    EXPECT_TRUE(pm_->set_risk_modules({portfolio_carver, std::make_shared<CarverRiskModule>("c2", rc)})
                    .is_error());
    // A magnitude-only module on the sleeve is fine.
    EXPECT_TRUE(pm_->set_risk_modules(
                       {portfolio_carver},
                       {{"SA", {std::make_shared<ConstantScaleRiskModule>("cut_a", 0.9)}}})
                    .is_ok());
    // A sleeve key must be a registered strategy.
    EXPECT_TRUE(pm_->set_risk_modules({}, {{"NOPE", {std::make_shared<ConstantScaleRiskModule>(
                                                         "cut", 0.9)}}})
                    .is_error());
}

TEST_F(RiskValidationTest, AtMostOneReplaceCapablePerScope) {
    make_two_sleeves(true, false, {{"AAA", make_pos("AAA", 1.0, 100.0)}},
                     {{"BBB", make_pos("BBB", 1.0, 100.0)}});
    EXPECT_TRUE(pm_->set_risk_modules({std::make_shared<ReplaceModule>(Book{}),
                                       std::make_shared<SpyModule>("r2", RiskAction::REPLACE)})
                    .is_error());
    EXPECT_TRUE(pm_->set_risk_modules({}, {{"SA",
                                            {std::make_shared<ReplaceModule>(Book{}),
                                             std::make_shared<SpyModule>("r2", RiskAction::REPLACE)}}})
                    .is_error());
    // One per scope is fine, on two scopes.
    EXPECT_TRUE(pm_->set_risk_modules({std::make_shared<ReplaceModule>(Book{})},
                                      {{"SA", {std::make_shared<ReplaceModule>(Book{})}}})
                    .is_ok());
}

TEST_F(RiskReplaceTest, SingleSleeveReplaceSetsTheBook) {
    make_pm(false, {{{"ZZA", make_pos("ZZA", 5.5, 100.0)}}});
    ASSERT_TRUE(pm_->set_risk_modules(
                       {std::make_shared<ReplaceModule>(Book{{"ZZB", make_pos("ZZB", 2.0, 50.0)}})})
                    .is_ok());
    ::testing::internal::CaptureStdout();
    ASSERT_TRUE(pm_->process_market_data(three_days()).is_ok());
    const std::string out = ::testing::internal::GetCapturedStdout();
    auto positions = pm_->get_strategy_positions().at("RML_S");
    ASSERT_EQ(positions.size(), 1u);
    EXPECT_EQ(static_cast<double>(positions.at("ZZB").quantity), 2.0);
    EXPECT_NE(out.find("Risk replacement: the strategy's targets replaced by risk module replace "
                       "after iteration 1; leaving the loop"),
              std::string::npos)
        << out;
    EXPECT_EQ(pm_->risk_decisions_json()["outcome"]["action"], "REPLACE");
}

TEST_F(RiskReplaceTest, MultiSleevePortfolioReplaceFailsClosed) {
    make_two_sleeves(true, false, {{"AAA", make_pos("AAA", 4.0, 100.0)}},
                     {{"BBB", make_pos("BBB", 6.0, 100.0)}});
    ASSERT_TRUE(pm_->update_strategy_position("SA", "AAA", make_pos("AAA", 1.0, 100.0)).is_ok());
    ASSERT_TRUE(pm_->update_strategy_position("SB", "BBB", make_pos("BBB", 2.0, 100.0)).is_ok());
    ASSERT_TRUE(pm_->set_risk_modules(
                       {std::make_shared<ReplaceModule>(Book{{"ZZB", make_pos("ZZB", 2.0, 50.0)}})})
                    .is_ok());
    ::testing::internal::CaptureStdout();
    ASSERT_TRUE(pm_->process_market_data(three_days()).is_ok());
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_EQ(qty("SA", "AAA"), 1.0);
    EXPECT_EQ(qty("SB", "BBB"), 2.0);
    EXPECT_EQ(pm_->get_strategy_positions().at("SA").count("ZZB"), 0u);
    EXPECT_NE(out.find("[ERROR] [RiskManager] Risk module replace replaced the book of a portfolio "
                       "of 2 strategies, which cannot be distributed"),
              std::string::npos)
        << out;
}

TEST_F(RiskPostRoundingTest, FinalizeSeesTheRoundedBookAndMayWarnOrRefuse) {
    // 2.5 lots never become whole: five laps, then forced rounding to 3 (the half rounds away).
    struct Case {
        RiskAction action;
        double expected_qty;
        RiskAction expected_applied;
        const char* expected_line;
    };
    const std::vector<Case> cases = {
        {RiskAction::WARN, 3.0, RiskAction::WARN,
         "[WARNING] [RiskManager] Risk module fin warning on portfolio"},
        {RiskAction::REFUSE, 1.0, RiskAction::REFUSE, "Risk module fin refused portfolio"},
        {RiskAction::SCALE, 3.0, RiskAction::NONE,
         "[ERROR] [RiskManager] Risk module fin returned SCALE at the post-rounding point; only "
         "NONE, WARN and REFUSE are applied there"},
    };
    for (const auto& c : cases) {
        SCOPED_TRACE(risk_action_name(c.action));
        make_pm(false, {{{"ZZA", make_pos("ZZA", 2.5, 100.0)}}});
        ASSERT_TRUE(
            pm_->update_strategy_position("RML_S", "ZZA", make_pos("ZZA", 1.0, 100.0)).is_ok());
        auto fin = std::make_shared<FinalizeSpy>("fin", c.action);
        ASSERT_TRUE(pm_->set_risk_modules({fin}).is_ok());
        ::testing::internal::CaptureStdout();
        ASSERT_TRUE(pm_->process_market_data(three_days()).is_ok());
        const std::string out = ::testing::internal::GetCapturedStdout();
        ASSERT_EQ(fin->finalized.size(), 1u);
        EXPECT_EQ(fin->finalized[0].at("ZZA").quantity, Decimal(3.0)) << "the rounded book";
        EXPECT_EQ(fin->contexts[0].phase, RiskPhase::POST_ROUNDING);
        EXPECT_EQ(fin->contexts[0].lap, 5);
        EXPECT_EQ(quantity("ZZA"), c.expected_qty);
        EXPECT_NE(out.find(c.expected_line), std::string::npos) << out;
        const auto post = rows_of(pm_->last_risk_decisions(), RiskPhase::POST_ROUNDING);
        ASSERT_EQ(post.size(), 1u);
        EXPECT_EQ(post[0].requested.action, c.action);
        EXPECT_EQ(post[0].applied_action, c.expected_applied);
        pm_.reset();
        StateManager::reset_instance();
    }
}

TEST_F(RiskPostRoundingTest, TheCarverModuleIsSilentThere) {
    // The golden log test pins the lines; here: one Carver post-rounding row, NONE, and no
    // second "Risk management result" line in the rebalance.
    make_pm(true, {{{"ZZA", make_pos("ZZA", 7.0, 100.0)}}});
    ::testing::internal::CaptureStdout();
    ASSERT_TRUE(pm_->process_market_data(three_days()).is_ok());
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_EQ(count_of(out, "Risk management result:"), 1u);
    const auto post = rows_of(pm_->last_risk_decisions(), RiskPhase::POST_ROUNDING);
    ASSERT_EQ(post.size(), 1u);
    EXPECT_EQ(post[0].module_id, "carver");
    EXPECT_EQ(post[0].requested.action, RiskAction::NONE);
    EXPECT_FALSE(post[0].requested.metrics.has_value());
}
