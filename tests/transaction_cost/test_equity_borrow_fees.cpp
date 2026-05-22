#include <gtest/gtest.h>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/instruments/equity.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/transaction_cost/transaction_cost_manager.hpp"

using namespace trade_ngin;
using namespace trade_ngin::transaction_cost;
using namespace trade_ngin::testing;

// Audit tests T2.6, T2.7, T2.8 -- borrow fee model on TCM (§3.2).
// Multi-factor risk scoring (dollar-volume + price + is_easy_to_borrow flag)
// × volatility multiplier × short notional / 365. Per-symbol
// borrow_rate_override bypasses the formula.

namespace {

// Synthesize bars at constant price/volume so ADV is exactly `volume`.
std::vector<Bar> bars_for(const std::string& symbol, int days, double price, double volume) {
    std::vector<Bar> bars;
    bars.reserve(days);
    auto now = std::chrono::system_clock::now();
    for (int i = 0; i < days; ++i) {
        Bar b;
        b.symbol = symbol;
        b.timestamp = now - std::chrono::hours(24 * (days - i));
        b.open = price;
        b.high = price * 1.001;
        b.low = price * 0.999;
        b.close = price;
        b.volume = volume;
        bars.push_back(b);
    }
    return bars;
}

EquitySpec regt_spec(bool shorts = true) {
    EquitySpec spec;
    spec.exchange = "NASDAQ";
    spec.currency = "USD";
    spec.tick_size = 0.01;
    spec.account_mode = EquityAccountMode::REG_T;
    spec.short_selling_allowed = shorts;
    return spec;
}

class BorrowFeesTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        auto& registry = InstrumentRegistry::instance();
        auto db = std::make_shared<MockPostgresDatabase>("mock://borrow_fees_test");
        ASSERT_TRUE(db->connect().is_ok());
        (void)registry.initialize(db);  // Idempotent.
    }
};

}  // namespace

// T2.6: REG_T short 100 AAPL @ $150, mega-cap (ADV >10M -> dollar volume
// >$1.5B/day, no flag), price > $10 (no flag), default is_easy_to_borrow=true
// (no flag) -> 0 flags -> 25 bps base. Annual vol 25% -> multiplier 1.0.
// Daily fee = 0.0025 * $15,000 / 365 ~= $0.1027.
TEST_F(BorrowFeesTest, MegaCapShortYieldsLowFee) {
    TransactionCostManager::Config tcm_config;
    TransactionCostManager tcm(tcm_config);
    auto& registry = InstrumentRegistry::instance();

    const std::string sym = "PHASE2_T26_AAPL";
    registry.register_instrument(sym, std::make_shared<EquityInstrument>(sym, regt_spec()));

    // Register equity cost config so impact_model knows the ADV.
    std::unordered_map<std::string, std::vector<Bar>> bars;
    bars[sym] = bars_for(sym, 30, 150.0, 50'000'000.0);
    tcm.register_equity_costs_from_bars({sym}, bars);

    // Feed log returns to seed the vol estimate at 25% annualized.
    // daily_log_return for 25% annual vol = 0.25/sqrt(252) ~ 0.01575.
    const double daily_ret = 0.25 / std::sqrt(252.0);
    for (int i = 0; i < 22; ++i) {
        const double r = (i % 2 == 0) ? daily_ret : -daily_ret;
        tcm.update_market_data(sym, 50'000'000.0, 150.0 + r * 150.0, 150.0);
    }

    // Short 100 shares.
    std::unordered_map<std::string, Position> positions;
    Position p;
    p.symbol = sym;
    p.quantity = -100.0;
    p.average_price = 150.0;
    positions[sym] = p;

    std::unordered_map<std::string, double> prices;
    prices[sym] = 150.0;

    auto fees = tcm.calculate_overnight_borrow_fees(positions, prices, registry);
    ASSERT_EQ(fees.size(), 1u);

    const double expected = 0.0025 * 15000.0 / 365.0;  // ~0.1027
    EXPECT_NEAR(fees.at(sym), expected, 0.05)
        << "Mega-cap short fee should be ~$0.10/day (25 bps × $15K / 365); "
           "got " << fees.at(sym);
}

