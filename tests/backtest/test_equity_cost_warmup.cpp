// tests/backtest/test_equity_cost_warmup.cpp
//
// T-7a commit 6 (ledger H-13; HD 2026-09-21, the root fix). The equity backtest's cost warm-up.
//
// bt_equity_mr registered each symbol's liquidity-tiered cost config from the 30 days AFTER
// start_date (a look-ahead) and loaded that window with the MarketDataBus ON, so every warm-up row
// was a BAR event and the PortfolioManager ran a full process_market_data per row: the
// mean-reversion sleeve entered the run holding ~21 future closes and the PM's own date-keyed
// history held them on the warm-up query's adjustment basis (the 189 PM_HISTORY_REPEATED_DATE
// WARNs on bteq0806). Now the window is the 30 calendar days before start_date, ending strictly
// before it, and it is loaded with publishing disabled, so the bars reach the two cost
// registrations and nothing else.
//
// The loader below does what PostgresDatabase::get_market_data does: it publishes every row it
// returns as a BAR event (postgres_database.cpp, "Publish market data events"), then returns the
// table. The runner is a main(), so its call site is checked in its source.

#include <gtest/gtest.h>

#include <arrow/api.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include "../risk/risk_module_test_helpers.hpp"

#define private public
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#undef private

#include "trade_ngin/backtest/equity_cost_warmup.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/data/market_data_bus.hpp"
#include "trade_ngin/strategy/mean_reversion.hpp"
#include "trade_ngin/transaction_cost/transaction_cost_manager.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

namespace {

constexpr auto npos = std::string::npos;

// bteq0806's start_date: 2024-08-06 00:00 America/New_York (EDT) = 2024-08-06 04:00:00Z.
const Timestamp kStart = Timestamp(std::chrono::seconds(1722916800LL));
// 2024-07-08 00:00:00Z, the first bar the 30 days before kStart hold.
constexpr int64_t kFirstWarmupBar = 1720396800LL;

struct DayBar {
    std::string symbol;
    int64_t epoch;
    double close;
    double volume;
};

// 21 daily bars per symbol at 00:00Z from 2024-07-08, ending 2024-07-28 (inside the window).
std::vector<DayBar> warmup_rows(const std::vector<std::pair<std::string, double>>& symbol_volume) {
    std::vector<DayBar> rows;
    for (int d = 0; d < 21; ++d) {
        for (const auto& [symbol, volume] : symbol_volume) {
            rows.push_back({symbol, kFirstWarmupBar + 86400LL * d, 100.0 + d, volume});
        }
    }
    return rows;
}

std::shared_ptr<arrow::Table> to_table(const std::vector<DayBar>& rows) {
    auto* pool = arrow::default_memory_pool();
    auto schema = arrow::schema(
        {arrow::field("time", arrow::timestamp(arrow::TimeUnit::SECOND)),
         arrow::field("symbol", arrow::utf8()), arrow::field("open", arrow::float64()),
         arrow::field("high", arrow::float64()), arrow::field("low", arrow::float64()),
         arrow::field("close", arrow::float64()), arrow::field("volume", arrow::float64())});
    arrow::TimestampBuilder t(arrow::timestamp(arrow::TimeUnit::SECOND), pool);
    arrow::StringBuilder s(pool);
    arrow::DoubleBuilder o(pool), h(pool), l(pool), c(pool), v(pool);
    for (const auto& r : rows) {
        ARROW_CHECK_OK(t.Append(r.epoch));
        ARROW_CHECK_OK(s.Append(r.symbol));
        ARROW_CHECK_OK(o.Append(r.close));
        ARROW_CHECK_OK(h.Append(r.close + 1.0));
        ARROW_CHECK_OK(l.Append(r.close - 1.0));
        ARROW_CHECK_OK(c.Append(r.close));
        ARROW_CHECK_OK(v.Append(r.volume));
    }
    std::shared_ptr<arrow::Array> ta, sa, oa, ha, la, ca, va;
    ARROW_CHECK_OK(t.Finish(&ta));
    ARROW_CHECK_OK(s.Finish(&sa));
    ARROW_CHECK_OK(o.Finish(&oa));
    ARROW_CHECK_OK(h.Finish(&ha));
    ARROW_CHECK_OK(l.Finish(&la));
    ARROW_CHECK_OK(c.Finish(&ca));
    ARROW_CHECK_OK(v.Finish(&va));
    return arrow::Table::Make(schema, {ta, sa, oa, ha, la, ca, va});
}

MarketDataEvent bar_event(const DayBar& r) {
    MarketDataEvent e;
    e.type = MarketDataEventType::BAR;
    e.symbol = r.symbol;
    e.timestamp = Timestamp(std::chrono::seconds(r.epoch));
    e.numeric_fields["open"] = r.close;
    e.numeric_fields["high"] = r.close + 1.0;
    e.numeric_fields["low"] = r.close - 1.0;
    e.numeric_fields["close"] = r.close;
    e.numeric_fields["volume"] = r.volume;
    return e;
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

size_t count_of(const std::string& text, const std::string& needle) {
    size_t n = 0;
    for (auto p = text.find(needle); p != npos; p = text.find(needle, p + needle.size())) ++n;
    return n;
}

}  // namespace

class EquityCostWarmupTest : public TestBase {
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
        db_ = std::make_shared<MockPostgresDatabase>("mock://testdb");
        ASSERT_TRUE(db_->connect().is_ok());

