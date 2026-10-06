#pragma once
#include <map>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "trade_ngin/apps/equity_run_projection.hpp"
#include "trade_ngin/strategy/consumption.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"
namespace trade_ngin {
enum class EquityStageOutcome { NotReached, ReturnedOk, ReturnedError, Threw, Skipped };
enum class EquityStageSkip { None, NonTradingDay, ProvedDeskSuccessor };
struct EquitySetupReads {
    std::optional<double> capital_allocation;
    std::optional<double> max_leverage;
    std::optional<double> max_drawdown;
    std::optional<double> reserve_capital;
    std::optional<bool> use_optimization;
    std::optional<bool> use_risk_management;
    std::optional<bool> allow_fractional_positions;
};
struct EquityMarketInputReads {
    std::optional<int> historical_days;
    std::optional<std::string> asset_type;
    std::optional<std::string> frequency;
    std::optional<std::string> start_day;
    std::optional<std::string> end_day;
    std::optional<int> data_staleness_tolerance_days;
};
struct EquityCostHistoryReads {
};
struct EquityCostHistorySymbolReads {
    std::optional<int> adv_lookback_days;
    std::optional<int> log_return_lookback_days;
    std::optional<std::string> previous_close_source;
    std::optional<double> previous_close_forwarded;
};
struct EquityPriorReads {
    std::optional<std::string> mode;
    std::optional<std::string> source_day;
    std::optional<std::string> valuation_day;
    std::optional<std::string> decision_id;
    std::optional<std::string> finalization_id;
    std::optional<std::string> finalization_digest;
    std::optional<std::string> finalization_source_digest;
    std::optional<std::string> accounting_input_digest;
    std::optional<std::string> observation_digest;
    std::optional<std::string> results_digest;
};
struct EquityCorporateActionsReads {
    std::optional<std::string> path;
    std::optional<std::string> spinoff_child_policy_requested;
    std::optional<std::string> spinoff_child_policy_effective;
    std::optional<int> effective_event_count;
    std::optional<int> original_action_count;
    std::optional<int> successor_action_count;
    std::optional<std::string> original_action_digest;
    std::optional<std::string> successor_action_digest;
    std::optional<std::string> basis_frame_digest;
};
struct EquityPreparationReads {
};
struct EquityPreparationSymbolReads {
    std::optional<int> lookback_period;
    std::optional<int> vol_lookback;
    std::optional<int> max_history_size;
};
struct EquityPrimaryReads {
    std::optional<StrategyConsumptionTrace> strategy_invocation;
    std::optional<PortfolioConsumptionTrace> portfolio_invocation;
};
struct EquityExecutionReads {
    std::optional<int> execution_price_max_staleness_days;
};
struct EquityExecutionExecutionReads {
    std::optional<double> quantity;
    std::optional<double> reference_price;
    std::optional<double> retrieved_adv;
    std::optional<double> retrieved_volatility_multiplier;
    std::optional<double> effective_adv;
    std::optional<double> effective_volatility_multiplier;
    std::optional<double> explicit_fee_per_contract;
    std::optional<double> commission_per_unit;
    std::optional<double> min_commission_per_order;
    std::optional<double> max_commission_per_order;
    std::optional<double> max_commission_pct;
    std::optional<double> sec_fee_per_million;
    std::optional<double> finra_taf_per_share;
    std::optional<double> finra_taf_cap_per_trade;
    std::optional<double> max_total_implicit_bps;
    std::optional<double> point_value;
    std::optional<double> volatility_lambda;
    std::optional<double> volatility_min_multiplier;
    std::optional<double> volatility_max_multiplier;
    std::optional<double> baseline_spread_ticks;
    std::optional<double> min_spread_ticks;
    std::optional<double> max_spread_ticks;
    std::optional<double> spread_cost_multiplier;
    std::optional<double> tick_size;
    std::optional<double> min_adv;
    std::optional<double> min_participation;
    std::optional<double> max_participation;
    std::optional<double> max_impact_bps;
    std::optional<double> selected_k_bps;
    std::optional<bool> volatility_calculation_reached;
    std::optional<bool> apply_regulatory_fees;
    std::optional<bool> tick_constrained;
    std::optional<std::string> asset_type;
    std::optional<std::string> asset_lookup_path;
    std::optional<std::string> input_source;
    std::optional<Decimal> commissions_fees_exact;
    std::optional<Decimal> implicit_price_impact_exact;
    std::optional<Decimal> slippage_market_impact_exact;
    std::optional<Decimal> total_transaction_costs_exact;
};
struct EquityEodReads {
    std::optional<std::string> path;
    std::optional<Decimal> previous_equity_exact;
    std::optional<Decimal> previous_total_pnl_exact;
    std::optional<Decimal> previous_total_realized_pnl_exact;
    std::optional<Decimal> previous_total_transaction_costs_exact;
    std::optional<Decimal> initial_capital_exact;
};
struct EquityResultAssemblyReads {
    std::optional<std::string> currency;
    std::optional<Decimal> current_portfolio_value_exact;
    std::optional<Decimal> total_realized_pnl_exact;
    std::optional<Decimal> total_unrealized_pnl_exact;
    std::optional<Decimal> total_transaction_costs_exact;
};
template<class Reads> struct EquityRunStage {
    EquityStageOutcome outcome{EquityStageOutcome::NotReached};
    EquityStageSkip skip{EquityStageSkip::None};
    Reads reads;
};
struct EquityExecutionConsumption {
    std::string symbol,portfolio_id,strategy_id,strategy_name,execution_id;
    std::size_t index{0};
    EquityExecutionExecutionReads reads;
};
struct EquityRunConsumption {
    std::string portfolio_id,strategy_id,strategy_name,date;
    EquityRunStage<EquitySetupReads> setup;
    EquityRunStage<EquityMarketInputReads> market_input;
    EquityRunStage<EquityCostHistoryReads> cost_history;
    EquityRunStage<EquityPriorReads> prior;
    EquityRunStage<EquityCorporateActionsReads> corporate_actions;
    EquityRunStage<EquityPreparationReads> preparation;
    EquityRunStage<EquityPrimaryReads> primary;
    EquityRunStage<EquityExecutionReads> execution;
    EquityRunStage<EquityEodReads> eod;
    EquityRunStage<EquityResultAssemblyReads> result_assembly;
    std::map<std::string,EquityCostHistorySymbolReads> cost_symbols;
    std::map<std::string,EquityPreparationSymbolReads> preparation_symbols;
    std::vector<EquityExecutionConsumption> executions;
};

} // namespace trade_ngin
