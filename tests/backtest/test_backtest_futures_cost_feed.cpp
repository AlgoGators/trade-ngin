// tests/backtest/test_backtest_futures_cost_feed.cpp
//
// T-7b-2 8c, COST-H3 (T-VOL §4, proven): the futures backtest's two cost managers (the coordinator's
// execution manager's, which prices the stored fills, and the PortfolioManager's, which prices them
// in backtest.executions and the optimizer's cost vector) read the same basis the live futures
// runners read (futures_cost_feed.hpp, T-7b-1 C8a/C8d): the fill day's OWN volume, the volume of the
// symbol's signal bar T-1 whose close prices the fill, as the impact model's only observation, and
// the walk of the symbol's consecutive returns ending at that bar for the volatility term; from the
// strategy feed, so a JUNK signal bar is withheld on its cycle as live withholds it (T-7b-1 C7b R10).
//
// The parent fed each cycle's own group, day T, to both managers before the book was sized on the
// T-1 group and filled at the T-1 close: a 20-bar mean ending at T (a one-bar look-ahead) and a
// volatility window with no return across a gap in the symbol's bars.
//
// Each test runs a whole futures backtest (run_portfolio) on bars the test controls and compares the
// managers and the stored fill costs with live's feed of the same bars (feed_futures_cost_model into
// a fresh manager, exactly what a live run on that T computes).

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../core/test_base.hpp"
#include "../risk/risk_module_test_helpers.hpp"
#include "cost_basis_test_helpers.hpp"
#include "trade_ngin/backtest/backtest_coordinator.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/data/market_data_bus.hpp"
#include "trade_ngin/live/futures_cost_feed.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/transaction_cost/transaction_cost_manager.hpp"

using namespace trade_ngin;
using namespace trade_ngin::backtest;
using namespace trade_ngin::testing;
using namespace trade_ngin::testing::cost_basis;

namespace {

using transaction_cost::TransactionCostManager;

// A stored cost is a Decimal the execution report carries to 8 decimals.
constexpr double kStoredTol = 1e-7;

const std::string kA = "XA.v.0";
const std::string kB = "XB.v.0";
constexpr int kThinDay = 30;   // XA prints 5,000 lots (a thin session, not junk)
constexpr int kJunkDay = 35;   // XA's bar is locked (high == low): JUNK

double close_of(const std::string& symbol, int d) {
    const double phase = symbol == kA ? 0.0 : 1.7;
    return 100.0 * (1.0 + 0.02 * std::sin(0.9 * d + phase) + 0.001 * d);
}

double volume_of(const std::string& symbol, int d) {
    if (symbol == kA && d == kThinDay) return 5000.0;
    const double base = symbol == kA ? 300000.0 : 60000.0;
    return base * (1.0 + 0.5 * std::sin(1.3 * d));
}

// XB has no bar on a Monday (d % 5 == 0, d > 0): a feed hole, as the ags have no Sunday session.
bool has_bar(const std::string& symbol, int d) { return !(symbol == kB && d > 0 && d % 5 == 0); }

std::vector<Row> rows(int last_day, bool junk) {
    std::vector<Row> out;
    for (int d = 0; d <= last_day; ++d) {
        for (const auto& s : {kA, kB}) {
            if (!has_bar(s, d)) continue;
            out.push_back({s, d, close_of(s, d), volume_of(s, d), junk && s == kA && d == kJunkDay});
        }
    }
    return out;
}

std::vector<Bar> bars_through(const std::string& symbol, int last_day, int skip_day = -1) {
    std::vector<Bar> out;
    for (int d = 0; d <= last_day; ++d) {
        if (!has_bar(symbol, d) || d == skip_day) continue;
        Bar b;
        b.symbol = symbol;
        b.timestamp = trading_day(d);
        b.open = Decimal(close_of(symbol, d));
        b.high = Decimal(close_of(symbol, d) * 1.01);
        b.low = Decimal(close_of(symbol, d) * 0.99);
        b.close = Decimal(close_of(symbol, d));
        b.volume = volume_of(symbol, d);
        out.push_back(b);
    }
    return out;
}

// What a live run whose T-1 bar is `signal_day` feeds its cost model (futures_cost_feed.hpp), on the
// same bars (`skip_day`: a JUNK T-1 bar the strategy feed withholds).
TransactionCostManager live_feed(const std::string& symbol, int signal_day, int skip_day = -1) {
    TransactionCostManager tcm;
    feed_futures_cost_model(tcm, bars_through(symbol, signal_day, skip_day));
    return tcm;
}

}  // namespace

