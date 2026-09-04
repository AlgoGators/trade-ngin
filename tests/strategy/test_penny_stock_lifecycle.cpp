// tests/strategy/test_penny_stock_lifecycle.cpp
//
// T-OR.2 -- a sub-dollar name, end to end: sizing -> cost registration -> cost
// calculation -> fill -> P&L.
//
// The only coverage a penny stock had was one lookup of get_tiered_equity_config(0.50,
// 50000) in test_equity_pnl_accounting.cpp -- a table read, not a lifecycle. Everything
// downstream of it is live: register_equity_costs_from_bars is called by the live runner,
// the backtest coordinator and PortfolioManager, and the sizing gates that keep a $0.25
// name out of fractional shares sit in MeanReversionStrategy::calculate_position_size.
//
// A sub-dollar stock is where four rules meet that never meet on a $150 stock:
//   * fractional shares are refused below `fractional_min_price`, so the target floors;
//   * the position limit binds in SHARES, and $10k of a $0.25 stock is 40,000 of them;
//   * SEC Rule 612 makes the tick 0.0001 rather than 0.01, so the spread model prices
//     off a different grid;
//   * the commission is $0.005/share against a 1% cap on trade value, and below $0.50 a
//     share the CAP is the binding term -- the case E2-C2 reordered the expression for.
//
// Scenario pin: no red is expected today. It exists so a change to any of those four
// rules that is harmless at $150 cannot pass unnoticed at $0.25.

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include "trade_ngin/strategy/mean_reversion.hpp"
#include "trade_ngin/transaction_cost/asset_cost_config.hpp"
#include "trade_ngin/transaction_cost/transaction_cost_manager.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;
using trade_ngin::transaction_cost::TransactionCostManager;

class PennyStockLifecycleTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        StateManager::reset_instance();

        db_ = std::make_shared<MockPostgresDatabase>("mock://testdb");
        ASSERT_TRUE(db_->connect().is_ok());

        strategy_config_.capital_allocation = 100000.0;
        strategy_config_.max_leverage = 1.0;
        strategy_config_.asset_classes = {AssetClass::EQUITIES};
        strategy_config_.frequencies = {DataFrequency::DAILY};
        for (const auto& symbol : {kPenny, kSubDollar, kNormal}) {
            strategy_config_.trading_params[symbol] = 1.0;
            // $10,000 of a $0.25 stock is 40,000 shares; the limit is in SHARES.
            strategy_config_.position_limits[symbol] = 40000.0;
        }

        mr_config_.lookback_period = 20;
        mr_config_.vol_lookback = 20;
        mr_config_.entry_threshold = 2.0;
        mr_config_.exit_threshold = 0.5;
        mr_config_.risk_target = 0.15;
        mr_config_.position_size = 0.1;
        mr_config_.use_stop_loss = false;
        mr_config_.allow_fractional_shares = true;
        mr_config_.fractional_min_price = 1.0;
        mr_config_.fractional_min_adv = 50000.0;

        risk_limits_.max_position_size = 1e9;
        risk_limits_.max_notional_value = 1e9;
        risk_limits_.max_drawdown = 0.9;
        risk_limits_.max_leverage = 10.0;
    }

    void TearDown() override {
        if (db_) {
            db_->disconnect();
            db_.reset();
        }
        TestBase::TearDown();
    }

    std::unique_ptr<MeanReversionStrategy> create_strategy() {
        static int id = 0;
        auto strategy = std::make_unique<MeanReversionStrategy>(
            "TEST_PENNY_" + std::to_string(++id), strategy_config_, mr_config_, db_);
        EXPECT_TRUE(strategy->initialize().is_ok());
        EXPECT_TRUE(strategy->update_risk_limits(risk_limits_).is_ok());
        EXPECT_TRUE(strategy->start().is_ok());
        return strategy;
    }

    // 60 bars oscillating around `level`, closing well below the mean so the entry
    // branch fires long. Volume is constant so ADV is exactly `volume`.
    std::vector<Bar> series(const std::string& symbol, double level, double volume) const {
        std::vector<Bar> bars;
        const auto now = std::chrono::system_clock::now();
        for (int i = 0; i < 59; ++i) {
            const double close = (i % 2 == 0) ? level * 0.98 : level * 1.02;
            bars.push_back(bar(symbol, close, volume, now - std::chrono::hours(24 * (60 - i))));
        }
        bars.push_back(bar(symbol, level * 0.90, volume, now));  // well below the mean
        return bars;
    }

    Bar bar(const std::string& symbol, double close, double volume, Timestamp ts) const {
        Bar b;
        b.symbol = symbol;
        b.timestamp = ts;
        b.open = close;
        b.high = close;
        b.low = close;
        b.close = close;
        b.volume = volume;
        return b;
    }

    static constexpr const char* kPenny = "PENNYA";      // $0.25
    static constexpr const char* kSubDollar = "PENNYB";  // $0.50
    static constexpr const char* kNormal = "NORMAL";     // $150, the control

    std::shared_ptr<MockPostgresDatabase> db_;
    StrategyConfig strategy_config_;
    RiskLimits risk_limits_;
    MeanReversionConfig mr_config_;
};

