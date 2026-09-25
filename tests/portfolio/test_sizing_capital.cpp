// tests/portfolio/test_sizing_capital.cpp
//
// T-7b-2 9c (HD 2026-09-25): compounding. The futures book is sized on the account's current
// equity, not on the constant initial capital (Carver sizes on current trading capital).
//
// Before: both futures runners and the futures backtest set every strategy's capital_allocation
// = initial_capital x allocation once, and calculate_position read that constant every day, while
// the equity the book is measured against moved (T-VOL waterfall stage 9: -1.00 point of ex-ante
// vol on the frozen window, equity 496,066 .. 598,970 against a constant 500,000).
//
// After: before every rebalance the caller hands the PortfolioManager the account's equity at
// the close of the newest bar the sizing reads (portfolio/sizing_capital.hpp), and every sizing
// input moves with it (T-4c's W-B rule): each strategy's capital (x its allocation), which the
// position line, the notional concentration cap and the buffer width's Carver term read; the
// optimizer's weight per contract; the Carver gate's leverage denominator. The equity curve, P&L
// and returns (valuation) do not move.
//
// The cases: (1) the strategy sizes on the capital it was given today, in all three places;
// (2) set_sizing_capital moves every sizing input and refuses a bad capital; (3) the backtest
// sizes each cycle on the PREVIOUS cycle's equity row, never on the row the cycle writes (the
// look-ahead case), and the equity backtest does not compound; (4) the live figure is STEP 4's own
// arithmetic (the row before Day T-1, plus the Day T-1 settlement move PHASE 5 finalises, less Day T-1's
// costs), done before the rebalance, so a replayed date's finalised row is not counted twice; (5) the runners' wiring (both futures runners, byte-identical).

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include "../risk/risk_module_test_helpers.hpp"
#include "trade_ngin/instruments/futures.hpp"
#include "trade_ngin/portfolio/sizing_capital.hpp"
#include "trade_ngin/strategy/base_strategy.hpp"

#define private public
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/live/live_pnl_manager.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/backtest/backtest_coordinator.hpp"
#include "trade_ngin/risk/carver_risk_module.hpp"
#include "trade_ngin/strategy/trend_following.hpp"
#undef private

using namespace trade_ngin;
using namespace trade_ngin::backtest;
using namespace trade_ngin::testing;

namespace {

constexpr auto npos = std::string::npos;

// ------------------------------------------------------------------------------------------------
// (1) the strategy sizes on the capital it was given today
// ------------------------------------------------------------------------------------------------

// One instrument, numbers chosen so each term is easy to follow by hand:
//   position = forecast x capital x weight x idm x risk_target / (10 x size x price x fx x vol)
//            = 10 x 500,000 x 0.05 x 2.5 x 0.2 / (10 x 1 x 250 x 1 x 0.1) = 500 contracts
//   Carver buffer width = 0.1 x capital x idm x risk_target x weight / (size x price x fx x vol)
//            = 0.1 x 500,000 x 2.5 x 0.2 x 0.05 / (1 x 250 x 1 x 0.1) = 50 contracts
const std::string kSym = "SZ.v.0";
constexpr double kCapital = 500'000.0;
constexpr double kWeight = 0.05;
constexpr double kPrice = 250.0;
constexpr double kVol = 0.1;

class StrategySizingCapital : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        db_ = std::make_shared<MockPostgresDatabase>("mock://testdb");
        ASSERT_TRUE(db_->connect().is_ok());
    }
    void TearDown() override {
        trend_.reset();
        db_.reset();
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        TestBase::TearDown();
    }

    void make(double max_leverage, double concentration) {
        static int n = 0;
        StrategyConfig sc;
        sc.capital_allocation = kCapital;
        sc.max_leverage = max_leverage;
        sc.asset_classes = {AssetClass::FUTURES};
        sc.frequencies = {DataFrequency::DAILY};
        TrendFollowingConfig tc;
        tc.weight = kWeight;  // the buffer's weight before and after C9a's C2 is the same number
        tc.idm = 2.5;
        tc.risk_target = 0.2;
        tc.fx_rate = 1.0;
        tc.max_symbol_concentration = concentration;
        tc.use_position_buffering = true;
        tc.carver_buffer_floor = 0.0;             // the Carver term alone sets the width
        tc.carver_buffer_position_factor = 0.0;
        trend_ = std::make_shared<TrendFollowingStrategy>("TF_SIZING_" + std::to_string(++n), sc,
                                                          tc, db_, nullptr);
        trend_->instrument_data_[kSym].contract_size = 1.0;
        trend_->instrument_data_[kSym].weight = kWeight;
    }

    double position_at(double forecast) const {
        return trend_->calculate_position(kSym, forecast, kPrice, kVol);
    }

    std::shared_ptr<MockPostgresDatabase> db_;
    std::shared_ptr<TrendFollowingStrategy> trend_;
};

