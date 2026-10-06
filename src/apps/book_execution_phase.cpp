#include "trade_ngin/apps/book_execution_phase.hpp"
#include <cmath>
#include <limits>
#include <stdexcept>

namespace trade_ngin { namespace {
using transaction_cost::TransactionCostResult;
using transaction_cost::TransactionCostManager;
using transaction_cost::CostChargeObservation;
void need(bool value) {
    if(!value) throw std::invalid_argument("book_execution_charge_unavailable");
}
Decimal checked_cash(double value) {
    need(std::isfinite(value) && value>=0);
    // The same Decimal(double) operations already used by the QT producers,
    // with their range check before the potentially undefined integer cast.
    const double scaled=value*100000000.0+(value>=0?0.5:-0.5);
    need(std::isfinite(scaled) &&
         static_cast<long double>(scaled)>=static_cast<long double>(INT64_MIN) &&
         static_cast<long double>(scaled)<=static_cast<long double>(INT64_MAX));
    return Decimal::from_raw(static_cast<int64_t>(scaled));
}
BookExecutionCharge assemble(const TransactionCostResult& raw,bool checked) {
    const auto decimal=[checked](double value){return checked?checked_cash(value):Decimal(value);};
    // Round the kernel's total directly. Adding individually rounded charges
    // here would change legacy MODEL/QT monetary results by a Decimal atom.
    return {raw,decimal(raw.commissions_fees),decimal(raw.implicit_price_impact),
        decimal(raw.slippage_market_impact),decimal(raw.total_transaction_costs)};
}
}
BookExecutionCharge charge_model_book_execution(
    const TransactionCostManager& manager,const ModelTrackedExecutionCharge& input,
    CostChargeObservation* observation) {
    need(input.asset_type==AssetType::FUTURE || input.asset_type==AssetType::EQUITY);
    // Preserve the exact tracked inputs and Decimal conversions, including
    // their exception categories. MODEL's caller handles those categories.
    const auto raw=input.asset_type==AssetType::FUTURE
        ? manager.calculate_costs(input.symbol,input.quantity,input.reference_price,AssetType::FUTURE,observation)
        : manager.calculate_costs(input.symbol,input.quantity,input.reference_price,AssetType::EQUITY,observation);
    return assemble(raw,false);
}
Result<BookExecutionCharge> charge_book_execution(
    const TransactionCostManager& manager,const ModelTrackedExecutionCharge& input,
    CostChargeObservation* observation) {
    try {
        return charge_model_book_execution(manager,input,observation);
    } catch(const std::exception& error) {
        return make_error<BookExecutionCharge>(ErrorCode::INVALID_ARGUMENT,error.what(),"book_execution_phase");
    }
}
Result<BookExecutionCharge> charge_book_execution(
    const TransactionCostManager& manager,const GovernedExecutionCharge& input,
    CostChargeObservation* observation) {
    try {
        if(observation)*observation={};
        need(input.asset_type==AssetType::FUTURE || input.asset_type==AssetType::EQUITY);
        need(!input.symbol.empty() && !input.signed_quantity.is_zero() &&
             input.signed_quantity.raw_value()!=INT64_MIN);
        need(input.asset_type!=AssetType::FUTURE || input.signed_quantity.raw_value()%100000000==0);
        for(double value:{input.reference_price,input.adv,input.volatility_multiplier})
            need(std::isfinite(value) && value>0);
        CostChargeObservation local;
        auto* used=observation?observation:&local;
        const auto raw=input.asset_type==AssetType::FUTURE
            ? manager.calculate_costs(input.symbol,input.signed_quantity.as_double(),input.reference_price,
                input.adv,input.volatility_multiplier,AssetType::FUTURE,used)
            : manager.calculate_costs(input.symbol,input.signed_quantity.as_double(),input.reference_price,
                input.adv,input.volatility_multiplier,AssetType::EQUITY,used);
        // Each admitted QT instrument is registered by its exact symbol/domain.
        // A missing registration is unavailable, never a MODEL fallback.
        need(used->input_source==transaction_cost::CostInputSource::explicit_values &&
             used->asset_lookup.path==transaction_cost::AssetLookupPath::exact_symbol);
        return assemble(raw,true);
    } catch(const std::exception&) {
        return make_error<BookExecutionCharge>(ErrorCode::INVALID_DATA,
            "book_execution_charge_unavailable","book_execution_phase");
    }
}
}
