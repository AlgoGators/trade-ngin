// tests/backtest/test_backtest_equity_cost_retier.cpp
//
// T-7b-2 8c, K1 (T-4b BT-cost-tier-warmup; STAGE3_PLAN §25a item 3): the equity backtest prices
// every fill with the liquidity tier of the 20 bars ending at the bar the fill is priced at (the
// signal bar T-1, as live re-tiers on every run), in the window-end share unit (raw volume times
// the split factors of every later ex-date, the unit of the back-adjusted price and quantity), on
// both cost managers; the impact model's ADV is fed in the same unit.
//
// The parent registered each symbol's tier once, before the run, from the 20 bars before
// start_date in raw shares (equity_cost_warmup.hpp), and every fill of the window was priced with
// it. Each test runs a whole backtest (run_portfolio) on bars the test controls and checks the
// stored cost of a fill against the cost the stated inputs give.

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../core/test_base.hpp"
#include "../risk/risk_module_test_helpers.hpp"
#include "cost_basis_test_helpers.hpp"
#include "trade_ngin/backtest/backtest_coordinator.hpp"
#include "trade_ngin/backtest/equity_cost_warmup.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/data/market_data_bus.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/transaction_cost/asset_cost_config.hpp"
#include "trade_ngin/transaction_cost/transaction_cost_manager.hpp"

using namespace trade_ngin;
using namespace trade_ngin::backtest;
using namespace trade_ngin::testing;
using namespace trade_ngin::testing::cost_basis;

namespace {

using transaction_cost::AssetCostConfigRegistry;
using transaction_cost::TransactionCostManager;

// A stored cost is a Decimal the execution report carries to 8 decimals.
constexpr double kStoredTol = 1e-7;

constexpr int kLastDay = 70;
constexpr int kSplitDay = 40;       // SS goes ex a 4-for-1 split on trading day 40
constexpr double kSplitFactor = 4.0;

double close_of(const std::string& symbol, int d) {
    const double base = symbol == "SS" ? 50.0 : 100.0;
    return base * (1.0 + 0.01 * std::sin(0.7 * d + (symbol == "VV" ? 1.0 : 0.0)));
}

// LA: 150,000 shares a day (SMALL) except one 1e9 day, trading day 25.
// VV: 150,000 (SMALL) through day 29, 3,000,000 (LARGE) from day 30.
// SS: raw 400,000 before its day-40 split and 1,600,000 from it: 1,600,000 (MID) throughout in
//     post-split shares, 400,000 (SMALL) in raw pre-split shares.
// Every symbol has the 20 weekdays before start_date (days -20..-1), which the 30 calendar days
// before start_date hold.
std::vector<Row> rows() {
    std::vector<Row> out;
    for (int d = -20; d <= kLastDay; ++d) {
        out.push_back({"LA", d, close_of("LA", d), d == 25 ? 1.0e9 : 150000.0});
        out.push_back({"VV", d, close_of("VV", d), d < 30 ? 150000.0 : 3000000.0});
        out.push_back({"SS", d, close_of("SS", d), d < kSplitDay ? 400000.0 : 1600000.0});
    }
    return out;
}

double split_multiplier(const std::string& symbol, int d) {
    return (symbol == "SS" && d < kSplitDay) ? kSplitFactor : 1.0;
}

// The cost of a fill stamped `fill_day` of `qty` shares at the day-(fill_day - 1) close, priced as
// the coordinator prices an equity fill: the impact ADV and the volatility term from the bars fed
// before the fill (every in-window bar from day 1 to fill_day: the first group is never fed), and
// the tier either from the 20 bars ending at fill_day - 1 (`per_bar_tier`) or from the 20 bars
// before start_date (the parent's one registration). `split_unit` puts every volume (tier and
// impact) in the window-end share unit; otherwise raw.
double priced(const std::string& symbol, int fill_day, double qty, bool per_bar_tier,
              bool split_unit) {
    const auto all = rows();
    auto volume = [&](const Row& r) {
        return r.volume * (split_unit ? split_multiplier(r.symbol, r.day) : 1.0);
    };
    std::vector<Bar> tier_window;
    for (const auto& r : all) {
        if (r.symbol != symbol) continue;
        const bool in_tier = per_bar_tier ? (r.day <= fill_day - 1) : (r.day < 0);
        if (!in_tier) continue;
        Bar b;
        b.symbol = r.symbol;
        b.timestamp = trading_day(r.day);
        b.close = Decimal(r.close);
        b.volume = volume(r);
        tier_window.push_back(b);
    }
    TransactionCostManager tcm;
    tcm.register_equity_costs_from_bars({symbol}, {{symbol, tier_window}}, 20);
    for (int d = 1; d <= fill_day; ++d) {
        for (const auto& r : all) {
            if (r.symbol == symbol && r.day == d) {
                tcm.update_market_data(symbol, volume(r), r.close, close_of(symbol, d - 1));
            }
        }
    }
    return tcm.calculate_costs(symbol, qty, close_of(symbol, fill_day - 1)).total_transaction_costs;
}

std::string tier_ticks(const transaction_cost::AssetCostConfig& c) {
    return std::to_string(c.baseline_spread_ticks) + "/" + std::to_string(c.max_total_implicit_bps);
}

std::string tier_ticks_of_adv(double adv) {
    return tier_ticks(AssetCostConfigRegistry::get_tiered_equity_config(100.0, adv));
}

}  // namespace

