#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "trade_ngin/core/config_loader.hpp"
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/holiday_checker.hpp"
#include "trade_ngin/live/corporate_actions_applier.hpp"
#include "trade_ngin/live/corporate_actions_lifecycle.hpp"
#include "trade_ngin/strategy/equity_strategy_builder.hpp"

namespace trade_ngin {
class InstrumentRegistry;
class PostgresDatabase;
}

namespace trade_ngin::apps {

struct EquityMultiLivePriorAccounting {
    bool found{false};
    double current_portfolio_value{0.0};
    double total_pnl{0.0};
    double total_realized_pnl{0.0};
    double total_unrealized_pnl{0.0};
    double total_transaction_costs{0.0};
};

struct EquityMultiLiveDailyAccounting {
    double initial_capital{0.0};
    double daily_realized_pnl{0.0};
    double total_unrealized_pnl{0.0};
    double daily_transaction_costs{0.0};
    double gross_notional{0.0};
    double net_notional{0.0};
    double margin_posted{0.0};
    int trading_days{0};
};

struct EquityMultiLiveAccounting {
    double total_cumulative_return{0.0};
    double total_annualized_return{0.0};
    double total_pnl{0.0};
    double total_unrealized_pnl{0.0};
    double total_realized_pnl{0.0};
    double current_portfolio_value{0.0};
    double gross_leverage{0.0};
    double net_leverage{0.0};
    double portfolio_leverage{0.0};
    double gross_notional{0.0};
    double net_notional{0.0};
    double daily_return{0.0};
    double daily_pnl{0.0};
    double total_transaction_costs{0.0};
    double daily_realized_pnl{0.0};
    double daily_unrealized_pnl{0.0};
    double daily_transaction_costs{0.0};
    double margin_posted{0.0};
    double cash_available{0.0};

    std::unordered_map<std::string, double> live_result_metrics() const;
};

Result<EquityMultiLiveAccounting> derive_equity_multi_live_accounting(
    const EquityMultiLivePriorAccounting& prior,
    const EquityMultiLiveDailyAccounting& daily);

struct EquityOwnerCorporateActionInput {
    std::unordered_map<std::string, Position> positions;
    std::vector<SpinoffEvent> spinoffs;
    std::vector<CorpActionEvent> price_restatements;
    std::vector<TickerAlias> aliases;
    std::unordered_map<std::string, std::string> holding_start_dates;
    std::string as_of_date;
    std::vector<TerminationEvent> terminations;
    std::unordered_map<std::string, double> final_closes;
    std::string termination_feed_last_date;
    SpinoffChildPolicy spinoff_child_policy{
        SpinoffChildPolicy::LIQUIDATE_AT_FIRST_CLOSE};
    Timestamp execution_time{};
};

struct EquityOwnerCorporateActionOutput {
    std::unordered_map<std::string, Position> positions;
    std::unordered_map<std::string, Position> post_class1_positions;
    std::unordered_map<std::string, std::string> applied_class1_ex_date;
    std::vector<PositionAdjustment> price_adjustments;
    std::vector<LifecycleAdjustment> lifecycle_adjustments;
    std::vector<ExecutionReport> evidence_executions;
    std::unordered_map<std::string, double> realized_by_symbol;
};

Result<EquityOwnerCorporateActionOutput> apply_equity_owner_corporate_actions(
    EquityOwnerCorporateActionInput input);

// Multi-sleeve-only daily path. The legacy MEAN_REVERSION book deliberately
// stays in the original runner so its ordering, identities, and output remain
// byte-for-byte unchanged.
Result<void> run_multi_sleeve_equity_live_day(
    const AppConfig& config, const EquityLiveBookPlan& plan,
    const std::shared_ptr<PostgresDatabase>& db, InstrumentRegistry& registry,
    const HolidayChecker& holidays, const Timestamp& now,
    const Timestamp& start_date, const Timestamp& end_date,
    bool historical_replay);

}  // namespace trade_ngin::apps
