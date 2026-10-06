#pragma once
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/transaction_cost/transaction_cost_manager.hpp"

namespace trade_ngin {
// Keep the legacy MODEL's tracked state and Decimal conversion separate from
// QT's admitted explicit state and checked cash conversion. Neither operation
// selects quantities, opens transactions, stores rows or publishes a report.
struct ModelTrackedExecutionCharge {
    std::string symbol;
    double quantity;
    double reference_price;
    AssetType asset_type = AssetType::FUTURE;
};
struct GovernedExecutionCharge {
    std::string symbol;
    Decimal signed_quantity;
    double reference_price;
    double adv;
    double volatility_multiplier;
    AssetType asset_type;
};
struct BookExecutionCharge {
    transaction_cost::TransactionCostResult raw;
    Decimal commissions_fees;
    Decimal implicit_price_impact;
    Decimal slippage_market_impact;
    Decimal total_transaction_costs;
};
// MODEL preserves the original cost/Decimal exception categories and text.
BookExecutionCharge charge_model_book_execution(
    const transaction_cost::TransactionCostManager&, const ModelTrackedExecutionCharge&,
    transaction_cost::CostChargeObservation* = nullptr);
Result<BookExecutionCharge> charge_book_execution(
    const transaction_cost::TransactionCostManager&, const ModelTrackedExecutionCharge&,
    transaction_cost::CostChargeObservation* = nullptr);
Result<BookExecutionCharge> charge_book_execution(
    const transaction_cost::TransactionCostManager&, const GovernedExecutionCharge&,
    transaction_cost::CostChargeObservation* = nullptr);
}