// ---------------------------------------------------------------------------
// sizing
// ---------------------------------------------------------------------------

// Below fractional_min_price the target is a whole number of shares, however large.
// Fractional sizing on a $0.25 name is not a rounding nicety: brokers do not fill it,
// so a fractional target would be a position the book claims and does not have.
TEST_F(PennyStockLifecycleTest, SubDollarTargetsAreWholeShares) {
    auto strategy = create_strategy();
    for (const auto& symbol : {kPenny, kSubDollar}) {
        ASSERT_TRUE(strategy->on_data(series(symbol, symbol == std::string(kPenny) ? 0.25 : 0.50,
                                             500000.0))
                        .is_ok());
    }
    ASSERT_TRUE(strategy->on_data(series(kNormal, 150.0, 500000.0)).is_ok());

    for (const auto& symbol : {kPenny, kSubDollar}) {
        const double target = strategy->get_position(symbol);
        ASSERT_NE(target, 0.0) << symbol << " did not open; the scenario proves nothing";
        EXPECT_DOUBLE_EQ(target, std::floor(target))
            << symbol << " priced below fractional_min_price must size in whole shares";
    }

    // The control: the same code path leaves a $150 name free to hold a fraction, so the
    // assertion above is about the price gate and not about rounding everywhere.
    const double normal_target = strategy->get_position(kNormal);
    ASSERT_NE(normal_target, 0.0);
    EXPECT_NE(normal_target, std::floor(normal_target))
        << "a $150 name with ADV above the floor is fractional-eligible";
}

// ADV below fractional_min_adv fails CLOSED even above the price floor: an unreliable
// liquidity estimate is not a licence to place an order the venue may not accept.
TEST_F(PennyStockLifecycleTest, ThinADVForcesWholeSharesEvenAboveThePriceFloor) {
    auto strategy = create_strategy();
    ASSERT_TRUE(strategy->on_data(series(kNormal, 150.0, 10000.0)).is_ok());  // ADV 10k < 50k

    const double target = strategy->get_position(kNormal);
    ASSERT_NE(target, 0.0);
    EXPECT_DOUBLE_EQ(target, std::floor(target));
}

// `position_limits` is expressed in SHARES, and a share count means something very
// different at $0.25 than at $150: the live config's 500-share live limit is $75,000 of
// a $150 name and $125 of a penny name. This pins that the clamp is applied in share
// space -- the unclamped volatility-targeted size is measured first, so the assertion
// cannot pass by the limit happening to sit above it.
TEST_F(PennyStockLifecycleTest, PositionLimitClampBindsInSharesOnAPennyName) {
    double unclamped = 0.0;
    {
        auto strategy = create_strategy();  // limit 40,000, well above the sized target
        ASSERT_TRUE(strategy->on_data(series(kPenny, 0.25, 500000.0)).is_ok());
        unclamped = strategy->get_position(kPenny);
        ASSERT_GT(unclamped, 0.0);
        ASSERT_LT(unclamped, strategy_config_.position_limits.at(kPenny))
            << "fixture precondition: the default limit must NOT bind, or the clamp below "
               "proves nothing";
    }

    const double tight = std::floor(unclamped / 2.0);
    strategy_config_.position_limits[kPenny] = tight;
    auto strategy = create_strategy();
    ASSERT_TRUE(strategy->on_data(series(kPenny, 0.25, 500000.0)).is_ok());

    EXPECT_DOUBLE_EQ(strategy->get_position(kPenny), tight)
        << "the share-count clamp must cut the target, not the notional";
}

// ---------------------------------------------------------------------------
// cost registration
// ---------------------------------------------------------------------------

