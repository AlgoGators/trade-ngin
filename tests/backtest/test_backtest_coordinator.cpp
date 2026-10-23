#include <gtest/gtest.h>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include "../portfolio/mock_strategy.hpp"
#include "trade_ngin/backtest/backtest_coordinator.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"

using namespace trade_ngin;
using namespace trade_ngin::backtest;
using namespace trade_ngin::testing;

namespace {

BacktestCoordinatorConfig default_config() {
    BacktestCoordinatorConfig c;
    c.initial_capital = 1'000'000.0;
    c.use_optimization = false;
    c.store_results = false;
    c.warmup_days = 0;
    c.portfolio_id = "TEST_PORTFOLIO";
    return c;
}

[[maybe_unused]] std::shared_ptr<MockStrategy> make_mock_strategy(
    const std::string& id, std::shared_ptr<MockPostgresDatabase> db) {
    StrategyConfig sc;
    sc.capital_allocation = 1'000'000.0;
    sc.max_leverage = 2.0;
    sc.asset_classes = {AssetClass::FUTURES};
    sc.frequencies = {DataFrequency::DAILY};
    sc.trading_params["ES"] = 1.0;
    sc.position_limits["ES"] = 1000.0;
    return std::make_shared<MockStrategy>(id, sc, db);
}

[[maybe_unused]] Bar make_bar(const std::string& symbol, double close) {
    Bar bar;
    bar.symbol = symbol;
    bar.timestamp = Timestamp{};
    bar.open = Decimal(close);
    bar.high = Decimal(close);
    bar.low = Decimal(close);
    bar.close = Decimal(close);
    bar.volume = 1000.0;
    return bar;
}

}  // namespace

class BacktestCoordinatorTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        db_ = std::make_shared<MockPostgresDatabase>("mock://testdb");
        registry_ = &InstrumentRegistry::instance();
    }
    std::shared_ptr<MockPostgresDatabase> db_;
    InstrumentRegistry* registry_ = nullptr;
};

TEST_F(BacktestCoordinatorTest, ConstructorStoresInitialCapitalAsPortfolioValue) {
    auto cfg = default_config();
    cfg.initial_capital = 2'500'000.0;
    BacktestCoordinator coord(db_, registry_, cfg);
    EXPECT_FALSE(coord.is_initialized());
    EXPECT_DOUBLE_EQ(coord.get_current_portfolio_value(), 2'500'000.0);
    EXPECT_TRUE(coord.get_current_positions().empty());
}

TEST_F(BacktestCoordinatorTest, ComponentGettersAreNullBeforeInitialize) {
    BacktestCoordinator coord(db_, registry_, default_config());
    EXPECT_EQ(coord.get_data_loader(), nullptr);
    EXPECT_EQ(coord.get_metrics_calculator(), nullptr);
    EXPECT_EQ(coord.get_price_manager(), nullptr);
    EXPECT_EQ(coord.get_pnl_manager(), nullptr);
    EXPECT_EQ(coord.get_execution_manager(), nullptr);
}

TEST_F(BacktestCoordinatorTest, InitializeWithNullDatabaseFailsWithConnectionError) {
    BacktestCoordinator coord(nullptr, registry_, default_config());
    auto r = coord.initialize();
    ASSERT_TRUE(r.is_error());
    EXPECT_EQ(r.error()->code(), ErrorCode::CONNECTION_ERROR);
    EXPECT_FALSE(coord.is_initialized());
}

TEST_F(BacktestCoordinatorTest, InitializeAutoConnectsDisconnectedDatabase) {
    ASSERT_FALSE(db_->is_connected());
    BacktestCoordinator coord(db_, registry_, default_config());
    auto r = coord.initialize();
    ASSERT_TRUE(r.is_ok()) << r.error()->what();
    EXPECT_TRUE(coord.is_initialized());
    EXPECT_TRUE(db_->is_connected());
}

TEST_F(BacktestCoordinatorTest, InitializeUsesAlreadyConnectedDatabase) {
    db_->connect();
    BacktestCoordinator coord(db_, registry_, default_config());
    auto r = coord.initialize();
    EXPECT_TRUE(r.is_ok());
}

TEST_F(BacktestCoordinatorTest, InitializeIsIdempotentOnSecondCall) {
    BacktestCoordinator coord(db_, registry_, default_config());
    ASSERT_TRUE(coord.initialize().is_ok());
    auto* dl_first = coord.get_data_loader();
    auto r2 = coord.initialize();  // second call short-circuits
    ASSERT_TRUE(r2.is_ok());
    EXPECT_EQ(coord.get_data_loader(), dl_first);  // pointer stable, not recreated
}

