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
#include <ctime>
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
#include "trade_ngin/data/roll_series.hpp"
#include "trade_ngin/live/live_sizing_read.hpp"
#include "trade_ngin/live/session_book_gate.hpp"
#include "trade_ngin/live/risk_module_failure.hpp"
#include "trade_ngin/live/run_metadata_marks.hpp"
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
        tc.idm = 2.5;
        tc.risk_target = 0.2;
        tc.fx_rate = 1.0;
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

TEST_F(PortfolioSizingCapital, TheWeightPerContractFollowsTheCapital) {
    // The one pass weighs a contract on the sizing capital (not total_capital): the manager hands
    // the pass sizing_capital_, and the pass divides a contract's notional by it. The generic
    // optimiser step, which only a book with no overlay sleeve reaches, reads no capital at all.
    const auto read = [](const std::string& relative) {
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
        return std::string();
    };
    const std::string src = read("src/portfolio/portfolio_manager.cpp");
    ASSERT_FALSE(src.empty());
    const auto pass = src.find("Result<void> PortfolioManager::rebalance_one_pass(");
    ASSERT_NE(pass, npos);
    EXPECT_NE(src.find("in.capital = static_cast<double>(sizing_capital_);", pass), npos);
    const std::string one_pass = read("src/optimization/one_pass.cpp");
    EXPECT_NE(one_pass.find("out.u[i] = in.multiplier[i] * in.close[i] / in.capital;"), npos);
    const auto opt = src.find("Result<void> PortfolioManager::optimize_positions()");
    ASSERT_NE(opt, npos);
    const auto end = src.find("bool PortfolioManager::one_pass_book() const", opt);
    ASSERT_NE(end, npos);
    const std::string body = src.substr(opt, end - opt);
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
    bool below_start = false;
    for (int d = 1; d < 5; ++d) {
        // The capital the cycle must size on: the half compounding of the curve as it stands, whose
        // last row is the previous cycle's (LOOP_SPEC section 3.1).
        const double before = backtest_sizing_equity(equity_, 1'000'000.0);
        const double before_row = equity_.back().second;
        const size_t seen_before = probe_->seen.size();
        cycle(d, closes[d]);
        ASSERT_EQ(probe_->seen.size(), seen_before + 1);
        const double written = equity_.back().second;  // the row this cycle appended
        EXPECT_DOUBLE_EQ(probe_->seen.back(), before * 0.5)
            << "cycle " << d << " sizes on the capital through the previous row x the allocation";
        EXPECT_LE(probe_->seen.back(), 1'000'000.0 * 0.5) << "never above the starting capital";
        if (d >= 2) {
            ASSERT_NE(written, before_row) << "cycle " << d << " holds XX through a price move";
            const double with_own_row = backtest_sizing_equity(equity_, 1'000'000.0);
            if (with_own_row != before) {
                EXPECT_NE(probe_->seen.back(), with_own_row * 0.5)
                    << "cycle " << d << " must not size on the row it writes (look-ahead)";
            }
        }
        below_start = below_start || probe_->seen.back() < 1'000'000.0 * 0.5;
    }
    // Cycle 2 onward the probe held one XX (multiplier 10) through a rise and then a fall: the
    // fall came off the capital, so some cycle sized below the starting capital.
    EXPECT_TRUE(below_start);
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
        [&](const std::string& s) { return pnl.get_point_value(s); }, {});
    EXPECT_NEAR(r.t1_settlement, 21.875, 1e-9);
    EXPECT_NEAR(r.equity, 497'280.0451, 1e-9);
    EXPECT_EQ(r.priced, 2);
    EXPECT_EQ(r.unpriced, 0);
}

// D-B (T-ROLLX-FIX commit 4; LOOP_SPEC v6.2 sections 2.1, 3.1, 6.6): the book is sized on the settlement PHASE 5
// finalises Day T-1 with, the one on the CONSUMED bars. T-1 = 2026-05-01. MES is held 2 and its T-1 bar is a change
// bar (the vendor switched contract: 7,230.00 on A, 7,252.50 on B); ZN is held 1 and its T-2 print (111.500000) is a
// corrupt print the K-01 feed withheld, so its previous consumed close is T-3's 110.906250:
//   MES  change bar                                 =     0.000   (raw: 2 x (7,252.50 - 7,230.00) x 5 = 225.00)
//   ZN   1 x (110.703125 - 110.906250) x 1,000      =  -203.125   (raw: 1 x (110.703125 - 111.500000) x 1,000 = -796.875)
// The runner passes the settlement's maps and zero set to the sizing read and to finalize_previous_day: both book
// -203.125, and the sizing equity is the value STEP 4 writes. Reading the price manager's raw maps (the runner before
// this commit) sized on -571.875, an equity 368.75 away from its own stored row.
TEST_F(LiveSizingCapital, TheBookIsSizedOnTheConsumedSettlementPhase5Finalises) {
    LivePnLManager pnl(500'000.0, InstrumentRegistry::instance());
    auto consumed_bar = [](const std::string& symbol, int day, double close, const std::string& id) {
        Bar b;
        b.symbol = symbol;
        b.timestamp = std::chrono::sys_days{std::chrono::year{2026} / std::chrono::month{4} / std::chrono::day{27}} +
                      std::chrono::hours(24 * day);
        b.open = b.high = b.low = b.close = Decimal(close);
        b.volume = 100000.0;
        b.instrument_id = id;
        return b;
    };
    // Day 0 = Mon 04-27 ... day 4 = Fri 05-01 (T-1). ZN's day-3 print is withheld: it is not in the consumed feed.
    const std::vector<Bar> consumed = {
        consumed_bar("MES.v.0", 2, 7215.00, "A"), consumed_bar("MES.v.0", 3, 7230.00, "A"),
        consumed_bar("MES.v.0", 4, 7252.50, "B"), consumed_bar("ZN.v.0", 1, 110.800000, "Z"),
        consumed_bar("ZN.v.0", 2, 110.906250, "Z"), consumed_bar("ZN.v.0", 4, 110.703125, "Z")};
    const std::unordered_map<std::string, double> raw_t1{{"MES.v.0", 7252.50}, {"ZN.v.0", 110.703125}};
    const std::unordered_map<std::string, double> raw_t2{{"MES.v.0", 7230.00}, {"ZN.v.0", 111.500000}};
    const auto status = roll_series::roll_status_of(consumed);
    const auto settlement = consumed_t1_settlement(consumed, "2026-05-01", status, {}, raw_t2, raw_t1, {});
    ASSERT_TRUE(settlement.zero_pnl_symbols.count("MES.v.0")) << "the change bar books no move";
    const std::vector<std::unordered_map<std::string, Position>> books = {
        {{"MES.v.0", held("MES.v.0", 2.0)}}, {{"ZN.v.0", held("ZN.v.0", 1.0)}}};
    const auto point = [&](const std::string& s) { return pnl.get_point_value(s); };

    const auto sized = live_sizing_equity(true, 500'000.0, 10.0, books, settlement.t1_close_prices,
                                          settlement.t2_close_prices, point, settlement.zero_pnl_symbols);
    double finalised = 0.0;
    for (const auto& book : books) {
        std::vector<Position> rows;
        for (const auto& [_, p] : book) rows.push_back(p);
        auto f = pnl.finalize_previous_day(rows, settlement.t1_close_prices, settlement.t2_close_prices, 250'000.0, 0.0,
                                           LivePnLManager::UnrealizedPolicy::SETTLED, settlement.zero_pnl_symbols);
        ASSERT_TRUE(f.is_ok()) << f.error()->what();
        finalised += f.value().finalized_daily_pnl;
    }
    EXPECT_NEAR(finalised, -203.125, 1e-9);
    EXPECT_NEAR(sized.t1_settlement, finalised, 1e-9) << "sized on the move PHASE 5 finalises";
    EXPECT_NEAR(sized.equity, 500'000.0 - 203.125 - 10.0, 1e-9) << "the value STEP 4 writes on Day T-1's row";
    EXPECT_EQ(sized.priced, 2);
}

// Without a Day T-1 row STEP 4 updates nothing and STEP 5 reads the latest stored row before the run date: the
// figure is that value, whatever the books say.
TEST_F(LiveSizingCapital, WithoutADayT1RowTheLatestStoredValueIsTheFigure) {
    LivePnLManager pnl(500'000.0, InstrumentRegistry::instance());
    const auto r = live_sizing_equity(
        false, 503'294.348, 12.5, {{{"MES.v.0", held("MES.v.0", 2.0)}}}, {{"MES.v.0", 7252.50}},
        {{"MES.v.0", 7230.00}}, [&](const std::string& s) { return pnl.get_point_value(s); }, {});
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
    }, {});
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
        [&](const std::string& s) { return pnl.get_point_value(s); }, {});
    EXPECT_NEAR(r.t1_settlement, 225.0, 1e-9);
    EXPECT_EQ(r.priced, 1);
    EXPECT_EQ(r.unpriced, 1);
}