        PortfolioConfig pc{1'000'000.0, 1.0, 0.0, /*optimization=*/false};
        pc.opt_config.capital = 1'000'000.0;
        pc.risk_config.capital = 1'000'000.0;
        pc.risk_modules = {test_none_module()};
        pc.allow_fractional_positions = true;
        pm_ = std::make_shared<PortfolioManager>(pc, "PM_EQ_WARMUP");

        StrategyConfig sc;
        sc.capital_allocation = 1'000'000.0;
        sc.max_leverage = 1.0;
        sc.asset_classes = {AssetClass::EQUITIES};
        sc.frequencies = {DataFrequency::DAILY};
        for (const auto& sym : symbols_) {
            sc.trading_params[sym] = 1.0;
            sc.position_limits[sym] = 1000.0;
        }
        MeanReversionConfig mr;
        mr.lookback_period = 20;
        mr.vol_lookback = 20;
        mr_ = std::make_shared<MeanReversionStrategy>("MEAN_REVERSION", sc, mr, db_);
        ASSERT_TRUE(mr_->initialize().is_ok());
        ASSERT_TRUE(mr_->start().is_ok());
        ASSERT_TRUE(pm_->add_strategy(mr_, 1.0, false).is_ok());
    }

    void TearDown() override {
        MarketDataBus::instance().set_publish_enabled(true);
        // The PM's constructor subscribed its `this`; nothing unsubscribes it on destruction.
        (void)MarketDataBus::instance().unsubscribe("PORTFOLIO_MANAGER");
        mr_.reset();
        pm_.reset();
        db_.reset();
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        TestBase::TearDown();
    }

    size_t sleeve_prices(const std::string& symbol) const {
        auto h = mr_->get_price_history();
        auto it = h.find(symbol);
        return it == h.end() ? 0 : it->second.size();
    }

