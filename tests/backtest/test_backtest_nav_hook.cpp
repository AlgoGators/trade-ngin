// tests/backtest/test_backtest_nav_hook.cpp
//
// BacktestCoordinator hands every strategy the NAV at the close of the PREVIOUS cycle
// (StrategyInterface::on_portfolio_value) before it feeds the cycle's bars. A NAV-sized strategy
// (BPGV) sizes its targets on that value, so it must be yesterday's: today's would be look-ahead.
// The test runs a whole backtest (run_portfolio) on bars it controls, with a strategy that holds a
// position so the NAV moves, and records the order and value of every call.

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../core/test_base.hpp"
#include "../risk/risk_module_test_helpers.hpp"
#include "cost_basis_test_helpers.hpp"
#include "trade_ngin/backtest/backtest_coordinator.hpp"
#include "trade_ngin/backtest/equity_cost_warmup.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/data/market_data_bus.hpp"
#include "trade_ngin/instruments/equity.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"

using namespace trade_ngin;
using namespace trade_ngin::backtest;
using namespace trade_ngin::testing;
using namespace trade_ngin::testing::cost_basis;

namespace {

constexpr int kLastDay = 30;
const std::string kSymbol = "NAVHOOK";

// A registry row, so the position is marked to market (an unregistered symbol books no P&L).
void register_equity(const std::string& symbol) {
    EquitySpec spec;
    spec.exchange = "NYSE";
    spec.currency = "USD";
    spec.lot_size = 1.0;
    spec.tick_size = 0.01;
    spec.account_mode = EquityAccountMode::CASH;
    InstrumentRegistry::instance().register_instrument(
        symbol, std::make_shared<EquityInstrument>(symbol, spec));
}

// Records each NAV handed in and each bar feed, in call order.
class NavRecordingStrategy : public ScheduledStrategy {
public:
    using ScheduledStrategy::ScheduledStrategy;
    struct Event {
        bool is_nav;
        double nav;
    };
    std::vector<Event> events;

    // Marked to market daily (MIXED, as BPGV), so the NAV moves with the price.
    Result<void> initialize() override {
        auto r = ScheduledStrategy::initialize();
        if (r.is_ok()) set_pnl_accounting_method(PnLAccountingMethod::MIXED);
        return r;
    }
    void on_portfolio_value(double nav) override { events.push_back({true, nav}); }
    Result<void> on_data(const std::vector<Bar>& data) override {
        events.push_back({false, 0.0});
        return ScheduledStrategy::on_data(data);
    }
};

}  // namespace

class BacktestNavHook : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        MarketDataBus::instance().set_publish_enabled(true);
        LoggerConfig lc;
        lc.destination = LogDestination::CONSOLE;
        lc.min_level = LogLevel::ERR;
        lc.include_timestamp = false;
        Logger::instance().initialize(lc);
        db_ = std::make_shared<ServingDb>("mock://testdb");
        ASSERT_TRUE(db_->connect().is_ok());
    }

    void TearDown() override {
        MarketDataBus::instance().set_publish_enabled(true);
        (void)MarketDataBus::instance().unsubscribe("PORTFOLIO_MANAGER");
        coord_.reset();
        strat_.reset();
        pm_.reset();
        db_.reset();
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        TestBase::TearDown();
    }

    std::shared_ptr<ServingDb> db_;
    std::unique_ptr<BacktestCoordinator> coord_;
    std::shared_ptr<PortfolioManager> pm_;
    std::shared_ptr<NavRecordingStrategy> strat_;
};