class FuturesCostFeedBacktest : public TestBase {
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

    void run(AssetClass asset_class, int last_day, bool junk,
             const std::map<std::string, std::map<int, double>>& targets) {
        static int n = 0;
        ++n;
        db_->rows = rows(last_day, junk);

        BacktestCoordinatorConfig cc;
        cc.initial_capital = 1'000'000.0;
        cc.store_results = false;
        cc.store_trade_details = false;
        cc.portfolio_id = "COSTH3_TEST";
        cc.csv_output_path = temp_csv_dir("costh3_" + std::to_string(n));
        coord_ = std::make_unique<BacktestCoordinator>(db_, &InstrumentRegistry::instance(), cc);
        ASSERT_TRUE(coord_->initialize().is_ok());

        PortfolioConfig pc{1'000'000.0, 1.0, 0.0, /*optimization=*/false};
        pc.opt_config.capital = 1'000'000.0;
        pc.risk_config.capital = 1'000'000.0;
        pc.risk_modules = {test_none_module()};
        pm_ = std::make_shared<PortfolioManager>(pc, "PM_COSTH3_" + std::to_string(n));

        StrategyConfig sc;
        sc.capital_allocation = 1'000'000.0;
        sc.max_leverage = 10.0;
        sc.asset_classes = {asset_class};
        sc.frequencies = {DataFrequency::DAILY};
        for (const auto& s : {kA, kB}) {
            sc.trading_params[s] = 1.0;
            sc.position_limits[s] = 1.0e6;
        }
        strat_ = std::make_shared<ScheduledStrategy>("COSTH3_S", sc, db_);
        strat_->targets = targets;
        ASSERT_TRUE(strat_->initialize().is_ok());
        ASSERT_TRUE(strat_->start().is_ok());
        ASSERT_TRUE(pm_->add_strategy(strat_, 1.0, false).is_ok());

        auto result = coord_->run_portfolio(pm_, {kA, kB}, trading_day(0), trading_day(last_day),
                                            asset_class, DataFrequency::DAILY);
        ASSERT_TRUE(result.is_ok()) << result.error()->what();
        results_ = result.value();
    }

    TransactionCostManager& execution_costs() {
        return coord_->get_execution_manager()->get_transaction_cost_manager();
    }

    // Both managers hold exactly what live's feed of `expected` holds, for `symbol`.
    void expect_managers_equal(const std::string& symbol, const TransactionCostManager& expected,
                               const std::string& what) {
        EXPECT_DOUBLE_EQ(execution_costs().get_adv(symbol), expected.get_adv(symbol))
            << what << ": the execution manager's impact ADV";
        EXPECT_DOUBLE_EQ(pm_->get_transaction_cost_manager().get_adv(symbol),
                         expected.get_adv(symbol))
            << what << ": the PortfolioManager's impact ADV";
        EXPECT_DOUBLE_EQ(execution_costs().get_volatility_multiplier(symbol),
                         expected.get_volatility_multiplier(symbol))
            << what << ": the execution manager's volatility multiplier";
        EXPECT_DOUBLE_EQ(pm_->get_transaction_cost_manager().get_volatility_multiplier(symbol),
                         expected.get_volatility_multiplier(symbol))
            << what << ": the PortfolioManager's volatility multiplier";
    }

    double stored_cost(const std::string& symbol, int day) {
        const Timestamp ts = trading_day(day);
        std::vector<double> coordinator, portfolio;
        for (const auto& e : results_.executions) {
            if (e.symbol == symbol && e.fill_time == ts) {
                coordinator.push_back(static_cast<double>(e.total_transaction_costs));
            }
        }
        for (const auto& [_, execs] : pm_->get_strategy_executions()) {
            for (const auto& e : execs) {
                if (e.symbol == symbol && e.fill_time == ts) {
                    portfolio.push_back(static_cast<double>(e.total_transaction_costs));
                }
            }
        }
        EXPECT_EQ(coordinator.size(), 1u) << symbol << " fill on day " << day;
        EXPECT_EQ(portfolio.size(), 1u) << symbol << " fill on day " << day;
        if (coordinator.size() != 1u || portfolio.size() != 1u) return -1.0;
        EXPECT_NEAR(coordinator[0], portfolio[0], 1e-9)
            << "the two cost managers priced the same fill differently";
        return portfolio[0];
    }