    // AAA ADV 3,000,000 shares (LARGE: 2 ticks), BBB 600,000 (MID: 3 ticks).
    const std::vector<std::string> symbols_{"AAA", "BBB"};
    const std::vector<std::pair<std::string, double>> volumes_{{"AAA", 3'000'000.0},
                                                               {"BBB", 600'000.0}};
    std::shared_ptr<MockPostgresDatabase> db_;
    std::shared_ptr<PortfolioManager> pm_;
    std::shared_ptr<MeanReversionStrategy> mr_;
};

// The window: the 30 calendar days (24 h each, as the window has always been counted) BEFORE
// start_date, ending strictly before it. The parent read [start, start + 30 d].
TEST(EquityCostWarmupWindow, IsTheThirtyCalendarDaysEndingStrictlyBeforeStartDate) {
    const auto w = backtest::equity_cost_warmup_window(kStart);
    EXPECT_LT(w.end, kStart) << "the warm-up window reaches start_date or beyond (look-ahead)";
    EXPECT_EQ(w.end, kStart - std::chrono::seconds(1))
        << "the window should end one second before start_date, the last instant before the run's "
           "own [start, end] load";
    EXPECT_EQ(w.end - w.start, std::chrono::hours(24 * 30) - std::chrono::seconds(1))
        << "the window should span 30 calendar days";
    EXPECT_EQ(w.start, kStart - std::chrono::hours(24 * 30));
    EXPECT_EQ(backtest::kEquityCostWarmupCalendarDays, 30);
}

// The loader is asked for exactly that window; the bars it publishes on the bus (as
// get_market_data does) reach neither the sleeve nor the PM's own history, and both cost managers
// are registered from them. With the bus on (the parent) the sleeve and the PM held 21 closes per
// symbol before the run.
TEST_F(EquityCostWarmupTest, TheWarmupBarsReachOnlyTheTwoCostRegistrations) {
    const auto rows = warmup_rows(volumes_);
    std::vector<std::pair<Timestamp, Timestamp>> calls;
    auto loader = [&](const Timestamp& from,
                      const Timestamp& to) -> Result<std::shared_ptr<arrow::Table>> {
        calls.emplace_back(from, to);
        for (const auto& r : rows) MarketDataBus::instance().publish(bar_event(r));
        return Result<std::shared_ptr<arrow::Table>>(to_table(rows));
    };

    transaction_cost::TransactionCostManager execution_costs;
    const auto summary = backtest::register_equity_cost_warmup(symbols_, kStart, loader,
                                                               execution_costs, *pm_);

    ASSERT_EQ(calls.size(), 1u);
    EXPECT_LT(calls[0].second, kStart) << "the loader was asked for bars at or after start_date";
    EXPECT_EQ(calls[0].first, kStart - std::chrono::hours(24 * 30));
    EXPECT_EQ(calls[0].second, kStart - std::chrono::seconds(1));

    // Nothing reached the strategies or the PM's own price history.
    EXPECT_EQ(sleeve_prices("AAA"), 0u)
        << "the mean-reversion sleeve received the warm-up bars through the bus";
    EXPECT_EQ(sleeve_prices("BBB"), 0u)
        << "the mean-reversion sleeve received the warm-up bars through the bus";
    EXPECT_TRUE(pm_->closes_by_date_.empty())
        << "the PortfolioManager recorded warm-up closes in its own history ("
        << pm_->closes_by_date_.size() << " symbols)";
    EXPECT_TRUE(pm_->historical_returns_.empty());
    EXPECT_TRUE(MarketDataBus::instance().is_publish_enabled())
        << "bus publishing was not re-enabled after the load";

    // Both cost managers were registered from the same bars.
    EXPECT_TRUE(summary.loaded);
    EXPECT_EQ(summary.bars, rows.size());
    EXPECT_EQ(summary.registered_execution_costs, 2);
    EXPECT_EQ(summary.registered_portfolio_costs, 2);
    EXPECT_DOUBLE_EQ(execution_costs.get_asset_config("AAA").baseline_spread_ticks, 2.0);
    EXPECT_DOUBLE_EQ(execution_costs.get_asset_config("BBB").baseline_spread_ticks, 3.0);
    EXPECT_DOUBLE_EQ(pm_->cost_manager_.get_asset_config("AAA").baseline_spread_ticks, 2.0);
    EXPECT_DOUBLE_EQ(pm_->cost_manager_.get_asset_config("BBB").baseline_spread_ticks, 3.0);
    EXPECT_DOUBLE_EQ(pm_->cost_manager_.get_asset_config("BBB").max_impact_bps, 100.0);

    // Control: the sleeve and the PM ARE on the bus, so the zeros above are the guard's doing.
    MarketDataBus::instance().publish(bar_event({"AAA", kFirstWarmupBar + 86400LL * 40, 123.0, 1.0}));
    EXPECT_EQ(sleeve_prices("AAA"), 1u) << "control: a bar published with the bus on reaches the sleeve";
    EXPECT_EQ(pm_->closes_by_date_.count("AAA"), 1u) << "control: and the PM's own history";
}

// The bus comes back on whatever the load does: an error result (WARN, nothing registered, the
// symbols fall to the untiered default at their first cost, as before) or an exception.
TEST_F(EquityCostWarmupTest, TheBusIsReEnabledWhenTheLoadFailsOrThrows) {
    transaction_cost::TransactionCostManager execution_costs;
    auto failing = [](const Timestamp&, const Timestamp&) -> Result<std::shared_ptr<arrow::Table>> {
        return make_error<std::shared_ptr<arrow::Table>>(ErrorCode::DATABASE_ERROR, "no rows",
                                                         "test");
    };
    const auto summary =
        backtest::register_equity_cost_warmup(symbols_, kStart, failing, execution_costs, *pm_);
    EXPECT_FALSE(summary.loaded);
    EXPECT_EQ(summary.registered_execution_costs, 0);
    EXPECT_EQ(summary.registered_portfolio_costs, 0);
    EXPECT_TRUE(MarketDataBus::instance().is_publish_enabled());

    auto throwing = [](const Timestamp&, const Timestamp&) -> Result<std::shared_ptr<arrow::Table>> {
        throw std::runtime_error("connection lost");
    };
    EXPECT_THROW(
        backtest::register_equity_cost_warmup(symbols_, kStart, throwing, execution_costs, *pm_),
        std::runtime_error);
    EXPECT_TRUE(MarketDataBus::instance().is_publish_enabled())
        << "an exception in the load left bus publishing disabled";
}

// bt_equity_mr registers its cost configs only through the guarded seam, before the run, and
// reads no other bars ahead of run_portfolio. The parent loaded [start_date, start_date + 30 d]
// directly, with the bus on, and called both registrations itself.
TEST(BtEquityMrCostWarmupSource, TheRunnerRegistersCostsOnlyThroughTheGuardedWindowBeforeStart) {
    const std::string src = read_source("apps/backtest/bt_equity_mean_reversion.cpp");
    if (src.empty()) GTEST_SKIP() << "runner source not found";

    const auto seam = src.find("backtest::register_equity_cost_warmup(");
    const auto run = src.find("coordinator->run_portfolio(");
    ASSERT_NE(run, npos);
    ASSERT_NE(seam, npos) << "the runner does not load its cost warm-up through the guarded seam";
    EXPECT_LT(seam, run) << "the cost warm-up must be registered before the run";

    EXPECT_EQ(src.find("start_date + std::chrono::hours(24 * 30)"), npos)
        << "the runner still builds the window after start_date (H-13 look-ahead)";
    EXPECT_EQ(count_of(src, "register_equity_costs_from_bars("), 0u)
        << "the runner registers costs outside the seam";
    EXPECT_EQ(count_of(src, "register_equity_cost_configs("), 0u)
        << "the runner registers costs outside the seam";
    // The one bar read in the runner is the loader handed to the seam.
    ASSERT_EQ(count_of(src, "db->get_market_data("), 1u);
    const auto load = src.find("db->get_market_data(");
    EXPECT_GT(load, seam);
    EXPECT_LT(load, run);
}