// T2.7: short 100 of small-cap @ $4. ADV 80K -> dollar volume $320K/day -> 1 flag.
// Price < $5 -> 1 flag. is_easy_to_borrow=true so no extra flag. Total 2 flags
// -> 150 bps base. Annual vol set to 75% -> raw multiplier 3.0 (capped).
// Annual rate = 0.015 × 3.0 = 0.045 (4.5%). Daily fee = 0.045 × $400 / 365 ~= $0.0493.
TEST_F(BorrowFeesTest, SmallCapHighVolYieldsScaledFee) {
    TransactionCostManager::Config tcm_config;
    TransactionCostManager tcm(tcm_config);
    auto& registry = InstrumentRegistry::instance();

    const std::string sym = "PHASE2_T27_SMALLCAP";
    registry.register_instrument(sym, std::make_shared<EquityInstrument>(sym, regt_spec()));

    std::unordered_map<std::string, std::vector<Bar>> bars;
    bars[sym] = bars_for(sym, 30, 4.0, 80'000.0);
    tcm.register_equity_costs_from_bars({sym}, bars);

    // Seed 75% annual vol via daily log returns.
    const double daily_ret = 0.75 / std::sqrt(252.0);
    for (int i = 0; i < 22; ++i) {
        const double r = (i % 2 == 0) ? daily_ret : -daily_ret;
        tcm.update_market_data(sym, 80'000.0, 4.0 + r * 4.0, 4.0);
    }

    std::unordered_map<std::string, Position> positions;
    Position p;
    p.symbol = sym;
    p.quantity = -100.0;
    p.average_price = 4.0;
    positions[sym] = p;

    std::unordered_map<std::string, double> prices;
    prices[sym] = 4.0;

    auto fees = tcm.calculate_overnight_borrow_fees(positions, prices, registry);
    ASSERT_EQ(fees.size(), 1u);

    // 2 flags -> 150 bps base. 75% vol -> 3× cap. Annual rate = 0.045.
    // Daily fee = 0.045 × $400 / 365 ~= $0.0493
    const double expected = 0.045 * 400.0 / 365.0;
    EXPECT_NEAR(fees.at(sym), expected, 0.02)
        << "Small-cap high-vol short should scale via 2-flag base × 3× vol mult; "
           "got " << fees.at(sym);
}

// T2.8: borrow_rate_override = 0.50 (50% annual). Formula is bypassed entirely.
// Daily fee = 0.50 × $15,000 / 365 ~= $20.55.
TEST_F(BorrowFeesTest, OverrideBypassesFormula) {
    TransactionCostManager::Config tcm_config;
    TransactionCostManager tcm(tcm_config);
    auto& registry = InstrumentRegistry::instance();

    const std::string sym = "PHASE2_T28_OVERRIDE";
    EquitySpec spec = regt_spec();
    spec.borrow_rate_override = 0.50;
    registry.register_instrument(sym, std::make_shared<EquityInstrument>(sym, spec));

    // No ADV / vol seeding -- override means formula isn't consulted.

    std::unordered_map<std::string, Position> positions;
    Position p;
    p.symbol = sym;
    p.quantity = -100.0;
    p.average_price = 150.0;
    positions[sym] = p;

    std::unordered_map<std::string, double> prices;
    prices[sym] = 150.0;

    auto fees = tcm.calculate_overnight_borrow_fees(positions, prices, registry);
    ASSERT_EQ(fees.size(), 1u);

    const double expected = 0.50 * 15000.0 / 365.0;  // ~$20.55
    EXPECT_NEAR(fees.at(sym), expected, 0.05)
        << "Override should bypass formula; expected " << expected
        << ", got " << fees.at(sym);
}

// Longs and non-equities should be skipped (no borrow fee).
TEST_F(BorrowFeesTest, LongsAndNonEquitiesAreSkipped) {
    TransactionCostManager::Config tcm_config;
    TransactionCostManager tcm(tcm_config);
    auto& registry = InstrumentRegistry::instance();

    const std::string sym = "PHASE2_T28_LONG";
    registry.register_instrument(sym, std::make_shared<EquityInstrument>(sym, regt_spec()));

    std::unordered_map<std::string, Position> positions;
    Position p;
    p.symbol = sym;
    p.quantity = 100.0;  // Long.
    p.average_price = 150.0;
    positions[sym] = p;

    std::unordered_map<std::string, double> prices;
    prices[sym] = 150.0;

    auto fees = tcm.calculate_overnight_borrow_fees(positions, prices, registry);
    EXPECT_TRUE(fees.empty()) << "Long positions must not accrue borrow fees.";
}