// LOOP_SPEC section 3.1 (D19): the backtest sizes on the half-compounded capital of its own curve.
// A profit is never sized on (the capital stays at the starting capital); a loss comes off at
// once; row by row the capital follows min(S_0, capital + net).
TEST(BacktestSizingEquity, AProfitThenALossFollowsTheHalfCompoundingRowByRow) {
    const double s0 = 500'000.0;
    EXPECT_DOUBLE_EQ(backtest_sizing_equity({}, s0), s0);
    // the start, a profit, a larger profit, a loss, a loss below the start, a recovery, a new high
    const std::vector<double> rows = {500'000.0, 503'125.5, 507'900.0, 504'000.0,
                                      498'250.25, 501'000.0, 509'100.0};
    std::vector<std::pair<Timestamp, double>> curve;
    double capital = s0;  // the recursion, applied beside the closed form
    for (size_t k = 0; k < rows.size(); ++k) {
        if (k > 0) capital = std::min(s0, capital + (rows[k] - rows[k - 1]));
        curve.emplace_back(wday(static_cast<int>(k)), rows[k]);
        EXPECT_NEAR(backtest_sizing_equity(curve, s0), capital, 1e-6) << "row " << k;
    }
    // A profit is not sized on: after the first two rows the capital is still the starting capital.
    EXPECT_DOUBLE_EQ(backtest_sizing_equity({{wday(0), s0}, {wday(1), 503'125.5}}, s0), s0);
    // A loss from a high comes off the starting capital, not off the high.
    EXPECT_NEAR(backtest_sizing_equity({{wday(0), s0}, {wday(1), 507'900.0}, {wday(2), 504'000.0}}, s0),
                s0 - 3'900.0, 1e-6);
    const auto h = backtest_half_compounding(curve, s0);
    EXPECT_NEAR(h.account, 509'100.0, 1e-6);
    EXPECT_NEAR(h.peak, 9'100.0, 1e-6);
    EXPECT_NEAR(h.cumulative, 9'100.0, 1e-6);
    EXPECT_NEAR(h.capital, s0, 1e-6);
}

// The closed form over a list of settled nets, the recursion beside it, and a seeded start.
TEST(HalfCompounding, TheClosedFormIsTheRecursionInDateOrder) {
    const double s0 = 500'000.0;
    const std::vector<double> nets = {1200.0, -300.5, -2500.0, 800.0, 4100.25, -50.0, -6000.0, 9000.0};
    for (double d0 : {0.0, 12'500.0}) {
        double capital = s0 - d0;
        std::vector<double> seen;
        EXPECT_NEAR(half_compounded_capital(s0, seen, d0).capital, capital, 1e-9);
        for (double net : nets) {
            capital = std::min(s0, capital + net);
            seen.push_back(net);
            const auto h = half_compounded_capital(s0, seen, d0);
            EXPECT_NEAR(h.capital, capital, 1e-6) << "after " << seen.size() << " nets, D_0 " << d0;
            EXPECT_LE(h.capital, s0);
        }
    }
    // A seeded chain (D_0 > 0): the capital starts at the seed and a profit rebuilds it only up
    // to the starting capital.
    EXPECT_NEAR(half_compounded_capital(s0, {}, 12'500.0).capital, 487'500.0, 1e-9);
    EXPECT_NEAR(half_compounded_capital(s0, {5'000.0}, 12'500.0).capital, 492'500.0, 1e-9);
    EXPECT_NEAR(half_compounded_capital(s0, {5'000.0, 20'000.0}, 12'500.0).capital, s0, 1e-9);
    EXPECT_NEAR(half_compounded_capital(s0, {5'000.0, 20'000.0, -1'000.0}, 12'500.0).capital,
                s0 - 1'000.0, 1e-9);
    // The account the history gives is the starting capital plus the cumulative settled P&L.
    EXPECT_NEAR(half_compounded_capital(s0, nets).account, s0 + 6249.75, 1e-9);
}

// ------------------------------------------------------------------------------------------------
// (4b) T-7b-3 R-3: the sizing reads. "Nothing stored" sizes as before. A database error holds the
// book (the books loaded, the equity or previous-row read failed: exit 3, mark, email flag) or
// refuses the run (a sleeve book failed to load: exit 1, no row); HD 2026-09-27 ruling 5.
// The runner helper is fed the REAL LiveDataLoader over a mock database, so load_live_results
// answers exactly as it does in production (its "No live results found" for an empty result, its
// "Failed to load live results" for a failed query).
// ------------------------------------------------------------------------------------------------

class SizingReadDatabase : public MockPostgresDatabase {
public:
    enum class T1 { kRow, kNoRow, kError };

    SizingReadDatabase() : MockPostgresDatabase("mock://sizing_read") { (void)connect(); }

    T1 t1 = T1::kRow;
    double t1_costs = 0.0;
    std::optional<double> previous_value;  // nullopt: no row before the date
    bool previous_error = false;
    std::set<std::string> failing_sleeves;
    std::map<std::string, std::unordered_map<std::string, Position>> books;
    std::vector<Timestamp> previous_asked;  // the dates get_previous_live_aggregates was given
    int book_loads = 0;

    // The stored P&L history before Day T-1 (load_sizing_pnl_history): date, daily_pnl, positions.
    std::vector<std::tuple<std::string, double, int>> history;
    bool history_error = false;
    int history_reads = 0;
    // The dates of `history` whose row carries a settled_at stamp (migration 029). Empty: no row
    // is stamped, so a read settles its history on the no-bar-day rule alone.
    std::set<std::string> stamped;

    // load_live_results' query and the sizing history's query (the two live_results SELECTs the
    // loader sends here).
    Result<std::shared_ptr<arrow::Table>> execute_query(const std::string& query) override {
        if (query.find(".live_results") == npos) return MockPostgresDatabase::execute_query(query);
        if (query.find("sizing_history_date") != npos) {
            ++history_reads;
            if (history_error) {
                return make_error<std::shared_ptr<arrow::Table>>(
                    ErrorCode::DATABASE_ERROR, "canceling statement due to statement timeout",
                    "PostgresDatabase");
            }
            // Every column a string, as the production converter builds a generic result.
            arrow::StringBuilder d, p, a, st;
            for (const auto& [date, pnl, held_positions] : history) {
                ARROW_CHECK_OK(d.Append(date));
                ARROW_CHECK_OK(p.Append(std::to_string(pnl)));
                ARROW_CHECK_OK(a.Append(std::to_string(held_positions)));
                ARROW_CHECK_OK(st.Append(stamped.count(date) != 0 ? "1" : "0"));
            }
            std::shared_ptr<arrow::Array> da, pa, aa, sa;
            ARROW_CHECK_OK(d.Finish(&da));
            ARROW_CHECK_OK(p.Finish(&pa));
            ARROW_CHECK_OK(a.Finish(&aa));
            ARROW_CHECK_OK(st.Finish(&sa));
            return Result<std::shared_ptr<arrow::Table>>(arrow::Table::Make(
                arrow::schema({arrow::field("sizing_history_date", arrow::utf8()),
                               arrow::field("daily_pnl", arrow::utf8()),
                               arrow::field("active_positions", arrow::utf8()),
                               arrow::field("settled_at_set", arrow::utf8())}),
                {da, pa, aa, sa}));
        }
        if (t1 == T1::kError) {
            return make_error<std::shared_ptr<arrow::Table>>(
                ErrorCode::DATABASE_ERROR, "server closed the connection unexpectedly",
                "PostgresDatabase");
        }
        // The loader's 31 SELECT columns; daily_transaction_costs is the 14th (index 13).
        std::vector<std::shared_ptr<arrow::Field>> fields;
        std::vector<std::shared_ptr<arrow::Array>> arrays;
        for (int c = 0; c < 31; ++c) {
            arrow::DoubleBuilder b;
            if (t1 == T1::kRow) {
                ARROW_CHECK_OK(b.Append(c == 13 ? t1_costs : 0.0));
            }
            std::shared_ptr<arrow::Array> a;
            ARROW_CHECK_OK(b.Finish(&a));
            fields.push_back(arrow::field("c" + std::to_string(c), arrow::float64()));
            arrays.push_back(a);
        }
        return Result<std::shared_ptr<arrow::Table>>(
            arrow::Table::Make(arrow::schema(fields), arrays));
    }

    Result<std::tuple<double, double, double>> get_previous_live_aggregates(
        const std::string& strategy_id, const std::string& portfolio_id, const Timestamp& date,
        const std::string& table_name) override {
        (void)table_name;
        previous_asked.push_back(date);
        if (previous_error) {
            return make_error<std::tuple<double, double, double>>(
                ErrorCode::DATABASE_ERROR,
                "Failed to fetch previous live aggregates: terminating connection due to "
                "administrator command",
                "PostgresDatabase");
        }
        if (!previous_value) {  // PostgresDatabase's own no-row answer, verbatim
            return make_error<std::tuple<double, double, double>>(
                ErrorCode::DATABASE_ERROR, "No previous aggregates found for strategy " +
                                               strategy_id + " (portfolio: " + portfolio_id + ")");
        }
        return Result<std::tuple<double, double, double>>(
            std::make_tuple(*previous_value, 0.0, 0.0));
    }

    Result<std::unordered_map<std::string, Position>> load_positions_by_date(
        const std::string& strategy_id, const std::string& strategy_name,
        const std::string& portfolio_id, const Timestamp& date,
        const std::string& table_name) override {
        (void)strategy_id; (void)portfolio_id; (void)date; (void)table_name;
        ++book_loads;
        if (failing_sleeves.count(strategy_name)) {
            return make_error<std::unordered_map<std::string, Position>>(
                ErrorCode::DATABASE_ERROR,
                "Failed to load positions by date: could not receive data from server",
                "PostgresDatabase");
        }
        auto it = books.find(strategy_name);
        return Result<std::unordered_map<std::string, Position>>(
            it == books.end() ? std::unordered_map<std::string, Position>{} : it->second);
    }
};

class LiveSizingReads : public LiveSizingCapital {
protected:
    void SetUp() override {
        LiveSizingCapital::SetUp();
        db_ = std::make_shared<SizingReadDatabase>();
        loader_ = std::make_unique<LiveDataLoader>(db_, "trading");
        // The worked example of (4): the row before Day T-1 497,274.5217, Day T-1's costs 16.3516,
        // sleeve A held MES 2 and sleeve B ZN 1 over Day T-1 (a move of +225 and -203.125).
        db_->previous_value = 497'274.5217;
        db_->t1_costs = 16.3516;
        db_->books["A"] = {{"MES.v.0", held("MES.v.0", 2.0)}};
        db_->books["B"] = {{"ZN.v.0", held("ZN.v.0", 1.0)}};
    }
    LiveSizingRead read() {
        return read_live_sizing_equity(
            *loader_, *db_, "LIVE_TREND_FOLLOWING", "BASE_PORTFOLIO", {"A", "B"}, now_, 500'000.0,
            {{"MES.v.0", 7252.50}, {"ZN.v.0", 110.703125}},
            {{"MES.v.0", 7230.00}, {"ZN.v.0", 110.906250}},
            [&](const std::string& s) { return pnl_.get_point_value(s); }, {}, calendar_);
    }
    // The dates the run loaded a bar on: every weekday of April 2026 (2026-04-27, Day T-1, among
    // them), so every stored day of the fixtures is a settled one unless a test removes its date.
    LiveSizingCalendar calendar_ = [] {
        LiveSizingCalendar c;
        for (int d = 1; d <= 30; ++d) {
            const int weekday = (d + 2) % 7;  // 2026-04-01 is a Wednesday (3)
            if (weekday == 0 || weekday == 6) continue;
            char buf[16];
            std::snprintf(buf, sizeof(buf), "2026-04-%02d", d);
            c.bar_dates.insert(buf);
        }
        c.first_bar_date = *c.bar_dates.begin();
        return c;
    }();
    const Timestamp now_ = Timestamp(std::chrono::seconds(1777334400LL));  // 2026-04-28
    LivePnLManager pnl_{500'000.0, InstrumentRegistry::instance()};
    std::shared_ptr<SizingReadDatabase> db_;
    std::unique_ptr<LiveDataLoader> loader_;
};

std::string outcome_of(const LiveSizingRead& r) {
    switch (r.outcome) {
        case LiveSizingOutcome::kSized:
            return "it sized on " + std::to_string(r.equity.equity) + " (" + r.day_before_source +
                   ")";
        case LiveSizingOutcome::kHoldBook: return "it held the book: " + r.failure;
        case LiveSizingOutcome::kRefuseRun: return "it refused the run: " + r.failure;
    }
    return "?";
}

// Control: every read answers, the figure is (4)'s worked example.
TEST_F(LiveSizingReads, EveryReadAnsweredSizesOnStep4sArithmetic) {
    const auto r = read();
    ASSERT_EQ(r.outcome, LiveSizingOutcome::kSized) << outcome_of(r);
    EXPECT_NEAR(r.equity.equity, 497'280.0451, 1e-9);
    EXPECT_TRUE(r.equity.t1_row);
    EXPECT_EQ(r.day_before_source, "the latest stored row before Day T-1");
    ASSERT_EQ(db_->previous_asked.size(), 1u);
    EXPECT_EQ(db_->previous_asked[0], now_ - std::chrono::hours(24));
    EXPECT_EQ(db_->book_loads, 2);
}

// The brief's case: the database drops for the run (every read fails). On f2932054 every failure
// read as "nothing stored" and the whole book sized on 500,000 and traded on it. The positions did
// not load, so there is no book to hold: the run refuses (exit 1, no row).
TEST_F(LiveSizingReads, TheDatabaseDownRefusesTheRunInsteadOfSizingOnTheInitialCapital) {
    db_->t1 = SizingReadDatabase::T1::kError;
    db_->previous_error = true;
    db_->failing_sleeves = {"A", "B"};
    const auto r = read();
    ASSERT_EQ(r.outcome, LiveSizingOutcome::kRefuseRun) << outcome_of(r);
    EXPECT_NE(r.failure.find("sleeve A's Day T-1 book could not be read"), npos) << r.failure;
    EXPECT_NE(r.failure.find("Day T-1's live_results row could not be read"), npos) << r.failure;
    EXPECT_NE(r.failure.find("server closed the connection unexpectedly"), npos) << r.failure;
}

// The exit-1 path alone: one sleeve's Day T-1 book fails to load, the other reads answer. On
// f2932054 the sleeve was dropped and the book sized without its move (497,483.1701 instead of
// 497,280.0451).
TEST_F(LiveSizingReads, AFailedSleeveBookLoadRefusesTheRun) {
    db_->failing_sleeves = {"B"};
    const auto r = read();
    ASSERT_EQ(r.outcome, LiveSizingOutcome::kRefuseRun) << outcome_of(r);
    EXPECT_NE(r.failure.find("sleeve B's Day T-1 book could not be read (Failed to load positions "
                             "by date: could not receive data from server)"),
              npos)
        << r.failure;
}

// The hold path, the equity read: load_live_results fails (a failed query, not an empty result)
// and every book loaded, so the book is held. On f2932054 it read as "no Day T-1 row" and sized on
// the row before the run date, dropping Day T-1's move and costs (497,274.5217).
TEST_F(LiveSizingReads, ADatabaseErrorOnTheDayT1RowHoldsTheBook) {
    db_->t1 = SizingReadDatabase::T1::kError;
    const auto r = read();
    ASSERT_EQ(r.outcome, LiveSizingOutcome::kHoldBook) << outcome_of(r);
    EXPECT_EQ(r.failure,
              "Day T-1's live_results row could not be read (Failed to load live results: server "
              "closed the connection unexpectedly)");
    EXPECT_EQ(db_->book_loads, 2) << "the books are read: a hold needs them to have loaded";
}

// The hold path, the aggregates read: get_previous_live_aggregates fails with a Day T-1 row stored
// and every book loaded. On f2932054 it read as "none stored" and sized on 500,000 plus Day T-1's
// move less its costs (500,005.5234).
TEST_F(LiveSizingReads, ADatabaseErrorOnThePreviousRowHoldsTheBook) {
    db_->previous_error = true;
    const auto r = read();
    ASSERT_EQ(r.outcome, LiveSizingOutcome::kHoldBook) << outcome_of(r);
    EXPECT_NE(r.failure.find("the latest live_results row before Day T-1 could not be read "
                             "(Failed to fetch previous live aggregates:"),
              npos)
        << r.failure;
}

// A hold is marked, flagged and exits as a RISK_MODULE_FAILURE day does, and names itself: the
// metadata row's risk_refusal says scope "sizing", module "SIZING_CAPITAL"; the exit code is
// kRiskModuleFailureExitCode (3); the email carries the operator's flag in the subject and a
// banner that says a sizing read failed, not a risk module.
TEST_F(LiveSizingReads, AHoldIsMarkedFlaggedAndExitsThreeLikeARiskModuleFailure) {
    db_->t1 = SizingReadDatabase::T1::kError;
    const auto r = read();
    ASSERT_EQ(r.outcome, LiveSizingOutcome::kHoldBook) << outcome_of(r);
    const nlohmann::json hold = sizing_hold_refusal(r.failure);

    EXPECT_EQ(live_run_exit_code(hold), kRiskModuleFailureExitCode);
    EXPECT_EQ(live_run_exit_code(std::nullopt), 0);

    nlohmann::json config = {{"total_capital", 500'000.0}, {"use_optimization", true}};
    const nlohmann::json marked = mark_risk_refusal(config, hold, nlohmann::json::object());
    ASSERT_TRUE(marked.contains("risk_refusal")) << "the watchdog reads this key";
    EXPECT_EQ(marked["risk_refusal"]["scope"], "sizing");
    EXPECT_EQ(marked["risk_refusal"]["module"], "SIZING_CAPITAL");
    EXPECT_EQ(marked["risk_refusal"]["action"], "REFUSE");
    EXPECT_EQ(marked["risk_refusal"]["error"], r.failure);
    EXPECT_EQ(marked["total_capital"], 500'000.0) << "every other key of the row is kept";

    EXPECT_EQ(risk_module_failure_email_subject("Daily Trading Report - 2026-04-28"),
              "[RISK MODULE FAILED - BOOK HELD] Daily Trading Report - 2026-04-28");
    const std::string flagged =
        flag_email_body_for_risk_module_failure("<div class=\"container\">\n<h1>x</h1>", hold);
    EXPECT_NE(flagged.find("<strong>SIZING READ FAILED - BOOK HELD:</strong> the account's equity "
                           "could not be read to size today's book (Day T-1&#39;s live_results row "
                           "could not be read (Failed to load live results: server closed the "
                           "connection unexpectedly)). Every strategy is held at the previous "
                           "day's positions and no orders were generated. The run exited with "
                           "code 3."),
              npos)
        << flagged;
    // The failure text is HTML-escaped in the body, as a risk module's error is.
    EXPECT_EQ(flagged.find("risk module"), npos) << "the risk module wording: " << flagged;
}

// "No live results found" (the loader's answer to an empty result) is still "no Day T-1 row":
// the figure is the latest stored row before the run date, as on f2932054.
TEST_F(LiveSizingReads, NoLiveResultsFoundIsStillNoDayT1Row) {
    db_->t1 = SizingReadDatabase::T1::kNoRow;
    const auto r = read();
    ASSERT_EQ(r.outcome, LiveSizingOutcome::kSized) << outcome_of(r);
    EXPECT_FALSE(r.equity.t1_row);
    EXPECT_DOUBLE_EQ(r.equity.equity, 497'274.5217);
    EXPECT_EQ(r.day_before_source, "no Day T-1 row: the latest stored row before the run date");
    ASSERT_EQ(db_->previous_asked.size(), 1u);
    EXPECT_EQ(db_->previous_asked[0], now_);
}

// A first day (nothing stored at all) and sleeves with no stored book (OK and empty) size on the
// initial capital, as on f2932054.
TEST_F(LiveSizingReads, NothingStoredAnywhereSizesOnTheInitialCapital) {
    db_->t1 = SizingReadDatabase::T1::kNoRow;
    db_->previous_value.reset();
    db_->books.clear();
    const auto r = read();
    ASSERT_EQ(r.outcome, LiveSizingOutcome::kSized) << outcome_of(r);
    EXPECT_DOUBLE_EQ(r.equity.equity, 500'000.0);
    EXPECT_EQ(r.day_before_source, "none stored, the initial capital");
}

// A Day T-1 row but no row before it (the portfolio's second day): "none stored" is the initial
// capital, plus Day T-1's move less its costs; a sleeve with no stored book settles nothing.
TEST_F(LiveSizingReads, NoRowBeforeDayT1AndAnEmptySleeveBookAreNotErrors) {
    db_->previous_value.reset();
    db_->books.erase("B");
    const auto r = read();
    ASSERT_EQ(r.outcome, LiveSizingOutcome::kSized) << outcome_of(r);
    EXPECT_NEAR(r.equity.equity, 500'000.0 + 225.0 - 16.3516, 1e-9);
    EXPECT_EQ(r.day_before_source, "none stored, the initial capital");
    EXPECT_EQ(r.equity.priced, 1);
}

TEST(LiveSizingNoRow, OnlyTheLoadersOwnNoRowAnswersCount) {
    EXPECT_TRUE(is_no_live_results_row(
        TradeError(ErrorCode::INVALID_ARGUMENT, "No live results found for date 2026-04-27")));
    EXPECT_FALSE(is_no_live_results_row(TradeError(
        ErrorCode::DATABASE_ERROR, "Failed to load live results: server closed the connection")));
    EXPECT_FALSE(is_no_live_results_row(
        TradeError(ErrorCode::DATABASE_ERROR, "Database is not connected")));
    EXPECT_FALSE(is_no_live_results_row(
        TradeError(ErrorCode::DATABASE_ERROR, "No live results found for date 2026-04-27")));
    EXPECT_TRUE(is_no_previous_live_aggregates(TradeError(
        ErrorCode::DATABASE_ERROR, "No previous aggregates found for strategy S (portfolio: P)")));
    EXPECT_FALSE(is_no_previous_live_aggregates(TradeError(
        ErrorCode::DATABASE_ERROR, "Failed to fetch previous live aggregates: timeout")));
    EXPECT_FALSE(is_no_previous_live_aggregates(
        TradeError(ErrorCode::CONNECTION_ERROR, "Not connected to database")));
}

std::string read_source(const std::string& relative);

// The discriminators name the loaders' own no-row answers: the message and the code each returns
// for an empty result. A reworded loader breaks this test, not the refusal.
TEST(LiveSizingNoRow, TheDiscriminatorsMatchTheLoadersSource) {
    const auto code_before = [](const std::string& src, const std::string& message) {
        const auto at = src.find(message);
        if (at == npos) return std::string("message not found");
        const auto code_at = src.rfind("ErrorCode::", at);
        const auto code_end = src.find_first_of(",)", code_at);
        return src.substr(code_at, code_end - code_at);
    };
    const std::string loader = read_source("src/live/live_data_loader.cpp");
    ASSERT_FALSE(loader.empty());
    EXPECT_EQ(code_before(loader, "\"No live results found for date \""),
              "ErrorCode::INVALID_ARGUMENT");
    const std::string pg = read_source("src/data/postgres_database.cpp");
    ASSERT_FALSE(pg.empty());
    EXPECT_EQ(code_before(pg, "\"No previous aggregates found for strategy \""),
              "ErrorCode::DATABASE_ERROR");
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
        // LOOP_SPEC section 3.1: the book is sized on the half-compounded capital the read returns,
        // never on the account's rebuilt value.
        EXPECT_NE(blocks[i].find("portfolio->set_sizing_capital(sizing_read.capital.capital)"), npos);
        EXPECT_EQ(blocks[i].find("set_sizing_capital(sizing_equity.equity)"), npos);
        EXPECT_NE(blocks[i].find("INFO(sizing_capital_log_line("), npos);
        EXPECT_NE(blocks[i].find("WARN(sizing_capital_unsettled_log_line("), npos);
        // Day T-1 and Day T-2 closes only: the day being sized has no mark yet. Since T-ROLLX-FIX
        // commit 4 (D-B) they are the T-1 settlement on the consumed bars, built from the price
        // manager's two maps above the block, the settlement PHASE 5 finalises with.
        EXPECT_NE(blocks[i].find("t1_settlement.t1_close_prices"), npos);
        EXPECT_NE(blocks[i].find("t1_settlement.t2_close_prices"), npos);
        EXPECT_NE(blocks[i].find("t1_settlement.zero_pnl_symbols"), npos);
        const auto settlement_at = src.find("const ConsumedT1Settlement t1_settlement = consumed_t1_settlement(");
        ASSERT_NE(settlement_at, npos) << runners[i];
        EXPECT_LT(settlement_at, block_at) << runners[i] << ": the settlement is built before the sizing read";
        const std::string settlement = src.substr(settlement_at, block_at - settlement_at);
        EXPECT_NE(settlement.find("price_manager->get_all_previous_day_prices()"), npos);
        EXPECT_NE(settlement.find("price_manager->get_all_two_days_ago_prices()"), npos);
        EXPECT_EQ(blocks[i].find("current_price"), npos);
        // STEP 4's parts are read by the runner helper (T-7b-3 R-3, pinned below); the decision
        // is routed by SizingCapitalWiring.BothFuturesRunnersRouteTheSizingDecisionTheSameWay.
        const auto read_at = blocks[i].find("auto sizing_read = read_live_sizing_equity(");
        ASSERT_NE(read_at, npos) << runners[i];
        EXPECT_NE(blocks[i].find("*data_loader, *db, combined_strategy_id, coordinator_config.portfolio_id,"),
                  npos);
        EXPECT_LT(read_at, blocks[i].find("sizing_equity = sizing_read.equity;"));
        EXPECT_EQ(blocks[i].find("load_live_results"), npos) << "no read of its own in the runner";
        EXPECT_EQ(blocks[i].find("get_previous_live_aggregates"), npos);
        EXPECT_EQ(blocks[i].find("load_positions_by_date"), npos);
        EXPECT_EQ(blocks[i].find("current_portfolio_value"), npos);
        EXPECT_NE(blocks[i].find("return 1;"), npos) << "a run that cannot size refuses";
        EXPECT_NE(src.find("snapshot_risk_config.capital = Decimal(portfolio->sizing_capital());"),
                  npos)
            << runners[i] << ": the reporter measures against the capital the book was sized on";
        EXPECT_NE(src.find("SIZING_CAPITAL_CHECK"), npos) << runners[i];
    }
    EXPECT_EQ(blocks[0], blocks[1]) << "the twins' sizing blocks are byte-identical";
    // STEP 4's parts: Day T-1's row (its costs), the row before Day T-1 (date < T-1), or with no Day T-1 row
    // the row before the run date (date < now). Day T-1's own stored value is never read.
    const std::string helper = read_source("include/trade_ngin/live/live_sizing_read.hpp");
    ASSERT_FALSE(helper.empty());
    EXPECT_NE(helper.find("const auto sizing_t1 = now - std::chrono::hours(24);"), npos);
    EXPECT_NE(helper.find("data_loader.load_live_results(strategy_id, portfolio_id, sizing_t1);"), npos);
    EXPECT_NE(helper.find("t1_row_stored ? sizing_t1 : now,"), npos);
    EXPECT_NE(helper.find("t1_row_stored ? t1_row.value().daily_transaction_costs : 0.0"), npos);
    // (its code: the no-bar-day comment names the column, which already carries the costs)
    {
        std::istringstream lines(helper);
        for (std::string line; std::getline(lines, line);) {
            const auto first = line.find_first_not_of(" \t");
            if (first == npos) continue;
            const std::string text = line.substr(first);
            if (text.rfind("//", 0) == 0 || text.rfind("*", 0) == 0 || text.rfind("/*", 0) == 0) continue;
            EXPECT_EQ(text.find("current_portfolio_value"), npos) << text;
        }
    }
}

// T-ROLLX-FIX commit 5 (finding 12; D-B): the other half of the wiring. PHASE 5 finalises Day T-1 on
// the SAME settlement the sizing read was given: its T-1 map (a late roll's symbol books to its last
// consumed close), its T-2 map and its zero set. The sizing block's pin above did not cover the
// finalize call, so a call put back on the price manager's raw T-1 map would size the book on one
// settlement and store another with every test green.
TEST(SizingCapitalWiring, Phase5FinalisesOnTheSettlementTheBookWasSizedOn) {
    const char* const runners[] = {"apps/strategies/live_portfolio_conservative.cpp",
                                   "apps/strategies/live_portfolio.cpp"};
    std::string calls[2];
    for (int i = 0; i < 2; ++i) {
        const std::string src = read_source(runners[i]);
        ASSERT_FALSE(src.empty()) << runners[i];
        const std::string phase5 = between(src, "// PHASE 5: PER-STRATEGY DAY T-1 FINALIZATION",
                                           "// STEP 2: CREATE TODAY'S (Day T) POSITIONS WITH ZERO PnL");
        ASSERT_FALSE(phase5.empty()) << runners[i];
        EXPECT_NE(phase5.find("const auto& t1_zero_pnl_symbols = t1_settlement.zero_pnl_symbols;"), npos)
            << runners[i];
        EXPECT_NE(phase5.find("const auto& t2_consumed_close_prices = t1_settlement.t2_close_prices;"), npos)
            << runners[i];
        const auto call_at = phase5.find("pnl_manager->finalize_previous_day(prev_positions_vec,");
        ASSERT_NE(call_at, npos) << runners[i];
        EXPECT_EQ(phase5.find("pnl_manager->finalize_previous_day(", call_at + 1), npos)
            << runners[i] << ": one finalize call";
        const auto call_end = phase5.find(");", call_at);
        ASSERT_NE(call_end, npos) << runners[i];
        calls[i] = phase5.substr(call_at, call_end - call_at);
        const auto t1_at = calls[i].find("t1_settlement.t1_close_prices,");
        const auto t2_at = calls[i].find("t2_consumed_close_prices,");
        const auto zero_at = calls[i].find("t1_zero_pnl_symbols");
        ASSERT_NE(t1_at, npos) << runners[i] << ": PHASE 5 does not finalise on the settlement's T-1 map";
        ASSERT_NE(t2_at, npos) << runners[i];
        ASSERT_NE(zero_at, npos) << runners[i];
        EXPECT_LT(t1_at, t2_at) << runners[i] << ": T-1 prices, then T-2 prices";
        EXPECT_LT(t2_at, zero_at) << runners[i];
        EXPECT_EQ(calls[i].find("previous_day_close_prices"), npos)
            << runners[i] << ": the raw T-1 map, not the settlement's";
        EXPECT_EQ(calls[i].find("get_all_previous_day_prices"), npos) << runners[i];
    }
    EXPECT_EQ(calls[0], calls[1]) << "the twins' finalize calls are byte-identical";
}

// T-7b-3 R-3 (HD 2026-09-27 ruling 5): both runners route the sizing decision the same way. A
// failed sleeve book refuses to start (exit 1, above the metadata row, so no row); a hold marks
// the row as it is first written, runs no rebalance (the PortfolioManager keeps the seeded book),
// and drives the held-book exit code and email flag through risk_module_failure.
TEST(SizingCapitalWiring, BothFuturesRunnersRouteTheSizingDecisionTheSameWay) {
    std::string routes[2];
    const char* const runners[] = {"apps/strategies/live_portfolio_conservative.cpp",
                                   "apps/strategies/live_portfolio.cpp"};
    for (int i = 0; i < 2; ++i) {
        SCOPED_TRACE(runners[i]);
        const std::string src = read_source(runners[i]);
        ASSERT_FALSE(src.empty());
        const auto refuse = src.find("if (sizing_read.outcome == LiveSizingOutcome::kRefuseRun) {");
        const auto hold =
            src.find("} else if (sizing_read.outcome == LiveSizingOutcome::kHoldBook) {");
        const auto sized = src.find("sizing_equity = sizing_read.equity;");
        ASSERT_NE(refuse, npos) << "a failed sleeve book never refuses the run";
        ASSERT_NE(hold, npos) << "a failed equity or previous-row read never holds the book";
        ASSERT_NE(sized, npos);
        const auto refuse_return = src.find("return 1;", refuse);
        EXPECT_LT(refuse_return, hold) << "the refusal exits 1 before anything else";
        EXPECT_NE(src.find("sizing_hold = sizing_hold_refusal(sizing_read.failure);", hold), npos);
        EXPECT_LT(src.find("sizing_hold = sizing_hold_refusal(sizing_read.failure);", hold), sized);
        EXPECT_EQ(src.find("portfolio->set_sizing_capital(", hold),
                  src.find("portfolio->set_sizing_capital(", sized))
            << "a hold sets no sizing capital";

        // The row: written marked, and after the refusal (a refused run leaves no row).
        const auto upsert = src.find("db->store_live_run_metadata(");
        const auto mark = src.find(
            "portfolio_config_json = mark_risk_refusal(portfolio_config_json, *sizing_hold,");
        ASSERT_NE(mark, npos) << "a hold never marks the metadata row";
        EXPECT_LT(refuse_return, upsert);
        EXPECT_LT(mark, upsert) << "the row is written with the mark";

        // No rebalance on a hold: the PortfolioManager keeps the book seed_every_sleeve seeded.
        const auto seed = src.find("seed_every_sleeve(strategies, strategy_names, *portfolio,");
        const auto process = src.find(
            "sizing_hold ? Result<void>() : portfolio->process_market_data(strategy_feed_bars);");
        ASSERT_NE(process, npos) << "a hold still rebalances";
        EXPECT_LT(seed, process);
        EXPECT_LT(process, src.find("strategy_positions_map = portfolio->get_strategy_positions();"));

        // The exit code and the email flag: risk_module_failure, set after the PM's own detection.
        const auto flag = src.find("            risk_module_failure = sizing_hold;");
        ASSERT_NE(flag, npos);
        EXPECT_LT(src.find("}  // End of if (!skip_strategy_processing)"), flag);
        EXPECT_LT(src.find("sleeve_risk_module_failure(portfolio->last_risk_decisions());"), flag)
            << "set after the PortfolioManager's detection, which would reset it";
        EXPECT_LT(flag, src.find("subject = risk_module_failure_email_subject(subject);"));
        EXPECT_LT(flag, src.find("return live_run_exit_code(risk_module_failure);"));

        routes[i] = between(src, "        std::optional<nlohmann::json> sizing_hold;",
                            "        // STORE LIVE RUN METADATA");
        ASSERT_FALSE(routes[i].empty());
    }
    EXPECT_EQ(routes[0], routes[1]) << "the twins route the sizing decision differently";
}

TEST(SizingCapitalWiring, TheBacktestCompoundsForFuturesOnly) {
    const std::string src = read_source("src/backtest/backtest_coordinator.cpp");
    ASSERT_FALSE(src.empty());
    EXPECT_NE(src.find("size_on_equity_enabled_ = (asset_class == AssetClass::FUTURES);"), npos);
    const std::string day = between(src, "Result<void> BacktestCoordinator::process_portfolio_day(",
                                    "equity_curve.emplace_back(timestamp, portfolio_value);");
    ASSERT_FALSE(day.empty());
    EXPECT_NE(day.find("backtest_half_compounding(equity_curve, initial_capital)"), npos);
    const auto set_at = day.find("portfolio->set_sizing_capital(sizing.capital)");
    ASSERT_NE(set_at, npos);
    EXPECT_LT(set_at, day.find("portfolio->process_market_data(*signal_feed"))
        << "sized before the rebalance, on the curve as it stands (this cycle's row comes after)";
}

}  // namespace

// ------------------------------------------------------------------------------------------------
// LOOP_SPEC section 3.1 (D19): the live runner's sizing capital. The half compounding of the book's
// settled daily P&L, recomputed on every run from the stored rows before Day T-1 plus Day T-1's
// rebuilt net; on a failure path the last settled capital is kept and nothing is added for the
// unsettled day; the run after it catches up from the stored history alone.
// ------------------------------------------------------------------------------------------------

class LiveHalfCompounding : public LiveSizingReads {
protected:
    // Day T-1's rebuilt net in this fixture: +225.00 - 203.125 - 16.3516.
    const double t1_net_ = 2.0 * (7252.50 - 7230.00) * 5.0 + 1.0 * (110.703125 - 110.906250) * 1000.0 - 16.3516;
};

// A profit then a loss in the stored history, then Day T-1: the capital follows min(S_0, E + net)
// row by row, and the account on the line is the starting capital plus the settled cumulative P&L.
TEST_F(LiveHalfCompounding, TheCapitalIsTheHalfCompoundingOfTheSettledHistoryAndDayT1) {
    db_->history = {{"2026-04-21", 0.0, 0}, {"2026-04-22", 4'000.0, 2}, {"2026-04-23", -1'500.0, 2},
                    {"2026-04-24", -3'200.0, 2}};
    const auto r = read();
    ASSERT_EQ(r.outcome, LiveSizingOutcome::kSized) << outcome_of(r);
    EXPECT_EQ(db_->history_reads, 1);
    double capital = 500'000.0, cumulative = 0.0;
    for (double net : {0.0, 4'000.0, -1'500.0, -3'200.0, t1_net_}) {
        capital = std::min(500'000.0, capital + net);
        cumulative += net;
    }
    EXPECT_NEAR(r.capital.capital, capital, 1e-6);
    EXPECT_NEAR(r.capital.capital, 500'000.0 - 4'700.0 + t1_net_, 1e-6)
        << "the 4,000 profit is not sized on; the two losses and Day T-1 come off the start";
    EXPECT_NEAR(r.capital.account, 500'000.0 + cumulative, 1e-6);
    EXPECT_NEAR(r.capital.peak, 4'000.0, 1e-6);
    EXPECT_EQ(r.settled_through, "2026-04-27");
    EXPECT_EQ(r.settled_rows, 5);
    EXPECT_FALSE(r.t1_unsettled);
    const std::string line = sizing_capital_log_line("2026-04-28", r);
    EXPECT_NE(line.find("SIZING_CAPITAL date=2026-04-28 capital="), std::string::npos) << line;
    EXPECT_NE(line.find(" account="), std::string::npos);
    EXPECT_NE(line.find(" peak=4000.000000"), std::string::npos) << line;
    EXPECT_NE(line.find(" settled_through=2026-04-27"), std::string::npos) << line;
}

// Failure path 1: no T-1 closes with positions held. The last settled capital is kept, nothing is
// added for Day T-1 (not even its stored costs), and the WARN line names the date.
TEST_F(LiveHalfCompounding, NoT1ClosesKeepsTheLastSettledCapital) {
    db_->history = {{"2026-04-23", 4'000.0, 2}, {"2026-04-24", -3'200.0, 2}};
    calendar_.no_t1_closes = true;
    const auto r = read();
    ASSERT_EQ(r.outcome, LiveSizingOutcome::kSized) << outcome_of(r);
    EXPECT_TRUE(r.t1_unsettled);
    EXPECT_EQ(r.t1_unsettled_reason, "no T-1 closes");
    EXPECT_NEAR(r.capital.capital, 496'800.0, 1e-6);
    EXPECT_EQ(r.settled_through, "2026-04-24");
    const std::string warn = sizing_capital_unsettled_log_line("2026-04-28", r);
    EXPECT_NE(warn.find("SIZING_CAPITAL_UNSETTLED date=2026-04-28 unsettled=2026-04-27 "
                        "reason=\"no T-1 closes\" capital=496800.000000"),
              std::string::npos)
        << warn;
}

// The same test with a flat book is not a failure path: there is no move to measure, and Day
// T-1's net is its stored costs.
TEST_F(LiveHalfCompounding, NoT1ClosesOnAFlatBookIsNotAFailurePath) {
    db_->history = {{"2026-04-24", -3'200.0, 2}};
    db_->books.clear();
    calendar_.no_t1_closes = true;
    const auto r = read();
    ASSERT_EQ(r.outcome, LiveSizingOutcome::kSized) << outcome_of(r);
    EXPECT_FALSE(r.t1_unsettled);
    EXPECT_NEAR(r.capital.capital, 496'800.0 - 16.3516, 1e-6);
    EXPECT_EQ(r.settled_through, "2026-04-27");
}

// Failure path 2: no T-2 closes.
TEST_F(LiveHalfCompounding, NoT2ClosesKeepsTheLastSettledCapital) {
    db_->history = {{"2026-04-23", 4'000.0, 2}, {"2026-04-24", -3'200.0, 2}};
    calendar_.no_t2_closes = true;
    const auto r = read();
    ASSERT_EQ(r.outcome, LiveSizingOutcome::kSized) << outcome_of(r);
    EXPECT_TRUE(r.t1_unsettled);
    EXPECT_EQ(r.t1_unsettled_reason, "no T-2 closes");
    EXPECT_NEAR(r.capital.capital, 496'800.0, 1e-6);
    EXPECT_NE(sizing_capital_unsettled_log_line("2026-04-28", r).find("unsettled=2026-04-27"),
              std::string::npos);
}

// Failure path 3: no Day T-1 row on a book that has rows. A book's first run (no row at all) is not
// one: there is nothing to settle and the capital is the starting capital.
TEST_F(LiveHalfCompounding, NoDayT1RowKeepsTheLastSettledCapital) {
    db_->history = {{"2026-04-23", 4'000.0, 2}, {"2026-04-24", -3'200.0, 2}};
    db_->t1 = SizingReadDatabase::T1::kNoRow;
    auto r = read();
    ASSERT_EQ(r.outcome, LiveSizingOutcome::kSized) << outcome_of(r);
    EXPECT_TRUE(r.t1_unsettled);
    EXPECT_EQ(r.t1_unsettled_reason, "no Day T-1 row");
    EXPECT_NEAR(r.capital.capital, 496'800.0, 1e-6);

    db_->history.clear();
    db_->previous_value.reset();
    r = read();
    ASSERT_EQ(r.outcome, LiveSizingOutcome::kSized) << outcome_of(r);
    EXPECT_FALSE(r.t1_unsettled) << "a book's first run has nothing unsettled";
    EXPECT_DOUBLE_EQ(r.capital.capital, 500'000.0);
    EXPECT_EQ(r.settled_through, "none");
}

// The no-bar-day rule. A held day on which no symbol prints (a Saturday) is never finalized: its
// stored daily_pnl, the costs of that day's fills, is final. It stays out only while no bar dated
// after it is loaded, and from the next run that has a later bar it counts in its own date's place.
//
// The nets are chosen so that the PLACE matters: the cap at 500,000 binds in the middle of the
// sequence. In date order the capital is 500,000 (the 4,000 profit is not sized on), 499,997,
// 499,957 after the Saturday's 40.00, and 499,962.5234 after Day T-1's 5.5234. With the late day
// appended after Day T-1 instead, Day T-1's profit would be cut off at the cap first (499,997 +
// 5.5234 capped to 500,000) and the answer would be 499,960: 2.5234 less.
TEST_F(LiveHalfCompounding, ANoBarDayCountsFromTheNextRunThatHasALaterBar) {
    // 2026-04-25 is a Saturday: the book held positions, its fills cost 40.00, no bar is dated it.
    db_->history = {{"2026-04-23", 4'000.0, 2}, {"2026-04-24", -3.0, 2}, {"2026-04-25", -40.0, 2}};
    // Read 1: no bar after the Saturday is loaded yet (Day T-1 has no closes either).
    for (const char* later : {"2026-04-27", "2026-04-28", "2026-04-29", "2026-04-30"}) {
        calendar_.bar_dates.erase(later);
    }
    calendar_.no_t1_closes = true;
    auto r = read();
    ASSERT_EQ(r.outcome, LiveSizingOutcome::kSized) << outcome_of(r);
    ASSERT_EQ(r.earlier_unsettled, std::vector<std::string>{"2026-04-25"});
    EXPECT_TRUE(r.t1_unsettled);
    EXPECT_NEAR(r.capital.capital, 499'997.0, 1e-6) << "no later bar: the Saturday adds nothing yet";
    EXPECT_NE(sizing_capital_log_line("2026-04-28", r).find(" earlier_unsettled=1 "), std::string::npos);

    // Read 2: a bar dated after the Saturday is loaded. No bar is dated the Saturday itself, and
    // none ever will be.
    calendar_.bar_dates.insert("2026-04-27");
    calendar_.no_t1_closes = false;
    r = read();
    EXPECT_TRUE(r.earlier_unsettled.empty()) << "the Saturday counts once a later bar is loaded";
    EXPECT_EQ(calendar_.bar_dates.count("2026-04-25"), 0u);
    double in_its_place = 500'000.0, appended_last = 500'000.0;
    for (double net : {4'000.0, -3.0, -40.0, t1_net_}) in_its_place = std::min(500'000.0, in_its_place + net);
    for (double net : {4'000.0, -3.0, t1_net_, -40.0}) appended_last = std::min(500'000.0, appended_last + net);
    ASSERT_NEAR(in_its_place, 499'962.5234, 1e-6);
    ASSERT_NEAR(appended_last, 499'960.0, 1e-6);
    ASSERT_GT(in_its_place - appended_last, 2.5) << "the fixture tells the two orders apart";
    EXPECT_NEAR(r.capital.capital, in_its_place, 1e-6) << "the Saturday's costs in its own date's place";
    EXPECT_GT(std::abs(r.capital.capital - appended_last), 2.5) << "not appended after Day T-1";
    EXPECT_NEAR(r.capital.account, 500'000.0 + 4'000.0 - 3.0 - 40.0 + t1_net_, 1e-6)
        << "the account is the stored one: the Saturday's costs are in it";
    EXPECT_EQ(r.settled_rows, 4);

    // A day with a FLAT book is settled whatever is loaded: with no bar dated after it either
    // (the later dates erased again), only the flat book settles it. The same day with a held
    // book and the same calendar is the unsettled one of read 1.
    calendar_.bar_dates.erase("2026-04-27");
    db_->history = {{"2026-04-25", -40.0, 0}};
    r = read();
    EXPECT_TRUE(r.earlier_unsettled.empty()) << "a flat book has nothing to settle";
    EXPECT_NEAR(r.capital.capital, 500'000.0 - 40.0 + t1_net_, 1e-6);
    db_->history = {{"2026-04-25", -40.0, 2}};
    r = read();
    EXPECT_EQ(r.earlier_unsettled, std::vector<std::string>{"2026-04-25"});
    EXPECT_NEAR(r.capital.capital, 500'000.0, 1e-6) << "the 40.00 stays out; Day T-1's profit is capped";
}

// The stored rows and the stored value should tell one story: the starting capital plus every
// stored daily_pnl before Day T-1 is the stored value of the row before Day T-1. When they part by
// more than a cent (a spliced or missing row in the history) the read says so, for a WARN line
// beside SIZING_CAPITAL; the capital is still the rows'.
TEST_F(LiveHalfCompounding, AHistoryThatDoesNotSumToTheStoredValueIsNamed) {
    db_->history = {{"2026-04-22", 4'000.0, 2}, {"2026-04-23", -1'500.0, 2}, {"2026-04-24", -3'200.0, 2}};
    db_->previous_value = 500'000.0 + 4'000.0 - 1'500.0 - 3'200.0;  // the rows' own sum
    auto r = read();
    ASSERT_EQ(r.outcome, LiveSizingOutcome::kSized) << outcome_of(r);
    EXPECT_TRUE(r.history_compared);
    EXPECT_NEAR(r.history_gap, 0.0, 1e-9);
    EXPECT_FALSE(sizing_history_mismatch(r));
    const double capital = r.capital.capital;

    db_->previous_value = 500'000.0 + 4'000.0 - 1'500.0 - 3'200.0 - 244.76;  // the value carries less
    r = read();
    EXPECT_NEAR(r.history_gap, 244.76, 1e-6);
    EXPECT_TRUE(sizing_history_mismatch(r));
    EXPECT_DOUBLE_EQ(r.capital.capital, capital) << "the capital is built from the rows either way";
    const std::string line = sizing_capital_history_log_line("2026-04-28", r);
    EXPECT_EQ(line.rfind("SIZING_CAPITAL_HISTORY date=2026-04-28 gap=244.760000 day_before=", 0), 0u) << line;

    db_->previous_value = 500'000.0 + 4'000.0 - 1'500.0 - 3'200.0 + 0.005;  // inside a cent
    EXPECT_FALSE(sizing_history_mismatch(read()));
    // no Day T-1 row: the row before the run date is not the row before Day T-1, nothing is compared
    db_->t1 = SizingReadDatabase::T1::kNoRow;
    EXPECT_FALSE(read().history_compared);
}

// The capital does not step when a no-bar day leaves the loaded window: the same stored history
// read with a long calendar (the Saturday inside it) and a short one (the Saturday before its
// first bar) gives one capital.
TEST_F(LiveHalfCompounding, TheCapitalDoesNotStepWhenANoBarDayLeavesTheLoadedWindow) {
    // 2024-06-01 and 2026-01-17 are Saturdays on which the book held positions and paid costs.
    db_->history = {{"2024-06-01", -75.0, 3}, {"2026-01-17", -25.0, 2}, {"2026-04-24", -3'200.0, 2}};
    const auto weekdays_from = [](int days_back) {
        LiveSizingCalendar c;
        const std::time_t t1 = 1777248000;  // 2026-04-27 00:00 UTC, Day T-1
        for (int back = days_back; back >= 0; --back) {
            const std::time_t day = t1 - static_cast<std::time_t>(back) * 86400;
            std::tm tm{};
            gmtime_r(&day, &tm);
            if (tm.tm_wday == 0 || tm.tm_wday == 6) continue;
            char buf[16];
            std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
            c.bar_dates.insert(buf);
        }
        c.first_bar_date = *c.bar_dates.begin();
        return c;
    };
    calendar_ = weekdays_from(730);
    ASSERT_LT(calendar_.first_bar_date, std::string("2024-06-01"));
    const auto wide = read();
    calendar_ = weekdays_from(100);
    ASSERT_GT(calendar_.first_bar_date, std::string("2024-06-01"));
    ASSERT_GT(calendar_.first_bar_date, std::string("2026-01-17"));
    const auto narrow = read();
    ASSERT_EQ(wide.outcome, LiveSizingOutcome::kSized) << outcome_of(wide);
    ASSERT_EQ(narrow.outcome, LiveSizingOutcome::kSized) << outcome_of(narrow);
    EXPECT_TRUE(wide.earlier_unsettled.empty());
    EXPECT_TRUE(narrow.earlier_unsettled.empty());
    EXPECT_NEAR(wide.capital.capital, 500'000.0 - 75.0 - 25.0 - 3'200.0 + t1_net_, 1e-6);
    EXPECT_DOUBLE_EQ(wide.capital.capital, narrow.capital.capital);
    EXPECT_DOUBLE_EQ(wide.capital.account, narrow.capital.account);
    EXPECT_EQ(wide.settled_rows, narrow.settled_rows);
}

// The three failure paths of Day T-1 still keep the last settled capital for their one run, and
// that capital now holds an earlier no-bar day's costs: only Day T-1 itself is withheld.
TEST_F(LiveHalfCompounding, TheFailurePathsWithholdOnlyDayT1) {
    // 2026-04-18 is a Saturday: held, 40.00 of costs, no bar; the week after it printed.
    const std::vector<std::tuple<std::string, double, int>> history = {
        {"2026-04-17", 4'000.0, 2}, {"2026-04-18", -40.0, 2}, {"2026-04-24", -3'200.0, 2}};
    const double last_settled = 500'000.0 - 40.0 - 3'200.0;
    const auto base_calendar = calendar_;
    for (const std::string path : {"no T-1 closes", "no T-2 closes", "no Day T-1 row"}) {
        db_->history = history;
        db_->t1 = SizingReadDatabase::T1::kRow;
        calendar_ = base_calendar;
        if (path == "no T-1 closes") {
            calendar_.no_t1_closes = true;
        } else if (path == "no T-2 closes") {
            calendar_.no_t2_closes = true;
        } else {
            db_->t1 = SizingReadDatabase::T1::kNoRow;
        }
        const auto r = read();
        ASSERT_EQ(r.outcome, LiveSizingOutcome::kSized) << path << ": " << outcome_of(r);
        EXPECT_TRUE(r.t1_unsettled) << path;
        EXPECT_EQ(r.t1_unsettled_reason, path);
        EXPECT_TRUE(r.earlier_unsettled.empty()) << path;
        EXPECT_NEAR(r.capital.capital, last_settled, 1e-6) << path;
        EXPECT_EQ(r.settled_through, "2026-04-24") << path;
        EXPECT_EQ(r.settled_rows, 3) << path;
    }
}

// ------------------------------------------------------------------------------------------------
// Migration 029 (T-8a commit (12)): a stored day counts in the sizing capital when its settled_at
// is set OR the no-bar-day rule calls it settled. The stamp is written after STEP 4, so the run
// after a no-prices day sizes before its own stamp exists.
// ------------------------------------------------------------------------------------------------

// The CONSERVATIVE chain's Monday, 2026-04-27. The book has 203 stored rows before Day T-1
// (2025-10-05 .. 2026-04-25). Saturday's run stamped everything through Friday 04-24; Sunday's run
// had no closes for the held Saturday (the no-prices skip) and stamped nothing, so the Saturday
// row is still NULL. Monday loads Sunday's bar: the Saturday row counts, settled_rows is 204 and
// earlier_unsettled 0, the figures of the run before the column existed.
TEST_F(LiveHalfCompounding, TheMondayAfterAHeldSaturdayCountsTheSaturdayRowBeforeItsStamp) {
    LiveSizingCalendar calendar;
    db_->history.clear();
    double capital = 500'000.0;
    const std::chrono::sys_days first = std::chrono::year{2025} / 10 / 5;
    const std::chrono::sys_days saturday = std::chrono::year{2026} / 4 / 25;
    for (auto day = first; day <= saturday + std::chrono::days{1}; day += std::chrono::days{1}) {
        const std::chrono::year_month_day ymd{day};
        char date[16];
        std::snprintf(date, sizeof(date), "%04d-%02u-%02u", static_cast<int>(ymd.year()),
                      static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()));
        const bool is_saturday = std::chrono::weekday{day} == std::chrono::Saturday;
        if (!is_saturday) calendar.bar_dates.insert(date);  // bars are dated Sunday to Friday
        if (day > saturday) break;                          // 04-26 is Day T-1, not history
        const double pnl = is_saturday ? -12.5 : (day == first ? 0.0 : -1.0);
        db_->history.emplace_back(date, pnl, 15);
        if (day < saturday) db_->stamped.insert(date);
        capital = std::min(500'000.0, capital + pnl);
    }
    calendar.first_bar_date = *calendar.bar_dates.begin();
    ASSERT_EQ(db_->history.size(), 203u);
    ASSERT_EQ(db_->stamped.count("2026-04-25"), 0u);
    ASSERT_EQ(calendar.bar_dates.count("2026-04-25"), 0u);
    ASSERT_EQ(calendar.bar_dates.count("2026-04-26"), 1u);

    const Timestamp monday = Timestamp(std::chrono::seconds(1777334400LL - 86400LL));  // 2026-04-27
    const auto r = read_live_sizing_equity(
        *loader_, *db_, "LIVE_TREND_FOLLOWING", "CONSERVATIVE_PORTFOLIO", {"A", "B"}, monday,
        500'000.0, {{"MES.v.0", 7252.50}, {"ZN.v.0", 110.703125}},
        {{"MES.v.0", 7230.00}, {"ZN.v.0", 110.906250}},
        [&](const std::string& s) { return pnl_.get_point_value(s); }, {}, calendar);
    ASSERT_EQ(r.outcome, LiveSizingOutcome::kSized) << outcome_of(r);
    EXPECT_EQ(r.t1_date, "2026-04-26");
    EXPECT_FALSE(r.t1_unsettled);
    EXPECT_TRUE(r.earlier_unsettled.empty())
        << "the held Saturday 2026-04-25 was dropped: its stamp is written only after this read";
    EXPECT_EQ(r.settled_rows, 204);
    EXPECT_EQ(r.settled_through, "2026-04-26");
    EXPECT_NEAR(r.capital.capital, std::min(500'000.0, capital + t1_net_), 1e-6)
        << "the Saturday's 12.50 of costs is in the capital, in its own date's place";
    const std::string line = sizing_capital_log_line("2026-04-27", r);
    EXPECT_NE(line.find(" settled_rows=204 earlier_unsettled=0 "), std::string::npos) << line;
}

// LOOP_SPEC section 15 erratum 1: a row that carries a stamp counts whatever the calendar says.
// 2026-04-25 is a held Saturday with no later bar loaded, the day the no-bar-day rule alone calls
// unsettled; with the migration's backfill on it (the row had a later row at migration) it counts.
TEST_F(LiveHalfCompounding, AStampedRowCountsWhereTheCalendarAloneWouldNot) {
    db_->history = {{"2026-04-23", 4'000.0, 2}, {"2026-04-24", -3.0, 2}, {"2026-04-25", -40.0, 2}};
    for (const char* later : {"2026-04-27", "2026-04-28", "2026-04-29", "2026-04-30"}) {
        calendar_.bar_dates.erase(later);
    }
    calendar_.no_t1_closes = true;
    auto r = read();
    ASSERT_EQ(r.outcome, LiveSizingOutcome::kSized) << outcome_of(r);
    ASSERT_EQ(r.earlier_unsettled, std::vector<std::string>{"2026-04-25"})
        << "control: unstamped, the calendar calls the Saturday unsettled";
    ASSERT_NEAR(r.capital.capital, 499'997.0, 1e-6);

    db_->stamped = {"2026-04-23", "2026-04-24", "2026-04-25"};
    r = read();
    ASSERT_EQ(r.outcome, LiveSizingOutcome::kSized) << outcome_of(r);
    EXPECT_TRUE(r.earlier_unsettled.empty()) << "a stamped row is settled: the stamp overrides";
    EXPECT_NEAR(r.capital.capital, 499'957.0, 1e-6) << "500,000 - 3.00 - 40.00 (the 4,000 is capped)";
    EXPECT_EQ(r.settled_through, "2026-04-25");
    EXPECT_EQ(r.settled_rows, 3);
}

// Neither limb: no stamp, and the calendar calls the day unsettled. It does not count, whatever
// the rows around it carry.
TEST_F(LiveHalfCompounding, ARowWithNoStampAndOffTheCalendarRuleDoesNotCount) {
    db_->history = {{"2026-04-23", 4'000.0, 2}, {"2026-04-24", -3.0, 2}, {"2026-04-25", -40.0, 2}};
    db_->stamped = {"2026-04-23", "2026-04-24"};
    for (const char* later : {"2026-04-27", "2026-04-28", "2026-04-29", "2026-04-30"}) {
        calendar_.bar_dates.erase(later);
    }
    calendar_.no_t1_closes = true;
    const auto r = read();
    ASSERT_EQ(r.outcome, LiveSizingOutcome::kSized) << outcome_of(r);
    EXPECT_EQ(r.earlier_unsettled, std::vector<std::string>{"2026-04-25"});
    EXPECT_NEAR(r.capital.capital, 499'997.0, 1e-6) << "the Saturday's 40.00 stays out";
    EXPECT_EQ(r.settled_through, "2026-04-24");
    EXPECT_EQ(r.settled_rows, 2);
}

// A history that cannot be read is an equity read that failed: the book is held, never sized on a
// capital built from part of the history.
TEST_F(LiveHalfCompounding, AFailedHistoryReadHoldsTheBook) {
    db_->history_error = true;
    const auto r = read();
    ASSERT_EQ(r.outcome, LiveSizingOutcome::kHoldBook) << outcome_of(r);
    EXPECT_NE(r.failure.find("the stored P&L history before Day T-1 could not be read"),
              std::string::npos)
        << r.failure;
}