TEST_F(BacktestNavHook, EachCycleSeesThePriorCloseNavBeforeItsBars) {
    register_equity(kSymbol);
    for (int d = -20; d <= kLastDay; ++d) {
        db_->rows.push_back({kSymbol, d, 100.0 * (1.0 + 0.02 * std::sin(0.9 * d)), 1.0e6});
    }

    BacktestCoordinatorConfig cc;
    cc.initial_capital = 1'000'000.0;
    cc.store_results = false;
    cc.store_trade_details = false;
    cc.portfolio_id = "NAV_HOOK_TEST";
    cc.statistics_sessions_per_year = 252.0;
    cc.csv_output_path = temp_csv_dir("nav_hook");
    coord_ = std::make_unique<BacktestCoordinator>(db_, &InstrumentRegistry::instance(), cc);
    ASSERT_TRUE(coord_->initialize().is_ok());

    PortfolioConfig pc{1'000'000.0, 1.0, 0.0, /*optimization=*/false};
    pc.opt_config.capital = 1'000'000.0;
    pc.risk_config.capital = 1'000'000.0;
    pc.risk_modules = {test_none_module()};
    pm_ = std::make_shared<PortfolioManager>(pc, "PM_NAV_HOOK");

    StrategyConfig sc;
    sc.capital_allocation = 1'000'000.0;
    sc.max_leverage = 10.0;
    sc.asset_classes = {AssetClass::EQUITIES};
    sc.frequencies = {DataFrequency::DAILY};
    sc.trading_params[kSymbol] = 1.0;
    sc.position_limits[kSymbol] = 1.0e6;
    strat_ = std::make_shared<NavRecordingStrategy>("NAV_S", sc, db_);
    strat_->targets = {{kSymbol, {{0, 5000.0}}}};  // hold 5,000 shares, so the NAV moves daily
    ASSERT_TRUE(strat_->initialize().is_ok());
    ASSERT_TRUE(strat_->start().is_ok());
    ASSERT_TRUE(pm_->add_strategy(strat_, 1.0, false).is_ok());

    register_equity_cost_warmup(
        {kSymbol}, trading_day(0),
        [&](const Timestamp& from, const Timestamp& to) {
            return db_->get_market_data({kSymbol}, from, to, AssetClass::EQUITIES,
                                        DataFrequency::DAILY, "ohlcv");
        },
        coord_->get_execution_manager()->get_transaction_cost_manager(), *pm_);

    auto result = coord_->run_portfolio(pm_, {kSymbol}, trading_day(0), trading_day(kLastDay),
                                        AssetClass::EQUITIES, DataFrequency::DAILY);
    ASSERT_TRUE(result.is_ok()) << result.error()->what();
    const auto& curve = result.value().equity_curve;

    // Every NAV is handed in before that cycle's bars, never after.
    std::vector<double> navs;
    for (size_t i = 0; i < strat_->events.size(); ++i) {
        if (!strat_->events[i].is_nav) continue;
        navs.push_back(strat_->events[i].nav);
        if (i + 1 < strat_->events.size()) {
            EXPECT_FALSE(strat_->events[i + 1].is_nav) << "cycle " << navs.size() << " fed no bars";
        }
    }
    ASSERT_FALSE(strat_->events.empty());
    EXPECT_TRUE(strat_->events.front().is_nav) << "the first bars arrived before any NAV";

    // The curve holds the start point, then one point per bar. The first bar's cycle has no prior
    // close to size on and returns before the hook, so the NAV handed in on bar k + 1 must be
    // curve point k + 1 -- the close of bar k -- and never point k + 2, bar k + 1's own close.
    ASSERT_EQ(navs.size() + 2, curve.size()) << "one NAV per bar after the first";
    int distinguishable = 0;
    for (size_t k = 0; k < navs.size(); ++k) {
        EXPECT_NEAR(navs[k], curve[k + 1].second, 1e-6) << "bar " << k + 1;
        if (k + 2 < curve.size() && std::abs(curve[k + 2].second - curve[k + 1].second) > 1.0) {
            ++distinguishable;
            EXPECT_GT(std::abs(navs[k] - curve[k + 2].second), 1.0)
                << "bar " << k + 1 << " was handed its own close";
        }
    }
    // The NAV must actually move, or today's and yesterday's could not be told apart.
    EXPECT_GT(distinguishable, 20);
}