class EquityCostRetierBacktest : public TestBase {
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

    // Builds the book and runs the whole window as bt_equity_mr does: the one warm-up
    // registration before the run (register_equity_cost_warmup), then run_portfolio.
    void run(AssetClass asset_class, const std::vector<std::string>& symbols,
             const std::map<std::string, std::map<int, double>>& targets) {
        static int n = 0;
        ++n;
        db_->rows = rows();
        db_->splits = {{"SS", kSplitDay, kSplitFactor}};

        BacktestCoordinatorConfig cc;
        cc.initial_capital = 1'000'000.0;
        cc.store_results = false;
        cc.store_trade_details = false;
        cc.portfolio_id = "K1_TEST";
        cc.csv_output_path = temp_csv_dir("k1_" + std::to_string(n));
        coord_ = std::make_unique<BacktestCoordinator>(db_, &InstrumentRegistry::instance(), cc);
        ASSERT_TRUE(coord_->initialize().is_ok());

        PortfolioConfig pc{1'000'000.0, 1.0, 0.0, /*optimization=*/false};
        pc.opt_config.capital = 1'000'000.0;
        pc.risk_config.capital = 1'000'000.0;
        pc.risk_modules = {test_none_module()};
        pm_ = std::make_shared<PortfolioManager>(pc, "PM_K1_" + std::to_string(n));

        StrategyConfig sc;
        sc.capital_allocation = 1'000'000.0;
        sc.max_leverage = 10.0;
        sc.asset_classes = {asset_class};
        sc.frequencies = {DataFrequency::DAILY};
        for (const auto& s : symbols) {
            sc.trading_params[s] = 1.0;
            sc.position_limits[s] = 1.0e6;
        }
        strat_ = std::make_shared<ScheduledStrategy>("K1_S", sc, db_);
        strat_->targets = targets;
        ASSERT_TRUE(strat_->initialize().is_ok());
        ASSERT_TRUE(strat_->start().is_ok());
        ASSERT_TRUE(pm_->add_strategy(strat_, 1.0, false).is_ok());

        if (asset_class == AssetClass::EQUITIES) {
            register_equity_cost_warmup(
                symbols, trading_day(0),
                [&](const Timestamp& from, const Timestamp& to) {
                    return db_->get_market_data(symbols, from, to, AssetClass::EQUITIES,
                                                DataFrequency::DAILY, "ohlcv");
                },
                coord_->get_execution_manager()->get_transaction_cost_manager(), *pm_);
        }
        db_->market_calls.clear();
        db_->corp_action_calls.clear();

        auto result = coord_->run_portfolio(pm_, symbols, trading_day(0), trading_day(kLastDay),
                                            asset_class, DataFrequency::DAILY);
        ASSERT_TRUE(result.is_ok()) << result.error()->what();
        results_ = result.value();
    }