// The tier a penny name gets comes from its own bars, not from a default. SEC Rule 612
// puts sub-dollar quotes on a 0.0001 grid, and the spread model prices in ticks, so the
// tick size is the whole difference between a plausible spread and a 100x one.
TEST_F(PennyStockLifecycleTest, RegisterEquityCostsFromBarsGivesAPennyNameThePennyTier) {
    TransactionCostManager tcm;
    std::unordered_map<std::string, std::vector<Bar>> bars_by_symbol;
    bars_by_symbol[kPenny] = series(kPenny, 0.25, 50000.0);
    bars_by_symbol[kNormal] = series(kNormal, 150.0, 50000000.0);

    const int registered = tcm.register_equity_costs_from_bars(
        {kPenny, kNormal}, bars_by_symbol);
    ASSERT_EQ(registered, 2);

    const auto penny = tcm.get_asset_config(kPenny);
    EXPECT_EQ(penny.asset_type, AssetType::EQUITY)
        << "an unregistered equity resolves as a FUTURE: $1.50/share and point_value 100";
    EXPECT_DOUBLE_EQ(penny.tick_size, 0.0001) << "SEC Rule 612 sub-dollar tick";
    EXPECT_DOUBLE_EQ(penny.point_value, 1.0);
    EXPECT_DOUBLE_EQ(penny.baseline_spread_ticks, 10.0) << "penny / illiquid tier";
    EXPECT_DOUBLE_EQ(penny.max_total_implicit_bps, 500.0);

    const auto normal = tcm.get_asset_config(kNormal);
    EXPECT_DOUBLE_EQ(normal.tick_size, 0.01);
    EXPECT_DOUBLE_EQ(normal.baseline_spread_ticks, 1.0) << "mega-cap tier from ADV 50m";
}

// ---------------------------------------------------------------------------
// cost calculation
// ---------------------------------------------------------------------------

// The 1%-of-trade-value CAP is the binding commission term on a penny clip, and it must
// win over the $1.00 per-order floor. 40,000 shares at $0.25 is $10,000 of value: raw
// per-share commission is $200, the cap is $100. Getting the order of these two wrong is
// exactly E2-C2, which over-charged the smallest clips by 21%.
TEST_F(PennyStockLifecycleTest, OnePercentCapBindsOnAPennyClipAndBeatsTheFloor) {
    TransactionCostManager tcm;
    std::unordered_map<std::string, std::vector<Bar>> bars_by_symbol;
    bars_by_symbol[kPenny] = series(kPenny, 0.25, 50000.0);
    ASSERT_EQ(tcm.register_equity_costs_from_bars({kPenny}, bars_by_symbol), 1);

    const double qty = 40000.0;
    const double price = 0.25;
    const auto cost = tcm.calculate_costs(kPenny, qty, price, 50000.0, 1.0, AssetType::EQUITY);

    EXPECT_DOUBLE_EQ(cost.commissions_fees, 0.01 * qty * price)
        << "raw per-share commission is $" << qty * 0.005 << "; the 1% cap is $"
        << 0.01 * qty * price << " and a cap that a floor can exceed is not a cap";
    EXPECT_LT(cost.commissions_fees, qty * 0.005);

    // The other side of the same expression: a tiny clip is capped BELOW the $1.00 floor.
    const auto tiny = tcm.calculate_costs(kPenny, 100.0, price, 50000.0, 1.0,
                                          AssetType::EQUITY);
    EXPECT_DOUBLE_EQ(tiny.commissions_fees, 0.01 * 100.0 * price);
    EXPECT_LT(tiny.commissions_fees, 1.00) << "the floor must not override the cap";
}

// Implicit cost is bounded by the tier's max_total_implicit_bps even when a large clip
// meets a thin book -- 40,000 shares against ADV 50,000 is 80% of a day's volume.
TEST_F(PennyStockLifecycleTest, ImplicitCostOnAPennyNameIsBoundedByItsTier) {
    TransactionCostManager tcm;
    std::unordered_map<std::string, std::vector<Bar>> bars_by_symbol;
    bars_by_symbol[kPenny] = series(kPenny, 0.25, 50000.0);
    ASSERT_EQ(tcm.register_equity_costs_from_bars({kPenny}, bars_by_symbol), 1);

    const double qty = 40000.0;
    const double price = 0.25;
    const auto cost = tcm.calculate_costs(kPenny, qty, price, 50000.0, 1.0, AssetType::EQUITY);

    const double implicit_bps = (cost.implicit_price_impact / price) * 10000.0;
    EXPECT_GT(implicit_bps, 0.0) << "a penny name trading 80% of ADV is not free";
    EXPECT_LE(implicit_bps, 500.0 + 1e-9)
        << "penny tier max_total_implicit_bps is 500 and must actually bind";
    EXPECT_NEAR(cost.slippage_market_impact, cost.implicit_price_impact * qty * 1.0, 1e-9)
        << "equities are point_value 1; a futures multiplier here would be 100x";
    EXPECT_NEAR(cost.total_transaction_costs,
                cost.commissions_fees + cost.slippage_market_impact, 1e-9);
}