TEST_F(BacktestCoordinatorTest, InitializeCreatesAllSubComponents) {
    BacktestCoordinator coord(db_, registry_, default_config());
    ASSERT_TRUE(coord.initialize().is_ok());
    EXPECT_NE(coord.get_data_loader(), nullptr);
    EXPECT_NE(coord.get_metrics_calculator(), nullptr);
    EXPECT_NE(coord.get_price_manager(), nullptr);
    EXPECT_NE(coord.get_pnl_manager(), nullptr);
    EXPECT_NE(coord.get_execution_manager(), nullptr);
}

TEST_F(BacktestCoordinatorTest, ResetRestoresPortfolioValueToInitialCapital) {
    BacktestCoordinator coord(db_, registry_, default_config());
    ASSERT_TRUE(coord.initialize().is_ok());
    // Exercise sub-managers so reset has something to clear.
    coord.get_pnl_manager()->set_portfolio_value(0.0);
    coord.get_execution_manager()->update_market_data("ES", 100000.0, 4010.0, 4000.0);
    ASSERT_GT(coord.get_execution_manager()->get_adv("ES"), 0.0);

    coord.reset();
    EXPECT_DOUBLE_EQ(coord.get_current_portfolio_value(), 1'000'000.0);
    EXPECT_TRUE(coord.get_current_positions().empty());
    // Sub-managers reset too
    EXPECT_DOUBLE_EQ(coord.get_execution_manager()->get_adv("ES"), 0.0);
}

TEST_F(BacktestCoordinatorTest, ResetIsSafeBeforeInitialize) {
    BacktestCoordinator coord(db_, registry_, default_config());
    // Sub-managers are null at this point; reset() must guard against null.
    coord.reset();
    EXPECT_DOUBLE_EQ(coord.get_current_portfolio_value(), 1'000'000.0);
}

TEST_F(BacktestCoordinatorTest, IsInitializedFlagFlipsAfterInitialize) {
    BacktestCoordinator coord(db_, registry_, default_config());
    EXPECT_FALSE(coord.is_initialized());
    ASSERT_TRUE(coord.initialize().is_ok());
    EXPECT_TRUE(coord.is_initialized());
}

// ---- required_warmup_days hook (StrategyInterface) ----------------------------------
namespace {
class WarmupHookStrategy : public MockStrategy {
public:
    WarmupHookStrategy(std::string id, StrategyConfig cfg, std::shared_ptr<MockPostgresDatabase> db,
                       int days)
        : MockStrategy(std::move(id), std::move(cfg), db), days_(days) {}
    int required_warmup_days() const override { return days_; }
private:
    int days_;
};
}  // namespace

TEST_F(BacktestCoordinatorTest, CalculateWarmupDaysUsesTheInterfaceHookForNonTrendStrategies) {
    StrategyConfig sc;
    sc.capital_allocation = 1'000'000.0;
    sc.asset_classes = {AssetClass::EQUITIES};
    sc.frequencies = {DataFrequency::DAILY};
    sc.trading_params["AAPL"] = 1.0;

    std::vector<std::shared_ptr<StrategyInterface>> strategies;
    strategies.push_back(std::make_shared<MockStrategy>("NO_HOOK", sc, db_));  // default 0
    EXPECT_EQ(BacktestCoordinator::calculate_warmup_days(strategies), 0);

    strategies.push_back(std::make_shared<WarmupHookStrategy>("HOOK_37", sc, db_, 37));
    EXPECT_EQ(BacktestCoordinator::calculate_warmup_days(strategies), 37);

    strategies.push_back(std::make_shared<WarmupHookStrategy>("HOOK_12", sc, db_, 12));
    EXPECT_EQ(BacktestCoordinator::calculate_warmup_days(strategies), 37)
        << "the maximum across strategies wins";

    strategies.push_back(nullptr);  // null entries are skipped, as before
    EXPECT_EQ(BacktestCoordinator::calculate_warmup_days(strategies), 37);
}

TEST_F(BacktestCoordinatorTest, DataLoadStrictnessFlagsDefaultOff) {
    BacktestCoordinatorConfig cfg;
    EXPECT_FALSE(cfg.require_all_symbols);
    EXPECT_FALSE(cfg.require_aligned_calendar);
}
