#include "trade_ngin/apps/equity_execution_consumption.hpp"
namespace trade_ngin {
Result<EquityExecutionConsumption> equity_execution_consumption(const ExecutionReport& executed,
    const DailyExecutionAttempt& attempt,const std::string& portfolio_id,const std::string& strategy_id,
    const std::string& strategy_name,std::size_t index) {
    if(!attempt.returned || attempt.execution.state!=ExecutionCallState::returned || attempt.symbol!=executed.symbol || attempt.sequence!=index)
        return make_error<EquityExecutionConsumption>(ErrorCode::INVALID_DATA,"equity_execution_trace_identity");
                EquityExecutionConsumption evidence;evidence.symbol=executed.symbol;evidence.portfolio_id=portfolio_id;
                evidence.strategy_id=strategy_id;evidence.strategy_name=strategy_name;evidence.execution_id=executed.exec_id;evidence.index=index;
                auto& read=evidence.reads;const auto& cost=attempt.execution.cost;
                read.quantity=cost.quantity;
                read.reference_price=cost.reference_price;
                read.retrieved_adv=cost.retrieved_adv;
                read.retrieved_volatility_multiplier=cost.retrieved_volatility_multiplier;
                read.effective_adv=cost.effective_adv;
                read.effective_volatility_multiplier=cost.effective_volatility_multiplier;
                read.explicit_fee_per_contract=cost.explicit_fee_per_contract;
                read.commission_per_unit=cost.commission_per_unit;
                read.min_commission_per_order=cost.min_commission_per_order;
                read.max_commission_per_order=cost.max_commission_per_order;
                read.max_commission_pct=cost.max_commission_pct;
                read.sec_fee_per_million=cost.sec_fee_per_million;
                read.finra_taf_per_share=cost.finra_taf_per_share;
                read.finra_taf_cap_per_trade=cost.finra_taf_cap_per_trade;
                read.max_total_implicit_bps=cost.max_total_implicit_bps;
                read.point_value=cost.point_value;
                read.apply_regulatory_fees=cost.apply_regulatory_fees;
                read.volatility_calculation_reached=cost.volatility.calculation_reached;
                read.volatility_lambda=cost.volatility.lambda;
                read.volatility_min_multiplier=cost.volatility.min_multiplier;
                read.volatility_max_multiplier=cost.volatility.max_multiplier;
                read.baseline_spread_ticks=cost.spread.baseline_spread_ticks;
                read.min_spread_ticks=cost.spread.min_spread_ticks;
                read.max_spread_ticks=cost.spread.max_spread_ticks;
                read.spread_cost_multiplier=cost.spread.spread_cost_multiplier;
                read.tick_size=cost.spread.tick_size;
                read.tick_constrained=cost.spread.tick_constrained;
                read.min_adv=cost.impact.min_adv;
                read.min_participation=cost.impact.min_participation;
                read.max_participation=cost.impact.max_participation;
                read.max_impact_bps=cost.impact.max_impact_bps;
                read.selected_k_bps=cost.impact.selected_k_bps;
                read.asset_type="EQUITY";
                if(cost.asset_lookup.path)read.asset_lookup_path=*cost.asset_lookup.path==transaction_cost::AssetLookupPath::exact_symbol?"exact_symbol":
                    *cost.asset_lookup.path==transaction_cost::AssetLookupPath::fallback?"fallback":"invalid";
                if(cost.input_source)read.input_source=*cost.input_source==transaction_cost::CostInputSource::internally_tracked?"internally_tracked":"invalid";
                read.commissions_fees_exact=executed.commissions_fees;read.implicit_price_impact_exact=executed.implicit_price_impact;
                read.slippage_market_impact_exact=executed.slippage_market_impact;read.total_transaction_costs_exact=executed.total_transaction_costs;
                return evidence;
}
}