TEST_F(StrategySizingCapital, ThePositionLineReadsTheCapitalItWasGivenToday) {
    make(/*max_leverage=*/10.0, /*concentration=*/0.15);  // the cap (750,000) does not bind
    EXPECT_NEAR(position_at(10.0), 500.0, 1e-9);

    ASSERT_TRUE(trend_->set_capital_allocation(600'000.0).is_ok());
    EXPECT_DOUBLE_EQ(trend_->get_config().capital_allocation, 600'000.0);
    EXPECT_NEAR(position_at(10.0), 600.0, 1e-9)
        << "10 x 600,000 x 0.05 x 2.5 x 0.2 / 250: the day's capital, not the constant 500,000";

    // A drawdown reverses it: at 450,000 the same forecast holds 10 percent fewer contracts.
    ASSERT_TRUE(trend_->set_capital_allocation(450'000.0).is_ok());
    EXPECT_NEAR(position_at(10.0), 450.0, 1e-9);
    EXPECT_NEAR(position_at(-10.0), -450.0, 1e-9) << "a short scales the same way";
}

TEST_F(StrategySizingCapital, TheNotionalConcentrationCapFollowsTheCapital) {
    // cap = capital x max_leverage x concentration = 500,000 x 1.0 x 0.1 = 50,000 of notional
    // = 200 contracts at 250; the uncapped 500 is cut to it.
    make(/*max_leverage=*/1.0, /*concentration=*/0.1);
    EXPECT_NEAR(position_at(10.0), 200.0, 1e-9);
    ASSERT_TRUE(trend_->set_capital_allocation(600'000.0).is_ok());
    EXPECT_NEAR(position_at(10.0), 240.0, 1e-9)
        << "600,000 x 1.0 x 0.1 = 60,000 of notional = 240 contracts";
}

TEST_F(StrategySizingCapital, TheBufferWidthsCarverTermFollowsTheCapital) {
    make(10.0, 0.15);
    Position held;
    held.symbol = kSym;
    held.quantity = Decimal(55.0);
    held.average_price = Decimal(kPrice);
    ASSERT_TRUE(trend_->seed_positions({{kSym, held}}).is_ok());
    // Target 0, holding 55. At 500,000 the width is 50: 55 is outside [-50, 50], so the buffer
    // trades down to the edge, 50.
    EXPECT_NEAR(trend_->apply_position_buffer(kSym, 0.0, kPrice, kVol), 50.0, 1e-9);
    // At 600,000 the width is 60: 55 is inside [-60, 60], so the buffer keeps 55.
    ASSERT_TRUE(trend_->set_capital_allocation(600'000.0).is_ok());
    EXPECT_NEAR(trend_->apply_position_buffer(kSym, 0.0, kPrice, kVol), 55.0, 1e-9);
}

TEST_F(StrategySizingCapital, ABadCapitalIsRefusedAndTheOldOneKept) {
    make(10.0, 0.15);
    EXPECT_TRUE(trend_->set_capital_allocation(0.0).is_error());
    EXPECT_TRUE(trend_->set_capital_allocation(-1.0).is_error());
    EXPECT_TRUE(trend_->set_capital_allocation(std::numeric_limits<double>::quiet_NaN()).is_error());
    EXPECT_TRUE(trend_->set_capital_allocation(std::numeric_limits<double>::infinity()).is_error());
    EXPECT_DOUBLE_EQ(trend_->get_config().capital_allocation, kCapital);
    EXPECT_NEAR(position_at(10.0), 500.0, 1e-9);
}

// ------------------------------------------------------------------------------------------------
// (2) set_sizing_capital moves every sizing input together
// ------------------------------------------------------------------------------------------------

/// Records the capital it sizes on at each on_data, holds one contract of XX from its first call.
class CapitalProbe : public BaseStrategy {
public:
    CapitalProbe(std::string id, StrategyConfig config, std::shared_ptr<DatabaseInterface> db)
        : BaseStrategy(std::move(id), std::move(config),
                       std::static_pointer_cast<trade_ngin::PostgresDatabase>(db)) {
        metadata_.name = "Capital Probe";
    }
    Result<void> on_data(const std::vector<Bar>& data) override {
        (void)data;
        seen.push_back(config_.capital_allocation);
        return Result<void>();
    }
    std::unordered_map<std::string, Position> get_target_positions() const override {
        if (seen.empty()) return {};
        Position p;
        p.symbol = "XX";
        p.quantity = Decimal(1.0);
        p.average_price = Decimal(100.0);
        return {{"XX", p}};
    }
    std::vector<double> seen;
};

/// A strategy on the bare interface default: it does not accept a sizing capital.
class NoCapitalStrategy : public CapitalProbe {
public:
    using CapitalProbe::CapitalProbe;
    Result<void> set_capital_allocation(double capital) override {
        return StrategyInterface::set_capital_allocation(capital);
    }
};

StrategyConfig probe_config(double capital) {
    StrategyConfig sc;
    sc.capital_allocation = capital;
    sc.max_leverage = 10.0;
    sc.asset_classes = {AssetClass::FUTURES};
    sc.frequencies = {DataFrequency::DAILY};
    return sc;
}

PortfolioConfig carver_config(double capital) {
    PortfolioConfig c{capital, 1.0, 0.0, false};
    c.opt_config.capital = capital;
    c.risk_config.capital = Decimal(capital);
    c.risk_modules = {test_carver_module(c.risk_config)};
    return c;
}

class PortfolioSizingCapital : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        db_ = std::make_shared<MockPostgresDatabase>("mock://testdb");
        ASSERT_TRUE(db_->connect().is_ok());
        static int n = 0;
        pm_ = std::make_shared<PortfolioManager>(carver_config(1'000'000.0),
                                                 "PM_SIZING_" + std::to_string(++n));
        a_ = std::make_shared<CapitalProbe>("SZ_A_" + std::to_string(n), probe_config(600'000.0), db_);
        b_ = std::make_shared<CapitalProbe>("SZ_B_" + std::to_string(n), probe_config(400'000.0), db_);
        ASSERT_TRUE(pm_->add_strategy(a_, 0.6, false).is_ok());
        ASSERT_TRUE(pm_->add_strategy(b_, 0.4, false).is_ok());
    }
    void TearDown() override {
        pm_.reset();
        a_.reset();
        b_.reset();
        db_.reset();
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        TestBase::TearDown();
    }
    const CarverRiskModule& carver() const {
        return dynamic_cast<const CarverRiskModule&>(*pm_->risk_modules_.at(0));
    }

    std::shared_ptr<MockPostgresDatabase> db_;
    std::shared_ptr<PortfolioManager> pm_;
    std::shared_ptr<CapitalProbe> a_, b_;
};

// Control (holds on the parent's numbers too): never called, the book sizes on total_capital.
TEST_F(PortfolioSizingCapital, NeverCalledTheBookSizesOnTheConfiguredCapital) {
    EXPECT_DOUBLE_EQ(pm_->sizing_capital(), 1'000'000.0);
    EXPECT_DOUBLE_EQ(static_cast<double>(carver().manager().get_config().capital), 1'000'000.0);
    EXPECT_DOUBLE_EQ(a_->get_config().capital_allocation, 600'000.0);
}

TEST_F(PortfolioSizingCapital, EverySizingInputMovesWithTheCapital) {
    ASSERT_TRUE(pm_->set_sizing_capital(1'100'000.0).is_ok());
    EXPECT_DOUBLE_EQ(pm_->sizing_capital(), 1'100'000.0);
    EXPECT_DOUBLE_EQ(a_->get_config().capital_allocation, 660'000.0) << "equity x 0.6";
    EXPECT_DOUBLE_EQ(b_->get_config().capital_allocation, 440'000.0) << "equity x 0.4";
    EXPECT_DOUBLE_EQ(static_cast<double>(carver().manager().get_config().capital), 1'100'000.0)
        << "the gate's leverage denominator";
    EXPECT_DOUBLE_EQ(static_cast<double>(pm_->config_.total_capital), 1'000'000.0)
        << "the configured account size is not rewritten";

    // The gate's leverage reading on one book, before and after: gross = notional / capital.
    MarketData md;
    md.ordered_symbols = {"XX"};
    md.symbol_indices = {{"XX", 0}};
    Position p;
    p.symbol = "XX";
    p.quantity = Decimal(22'000.0);
    p.average_price = Decimal(100.0);  // 2,200,000 of notional (unregistered: multiplier 1)
    EXPECT_NEAR(carver().manager().leverage_of({{"XX", p}}, md).gross_leverage, 2.0, 1e-12);
    ASSERT_TRUE(pm_->set_sizing_capital(880'000.0).is_ok());
    EXPECT_NEAR(carver().manager().leverage_of({{"XX", p}}, md).gross_leverage, 2.5, 1e-12)
        << "the same book reads larger against a smaller account";
    EXPECT_DOUBLE_EQ(a_->get_config().capital_allocation, 528'000.0);
}

TEST_F(PortfolioSizingCapital, TheOptimizersWeightPerContractFollowsTheCapital) {
    // optimize_positions divides a contract's notional by the sizing capital (not total_capital).
    const std::string src = [] {
        namespace fs = std::filesystem;
        fs::path dir = fs::current_path();
        for (int i = 0; i < 8 && !dir.empty(); ++i) {
            if (fs::exists(dir / "src/portfolio/portfolio_manager.cpp")) {
                std::ifstream in(dir / "src/portfolio/portfolio_manager.cpp");
                std::ostringstream ss;
                ss << in.rdbuf();
                return ss.str();
            }
            dir = dir.parent_path();
        }
        return std::string();
    }();
    ASSERT_FALSE(src.empty());
    const auto opt = src.find("Result<void> PortfolioManager::optimize_positions()");
    ASSERT_NE(opt, npos);
    const auto end = src.find("Result<void> PortfolioManager::validate_risk_modules(", opt);
    const std::string body = src.substr(opt, end - opt);
    EXPECT_NE(body.find("notional_per_contract /\n                                                   "
                        "static_cast<double>(sizing_capital_)"),
              npos);
    EXPECT_EQ(body.find("config_.total_capital"), npos)
        << "no sizing read of the constant is left in the optimizer";
}

TEST_F(PortfolioSizingCapital, ABadCapitalIsRefusedAndNothingMoves) {
    EXPECT_TRUE(pm_->set_sizing_capital(0.0).is_error());
    EXPECT_TRUE(pm_->set_sizing_capital(-5.0).is_error());
    EXPECT_TRUE(pm_->set_sizing_capital(std::numeric_limits<double>::quiet_NaN()).is_error());
    EXPECT_DOUBLE_EQ(pm_->sizing_capital(), 1'000'000.0);
    EXPECT_DOUBLE_EQ(a_->get_config().capital_allocation, 600'000.0);
    EXPECT_DOUBLE_EQ(static_cast<double>(carver().manager().get_config().capital), 1'000'000.0);
}

TEST_F(PortfolioSizingCapital, AStrategyThatCannotTakeACapitalFailsTheCall) {
    static int n = 0;
    auto pm = std::make_shared<PortfolioManager>(carver_config(1'000'000.0),
                                                 "PM_SIZING_NOCAP_" + std::to_string(++n));
    auto s = std::make_shared<NoCapitalStrategy>("SZ_NOCAP_" + std::to_string(n),
                                                 probe_config(1'000'000.0), db_);
    ASSERT_TRUE(pm->add_strategy(s, 1.0, false).is_ok());
    auto r = pm->set_sizing_capital(900'000.0);
    ASSERT_TRUE(r.is_error()) << "a caller that asked for compounding never sizes on a constant";
    EXPECT_NE(std::string(r.error()->what()).find("refused the sizing capital"), npos);
    EXPECT_DOUBLE_EQ(pm->sizing_capital(), 1'000'000.0);
}

// ------------------------------------------------------------------------------------------------
// (3) the backtest: each cycle sizes on the PREVIOUS cycle's equity row (the look-ahead case)
// ------------------------------------------------------------------------------------------------

// Day d at 00:00 UTC, counted from Monday 2026-01-05.
Timestamp wday(int d) { return Timestamp(std::chrono::seconds(1767571200LL + 86400LL * d)); }

Bar bar(const std::string& symbol, int d, double close) {
    Bar b;
    b.symbol = symbol;
    b.timestamp = wday(d);
    b.open = Decimal(close);
    b.high = Decimal(close * 1.01);
    b.low = Decimal(close * 0.99);
    b.close = Decimal(close);
    b.volume = 100000.0;
    return b;
}

PortfolioConfig plain_config() {
    PortfolioConfig c{1'000'000.0, 1.0, 0.0, false};
    c.opt_config.capital = 1'000'000.0;
    c.risk_config.capital = 1'000'000.0;
    c.risk_modules = {test_none_module()};
    return c;
}

class BacktestSizingCapital : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        db_ = std::make_shared<MockPostgresDatabase>("mock://testdb");
        ASSERT_TRUE(db_->connect().is_ok());
        FuturesSpec spec;
        spec.root_symbol = "XX";
        spec.exchange = "CME";
        spec.currency = "USD";
        spec.multiplier = 10.0;
        spec.tick_size = 0.25;
        spec.commission_per_contract = 2.0;
        spec.initial_margin = 1000.0;
        spec.maintenance_margin = 800.0;
        spec.weight = 1.0;
        spec.trading_hours = "09:30-16:00";
        auto& registry = InstrumentRegistry::instance();
        registry.instruments_["XX"] = std::make_shared<FuturesInstrument>("XX", spec);
        registry.initialized_ = true;
    }
    void TearDown() override {
        coord_.reset();
        pm_.reset();
        probe_.reset();
        auto& registry = InstrumentRegistry::instance();
        registry.instruments_.erase("XX");
        db_.reset();
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        TestBase::TearDown();
    }

    void make(bool futures) {
        static int n = 0;
        BacktestCoordinatorConfig cc;
        cc.initial_capital = 1'000'000.0;
        cc.store_results = false;
        cc.store_trade_details = false;
        cc.portfolio_id = "SIZING_TEST";
        coord_ = std::make_unique<BacktestCoordinator>(db_, &InstrumentRegistry::instance(), cc);
        ASSERT_TRUE(coord_->initialize().is_ok());
        coord_->reset_portfolio_state();
        coord_->size_on_equity_enabled_ = futures;  // what run_portfolio sets for FUTURES
        pm_ = std::make_shared<PortfolioManager>(plain_config(), "PM_SZ_BT_" + std::to_string(++n));
        probe_ = std::make_shared<CapitalProbe>("SZ_BT_" + std::to_string(n),
                                                probe_config(500'000.0), db_);
        ASSERT_TRUE(probe_->initialize().is_ok());
        ASSERT_TRUE(probe_->start().is_ok());
        ASSERT_TRUE(pm_->add_strategy(probe_, 0.5, false).is_ok());
    }

    void cycle(int d, double close) {
        auto r = coord_->process_portfolio_day(wday(d), {bar("XX", d, close)}, pm_, execs_,
                                               equity_, risk_, /*warmup=*/false, 1'000'000.0);
        ASSERT_TRUE(r.is_ok()) << r.error()->what();
    }

    std::shared_ptr<MockPostgresDatabase> db_;
    std::unique_ptr<BacktestCoordinator> coord_;
    std::shared_ptr<PortfolioManager> pm_;
    std::shared_ptr<CapitalProbe> probe_;
    std::vector<ExecutionReport> execs_;
    std::vector<std::pair<Timestamp, double>> equity_;
    std::vector<RiskResult> risk_;
};

TEST_F(BacktestSizingCapital, EachCycleSizesOnThePreviousRowNeverOnTheRowItWrites) {
    make(true);
    const double closes[] = {100.0, 110.0, 104.0, 121.0, 97.0};
    cycle(0, closes[0]);  // the first group is stored, not processed: one flat row
    ASSERT_EQ(equity_.size(), 1u);
    for (int d = 1; d < 5; ++d) {
        const double before = equity_.back().second;  // the row the cycle must size on
        const size_t seen_before = probe_->seen.size();
        cycle(d, closes[d]);
        ASSERT_EQ(probe_->seen.size(), seen_before + 1);
        const double written = equity_.back().second;  // the row this cycle appended
        EXPECT_DOUBLE_EQ(probe_->seen.back(), before * 0.5)
            << "cycle " << d << " sizes on the previous row x the allocation";
        if (d >= 2) {
            ASSERT_NE(written, before) << "cycle " << d << " holds XX through a price move";
            EXPECT_NE(probe_->seen.back(), written * 0.5)
                << "cycle " << d << " must not size on the row it writes (look-ahead)";
        }
    }
    // Cycle 2 onward the probe held one XX (multiplier 10): the rows moved with the price, so
    // the capital it sized on moved with them rather than staying at 500,000.
    EXPECT_NE(probe_->seen.back(), 500'000.0);
}

// Control (passes on the parent too): the equity backtest keeps the constant capital.
TEST_F(BacktestSizingCapital, WithoutTheFuturesSwitchTheCapitalStaysConstant) {
    make(false);
    const double closes[] = {100.0, 110.0, 104.0, 121.0};
    for (int d = 0; d < 4; ++d) cycle(d, closes[d]);
    ASSERT_EQ(probe_->seen.size(), 3u);
    for (double c : probe_->seen) EXPECT_DOUBLE_EQ(c, 500'000.0);
}

// ------------------------------------------------------------------------------------------------
// (4) the live figure: STEP 4's parts (the row before Day T-1, the settlement move, Day T-1's costs)
// ------------------------------------------------------------------------------------------------

Position held(const std::string& symbol, double qty) {
    Position p;
    p.symbol = symbol;
    p.quantity = Decimal(qty);
    p.average_price = Decimal(0.0);
    return p;
}

std::shared_ptr<FuturesInstrument> future(const std::string& root, double multiplier) {
    FuturesSpec spec;
    spec.root_symbol = root;
    spec.exchange = "CME";
    spec.currency = "USD";
    spec.multiplier = multiplier;
    spec.tick_size = 0.25;
    spec.commission_per_contract = 2.0;
    spec.initial_margin = 1000.0;
    spec.maintenance_margin = 800.0;
    spec.weight = 1.0;
    return std::make_shared<FuturesInstrument>(root, spec);
}

class LiveSizingCapital : public ::testing::Test {
protected:
    void SetUp() override {
        auto& reg = InstrumentRegistry::instance();
        reg.instruments_["MES"] = future("MES", 5.0);
        reg.instruments_["ZN"] = future("ZN", 1000.0);
    }
    void TearDown() override {
        auto& reg = InstrumentRegistry::instance();
        reg.instruments_.erase("MES");
        reg.instruments_.erase("ZN");
    }
};

// Worked by hand, STEP 4's own arithmetic. The row before Day T-1 is stored at 497,274.5217, Day T-1's row
// carries 16.3516 of costs. The sleeves held MES 2 and ZN 1 over Day T-1:
//   MES  2 x (7,252.50 - 7,230.00) x 5          =   225.00
//   ZN   1 x (110.703125 - 110.906250) x 1,000  =  -203.125
// sizing equity = 497,274.5217 + 21.875 - 16.3516 = 497,280.0451 = Day T-1's close, the value STEP 4 writes on Day
// T-1's row (day_before + aggregate - daily_transaction_costs) whether or not that row is finalised already.
TEST_F(LiveSizingCapital, TheRowBeforeDayT1PlusTheSettlementMoveLessDayT1Costs) {
    LivePnLManager pnl(500'000.0, InstrumentRegistry::instance());
    const auto r = live_sizing_equity(
        true, 497'274.5217, 16.3516,
        {{{"MES.v.0", held("MES.v.0", 2.0)}}, {{"ZN.v.0", held("ZN.v.0", 1.0)}}},
        {{"MES.v.0", 7252.50}, {"ZN.v.0", 110.703125}},
        {{"MES.v.0", 7230.00}, {"ZN.v.0", 110.906250}},
        [&](const std::string& s) { return pnl.get_point_value(s); });
    EXPECT_NEAR(r.t1_settlement, 21.875, 1e-9);
    EXPECT_NEAR(r.equity, 497'280.0451, 1e-9);
    EXPECT_EQ(r.priced, 2);
    EXPECT_EQ(r.unpriced, 0);
}

// Without a Day T-1 row STEP 4 updates nothing and STEP 5 reads the latest stored row before the run date: the
// figure is that value, whatever the books say.
TEST_F(LiveSizingCapital, WithoutADayT1RowTheLatestStoredValueIsTheFigure) {
    LivePnLManager pnl(500'000.0, InstrumentRegistry::instance());
    const auto r = live_sizing_equity(
        false, 503'294.348, 12.5, {{{"MES.v.0", held("MES.v.0", 2.0)}}}, {{"MES.v.0", 7252.50}},
        {{"MES.v.0", 7230.00}}, [&](const std::string& s) { return pnl.get_point_value(s); });
    EXPECT_DOUBLE_EQ(r.equity, 503'294.348);
    EXPECT_DOUBLE_EQ(r.t1_settlement, 0.0);
    EXPECT_DOUBLE_EQ(r.t1_costs, 0.0);
}

// The move is exactly what PHASE 5 finalises after the rebalance (LivePnLManager, per sleeve),
// so the sizing equity is the value STEP 4 writes and STEP 5 reads back.
TEST_F(LiveSizingCapital, TheMoveIsWhatPhase5FinalisesPerSleeve) {
    LivePnLManager pnl(500'000.0, InstrumentRegistry::instance());
    const std::unordered_map<std::string, double> t1{{"MES.v.0", 7101.25}, {"ZN.v.0", 111.5}};
    const std::unordered_map<std::string, double> t2{{"MES.v.0", 7188.75}, {"ZN.v.0", 111.25}};
    const std::vector<std::unordered_map<std::string, Position>> books{
        {{"MES.v.0", held("MES.v.0", 3.0)}, {"ZN.v.0", held("ZN.v.0", -2.0)}},
        {{"MES.v.0", held("MES.v.0", 1.0)}}};
    double finalised = 0.0;
    for (const auto& book : books) {
        std::vector<Position> v;
        for (const auto& [s, p] : book) v.push_back(p);
        auto f = pnl.finalize_previous_day(v, t1, t2, 500'000.0, 0.0);
        ASSERT_TRUE(f.is_ok());
        finalised += f.value().finalized_daily_pnl;
    }
    const auto r = live_sizing_equity(true, 500'000.0, 0.0, books, t1, t2, [&](const std::string& s) {
        return pnl.get_point_value(s);
    });
    EXPECT_NEAR(r.t1_settlement, finalised, 1e-9);
    // 4 x (7,101.25 - 7,188.75) x 5 - 2 x (111.5 - 111.25) x 1,000 = -1,750 - 500 = -2,250
    EXPECT_NEAR(r.t1_settlement, -2250.0, 1e-9);
    EXPECT_NEAR(r.equity, 497'750.0, 1e-9) << "a losing day shrinks the capital the book sizes on";
}

TEST_F(LiveSizingCapital, APositionMissingEitherCloseSettlesNothingAsPhase5Books) {
    LivePnLManager pnl(500'000.0, InstrumentRegistry::instance());
    const auto r = live_sizing_equity(
        true, 500'000.0, 0.0, {{{"MES.v.0", held("MES.v.0", 2.0)}, {"ZN.v.0", held("ZN.v.0", 1.0)}}},
        {{"MES.v.0", 7252.50}},                        // ZN has no T-1 close (NO_BAR)
        {{"MES.v.0", 7230.00}, {"ZN.v.0", 110.90625}},
        [&](const std::string& s) { return pnl.get_point_value(s); });
    EXPECT_NEAR(r.t1_settlement, 225.0, 1e-9);
    EXPECT_EQ(r.priced, 1);
    EXPECT_EQ(r.unpriced, 1);
}

TEST(BacktestSizingEquity, TheCurvesLastRowOrTheInitialCapital) {
    EXPECT_DOUBLE_EQ(backtest_sizing_equity({}, 500'000.0), 500'000.0);
    EXPECT_DOUBLE_EQ(backtest_sizing_equity({{wday(0), 500'000.0}, {wday(1), 503'125.5}}, 500'000.0),
                     503'125.5);
}

// ------------------------------------------------------------------------------------------------
// (5) the wiring: both futures runners, the same block, before the rebalance
// ------------------------------------------------------------------------------------------------

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

std::string between(const std::string& s, const std::string& from, const std::string& to) {
    const auto a = s.find(from);
    if (a == npos) return {};
    const auto b = s.find(to, a);
    if (b == npos) return {};
    return s.substr(a, b - a);
}

TEST(SizingCapitalWiring, BothFuturesRunnersSizeOnTheEquityBeforeTheRebalance) {
    std::string blocks[2];
    const char* const runners[] = {"apps/strategies/live_portfolio_conservative.cpp",
                                   "apps/strategies/live_portfolio.cpp"};
    for (int i = 0; i < 2; ++i) {
        const std::string src = read_source(runners[i]);
        ASSERT_FALSE(src.empty()) << runners[i];
        blocks[i] = between(src, "// SIZING CAPITAL (T-7b-2 9c", "// STORE LIVE RUN METADATA");
        ASSERT_FALSE(blocks[i].empty()) << runners[i] << ": no sizing block before the metadata row";
        const auto block_at = src.find("// SIZING CAPITAL (T-7b-2 9c");
        EXPECT_LT(block_at, src.find("portfolio->process_market_data(")) << runners[i];
        EXPECT_NE(blocks[i].find("portfolio->set_sizing_capital(sizing_equity.equity)"), npos);
        // Day T-1 and Day T-2 closes only: the day being sized has no mark yet.
        EXPECT_NE(blocks[i].find("price_manager->get_all_previous_day_prices()"), npos);
        EXPECT_NE(blocks[i].find("price_manager->get_all_two_days_ago_prices()"), npos);
        EXPECT_EQ(blocks[i].find("current_price"), npos);
        // STEP 4's parts: Day T-1's row (its costs), the row before Day T-1 (date < T-1), or with no Day T-1 row
        // the row before the run date (date < now). Day T-1's own stored value is never read.
        EXPECT_NE(blocks[i].find("const auto sizing_t1 = now - std::chrono::hours(24);"), npos);
        EXPECT_NE(blocks[i].find("data_loader->load_live_results(combined_strategy_id,"), npos);
        EXPECT_NE(blocks[i].find("t1_row_stored ? sizing_t1 : now, \"trading.live_results\")"), npos);
        EXPECT_NE(blocks[i].find("t1_row_stored ? t1_row.value().daily_transaction_costs : 0.0"), npos);
        EXPECT_EQ(blocks[i].find("current_portfolio_value"), npos);
        EXPECT_NE(blocks[i].find("return 1;"), npos) << "a run that cannot size refuses";
        EXPECT_NE(src.find("snapshot_risk_config.capital = Decimal(portfolio->sizing_capital());"),
                  npos)
            << runners[i] << ": the reporter measures against the capital the book was sized on";
        EXPECT_NE(src.find("SIZING_CAPITAL_CHECK"), npos) << runners[i];
    }
    EXPECT_EQ(blocks[0], blocks[1]) << "the twins' sizing blocks are byte-identical";
}

TEST(SizingCapitalWiring, TheBacktestCompoundsForFuturesOnly) {
    const std::string src = read_source("src/backtest/backtest_coordinator.cpp");
    ASSERT_FALSE(src.empty());
    EXPECT_NE(src.find("size_on_equity_enabled_ = (asset_class == AssetClass::FUTURES);"), npos);
    const std::string day = between(src, "Result<void> BacktestCoordinator::process_portfolio_day(",
                                    "equity_curve.emplace_back(timestamp, portfolio_value);");
    ASSERT_FALSE(day.empty());
    const auto set_at = day.find("portfolio->set_sizing_capital(sizing_equity)");
    ASSERT_NE(set_at, npos);
    EXPECT_LT(set_at, day.find("portfolio->process_market_data(*signal_feed"))
        << "sized before the rebalance, on the curve as it stands (this cycle's row comes after)";
}

}  // namespace