    // The stored cost of the one fill of `symbol` stamped `day`: the coordinator's copy (the
    // results' executions) and the PortfolioManager's (backtest.executions, the equity curve).
    // Both must exist and agree.
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

// A fill is priced with the tier of the 20 bars ending at its own signal bar. VV's volume moves
// from 150,000 (SMALL) to 3,000,000 (LARGE) at day 30: its day-60 fill is LARGE; the parent priced
// it SMALL, the tier of the bars before start_date.
TEST_F(EquityCostRetierBacktest, AFillIsPricedWithTheTierOfTheTwentyBarsEndingAtItsSignalBar) {
    run(AssetClass::EQUITIES, {"VV"}, {{"VV", {{9, 10.0}, {59, 20.0}}}});

    const double early = priced("VV", 10, 10.0, /*per_bar_tier=*/true, /*split_unit=*/true);
    EXPECT_NEAR(early, priced("VV", 10, 10.0, false, false), 1e-12)
        << "control: before the volume moves both tiers are SMALL";
    EXPECT_NEAR(stored_cost("VV", 10), early, kStoredTol);

    const double late = priced("VV", 60, 10.0, true, true);
    const double frozen = priced("VV", 60, 10.0, false, false);
    ASSERT_GT(std::abs(late - frozen), 1e-6) << "the scenario must tell the two tiers apart";
    EXPECT_NEAR(stored_cost("VV", 60), late, kStoredTol)
        << "the day-60 fill should be priced LARGE (the 20 bars ending day 59, 3,000,000 a day), "
           "not SMALL (the tier registered from the bars before start_date: "
        << frozen << ")";
}

// The tier's window ends at the signal bar and never includes the fill's own day: LA's 1e9 bar is
// day 25, so the fill stamped day 25 (priced at the day-24 close) is SMALL and the fill stamped
// day 26 (its signal bar is day 25) is MEGA.
TEST_F(EquityCostRetierBacktest, TheTierWindowEndsAtTheSignalBarNeverAtTheFillDay) {
    run(AssetClass::EQUITIES, {"LA"}, {{"LA", {{24, 10.0}, {25, 20.0}}}});

    const double on_spike_day = priced("LA", 25, 10.0, true, true);
    EXPECT_NEAR(stored_cost("LA", 25), on_spike_day, kStoredTol)
        << "the day-25 fill must not see the day-25 volume in its tier";

    const double after = priced("LA", 26, 10.0, true, true);
    ASSERT_GT(std::abs(after - priced("LA", 26, 10.0, false, false)), 1e-6);
    EXPECT_NEAR(stored_cost("LA", 26), after, kStoredTol)
        << "the day-26 fill's tier window (days 6..25) holds the 1e9 bar: MEGA";
}

// SS splits 4-for-1 on day 40. In post-split shares (the unit of the adjusted price and of the
// quantity) it trades 1,600,000 a day throughout: MID. Its raw pre-split volume, 400,000, reads
// SMALL. The day-20 fill (pre-split) is MID; the parent priced it SMALL, and tiering on raw
// volume would too.
TEST_F(EquityCostRetierBacktest, APreSplitFillIsTieredInTheWindowEndShareUnit) {
    run(AssetClass::EQUITIES, {"SS"}, {{"SS", {{19, 10.0}, {59, 20.0}}}});

    const double pre_split = priced("SS", 20, 10.0, true, true);
    ASSERT_GT(std::abs(pre_split - priced("SS", 20, 10.0, true, false)), 1e-6)
        << "the scenario must tell the split unit from raw volume";
    EXPECT_NEAR(stored_cost("SS", 20), pre_split, kStoredTol)
        << "raw per-bar tier would give " << priced("SS", 20, 10.0, true, false)
        << ", the parent's frozen raw tier " << priced("SS", 20, 10.0, false, false);
    EXPECT_NEAR(stored_cost("SS", 60), priced("SS", 60, 10.0, true, true), kStoredTol);
}

// The impact model's ADV is fed in the same unit (T-4b ADVERSARIAL A-3): with the tier held MID,
// the pre-split fill's participation and impact coefficient come from 1,600,000 a day, not
// 400,000.
TEST_F(EquityCostRetierBacktest, TheImpactAdvIsFedInTheSameShareUnitAsTheTier) {
    run(AssetClass::EQUITIES, {"SS"}, {{"SS", {{19, 10.0}}}});

    // Same MID tier on both sides of the comparison; only the impact feed's unit differs.
    auto mid_tier_cost = [](bool split_impact_feed) {
        const auto all = rows();
        std::vector<Bar> tier_window;
        for (const auto& r : all) {
            if (r.symbol != "SS" || r.day > 19) continue;
            Bar b;
            b.symbol = "SS";
            b.timestamp = trading_day(r.day);
            b.close = Decimal(r.close);
            b.volume = r.volume * split_multiplier("SS", r.day);
            tier_window.push_back(b);
        }
        TransactionCostManager tcm;
        tcm.register_equity_costs_from_bars({"SS"}, {{"SS", tier_window}}, 20);
        for (int d = 1; d <= 20; ++d) {
            const double raw = d < kSplitDay ? 400000.0 : 1600000.0;
            tcm.update_market_data("SS", raw * (split_impact_feed ? split_multiplier("SS", d) : 1.0),
                                   close_of("SS", d), close_of("SS", d - 1));
        }
        return tcm.calculate_costs("SS", 10.0, close_of("SS", 19)).total_transaction_costs;
    };
    ASSERT_GT(std::abs(mid_tier_cost(true) - mid_tier_cost(false)), 1e-9);
    EXPECT_NEAR(stored_cost("SS", 20), mid_tier_cost(true), kStoredTol)
        << "the impact term read raw pre-split volume (" << mid_tier_cost(false) << ")";
}

// Both cost managers end the run holding the tier of the last signal bar's 20-bar window.
TEST_F(EquityCostRetierBacktest, BothCostManagersHoldTheLastWindowsTier) {
    run(AssetClass::EQUITIES, {"LA", "VV", "SS"}, {});

    // The last cycle is day 70; its tier window is days 50..69.
    const std::map<std::string, double> last_window_adv{
        {"LA", 150000.0}, {"VV", 3000000.0}, {"SS", 1600000.0}};
    auto& exec_costs = coord_->get_execution_manager()->get_transaction_cost_manager();
    for (const auto& [symbol, adv] : last_window_adv) {
        EXPECT_EQ(tier_ticks(exec_costs.get_asset_config(symbol)), tier_ticks_of_adv(adv))
            << symbol << ": the coordinator's execution manager";
        EXPECT_EQ(tier_ticks(pm_->get_transaction_cost_manager().get_asset_config(symbol)),
                  tier_ticks_of_adv(adv))
            << symbol << ": the PortfolioManager's own cost manager";
    }
}

// The first tier window is the 20 bars before start_date, read from the 30 calendar days before
// it (the warm-up window, strictly before start_date: no look-ahead), with the split events up to
// the run's end date.
TEST_F(EquityCostRetierBacktest, TheFirstWindowIsReadStrictlyBeforeStartWithTheSplitsToTheEnd) {
    run(AssetClass::EQUITIES, {"SS"}, {});

    const auto w = equity_cost_warmup_window(trading_day(0));
    bool seed_read = false;
    for (const auto& c : db_->market_calls) {
        EXPECT_FALSE(c.from < trading_day(0) && c.to >= trading_day(0))
            << "a read before start_date reaches start_date";
        if (c.from == w.start && c.to == w.end && c.asset_class == AssetClass::EQUITIES) {
            seed_read = true;
        }
    }
    EXPECT_TRUE(seed_read) << "the coordinator did not read the 30 days before start_date";
    ASSERT_EQ(db_->corp_action_calls.size(), 1u);
    EXPECT_EQ(db_->corp_action_calls[0].first, core::format_utc_date(w.start));
    EXPECT_EQ(db_->corp_action_calls[0].second, ymd(kLastDay));
}

// Control (passes on the parent too): a futures run re-tiers nothing and reads no split.
TEST_F(EquityCostRetierBacktest, AFuturesRunIsUntouched) {
    run(AssetClass::FUTURES, {"VV"}, {{"VV", {{9, 1.0}}}});

    EXPECT_TRUE(db_->corp_action_calls.empty());
    TransactionCostManager fresh;
    auto& exec_costs = coord_->get_execution_manager()->get_transaction_cost_manager();
    EXPECT_EQ(tier_ticks(exec_costs.get_asset_config("VV")), tier_ticks(fresh.get_asset_config("VV")))
        << "a futures symbol received an equity tier";
    EXPECT_EQ(tier_ticks(pm_->get_transaction_cost_manager().get_asset_config("VV")),
              tier_ticks(fresh.get_asset_config("VV")));
    for (const auto& c : db_->market_calls) {
        EXPECT_EQ(c.asset_class, AssetClass::FUTURES);
    }
}