    std::shared_ptr<ServingDb> db_;
    std::unique_ptr<BacktestCoordinator> coord_;
    std::shared_ptr<PortfolioManager> pm_;
    std::shared_ptr<ScheduledStrategy> strat_;
    BacktestResults results_;
};

// After the last cycle (stamped day 40, its signal group day 39) both managers hold what a live run
// with T-1 = day 39 feeds: day 39's own volume as the impact ADV, and the volatility term from the
// walk of returns ending at day 39, XB's returns across its Monday holes included. The parent held
// the 20-bar mean ending at day 40 and a window without XB's gap returns.
TEST_F(FuturesCostFeedBacktest, TheManagersHoldLivesFeedEndingAtTheLastSignalBar) {
    run(AssetClass::FUTURES, 40, /*junk=*/false, {});

    for (const auto& s : {kA, kB}) {
        const auto live = live_feed(s, 39);
        EXPECT_DOUBLE_EQ(live.get_adv(s), volume_of(s, 39)) << "control: live's ADV is the own day";
        expect_managers_equal(s, live, s);
    }
}

// A fill is priced on its signal bar's own volume and the returns ending at it: the fill stamped
// day 31 is priced at the day-30 close on day 30's 5,000 lots (a thin session), never on day 31's
// bar; the fill stamped day 30 on day 29's volume.
TEST_F(FuturesCostFeedBacktest, AFillIsPricedOnItsSignalBarsOwnVolumeAndReturns) {
    run(AssetClass::FUTURES, 40, /*junk=*/false, {{kA, {{29, 1.0}, {30, 2.0}}}});

    const double before_thin = live_feed(kA, 29).calculate_costs(kA, 1.0, close_of(kA, 29))
                                   .total_transaction_costs;
    EXPECT_NEAR(stored_cost(kA, 30), before_thin, kStoredTol);

    const auto on_thin = live_feed(kA, kThinDay);
    const double thin = on_thin.calculate_costs(kA, 1.0, close_of(kA, kThinDay)).total_transaction_costs;
    TransactionCostManager parent_basis;  // the parent's: the 20 bars ending at day 31, day 31 included
    for (int d = 1; d <= 31; ++d) {
        parent_basis.update_market_data(kA, volume_of(kA, d), close_of(kA, d), close_of(kA, d - 1));
    }
    ASSERT_GT(std::abs(thin - parent_basis.calculate_costs(kA, 1.0, close_of(kA, kThinDay))
                                  .total_transaction_costs),
              1e-3)
        << "the scenario must tell the two bases apart";
    EXPECT_NEAR(stored_cost(kA, 31), thin, kStoredTol)
        << "the day-31 fill must be priced on day 30's own 5,000 lots";
}

// A JUNK signal bar is withheld from the cost feed on its cycle, as live's strategy feed withholds
// it: after the cycle stamped day 36 (XA's day-35 bar locked) the managers still hold XA's day-34
// own volume and the walk ending at day 34.
TEST_F(FuturesCostFeedBacktest, AJunkSignalBarIsWithheldFromTheCostFeedOnItsCycle) {
    run(AssetClass::FUTURES, 36, /*junk=*/true, {});

    expect_managers_equal(kA, live_feed(kA, kJunkDay, /*skip_day=*/kJunkDay), "XA (JUNK day 35)");
    EXPECT_DOUBLE_EQ(execution_costs().get_adv(kA), volume_of(kA, 34));
    expect_managers_equal(kB, live_feed(kB, kJunkDay), "XB");
}

// Control (passes on the parent too): the equity backtest keeps its own feed, the cycle's group
// fed before the fill (a 20-bar mean ending at the last group).
TEST_F(FuturesCostFeedBacktest, TheEquityBacktestKeepsItsFeed) {
    run(AssetClass::EQUITIES, 40, /*junk=*/false, {});

    // The closes as the backtest holds them (a Bar's close is a Decimal).
    auto held = [](int d) { return static_cast<double>(Decimal(close_of(kA, d))); };
    TransactionCostManager day_t;
    for (int d = 1; d <= 40; ++d) {
        day_t.update_market_data(kA, volume_of(kA, d), held(d), held(d - 1));
    }
    EXPECT_DOUBLE_EQ(execution_costs().get_adv(kA), day_t.get_adv(kA));
    EXPECT_DOUBLE_EQ(pm_->get_transaction_cost_manager().get_adv(kA), day_t.get_adv(kA));
    EXPECT_DOUBLE_EQ(execution_costs().get_volatility_multiplier(kA),
                     day_t.get_volatility_multiplier(kA));
}
