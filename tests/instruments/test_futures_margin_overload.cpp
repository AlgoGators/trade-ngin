#include <gtest/gtest.h>
#include "trade_ngin/instruments/futures.hpp"

using namespace trade_ngin;

// Regression test for the post-Phase-2 ultrareview bug_001.
//
// Phase 2 changed MarginManager::extract_margin_requirements to call
// instrument->get_margin_requirement(price, quantity) directly without
// multiplying by |quantity|, on the assumption the overload returned total
// dollars. EquityInstrument's override does multiply; but FuturesInstrument
// originally only overrode the no-arg version, so the base-class default
// forwarded to per-contract margin -- under-reporting total margin by the
// contract count and producing impossible initial < maintenance states.
//
// This test pins the contract: the price/qty overload must return total
// dollars across the position (|qty| * initial_margin).

namespace {

FuturesSpec make_es_spec() {
    FuturesSpec spec;
    spec.root_symbol = "ES";
    spec.exchange = "CME";
    spec.currency = "USD";
    spec.tick_size = 0.25;
    spec.multiplier = 50.0;
    spec.initial_margin = 12000.0;
    spec.maintenance_margin = 10000.0;
    spec.trading_hours = "00:00-23:59";
    return spec;
}

}  // namespace

// 100-contract ES position should report total initial margin = 100 × $12K = $1.2M.
TEST(FuturesMarginOverloadTest, OneHundredContractsReportsTotalDollars) {
    FuturesInstrument es("ES", make_es_spec());
    EXPECT_DOUBLE_EQ(es.get_margin_requirement(4500.0, 100.0), 1'200'000.0);
}

// Shorts must also report positive total margin (absolute value of quantity).
TEST(FuturesMarginOverloadTest, ShortPositionReportsTotalDollars) {
    FuturesInstrument es("ES", make_es_spec());
    EXPECT_DOUBLE_EQ(es.get_margin_requirement(4500.0, -50.0), 600'000.0);
}

// Single contract sanity check.
TEST(FuturesMarginOverloadTest, SingleContractMatchesPerContractAmount) {
    FuturesInstrument es("ES", make_es_spec());
    EXPECT_DOUBLE_EQ(es.get_margin_requirement(4500.0, 1.0), 12'000.0);
}

// Total initial margin must be >= total maintenance margin for N>1 positions
// (basic FCM contract that the pre-fix code violated). This pins the
// invariant by computing both via the same paths MarginManager uses.
TEST(FuturesMarginOverloadTest, InitialMarginCoversMaintenanceMargin) {
    FuturesInstrument es("ES", make_es_spec());
    const double qty = 100.0;
    const double price = 4500.0;
    const double total_initial = es.get_margin_requirement(price, qty);
    const double total_maintenance = std::abs(qty) * es.get_maintenance_margin();
    EXPECT_GE(total_initial, total_maintenance)
        << "Initial margin (" << total_initial << ") must cover maintenance ("
        << total_maintenance << "). FCM rules require initial >= maintenance.";
}

// Legacy no-arg overload still returns per-contract margin (unchanged).
TEST(FuturesMarginOverloadTest, NoArgOverloadStillReturnsPerContract) {
    FuturesInstrument es("ES", make_es_spec());
    EXPECT_DOUBLE_EQ(es.get_margin_requirement(), 12'000.0);
}
