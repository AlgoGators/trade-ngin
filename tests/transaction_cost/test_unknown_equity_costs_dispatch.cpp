#include <gtest/gtest.h>
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/transaction_cost/transaction_cost_manager.hpp"

using namespace trade_ngin;
using namespace trade_ngin::transaction_cost;

// Regression test for audit finding §1.1 dispatch dead-end.
//
// Before Phase 1a-i, TransactionCostManager::calculate_costs() called
// asset_configs_.get_config(symbol) without asset_type. The default branch
// at asset_cost_config.cpp:670 gated on `asset_type == EQUITY` but always
// received NONE, so unknown equity symbols fell through to the futures
// default: point_value=100, commission=$1.50/share. ~157× cost overstatement.
//
// Post-fix, calculate_costs accepts an optional `asset_type` parameter that
// threads through to get_config, allowing equity callers to opt into the
// correct fallback even for unregistered symbols.

// Unregistered symbol "ZZZZ", buy 100 shares at $50.
// With AssetType::EQUITY hint: commission = max($1 floor, 100 × $0.005) = $1.00.
// Without the hint: commission would be 100 × $1.50 = $150 (futures fallback).
TEST(UnknownEquityCostsDispatchTest, EquityHintRoutesUnregisteredToEquityDefault) {
    TransactionCostManager::Config tcm_config;
    TransactionCostManager tcm(tcm_config);

    // No registration of "ZZZZ"; calling code passes AssetType::EQUITY so
    // the fallback at AssetCostConfigRegistry::get_config returns equity
    // defaults instead of futures defaults.
    auto result = tcm.calculate_costs("ZZZZ", /*quantity=*/100.0,
                                       /*reference_price=*/50.0,
                                       AssetType::EQUITY);

    // $1.00 min commission floor binds (raw = 100 × $0.005 = $0.50).
    EXPECT_NEAR(result.commissions_fees, 1.00, 0.01)
        << "Unregistered equity should fall back to equity default ($0.005/share, $1 min). "
        << "Got " << result.commissions_fees
        << " -- if this is ~$150, the dispatch dead-end has regressed.";
}

// Larger trade so the per-share rate exceeds the floor. 500 shares × $0.005 = $2.50.
TEST(UnknownEquityCostsDispatchTest, EquityHintScalesByShares) {
    TransactionCostManager::Config tcm_config;
    TransactionCostManager tcm(tcm_config);

    auto result = tcm.calculate_costs("UNKNOWN_LARGE", /*quantity=*/500.0,
                                       /*reference_price=*/100.0,
                                       AssetType::EQUITY);
    EXPECT_NEAR(result.commissions_fees, 2.50, 0.01)
        << "500 × $0.005 = $2.50; got " << result.commissions_fees;
}

// Without the hint, an unknown symbol still gets the legacy futures default.
// This documents the contract: callers MUST pass AssetType::EQUITY for the
// equity fallback to apply.
TEST(UnknownEquityCostsDispatchTest, WithoutHintFallsThroughToFuturesDefault) {
    TransactionCostManager::Config tcm_config;
    TransactionCostManager tcm(tcm_config);

    // No asset_type hint -> defaults to AssetType::NONE -> falls through to
    // get_default_config() (futures: point_value=100, commission absent so
    // global config_.explicit_fee_per_contract kicks in).
    auto result = tcm.calculate_costs("UNKNOWN_NO_HINT", /*quantity=*/100.0,
                                       /*reference_price=*/50.0);
    // Documents that the unhinted path is NOT equity-shaped. Production
    // callers that handle equities should always pass AssetType::EQUITY.
    EXPECT_GT(result.commissions_fees, 1.50)
        << "Without an equity hint, fallback should still be futures-shaped. "
           "If this asserts because the result is ~$1, dispatch defaults changed.";
}