// ---------------------------------------------------------------------------
// fill and P&L
// ---------------------------------------------------------------------------

// The round trip on a penny name: 40,000 shares in at $0.25, out at $0.30, realized
// $2,000 with no basis left behind. Whole-share quantities and a sub-dollar basis are
// where a fixed-point rounding mistake would show up first.
TEST_F(PennyStockLifecycleTest, RoundTripRealizesOnTheWholeShareQuantity) {
    auto strategy = create_strategy();
    ASSERT_TRUE(strategy->on_data(series(kPenny, 0.25, 500000.0)).is_ok());

    ExecutionReport buy;
    buy.symbol = kPenny;
    buy.order_id = "PENNY_BUY";
    buy.exec_id = "PENNY_BUY_1";
    buy.side = Side::BUY;
    buy.filled_quantity = 40000.0;
    buy.fill_price = 0.25;
    buy.fill_time = std::chrono::system_clock::now();
    ASSERT_TRUE(strategy->on_execution(buy).is_ok());

    auto book = strategy->get_positions();
    ASSERT_EQ(book.count(kPenny), 1u);
    EXPECT_DOUBLE_EQ(book.at(kPenny).quantity.as_double(), 40000.0);
    EXPECT_DOUBLE_EQ(book.at(kPenny).average_price.as_double(), 0.25);
    EXPECT_DOUBLE_EQ(book.at(kPenny).realized_pnl.as_double(), 0.0);

    ExecutionReport sell = buy;
    sell.order_id = "PENNY_SELL";
    sell.exec_id = "PENNY_SELL_1";
    sell.side = Side::SELL;
    sell.fill_price = 0.30;
    ASSERT_TRUE(strategy->on_execution(sell).is_ok());

    book = strategy->get_positions();
    ASSERT_EQ(book.count(kPenny), 1u);
    EXPECT_DOUBLE_EQ(book.at(kPenny).quantity.as_double(), 0.0);
    EXPECT_NEAR(book.at(kPenny).realized_pnl.as_double(), 40000.0 * (0.30 - 0.25), 1e-6);
}

// A partial exit realizes on the shares that left and keeps the basis on the rest --
// the E2-F27 rule, exercised where the numbers are small enough that a per-share
// rounding error would be visible.
TEST_F(PennyStockLifecycleTest, PartialExitRealizesOnlyTheSharesThatLeft) {
    auto strategy = create_strategy();
    ASSERT_TRUE(strategy->on_data(series(kPenny, 0.25, 500000.0)).is_ok());

    ExecutionReport buy;
    buy.symbol = kPenny;
    buy.order_id = "P1";
    buy.exec_id = "P1_1";
    buy.side = Side::BUY;
    buy.filled_quantity = 40000.0;
    buy.fill_price = 0.25;
    buy.fill_time = std::chrono::system_clock::now();
    ASSERT_TRUE(strategy->on_execution(buy).is_ok());

    ExecutionReport sell = buy;
    sell.order_id = "P2";
    sell.exec_id = "P2_1";
    sell.side = Side::SELL;
    sell.filled_quantity = 15000.0;
    sell.fill_price = 0.31;
    ASSERT_TRUE(strategy->on_execution(sell).is_ok());

    const auto book = strategy->get_positions();
    ASSERT_EQ(book.count(kPenny), 1u);
    EXPECT_DOUBLE_EQ(book.at(kPenny).quantity.as_double(), 25000.0);
    EXPECT_DOUBLE_EQ(book.at(kPenny).average_price.as_double(), 0.25)
        << "a partial exit does not restate the basis of what is still held";
    EXPECT_NEAR(book.at(kPenny).realized_pnl.as_double(), 15000.0 * (0.31 - 0.25), 1e-6);
}
