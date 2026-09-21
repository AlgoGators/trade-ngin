// The risk layer's fail-CLOSED paths: what happens when a module cannot answer, when a
// PortfolioConfig assembled in code is invalid, and when a REFUSE has no previous book to pin to.
//
// Every one of these is a path the shipped configs never take (one carver module at portfolio
// scope, no sleeves, and it does not fail), so none of them can move a stored row today. They
// are the surface HD's "working, tested plug-and-play modules from day one" ruling opens, and
// before this commit three of them failed OPEN: a module that errored threw away the decisions
// its neighbours had already returned, a constructor that could not build a module logged one
// line and ran the book UNGATED, and a REFUSE on a live manager nobody had seeded shipped a FLAT
// book that the runner's diff against trading.positions reads as "sell everything".

#include <gtest/gtest.h>
#include "../risk/risk_module_test_helpers.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/risk/basic_risk_modules.hpp"
#include "trade_ngin/risk/carver_risk_module.hpp"
#include "trade_ngin/strategy/base_strategy.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

namespace {

using Book = std::unordered_map<std::string, Position>;

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

std::vector<Bar> three_days() {
    return {make_bar("ZZA", 1, 100.0), make_bar("ZZA", 2, 102.0), make_bar("ZZA", 3, 99.0)};
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

/// Fails on demand: an error Result, or a thrown exception, from evaluate or finalize.
/// `refuse_capable` decides whether its capabilities() claim it can say "do not trade".
class FailingModule final : public RiskModule {
public:
    FailingModule(std::string id, bool refuse_capable, bool throws = false,
                  bool fail_finalize = false)
        : id_(std::move(id)),
          refuse_capable_(refuse_capable),
          throws_(throws),
          fail_finalize_(fail_finalize) {}
    const std::string& id() const override { return id_; }
    const std::string& type() const override { return type_; }
    std::set<RiskTerm> terms() const override { return {RiskTerm::CUSTOM}; }
    std::set<RiskAction> capabilities() const override {
        return refuse_capable_ ? std::set<RiskAction>{RiskAction::REFUSE}
                               : std::set<RiskAction>{RiskAction::SCALE};
    }
    Result<RiskDecision> evaluate(const Book& book, const RiskContext& ctx) override {
        (void)book;
        (void)ctx;
        if (fail_finalize_) {
            RiskDecision d;
            d.module_id = id_;
            return Result<RiskDecision>(d);
        }
        return fail();
    }
    Result<RiskDecision> finalize(const Book& book, const RiskContext& ctx) override {
        (void)book;
        (void)ctx;
        if (!fail_finalize_) {
            RiskDecision d;
            d.module_id = id_;
            return Result<RiskDecision>(d);
        }
        return fail();
    }
    nlohmann::json describe() const override { return {{"id", id_}, {"type", type_}}; }
    void on_applied(const RiskApplied& a, const RiskContext& ctx) override {
        (void)a;
        if (ctx.phase == RiskPhase::LAP) ++lap_applied_calls;
        ++applied_calls;
    }

    int applied_calls{0};
    int lap_applied_calls{0};

private:
    Result<RiskDecision> fail() {
        if (throws_) throw std::runtime_error("the module threw: " + id_);
        return make_error<RiskDecision>(ErrorCode::UNKNOWN_ERROR, "the module failed: " + id_,
                                        "FailingModule");
    }
    std::string id_;
    std::string type_{"failing"};
    bool refuse_capable_;
    bool throws_;
    bool fail_finalize_;
};

PortfolioConfig base_config() {
    PortfolioConfig pc{1000.0, 0.0, 1.0, 0.0, /*optimization=*/false};
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

}  // namespace

class RiskFailClosedTest : public TestBase {
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

    std::shared_ptr<ScriptedStrategy> make_strategy(const std::string& id,
                                                    std::vector<Book> script) {
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

    /// One strategy "FC_S" whose target is `target` on every call, on a manager built from `pc`.
    void make_pm(PortfolioConfig pc, Book target) {
        make_pm_scripted(std::move(pc), {std::move(target)});
    }

    /// The same, with one target per process_market_data call.
    void make_pm_scripted(PortfolioConfig pc, std::vector<Book> script) {
        static int n = 0;
        pm_ = std::make_unique<PortfolioManager>(std::move(pc), "PM_FC_" + std::to_string(++n));
        strategy_ = make_strategy("FC_S", std::move(script));
        ASSERT_TRUE(pm_->add_strategy(strategy_, 1.0, false).is_ok());
    }

    /// Executions the manager holds RIGHT NOW. They ACCUMULATE across process_market_data
    /// calls (the synthesis appends and counts the existing ones), so "this call traded
    /// nothing" is a difference, not a total. get_strategy_executions() is the backtest
    /// coordinator's input; the live runners size their orders from trading.positions and
    /// never read it.
    size_t executions_now() {
        size_t n = 0;
        for (const auto& [sid, reports] : pm_->get_strategy_executions()) {
            (void)sid;
            n += reports.size();
        }
        return n;
    }

    double quantity(const std::string& symbol) {
        return static_cast<double>(
            pm_->get_strategy_positions().at("FC_S").at(symbol).quantity);
    }

    std::shared_ptr<MockPostgresDatabase> db_;
    std::unique_ptr<PortfolioManager> pm_;
    std::shared_ptr<ScriptedStrategy> strategy_;
};

// ===== A-1: combine first, then fail closed =====

// THE test the T-6a adversary asked for. [refuse, failing] on one scope: the refusal was taken
// before the failure happened, so the book that ships is the previous one.
// Before this commit the evaluate loop returned on the first error BEFORE combine_risk_decisions
// ran, the REFUSE was recorded with applied_action NONE and discarded, and the optimiser's book
// shipped -- exactly what risk_module.hpp:24 says cannot happen.
TEST_F(RiskFailClosedTest, ARefuseSurvivesALaterModulesFailure) {
    make_pm(base_config(), {{"ZZA", make_pos("ZZA", 5.0, 100.0)}});
    ASSERT_TRUE(pm_->update_strategy_position("FC_S", "ZZA", make_pos("ZZA", 1.0, 100.0)).is_ok());
    auto failing = std::make_shared<FailingModule>("boom", /*refuse_capable=*/false);
    ASSERT_TRUE(pm_->set_risk_modules(
                       {std::make_shared<RefuseOnConditionRiskModule>(
                            "stop", RiskCondition{RiskCondition::Kind::ALWAYS, 0.0}, "always"),
                        failing})
                    .is_ok());

    ::testing::internal::CaptureStdout();
    const auto result = pm_->process_market_data(three_days());
    const std::string out = ::testing::internal::GetCapturedStdout();
    ASSERT_TRUE(result.is_ok());

    EXPECT_EQ(quantity("ZZA"), 1.0) << "the seeded previous book, not the new one";
    EXPECT_NE(out.find("Risk module stop refused portfolio"), std::string::npos) << out;
    EXPECT_NE(out.find("Risk management calculation failed: the module failed: boom"),
              std::string::npos)
        << "the failure is still reported";
    // The failed module is recorded with its error and applied_action NONE, and is NOT told
    // what was applied on the lap it failed: it never evaluated anything there. Its finalize
    // did succeed, so the post-rounding point delivers one callback, which is correct.
    bool found = false;
    for (const auto& row : pm_->last_risk_decisions()) {
        if (row.module_id != "boom" || row.phase != RiskPhase::LAP) continue;
        found = true;
        EXPECT_EQ(row.applied_action, RiskAction::NONE);
        EXPECT_EQ(row.error, "the module failed: boom");
    }
    EXPECT_TRUE(found);
    EXPECT_EQ(failing->lap_applied_calls, 0);
}

// The same for a module that THROWS rather than returning an error Result: it used to escape to
// the catch around the whole apply, which returned OK with every decision discarded.
TEST_F(RiskFailClosedTest, ARefuseSurvivesALaterModulesThrow) {
    make_pm(base_config(), {{"ZZA", make_pos("ZZA", 5.0, 100.0)}});
    ASSERT_TRUE(pm_->update_strategy_position("FC_S", "ZZA", make_pos("ZZA", 1.0, 100.0)).is_ok());
    ASSERT_TRUE(pm_->set_risk_modules(
                       {std::make_shared<RefuseOnConditionRiskModule>(
                            "stop", RiskCondition{RiskCondition::Kind::ALWAYS, 0.0}, "always"),
                        std::make_shared<FailingModule>("boom", /*refuse_capable=*/false,
                                                        /*throws=*/true)})
                    .is_ok());

    ::testing::internal::CaptureStdout();
    const auto result = pm_->process_market_data(three_days());
    const std::string out = ::testing::internal::GetCapturedStdout();
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(quantity("ZZA"), 1.0);
    EXPECT_NE(out.find("Risk management calculation failed: the module threw: boom"),
              std::string::npos)
        << out;
}

// A SCALE another module already returned survives too: the magnitude term does not depend on
// the failed module's reading.
TEST_F(RiskFailClosedTest, AScaleSurvivesALaterModulesFailure) {
    PortfolioConfig pc = base_config();
    pc.allow_fractional_positions = true;
    make_pm(pc, {{"ZZA", make_pos("ZZA", 4.0, 100.0)}});
    ASSERT_TRUE(pm_->set_risk_modules(
                       {std::make_shared<ConstantScaleRiskModule>("cut", 0.5),
                        std::make_shared<FailingModule>("boom", /*refuse_capable=*/false)})
                    .is_ok());
    ASSERT_TRUE(pm_->process_market_data(three_days()).is_ok());
    EXPECT_EQ(quantity("ZZA"), 2.0) << "the 0.5 cut was applied, not thrown away";
}

// The other half of "fail closed": a module that could have REFUSED and could not answer has not
// said yes. Its silence refuses the scope.
TEST_F(RiskFailClosedTest, AFailedRefuseCapableModuleRefusesTheScope) {
    make_pm(base_config(), {{"ZZA", make_pos("ZZA", 5.0, 100.0)}});
    ASSERT_TRUE(pm_->update_strategy_position("FC_S", "ZZA", make_pos("ZZA", 2.0, 100.0)).is_ok());
    ASSERT_TRUE(pm_->set_risk_modules(
                       {std::make_shared<FailingModule>("gate", /*refuse_capable=*/true)})
                    .is_ok());

    ::testing::internal::CaptureStdout();
    const auto result = pm_->process_market_data(three_days());
    const std::string out = ::testing::internal::GetCapturedStdout();
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(quantity("ZZA"), 2.0) << "pinned to the seeded book";
    EXPECT_NE(out.find("Risk module gate failed on portfolio"), std::string::npos) << out;
    EXPECT_NE(out.find("and can refuse"), std::string::npos) << out;
}

// A module that cannot refuse does not refuse: its failure leaves the book to the others.
TEST_F(RiskFailClosedTest, AFailedScaleOnlyModuleDoesNotRefuseTheScope) {
    PortfolioConfig pc = base_config();
    pc.allow_fractional_positions = true;
    make_pm(pc, {{"ZZA", make_pos("ZZA", 4.0, 100.0)}});
    ASSERT_TRUE(pm_->update_strategy_position("FC_S", "ZZA", make_pos("ZZA", 2.0, 100.0)).is_ok());
    ASSERT_TRUE(pm_->set_risk_modules(
                       {std::make_shared<FailingModule>("cutter", /*refuse_capable=*/false)})
                    .is_ok());
    ASSERT_TRUE(pm_->process_market_data(three_days()).is_ok());
    EXPECT_EQ(quantity("ZZA"), 4.0) << "the strategy's own target, ungated but not pinned";
}

// The post-rounding point has the same shape and had the same hole.
TEST_F(RiskFailClosedTest, AFinalizeFailureDoesNotDiscardAFinalizeRefusal) {
    make_pm(base_config(), {{"ZZA", make_pos("ZZA", 5.0, 100.0)}});
    ASSERT_TRUE(pm_->update_strategy_position("FC_S", "ZZA", make_pos("ZZA", 3.0, 100.0)).is_ok());
    // The refusal fires only at POST_ROUNDING (the book there is whole); the failure sits after it.
    class FinalizeRefuse final : public RiskModule {
    public:
        const std::string& id() const override { return id_; }
        const std::string& type() const override { return id_; }
        std::set<RiskTerm> terms() const override { return {RiskTerm::CUSTOM}; }
        std::set<RiskAction> capabilities() const override { return {RiskAction::REFUSE}; }
        Result<RiskDecision> evaluate(const Book& book, const RiskContext& ctx) override {
            (void)book;
            (void)ctx;
            RiskDecision d;
            d.module_id = id_;
            return Result<RiskDecision>(d);
        }
        Result<RiskDecision> finalize(const Book& book, const RiskContext& ctx) override {
            (void)book;
            (void)ctx;
            RiskDecision d;
            d.module_id = id_;
            d.action = RiskAction::REFUSE;
            d.reason = "the rounded book is not acceptable";
            return Result<RiskDecision>(d);
        }
        nlohmann::json describe() const override { return {{"id", id_}}; }

    private:
        std::string id_{"final_stop"};
    };
    ASSERT_TRUE(pm_->set_risk_modules({std::make_shared<FinalizeRefuse>(),
                                       std::make_shared<FailingModule>(
                                           "boom", /*refuse_capable=*/false, /*throws=*/false,
                                           /*fail_finalize=*/true)})
                    .is_ok());

    ::testing::internal::CaptureStdout();
    ASSERT_TRUE(pm_->process_market_data(three_days()).is_ok());
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_EQ(quantity("ZZA"), 3.0) << "pinned by the post-rounding refusal";
    EXPECT_NE(out.find("Risk module final_stop refused portfolio"), std::string::npos) << out;
}

// ===== D-1 / A-3: the constructor validates, and rethrows =====

// Two carver modules are two COMPOSITION terms at one scope: the scale-invariant terms would be
// counted twice. set_risk_modules always refused this; the constructor used to accept it.
TEST_F(RiskFailClosedTest, ConstructingWithTwoCompositionModulesThrows) {
    PortfolioConfig pc = base_config();
    pc.risk_modules = {test_carver_module(pc.risk_config, "carver_a"),
                       test_carver_module(pc.risk_config, "carver_b")};
    EXPECT_THROW(PortfolioManager(pc, "PM_FC_TWO_COMPOSITION"), std::invalid_argument);
}

TEST_F(RiskFailClosedTest, ConstructingWithADuplicateModuleIdThrows) {
    PortfolioConfig pc = base_config();
    RiskModuleConfig scale;
    scale.id = "carver";  // the same id the carver module carries
    scale.type = "constant_scale";
    scale.params = ConstantScaleModuleConfig{0.5, false};
    pc.risk_modules = {test_carver_module(pc.risk_config, "carver"), scale};
    EXPECT_THROW(PortfolioManager(pc, "PM_FC_DUP_ID"), std::invalid_argument);
}

// A module whose CONSTRUCTOR throws (ConstantScaleRiskModule rejects a scale outside [0,1]).
// Before this commit the PM caught it, logged one ERROR, cleared the list, and ran the book with
// no risk layer at all and exit 0.
TEST_F(RiskFailClosedTest, ConstructingWithAnUnbuildableModuleThrowsInsteadOfRunningUngated) {
    PortfolioConfig pc = base_config();
    RiskModuleConfig bad;
    bad.id = "bad_scale";
    bad.type = "constant_scale";
    bad.params = ConstantScaleModuleConfig{1.5, false};
    pc.risk_modules = {bad};
    EXPECT_THROW(PortfolioManager(pc, "PM_FC_UNBUILDABLE"), std::invalid_argument);
}

// The loader checks the sleeve key against portfolio.json's `strategies`; the runner does not
// always register the strategy under that key (the live equity runner registers
// LIVE_EQUITY_MEAN_REVERSION for the config key MEAN_REVERSION). A key that matches nothing used
// to fire in the backtest and silently never fire live. It is now an error on the first call.
TEST_F(RiskFailClosedTest, ASleeveKeyThatNamesNoRegisteredStrategyErrorsOnTheFirstCall) {
    PortfolioConfig pc = base_config();
    RiskModuleConfig warn;
    warn.id = "sleeve_warn";
    warn.type = "warn";
    warn.params = ConditionModuleConfig{RiskCondition{RiskCondition::Kind::ALWAYS, 0.0}, "always"};
    pc.sleeve_risk_modules = {{"MEAN_REVERSION", {warn}}};
    make_pm(pc, {{"ZZA", make_pos("ZZA", 3.0, 100.0)}});  // registers "FC_S", not MEAN_REVERSION

    const auto result = pm_->process_market_data(three_days());
    ASSERT_TRUE(result.is_error());
    EXPECT_NE(std::string(result.error()->what())
                  .find("Sleeve risk modules for an unregistered strategy: MEAN_REVERSION"),
              std::string::npos)
        << result.error()->what();
}

TEST_F(RiskFailClosedTest, ASleeveKeyThatNamesARegisteredStrategyIsAccepted) {
    PortfolioConfig pc = base_config();
    pc.allow_fractional_positions = true;
    RiskModuleConfig warn;
    warn.id = "sleeve_warn";
    warn.type = "warn";
    warn.params = ConditionModuleConfig{RiskCondition{RiskCondition::Kind::ALWAYS, 0.0}, "always"};
    pc.sleeve_risk_modules = {{"FC_S", {warn}}};
    make_pm(pc, {{"ZZA", make_pos("ZZA", 3.0, 100.0)}});
    EXPECT_TRUE(pm_->process_market_data(three_days()).is_ok());
}

// ===== A-2: a REFUSE needs a previous book to pin to =====

// The live manager the equity runner builds: nobody ever calls update_strategy_position, so
// current_positions is whatever the last bar left there. Pinning to it ships a FLAT book, and
// the runner's diff against trading.positions turns that into a liquidation. Refuse the run.
TEST_F(RiskFailClosedTest, ARefuseOnANeverSeededLiveScopeIsAnError) {
    make_pm(base_config(), {{"ZZA", make_pos("ZZA", 5.0, 100.0)}});
    ASSERT_TRUE(pm_->set_risk_modules(
                       {std::make_shared<RefuseOnConditionRiskModule>(
                           "stop", RiskCondition{RiskCondition::Kind::ALWAYS, 0.0}, "always")})
                    .is_ok());

    ::testing::internal::CaptureStdout();
    const auto result = pm_->process_market_data(three_days());
    const std::string out = ::testing::internal::GetCapturedStdout();
    ASSERT_TRUE(result.is_error()) << "a flat book must not be shipped as a refusal";
    EXPECT_NE(std::string(result.error()->what()).find("never seeded"), std::string::npos)
        << result.error()->what();
    EXPECT_NE(out.find("pinning would ship a FLAT book"), std::string::npos) << out;
}

// Seeded, and the manager has already traded into the book: the refusal does what it says and
// synthesises NOTHING. Two calls, because the synthesis diffs the target against
// filled_positions_ -- see ARefusalOnAFreshLedgerStillSynthesisesTheWholeBook below.
TEST_F(RiskFailClosedTest, ARefuseOnASeededLiveScopePinsAndTradesNothing) {
    make_pm_scripted(base_config(), {{{"ZZA", make_pos("ZZA", 2.0, 100.0)}},
                                     {{"ZZA", make_pos("ZZA", 5.0, 100.0)}}});
    ASSERT_TRUE(pm_->update_strategy_position("FC_S", "ZZA", make_pos("ZZA", 2.0, 100.0)).is_ok());
    // Fires only above 3 lots, so call 1 trades into the book and call 2 is refused.
    ASSERT_TRUE(pm_->set_risk_modules(
                       {std::make_shared<RefuseOnConditionRiskModule>(
                           "stop", RiskCondition{RiskCondition::Kind::MAX_ABS_QUANTITY_ABOVE, 3.0},
                           "a line this large is a sizing error")})
                    .is_ok());
    ASSERT_TRUE(pm_->process_market_data(three_days()).is_ok());
    ASSERT_EQ(quantity("ZZA"), 2.0) << "call 1: not refused, the ledger now holds 2 lots";
    const size_t before = executions_now();
    ASSERT_EQ(before, 1u) << "call 1 traded into the book from flat";

    ASSERT_TRUE(pm_->process_market_data(three_days()).is_ok());
    EXPECT_EQ(quantity("ZZA"), 2.0) << "call 2: refused and pinned to the previous book";
    EXPECT_EQ(executions_now() - before, 0u) << "a refusal trades nothing";
}

// RECORDED, not fixed. The synthesis diffs the target against filled_positions_ -- what THIS
// manager has traded into -- and nothing seeds that ledger: update_strategy_position writes
// current_positions and target_positions only. So on a manager whose ledger is still empty, a
// refusal pins the target to the previous book and the synthesis then reports "buy the whole
// book from flat".
//
// Harmless today and deliberately left alone: get_strategy_executions() is read by the BACKTEST
// coordinator alone (backtest_coordinator.cpp:599/643/1249/1350), where the ledger accumulates
// across the period loop and equals the pinned book, so nothing is emitted -- and the live
// runners size their orders by diffing trading.positions, never this. It is a trap for the first
// live consumer of these rows, and it is the same shape as A-2: the fix is to seed the ledger
// beside current_positions, which moves stored rows and is therefore its own measured commit.
// T-6a's adversary stated "the execution synthesis diffs pinned == previous, so nothing is
// emitted"; that is true of the backtest and not of a fresh manager.
TEST_F(RiskFailClosedTest, ARefusalOnAFreshLedgerStillSynthesisesTheWholeBook) {
    make_pm(base_config(), {{"ZZA", make_pos("ZZA", 5.0, 100.0)}});
    ASSERT_TRUE(pm_->update_strategy_position("FC_S", "ZZA", make_pos("ZZA", 2.0, 100.0)).is_ok());
    ASSERT_TRUE(pm_->set_risk_modules(
                       {std::make_shared<RefuseOnConditionRiskModule>(
                           "stop", RiskCondition{RiskCondition::Kind::ALWAYS, 0.0}, "always")})
                    .is_ok());
    ASSERT_TRUE(pm_->process_market_data(three_days()).is_ok());
    EXPECT_EQ(quantity("ZZA"), 2.0) << "the book is pinned correctly";
    EXPECT_EQ(executions_now(), 1u)
        << "today's behaviour, pinned so a change is noticed: the empty filled ledger makes the "
           "pinned book look like a trade from flat";
}

// A BACKTEST manager needs no seed: the coordinator seeds every day, and on day one the empty
// book IS the previous book. The guard must not break day one of a backtest.
TEST_F(RiskFailClosedTest, ARefuseOnAnUnseededBacktestScopePinsAsBefore) {
    make_pm(base_config(), {{"ZZA", make_pos("ZZA", 5.0, 100.0)}});
    pm_->set_backtest_mode(true);
    ASSERT_TRUE(pm_->set_risk_modules(
                       {std::make_shared<RefuseOnConditionRiskModule>(
                           "stop", RiskCondition{RiskCondition::Kind::ALWAYS, 0.0}, "always")})
                    .is_ok());
    ASSERT_TRUE(pm_->process_market_data(three_days()).is_ok());
    auto positions = pm_->get_strategy_positions().at("FC_S");
    EXPECT_TRUE(positions.empty() ||
                static_cast<double>(positions.at("ZZA").quantity) == 0.0)
        << "day one's previous book is empty, and that is the honest answer";
}

// ===== 7e: the applied-scale line HD ruled on 2026-09-18 =====

namespace {

/// The value of `key=` in the first RISK_APPLIED line of `out`, or "" if there is none.
std::string applied_field(const std::string& out, const std::string& key) {
    const size_t line = out.find("RISK_APPLIED ");
    if (line == std::string::npos) return "";
    const size_t end_of_line = out.find('\n', line);
    const std::string text = out.substr(line, end_of_line - line);
    const size_t at = text.find(" " + key + "=");
    if (at == std::string::npos) return "";
    const size_t from = at + key.size() + 2;
    const size_t to = text.find(' ', from);
    return text.substr(from, to == std::string::npos ? std::string::npos : to - from);
}

size_t count_lines(const std::string& out, const std::string& needle) {
    size_t n = 0;
    for (size_t at = out.find(needle); at != std::string::npos; at = out.find(needle, at + 1)) ++n;
    return n;
}

}  // namespace

// One line per lap, naming what was ASKED for and what the book was actually multiplied by.
// The pre-existing WARN prints only the request, and only on a cutting lap, so until now no
// line anywhere said what the quantised factor was.
TEST_F(RiskFailClosedTest, EveryLapLogsWhatWasRequestedAndWhatWasApplied) {
    PortfolioConfig pc = base_config();
    pc.allow_fractional_positions = true;
    make_pm(pc, {{"ZZA", make_pos("ZZA", 4.0, 100.0)}});
    ASSERT_TRUE(pm_->set_risk_modules({std::make_shared<ConstantScaleRiskModule>("cut", 0.25)})
                    .is_ok());

    ::testing::internal::CaptureStdout();
    ASSERT_TRUE(pm_->process_market_data(three_days()).is_ok());
    const std::string out = ::testing::internal::GetCapturedStdout();

    ASSERT_EQ(count_lines(out, "RISK_APPLIED "), 1u) << out;
    EXPECT_EQ(applied_field(out, "lap"), "1");
    EXPECT_EQ(applied_field(out, "requested"), "0.25");
    EXPECT_EQ(applied_field(out, "applied"), "0.25");
    EXPECT_EQ(applied_field(out, "cumulative"), "0.25");
    EXPECT_EQ(applied_field(out, "action"), "SCALE");
    EXPECT_EQ(applied_field(out, "module"), "cut");
    EXPECT_EQ(applied_field(out, "scope"), "portfolio");
    EXPECT_EQ(quantity("ZZA"), 1.0);
}

// The line is printed on a lap that cuts NOTHING too, so its count is the number of laps that
// evaluated a book and not the number of cuts. That is what lets the gate declare the expected
// count in advance: it equals the "Risk management result:" count per unit.
TEST_F(RiskFailClosedTest, ALapThatCutsNothingStillLogsTheLine) {
    PortfolioConfig pc = base_config();
    pc.allow_fractional_positions = true;
    make_pm(pc, {{"ZZA", make_pos("ZZA", 4.0, 100.0)}});
    ASSERT_TRUE(pm_->set_risk_modules({std::make_shared<WarnRiskModule>(
                                          "w", RiskCondition{RiskCondition::Kind::ALWAYS, 0.0},
                                          "noisy")})
                    .is_ok());

    ::testing::internal::CaptureStdout();
    ASSERT_TRUE(pm_->process_market_data(three_days()).is_ok());
    const std::string out = ::testing::internal::GetCapturedStdout();

    ASSERT_EQ(count_lines(out, "RISK_APPLIED "), 1u) << out;
    EXPECT_EQ(applied_field(out, "requested"), "1");
    EXPECT_EQ(applied_field(out, "applied"), "1") << "nothing was multiplied";
    EXPECT_EQ(applied_field(out, "cumulative"), "1");
    EXPECT_EQ(applied_field(out, "action"), "WARN");
    EXPECT_EQ(quantity("ZZA"), 4.0);
}

// %.17g, not to_string. A six-decimal rendering of the factor below is 0.111111 on BOTH the
// requested and the applied field, which would hide the quantisation the factor actually took;
// feedback_measure_definitions_change_meaning records that log precision has already nearly
// published a false finding once.
TEST_F(RiskFailClosedTest, TheAppliedFactorIsPrintedAtFullPrecision) {
    PortfolioConfig pc = base_config();
    pc.allow_fractional_positions = true;
    make_pm(pc, {{"ZZA", make_pos("ZZA", 9.0, 100.0)}});
    const double requested = 1.0 / 9.0;  // 0.1111111111111111
    ASSERT_TRUE(
        pm_->set_risk_modules({std::make_shared<ConstantScaleRiskModule>("cut", requested)})
            .is_ok());

    ::testing::internal::CaptureStdout();
    ASSERT_TRUE(pm_->process_market_data(three_days()).is_ok());
    const std::string out = ::testing::internal::GetCapturedStdout();

    // The request keeps all seventeen digits; the applied factor is the QUANTISED Decimal, a
    // different number, and the line shows both rather than one rounded to look like the other.
    EXPECT_EQ(applied_field(out, "requested"), "0.1111111111111111");
    EXPECT_EQ(applied_field(out, "applied"), "0.11111111");
    EXPECT_NE(applied_field(out, "requested"), applied_field(out, "applied"))
        << "six decimals would print 0.111111 for both";
}

// An empty book returns before evaluate, so no line: the count stays equal to the number of
// gate evaluations rather than the number of laps.
TEST_F(RiskFailClosedTest, AnEmptyBookLapLogsNoLine) {
    PortfolioConfig pc = base_config();
    pc.allow_fractional_positions = true;
    make_pm(pc, Book{});
    ASSERT_TRUE(pm_->set_risk_modules({std::make_shared<ConstantScaleRiskModule>("cut", 0.5)})
                    .is_ok());

    ::testing::internal::CaptureStdout();
    ASSERT_TRUE(pm_->process_market_data(three_days()).is_ok());
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_EQ(count_lines(out, "RISK_APPLIED "), 0u) << out;
    EXPECT_NE(out.find("No positions to apply risk management to"), std::string::npos);
}

// ===== T-6b-fix F6: the residues of T-6b's review =====

namespace {

/// Refuses only at the post-rounding point, so the laps run and the refusal meets the final book.
class FinalizeOnlyRefuse final : public RiskModule {
public:
    const std::string& id() const override { return id_; }
    const std::string& type() const override { return id_; }
    std::set<RiskTerm> terms() const override { return {RiskTerm::CUSTOM}; }
    std::set<RiskAction> capabilities() const override { return {RiskAction::REFUSE}; }
    Result<RiskDecision> evaluate(const Book& book, const RiskContext& ctx) override {
        (void)book;
        (void)ctx;
        RiskDecision d;
        d.module_id = id_;
        return Result<RiskDecision>(d);
    }
    Result<RiskDecision> finalize(const Book& book, const RiskContext& ctx) override {
        (void)book;
        (void)ctx;
        RiskDecision d;
        d.module_id = id_;
        d.action = RiskAction::REFUSE;
        d.reason = "the rounded book is not acceptable";
        return Result<RiskDecision>(d);
    }
    nlohmann::json describe() const override { return {{"id", id_}}; }

private:
    std::string id_{"final_stop"};
};

}  // namespace

// A-1 on the SLEEVE path (apply_sleeve_risk has its own call of evaluate_scope_modules). A
// refuse-capable module that cannot answer has not said yes: the sleeve is refused and pinned.
TEST_F(RiskFailClosedTest, AFailedRefuseCapableModuleRefusesTheSleeve) {
    make_pm(base_config(), {{"ZZA", make_pos("ZZA", 5.0, 100.0)}});
    ASSERT_TRUE(pm_->update_strategy_position("FC_S", "ZZA", make_pos("ZZA", 2.0, 100.0)).is_ok());
    ASSERT_TRUE(pm_->set_risk_modules(
                       {}, {{"FC_S", {std::make_shared<FailingModule>("sleeve_boom",
                                                                      /*refuse_capable=*/true)}}})
                    .is_ok());
    ::testing::internal::CaptureStdout();
    ASSERT_TRUE(pm_->process_market_data(three_days()).is_ok());
    const std::string out = ::testing::internal::GetCapturedStdout();
    EXPECT_EQ(quantity("ZZA"), 2.0) << "the sleeve is pinned to its previous book";
    EXPECT_NE(out.find("Risk module sleeve_boom failed on sleeve FC_S before the loop and can "
                       "refuse"),
              std::string::npos)
        << out;
}

// ...and a REFUSE that one sleeve module already returned survives a later module's failure.
TEST_F(RiskFailClosedTest, ASleeveRefuseSurvivesALaterModulesFailure) {
    make_pm(base_config(), {{"ZZA", make_pos("ZZA", 5.0, 100.0)}});
    ASSERT_TRUE(pm_->update_strategy_position("FC_S", "ZZA", make_pos("ZZA", 2.0, 100.0)).is_ok());
    ASSERT_TRUE(pm_->set_risk_modules(
                       {}, {{"FC_S",
                             {std::make_shared<RefuseOnConditionRiskModule>(
                                  "sleeve_stop", RiskCondition{RiskCondition::Kind::ALWAYS, 0.0},
                                  "always"),
                              std::make_shared<FailingModule>("later_boom",
                                                              /*refuse_capable=*/false)}}})
                    .is_ok());
    ASSERT_TRUE(pm_->process_market_data(three_days()).is_ok());
    EXPECT_EQ(quantity("ZZA"), 2.0);
}

// A-2 at SLEEVE scope: pinning a never-seeded live sleeve would ship it flat. The run errors.
TEST_F(RiskFailClosedTest, ARefuseOnANeverSeededLiveSleeveIsAnError) {
    make_pm(base_config(), {{"ZZA", make_pos("ZZA", 5.0, 100.0)}});
    ASSERT_TRUE(pm_->set_risk_modules(
                       {}, {{"FC_S", {std::make_shared<RefuseOnConditionRiskModule>(
                                         "sleeve_stop",
                                         RiskCondition{RiskCondition::Kind::ALWAYS, 0.0},
                                         "always")}}})
                    .is_ok());
    ::testing::internal::CaptureStdout();
    const auto result = pm_->process_market_data(three_days());
    const std::string out = ::testing::internal::GetCapturedStdout();
    ASSERT_TRUE(result.is_error()) << "a flat sleeve must not be shipped as a refusal";
    EXPECT_NE(std::string(result.error()->what()).find("never seeded"), std::string::npos)
        << result.error()->what();
    EXPECT_NE(out.find("pinning would ship a FLAT book"), std::string::npos) << out;
}

// A-2 at the POST-ROUNDING point, which has its own unseeded branch.
TEST_F(RiskFailClosedTest, APostRoundingRefuseOnANeverSeededLiveScopeIsAnError) {
    make_pm(base_config(), {{"ZZA", make_pos("ZZA", 5.0, 100.0)}});
    ASSERT_TRUE(pm_->set_risk_modules({std::make_shared<FinalizeOnlyRefuse>()}).is_ok());
    ::testing::internal::CaptureStdout();
    const auto result = pm_->process_market_data(three_days());
    const std::string out = ::testing::internal::GetCapturedStdout();
    ASSERT_TRUE(result.is_error()) << out;
    EXPECT_NE(std::string(result.error()->what()).find("never seeded"), std::string::npos)
        << result.error()->what();
    EXPECT_NE(out.find("Risk module final_stop refused portfolio"), std::string::npos) << out;
    EXPECT_NE(out.find("after rounding"), std::string::npos) << out;
}

// C-2 in the SHARED validator: the loader refused a carver at sleeve scope, the constructor and
// set_risk_modules did not. A carver divides by the portfolio's capital, so on a sleeve its
// leverage limits would be read against the whole book's money.
TEST_F(RiskFailClosedTest, ACarverAtSleeveScopeIsRefusedOnTheConstructorPath) {
    PortfolioConfig pc = base_config();
    pc.risk_modules = {test_none_module("no_portfolio_risk")};
    pc.sleeve_risk_modules = {{"FC_S", {test_carver_module(pc.risk_config, "sleeve_carver")}}};
    try {
        PortfolioManager pm(pc, "PM_FC_SLEEVE_CARVER");
        FAIL() << "a sleeve-scope carver must not construct";
    } catch (const std::invalid_argument& e) {
        EXPECT_EQ(std::string(e.what()),
                  "Risk module sleeve_carver on sleeve FC_S is type \"carver\", which is only "
                  "valid at portfolio scope: the Carver gate divides by the portfolio's capital");
    }
}

TEST_F(RiskFailClosedTest, ACarverAtSleeveScopeIsRefusedBySetRiskModules) {
    make_pm(base_config(), {{"ZZA", make_pos("ZZA", 5.0, 100.0)}});
    auto r = pm_->set_risk_modules(
        {}, {{"FC_S", {std::make_shared<CarverRiskModule>("sleeve_carver", RiskConfig{})}}});
    ASSERT_TRUE(r.is_error());
    EXPECT_NE(std::string(r.error()->what()).find("only valid at portfolio scope"),
              std::string::npos)
        << r.error()->what();
}

// B-4: on a lap the gate reads as over its limits but the level cut declines (already cut to
// this level or deeper), the log must not say "not exceeded" under "risk_exceeded=1".
TEST_F(RiskFailClosedTest, ADeclinedLapIsNotReportedAsNotExceeded) {
    // VaR binds (tight var limit), leverage never does; 2.5 lots never become whole, so lap 2
    // reads the same invariant term off the same composition and declines.
    PortfolioConfig pc = base_config();
    pc.risk_config.var_limit = 1e-6;
    pc.risk_modules = {test_carver_module(pc.risk_config)};
    make_pm(pc, {{"ZZA", make_pos("ZZA", 2.5, 100.0)}});
    std::vector<Bar> bars;
    for (int d = 1; d <= 30; ++d) bars.push_back(make_bar("ZZA", d, 100.0 + 3.0 * std::sin(d)));
    ::testing::internal::CaptureStdout();
    ASSERT_TRUE(pm_->process_market_data(bars).is_ok());
    const std::string out = ::testing::internal::GetCapturedStdout();
    ASSERT_NE(out.find("RISK_APPLIED lap=2 "), std::string::npos) << "the precondition: a lap 2\n"
                                                                   << out;
    EXPECT_EQ(out.find("Risk limits not exceeded, no scaling needed"), std::string::npos) << out;
    EXPECT_NE(out.find("Risk cut already applied at this level or deeper; no further scaling this "
                       "lap"),
              std::string::npos)
        << out;
}
