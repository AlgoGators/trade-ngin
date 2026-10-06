#include "trade_ngin/apps/equity_run_consumption.hpp"
#include "trade_ngin/apps/equity_strategy_consumption.hpp"
#include "trade_ngin/apps/equity_portfolio_consumption.hpp"
#include <cmath>
#include <regex>
#include <set>
#include <stdexcept>
namespace trade_ngin { namespace {
using Json=nlohmann::json;
bool symbol_id(const std::string& s) { return !s.empty() && s.size()<=64 && std::regex_match(s,std::regex("[A-Za-z0-9_.\\/-]+")); }
bool owner_id(const std::string& s) { return !s.empty() && s.size()<=256 && s.find_first_of("\r\n\0")==std::string::npos; }
std::string outcome(EquityStageOutcome v) { switch(v) {
case EquityStageOutcome::NotReached:return "not_reached";case EquityStageOutcome::ReturnedOk:return "returned_ok";
case EquityStageOutcome::ReturnedError:return "returned_error";case EquityStageOutcome::Threw:return "threw";
case EquityStageOutcome::Skipped:return "skipped";} throw std::invalid_argument("invalid_outcome"); }
Json skip(EquityStageSkip v) { switch(v) { case EquityStageSkip::None:return nullptr;
case EquityStageSkip::NonTradingDay:return "non_trading_day";
case EquityStageSkip::ProvedDeskSuccessor:return "proved_desk_successor";} throw std::invalid_argument("invalid_skip"); }
void add(Json& j,const char* k,const std::optional<double>& v) {if(v){if(!std::isfinite(*v))throw std::invalid_argument("nonfinite_read");j[k]=*v;}}
void add(Json& j,const char* k,const std::optional<int>& v) {if(v){if(*v<0)throw std::invalid_argument("negative_read");j[k]=*v;}}
void add(Json& j,const char* k,const std::optional<bool>& v) {if(v)j[k]=*v;}
void add(Json& j,const char* k,const std::optional<std::string>& v) {if(v)j[k]=*v;}
void add(Json& j,const char* k,const std::optional<Decimal>& v) {if(v)j[k]=v->to_string();}
void add(Json& j,const char* k,const std::optional<PortfolioConsumptionTrace>& v) {if(v)j[k]=project_equity_portfolio_consumption(*v);}
void add(Json& j,const char* k,const std::optional<StrategyConsumptionTrace>& v) {if(v){auto p=project_equity_strategy_consumption(*v);if(p.is_error())throw std::invalid_argument("invalid_strategy_trace");j[k]=p.value();}}
Json reads(const EquitySetupReads& v) {Json j=Json::object();
add(j,"capital_allocation",v.capital_allocation);
add(j,"max_leverage",v.max_leverage);
add(j,"max_drawdown",v.max_drawdown);
add(j,"reserve_capital",v.reserve_capital);
add(j,"use_optimization",v.use_optimization);
add(j,"use_risk_management",v.use_risk_management);
add(j,"allow_fractional_positions",v.allow_fractional_positions);
return j;}
Json reads(const EquityMarketInputReads& v) {Json j=Json::object();
add(j,"historical_days",v.historical_days);
add(j,"asset_type",v.asset_type);
if(j.contains("asset_type") && (j.at("asset_type")!="EQUITY"))throw std::invalid_argument("invalid_asset_type");
add(j,"frequency",v.frequency);
if(j.contains("frequency") && (j.at("frequency")!="DAILY"))throw std::invalid_argument("invalid_frequency");
add(j,"start_day",v.start_day);
if(j.contains("start_day") && (!std::regex_match(j.at("start_day").get<std::string>(),std::regex("[0-9]{4}-[0-9]{2}-[0-9]{2}"))))throw std::invalid_argument("invalid_start_day");
add(j,"end_day",v.end_day);
if(j.contains("end_day") && (!std::regex_match(j.at("end_day").get<std::string>(),std::regex("[0-9]{4}-[0-9]{2}-[0-9]{2}"))))throw std::invalid_argument("invalid_end_day");
add(j,"data_staleness_tolerance_days",v.data_staleness_tolerance_days);
return j;}
Json reads(const EquityCostHistoryReads& v) {Json j=Json::object();
return j;}
Json reads(const EquityCostHistorySymbolReads& v) {Json j=Json::object();
add(j,"adv_lookback_days",v.adv_lookback_days);
add(j,"log_return_lookback_days",v.log_return_lookback_days);
add(j,"previous_close_source",v.previous_close_source);
if(j.contains("previous_close_source") && (j.at("previous_close_source")!="initial_current_close" && j.at("previous_close_source")!="stored_previous_close"))throw std::invalid_argument("invalid_previous_close_source");
add(j,"previous_close_forwarded",v.previous_close_forwarded);
return j;}
Json reads(const EquityPriorReads& v) {Json j=Json::object();
add(j,"mode",v.mode);
if(j.contains("mode") && (j.at("mode")!="system_reference" && j.at("mode")!="verified_desk_prior"))throw std::invalid_argument("invalid_mode");
add(j,"source_day",v.source_day);
if(j.contains("source_day") && (!std::regex_match(j.at("source_day").get<std::string>(),std::regex("[0-9]{4}-[0-9]{2}-[0-9]{2}"))))throw std::invalid_argument("invalid_source_day");
add(j,"valuation_day",v.valuation_day);
if(j.contains("valuation_day") && (!std::regex_match(j.at("valuation_day").get<std::string>(),std::regex("[0-9]{4}-[0-9]{2}-[0-9]{2}"))))throw std::invalid_argument("invalid_valuation_day");
add(j,"decision_id",v.decision_id);
if(j.contains("decision_id") && (!std::regex_match(j.at("decision_id").get<std::string>(),std::regex("[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}")) || j.at("decision_id")=="00000000-0000-0000-0000-000000000000"))throw std::invalid_argument("invalid_decision_id");
add(j,"finalization_id",v.finalization_id);
if(j.contains("finalization_id") && (!std::regex_match(j.at("finalization_id").get<std::string>(),std::regex("[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}")) || j.at("finalization_id")=="00000000-0000-0000-0000-000000000000"))throw std::invalid_argument("invalid_finalization_id");
add(j,"finalization_digest",v.finalization_digest);
if(j.contains("finalization_digest") && (!std::regex_match(j.at("finalization_digest").get<std::string>(),std::regex("^[0-9a-f]{64}$"))))throw std::invalid_argument("invalid_finalization_digest");
add(j,"finalization_source_digest",v.finalization_source_digest);
if(j.contains("finalization_source_digest") && (!std::regex_match(j.at("finalization_source_digest").get<std::string>(),std::regex("^[0-9a-f]{64}$"))))throw std::invalid_argument("invalid_finalization_source_digest");
add(j,"accounting_input_digest",v.accounting_input_digest);
if(j.contains("accounting_input_digest") && (!std::regex_match(j.at("accounting_input_digest").get<std::string>(),std::regex("^[0-9a-f]{64}$"))))throw std::invalid_argument("invalid_accounting_input_digest");
add(j,"observation_digest",v.observation_digest);
if(j.contains("observation_digest") && (!std::regex_match(j.at("observation_digest").get<std::string>(),std::regex("^[0-9a-f]{64}$"))))throw std::invalid_argument("invalid_observation_digest");
add(j,"results_digest",v.results_digest);
if(j.contains("results_digest") && (!std::regex_match(j.at("results_digest").get<std::string>(),std::regex("^[0-9a-f]{64}$"))))throw std::invalid_argument("invalid_results_digest");
return j;}
Json reads(const EquityCorporateActionsReads& v) {Json j=Json::object();
add(j,"path",v.path);
if(j.contains("path") && (j.at("path")!="system_history" && j.at("path")!="proved_action_free_prior"))throw std::invalid_argument("invalid_path");
add(j,"spinoff_child_policy_requested",v.spinoff_child_policy_requested);
if(j.contains("spinoff_child_policy_requested") && (j.at("spinoff_child_policy_requested").get<std::string>().empty() || j.at("spinoff_child_policy_requested").get<std::string>().size()>256))throw std::invalid_argument("invalid_spinoff_child_policy_requested");
add(j,"spinoff_child_policy_effective",v.spinoff_child_policy_effective);
if(j.contains("spinoff_child_policy_effective") && (j.at("spinoff_child_policy_effective")!="hold" && j.at("spinoff_child_policy_effective")!="liquidate_at_first_close"))throw std::invalid_argument("invalid_spinoff_child_policy_effective");
add(j,"effective_event_count",v.effective_event_count);
return j;}
Json reads(const EquityPreparationReads& v) {Json j=Json::object();
return j;}
Json reads(const EquityPreparationSymbolReads& v) {Json j=Json::object();
add(j,"lookback_period",v.lookback_period);
add(j,"vol_lookback",v.vol_lookback);
add(j,"max_history_size",v.max_history_size);
return j;}
Json reads(const EquityPrimaryReads& v) {Json j=Json::object();
add(j,"strategy_invocation",v.strategy_invocation);
add(j,"portfolio_invocation",v.portfolio_invocation);
return j;}
Json reads(const EquityExecutionReads& v) {Json j=Json::object();
add(j,"execution_price_max_staleness_days",v.execution_price_max_staleness_days);
return j;}
Json reads(const EquityExecutionExecutionReads& v) {Json j=Json::object();
add(j,"quantity",v.quantity);
add(j,"reference_price",v.reference_price);
add(j,"retrieved_adv",v.retrieved_adv);
add(j,"retrieved_volatility_multiplier",v.retrieved_volatility_multiplier);
add(j,"effective_adv",v.effective_adv);
add(j,"effective_volatility_multiplier",v.effective_volatility_multiplier);
add(j,"commission_per_unit",v.commission_per_unit);
add(j,"min_commission_per_order",v.min_commission_per_order);
add(j,"max_commission_per_order",v.max_commission_per_order);
add(j,"max_commission_pct",v.max_commission_pct);
add(j,"sec_fee_per_million",v.sec_fee_per_million);
add(j,"finra_taf_per_share",v.finra_taf_per_share);
add(j,"finra_taf_cap_per_trade",v.finra_taf_cap_per_trade);
add(j,"max_total_implicit_bps",v.max_total_implicit_bps);
add(j,"point_value",v.point_value);
add(j,"volatility_lambda",v.volatility_lambda);
add(j,"volatility_min_multiplier",v.volatility_min_multiplier);
add(j,"volatility_max_multiplier",v.volatility_max_multiplier);
add(j,"baseline_spread_ticks",v.baseline_spread_ticks);
add(j,"min_spread_ticks",v.min_spread_ticks);
add(j,"max_spread_ticks",v.max_spread_ticks);
add(j,"spread_cost_multiplier",v.spread_cost_multiplier);
add(j,"tick_size",v.tick_size);
add(j,"min_adv",v.min_adv);
add(j,"min_participation",v.min_participation);
add(j,"max_participation",v.max_participation);
add(j,"max_impact_bps",v.max_impact_bps);
add(j,"selected_k_bps",v.selected_k_bps);
add(j,"apply_regulatory_fees",v.apply_regulatory_fees);
add(j,"tick_constrained",v.tick_constrained);
add(j,"asset_type",v.asset_type);
if(j.contains("asset_type") && (j.at("asset_type")!="EQUITY"))throw std::invalid_argument("invalid_asset_type");
add(j,"asset_lookup_path",v.asset_lookup_path);
if(j.contains("asset_lookup_path") && (j.at("asset_lookup_path")!="exact_symbol" && j.at("asset_lookup_path")!="fallback"))throw std::invalid_argument("invalid_asset_lookup_path");
add(j,"input_source",v.input_source);
if(j.contains("input_source") && (j.at("input_source")!="internally_tracked"))throw std::invalid_argument("invalid_input_source");
add(j,"commissions_fees_exact",v.commissions_fees_exact);
if(j.contains("commissions_fees_exact") && (j.at("commissions_fees_exact").get<std::string>().empty() || j.at("commissions_fees_exact").get<std::string>().size()>256))throw std::invalid_argument("invalid_commissions_fees_exact");
add(j,"implicit_price_impact_exact",v.implicit_price_impact_exact);
if(j.contains("implicit_price_impact_exact") && (j.at("implicit_price_impact_exact").get<std::string>().empty() || j.at("implicit_price_impact_exact").get<std::string>().size()>256))throw std::invalid_argument("invalid_implicit_price_impact_exact");
add(j,"slippage_market_impact_exact",v.slippage_market_impact_exact);
if(j.contains("slippage_market_impact_exact") && (j.at("slippage_market_impact_exact").get<std::string>().empty() || j.at("slippage_market_impact_exact").get<std::string>().size()>256))throw std::invalid_argument("invalid_slippage_market_impact_exact");
add(j,"total_transaction_costs_exact",v.total_transaction_costs_exact);
if(j.contains("total_transaction_costs_exact") && (j.at("total_transaction_costs_exact").get<std::string>().empty() || j.at("total_transaction_costs_exact").get<std::string>().size()>256))throw std::invalid_argument("invalid_total_transaction_costs_exact");
return j;}
Json reads(const EquityEodReads& v) {Json j=Json::object();
add(j,"path",v.path);
if(j.contains("path") && (j.at("path")!="system_finalization" && j.at("path")!="proved_desk_successor"))throw std::invalid_argument("invalid_path");
add(j,"previous_equity_exact",v.previous_equity_exact);
if(j.contains("previous_equity_exact") && (j.at("previous_equity_exact").get<std::string>().empty() || j.at("previous_equity_exact").get<std::string>().size()>256))throw std::invalid_argument("invalid_previous_equity_exact");
add(j,"previous_total_pnl_exact",v.previous_total_pnl_exact);
if(j.contains("previous_total_pnl_exact") && (j.at("previous_total_pnl_exact").get<std::string>().empty() || j.at("previous_total_pnl_exact").get<std::string>().size()>256))throw std::invalid_argument("invalid_previous_total_pnl_exact");
add(j,"previous_total_realized_pnl_exact",v.previous_total_realized_pnl_exact);
if(j.contains("previous_total_realized_pnl_exact") && (j.at("previous_total_realized_pnl_exact").get<std::string>().empty() || j.at("previous_total_realized_pnl_exact").get<std::string>().size()>256))throw std::invalid_argument("invalid_previous_total_realized_pnl_exact");
add(j,"previous_total_transaction_costs_exact",v.previous_total_transaction_costs_exact);
if(j.contains("previous_total_transaction_costs_exact") && (j.at("previous_total_transaction_costs_exact").get<std::string>().empty() || j.at("previous_total_transaction_costs_exact").get<std::string>().size()>256))throw std::invalid_argument("invalid_previous_total_transaction_costs_exact");
add(j,"initial_capital_exact",v.initial_capital_exact);
if(j.contains("initial_capital_exact") && (j.at("initial_capital_exact").get<std::string>().empty() || j.at("initial_capital_exact").get<std::string>().size()>256))throw std::invalid_argument("invalid_initial_capital_exact");
return j;}
Json reads(const EquityResultAssemblyReads& v) {Json j=Json::object();
add(j,"currency",v.currency);
if(j.contains("currency") && (j.at("currency")!="USD"))throw std::invalid_argument("invalid_currency");
add(j,"current_portfolio_value_exact",v.current_portfolio_value_exact);
if(j.contains("current_portfolio_value_exact") && (j.at("current_portfolio_value_exact").get<std::string>().empty() || j.at("current_portfolio_value_exact").get<std::string>().size()>256))throw std::invalid_argument("invalid_current_portfolio_value_exact");
add(j,"total_realized_pnl_exact",v.total_realized_pnl_exact);
if(j.contains("total_realized_pnl_exact") && (j.at("total_realized_pnl_exact").get<std::string>().empty() || j.at("total_realized_pnl_exact").get<std::string>().size()>256))throw std::invalid_argument("invalid_total_realized_pnl_exact");
add(j,"total_unrealized_pnl_exact",v.total_unrealized_pnl_exact);
if(j.contains("total_unrealized_pnl_exact") && (j.at("total_unrealized_pnl_exact").get<std::string>().empty() || j.at("total_unrealized_pnl_exact").get<std::string>().size()>256))throw std::invalid_argument("invalid_total_unrealized_pnl_exact");
add(j,"total_transaction_costs_exact",v.total_transaction_costs_exact);
if(j.contains("total_transaction_costs_exact") && (j.at("total_transaction_costs_exact").get<std::string>().empty() || j.at("total_transaction_costs_exact").get<std::string>().size()>256))throw std::invalid_argument("invalid_total_transaction_costs_exact");
return j;}
void require(const Json& reads,std::initializer_list<const char*> names) {for(auto name:names)if(!reads.contains(name))throw std::invalid_argument("required_read_missing");}
void forbid(const Json& reads,std::initializer_list<const char*> names) {for(auto name:names)if(reads.contains(name))throw std::invalid_argument("unexpected_branch_read");}
} // namespace
EquityRunProjection project_equity_run_consumption(const EquityRunConsumption& v) {
Json j={{"schema_version","qt-equity-run-consumption/v1"},{"catalog_version","qt-equity-main08b15c-run/v1"},
{"scope","full_run"},{"profile","mean_reversion"},{"run_key",{{"portfolio_id",v.portfolio_id},{"strategy_id",v.strategy_id},{"strategy_name",v.strategy_name},{"date",v.date}}},
{"available",false},{"complete",false},{"unavailable_reason","instrumentation_missing"},{"stages",Json::object()}};
try {
if(!owner_id(v.portfolio_id) || v.strategy_id!="LIVE_EQUITY_MEAN_REVERSION" || v.strategy_name!="EQUITY_MEAN_REVERSION" || !std::regex_match(v.date,std::regex("[0-9]{4}-[0-9]{2}-[0-9]{2}")))throw std::invalid_argument("invalid_run_identity");
bool complete=true;
auto& setup=j["stages"]["setup"];setup={{"outcome",outcome(v.setup.outcome)},{"skip_reason",skip(v.setup.skip)},{"reads",reads(v.setup.reads)},{"symbols",Json::object()},{"executions",Json::array()}};
if(v.setup.outcome==EquityStageOutcome::ReturnedOk){require(setup["reads"],{"capital_allocation","max_leverage","max_drawdown","reserve_capital","use_optimization","use_risk_management","allow_fractional_positions"});if(v.setup.skip!=EquityStageSkip::None)throw std::invalid_argument("success_skip");}
if(v.setup.outcome==EquityStageOutcome::NotReached && (v.setup.skip!=EquityStageSkip::None || !setup["reads"].empty() || !setup["symbols"].empty() || !setup["executions"].empty()))throw std::invalid_argument("unreached_has_reads");
if(v.setup.outcome==EquityStageOutcome::Skipped)throw std::invalid_argument("invalid_skip_branch");
if(v.setup.outcome!=EquityStageOutcome::ReturnedOk && v.setup.outcome!=EquityStageOutcome::Skipped){complete=false;if(v.setup.skip!=EquityStageSkip::None)throw std::invalid_argument("failed_skip");}
auto& market_input=j["stages"]["market_input"];market_input={{"outcome",outcome(v.market_input.outcome)},{"skip_reason",skip(v.market_input.skip)},{"reads",reads(v.market_input.reads)},{"symbols",Json::object()},{"executions",Json::array()}};
if(v.market_input.outcome==EquityStageOutcome::ReturnedOk){require(market_input["reads"],{"historical_days","asset_type","frequency","start_day","end_day"});if(v.market_input.skip!=EquityStageSkip::None)throw std::invalid_argument("success_skip");}
if(v.market_input.outcome==EquityStageOutcome::NotReached && (v.market_input.skip!=EquityStageSkip::None || !market_input["reads"].empty() || !market_input["symbols"].empty() || !market_input["executions"].empty()))throw std::invalid_argument("unreached_has_reads");
if(v.market_input.outcome==EquityStageOutcome::Skipped)throw std::invalid_argument("invalid_skip_branch");
if(v.market_input.outcome!=EquityStageOutcome::ReturnedOk && v.market_input.outcome!=EquityStageOutcome::Skipped){complete=false;if(v.market_input.skip!=EquityStageSkip::None)throw std::invalid_argument("failed_skip");}
auto& cost_history=j["stages"]["cost_history"];cost_history={{"outcome",outcome(v.cost_history.outcome)},{"skip_reason",skip(v.cost_history.skip)},{"reads",reads(v.cost_history.reads)},{"symbols",Json::object()},{"executions",Json::array()}};
if(v.cost_symbols.size()>256)throw std::length_error("capacity_exceeded");for(const auto& [symbol,read]:v.cost_symbols){if(!symbol_id(symbol))throw std::invalid_argument("invalid_symbol");cost_history["symbols"][symbol]=reads(read);}
if(v.cost_history.outcome==EquityStageOutcome::ReturnedOk){require(cost_history["reads"],{});if(v.cost_history.skip!=EquityStageSkip::None)throw std::invalid_argument("success_skip");}
if(v.cost_history.outcome==EquityStageOutcome::ReturnedOk)for(const auto& read:cost_history["symbols"].items())require(read.value(),{"adv_lookback_days","log_return_lookback_days","previous_close_source","previous_close_forwarded"});
if(v.cost_history.outcome==EquityStageOutcome::NotReached && (v.cost_history.skip!=EquityStageSkip::None || !cost_history["reads"].empty() || !cost_history["symbols"].empty() || !cost_history["executions"].empty()))throw std::invalid_argument("unreached_has_reads");
if(v.cost_history.outcome==EquityStageOutcome::Skipped)throw std::invalid_argument("invalid_skip_branch");
if(v.cost_history.outcome!=EquityStageOutcome::ReturnedOk && v.cost_history.outcome!=EquityStageOutcome::Skipped){complete=false;if(v.cost_history.skip!=EquityStageSkip::None)throw std::invalid_argument("failed_skip");}
auto& prior=j["stages"]["prior"];prior={{"outcome",outcome(v.prior.outcome)},{"skip_reason",skip(v.prior.skip)},{"reads",reads(v.prior.reads)},{"symbols",Json::object()},{"executions",Json::array()}};
if(v.prior.outcome==EquityStageOutcome::ReturnedOk){require(prior["reads"],{"mode","source_day"});if(v.prior.skip!=EquityStageSkip::None)throw std::invalid_argument("success_skip");}
if(v.prior.outcome==EquityStageOutcome::NotReached && (v.prior.skip!=EquityStageSkip::None || !prior["reads"].empty() || !prior["symbols"].empty() || !prior["executions"].empty()))throw std::invalid_argument("unreached_has_reads");
if(v.prior.outcome==EquityStageOutcome::Skipped)throw std::invalid_argument("invalid_skip_branch");
if(v.prior.outcome!=EquityStageOutcome::ReturnedOk && v.prior.outcome!=EquityStageOutcome::Skipped){complete=false;if(v.prior.skip!=EquityStageSkip::None)throw std::invalid_argument("failed_skip");}
auto& corporate_actions=j["stages"]["corporate_actions"];corporate_actions={{"outcome",outcome(v.corporate_actions.outcome)},{"skip_reason",skip(v.corporate_actions.skip)},{"reads",reads(v.corporate_actions.reads)},{"symbols",Json::object()},{"executions",Json::array()}};
if(v.corporate_actions.outcome==EquityStageOutcome::ReturnedOk){require(corporate_actions["reads"],{"path"});if(v.corporate_actions.skip!=EquityStageSkip::None)throw std::invalid_argument("success_skip");}
if(v.corporate_actions.outcome==EquityStageOutcome::NotReached && (v.corporate_actions.skip!=EquityStageSkip::None || !corporate_actions["reads"].empty() || !corporate_actions["symbols"].empty() || !corporate_actions["executions"].empty()))throw std::invalid_argument("unreached_has_reads");
if(v.corporate_actions.outcome==EquityStageOutcome::Skipped)throw std::invalid_argument("invalid_skip_branch");
if(v.corporate_actions.outcome!=EquityStageOutcome::ReturnedOk && v.corporate_actions.outcome!=EquityStageOutcome::Skipped){complete=false;if(v.corporate_actions.skip!=EquityStageSkip::None)throw std::invalid_argument("failed_skip");}
auto& preparation=j["stages"]["preparation"];preparation={{"outcome",outcome(v.preparation.outcome)},{"skip_reason",skip(v.preparation.skip)},{"reads",reads(v.preparation.reads)},{"symbols",Json::object()},{"executions",Json::array()}};
if(v.preparation_symbols.size()>256)throw std::length_error("capacity_exceeded");for(const auto& [symbol,read]:v.preparation_symbols){if(!symbol_id(symbol))throw std::invalid_argument("invalid_symbol");preparation["symbols"][symbol]=reads(read);}
if(v.preparation.outcome==EquityStageOutcome::ReturnedOk){require(preparation["reads"],{});if(v.preparation.skip!=EquityStageSkip::None)throw std::invalid_argument("success_skip");}
if(v.preparation.outcome==EquityStageOutcome::ReturnedOk)for(const auto& read:preparation["symbols"].items())require(read.value(),{"lookback_period","vol_lookback","max_history_size"});
if(v.preparation.outcome==EquityStageOutcome::NotReached && (v.preparation.skip!=EquityStageSkip::None || !preparation["reads"].empty() || !preparation["symbols"].empty() || !preparation["executions"].empty()))throw std::invalid_argument("unreached_has_reads");
if(v.preparation.outcome==EquityStageOutcome::Skipped && !(preparation["skip_reason"]=="non_trading_day"))throw std::invalid_argument("invalid_skip_branch");
if(v.preparation.outcome!=EquityStageOutcome::ReturnedOk && v.preparation.outcome!=EquityStageOutcome::Skipped){complete=false;if(v.preparation.skip!=EquityStageSkip::None)throw std::invalid_argument("failed_skip");}
auto& primary=j["stages"]["primary"];primary={{"outcome",outcome(v.primary.outcome)},{"skip_reason",skip(v.primary.skip)},{"reads",reads(v.primary.reads)},{"symbols",Json::object()},{"executions",Json::array()}};
if(v.primary.outcome==EquityStageOutcome::ReturnedOk){require(primary["reads"],{"strategy_invocation","portfolio_invocation"});if(v.primary.skip!=EquityStageSkip::None)throw std::invalid_argument("success_skip");}
if(v.primary.outcome==EquityStageOutcome::NotReached && (v.primary.skip!=EquityStageSkip::None || !primary["reads"].empty() || !primary["symbols"].empty() || !primary["executions"].empty()))throw std::invalid_argument("unreached_has_reads");
if(v.primary.outcome==EquityStageOutcome::Skipped && !(primary["skip_reason"]=="non_trading_day"))throw std::invalid_argument("invalid_skip_branch");
if(v.primary.outcome!=EquityStageOutcome::ReturnedOk && v.primary.outcome!=EquityStageOutcome::Skipped){complete=false;if(v.primary.skip!=EquityStageSkip::None)throw std::invalid_argument("failed_skip");}
auto& execution=j["stages"]["execution"];execution={{"outcome",outcome(v.execution.outcome)},{"skip_reason",skip(v.execution.skip)},{"reads",reads(v.execution.reads)},{"symbols",Json::object()},{"executions",Json::array()}};
if(v.executions.size()>4096)throw std::length_error("capacity_exceeded");std::set<std::string> ids;
for(std::size_t i=0;i<v.executions.size();++i){const auto& e=v.executions[i];if(!symbol_id(e.symbol) || e.portfolio_id!=v.portfolio_id || e.strategy_id!=v.strategy_id || e.strategy_name!=v.strategy_name || e.index!=i || e.execution_id.empty() || e.execution_id.size()>50 || !ids.insert(e.execution_id).second)throw std::invalid_argument("invalid_execution_identity");
execution["executions"].push_back({{"symbol",e.symbol},{"portfolio_id",e.portfolio_id},{"strategy_id",e.strategy_id},{"strategy_name",e.strategy_name},{"index",e.index},{"execution_id",e.execution_id},{"reads",reads(e.reads)}});}
if(v.execution.outcome==EquityStageOutcome::ReturnedOk){require(execution["reads"],{});if(v.execution.skip!=EquityStageSkip::None)throw std::invalid_argument("success_skip");}
if(v.execution.outcome==EquityStageOutcome::ReturnedOk)for(const auto& read:execution["executions"])require(read.at("reads"),{"quantity","reference_price","retrieved_adv","retrieved_volatility_multiplier","effective_adv","effective_volatility_multiplier","commission_per_unit","min_commission_per_order","max_commission_per_order","max_commission_pct","sec_fee_per_million","finra_taf_per_share","finra_taf_cap_per_trade","max_total_implicit_bps","point_value","volatility_lambda","volatility_min_multiplier","volatility_max_multiplier","baseline_spread_ticks","min_spread_ticks","max_spread_ticks","spread_cost_multiplier","tick_size","min_adv","min_participation","max_participation","max_impact_bps","selected_k_bps","apply_regulatory_fees","tick_constrained","asset_type","asset_lookup_path","input_source","commissions_fees_exact","implicit_price_impact_exact","slippage_market_impact_exact","total_transaction_costs_exact"});
if(v.execution.outcome==EquityStageOutcome::NotReached && (v.execution.skip!=EquityStageSkip::None || !execution["reads"].empty() || !execution["symbols"].empty() || !execution["executions"].empty()))throw std::invalid_argument("unreached_has_reads");
if(v.execution.outcome==EquityStageOutcome::Skipped && !(execution["skip_reason"]=="non_trading_day"))throw std::invalid_argument("invalid_skip_branch");
if(v.execution.outcome!=EquityStageOutcome::ReturnedOk && v.execution.outcome!=EquityStageOutcome::Skipped){complete=false;if(v.execution.skip!=EquityStageSkip::None)throw std::invalid_argument("failed_skip");}
auto& eod=j["stages"]["eod"];eod={{"outcome",outcome(v.eod.outcome)},{"skip_reason",skip(v.eod.skip)},{"reads",reads(v.eod.reads)},{"symbols",Json::object()},{"executions",Json::array()}};
if(v.eod.outcome==EquityStageOutcome::ReturnedOk){require(eod["reads"],{"path"});if(v.eod.skip!=EquityStageSkip::None)throw std::invalid_argument("success_skip");}
if(v.eod.outcome==EquityStageOutcome::NotReached && (v.eod.skip!=EquityStageSkip::None || !eod["reads"].empty() || !eod["symbols"].empty() || !eod["executions"].empty()))throw std::invalid_argument("unreached_has_reads");
if(v.eod.outcome==EquityStageOutcome::Skipped && !(eod["skip_reason"]=="proved_desk_successor"))throw std::invalid_argument("invalid_skip_branch");
if(v.eod.outcome!=EquityStageOutcome::ReturnedOk && v.eod.outcome!=EquityStageOutcome::Skipped){complete=false;if(v.eod.skip!=EquityStageSkip::None)throw std::invalid_argument("failed_skip");}
auto& result_assembly=j["stages"]["result_assembly"];result_assembly={{"outcome",outcome(v.result_assembly.outcome)},{"skip_reason",skip(v.result_assembly.skip)},{"reads",reads(v.result_assembly.reads)},{"symbols",Json::object()},{"executions",Json::array()}};
if(v.result_assembly.outcome==EquityStageOutcome::ReturnedOk){require(result_assembly["reads"],{"currency","current_portfolio_value_exact","total_realized_pnl_exact","total_unrealized_pnl_exact","total_transaction_costs_exact"});if(v.result_assembly.skip!=EquityStageSkip::None)throw std::invalid_argument("success_skip");}
if(v.result_assembly.outcome==EquityStageOutcome::NotReached && (v.result_assembly.skip!=EquityStageSkip::None || !result_assembly["reads"].empty() || !result_assembly["symbols"].empty() || !result_assembly["executions"].empty()))throw std::invalid_argument("unreached_has_reads");
if(v.result_assembly.outcome==EquityStageOutcome::Skipped)throw std::invalid_argument("invalid_skip_branch");
if(v.result_assembly.outcome!=EquityStageOutcome::ReturnedOk && v.result_assembly.outcome!=EquityStageOutcome::Skipped){complete=false;if(v.result_assembly.skip!=EquityStageSkip::None)throw std::invalid_argument("failed_skip");}
if(prior["reads"].contains("mode")){
if(prior["reads"]["mode"]=="system_reference" && (v.prior.outcome==EquityStageOutcome::ReturnedOk || v.prior.outcome==EquityStageOutcome::Skipped)){require(prior["reads"],{"mode","source_day"});forbid(prior["reads"],{"valuation_day","decision_id","finalization_id","finalization_digest","finalization_source_digest","accounting_input_digest","observation_digest","results_digest"});
}
if(prior["reads"]["mode"]=="verified_desk_prior" && (v.prior.outcome==EquityStageOutcome::ReturnedOk || v.prior.outcome==EquityStageOutcome::Skipped)){require(prior["reads"],{"mode","source_day","valuation_day","decision_id","finalization_id","finalization_digest","finalization_source_digest","accounting_input_digest","observation_digest","results_digest"});forbid(prior["reads"],{});
}
}
if(corporate_actions["reads"].contains("path")){
if(corporate_actions["reads"]["path"]=="system_history" && (v.corporate_actions.outcome==EquityStageOutcome::ReturnedOk || v.corporate_actions.outcome==EquityStageOutcome::Skipped)){require(corporate_actions["reads"],{"path","spinoff_child_policy_requested","spinoff_child_policy_effective","effective_event_count"});forbid(corporate_actions["reads"],{});
}
if(corporate_actions["reads"]["path"]=="proved_action_free_prior" && (v.corporate_actions.outcome==EquityStageOutcome::ReturnedOk || v.corporate_actions.outcome==EquityStageOutcome::Skipped)){require(corporate_actions["reads"],{"path","effective_event_count"});forbid(corporate_actions["reads"],{"spinoff_child_policy_requested","spinoff_child_policy_effective"});
if(corporate_actions["reads"].at("effective_event_count")!=0)throw std::invalid_argument("adjusted_prior");
if(prior["reads"].value("mode","")!="verified_desk_prior")throw std::invalid_argument("prior_mode_mismatch");
}
}
if(eod["reads"].contains("path")){
if(eod["reads"]["path"]=="system_finalization" && (v.eod.outcome==EquityStageOutcome::ReturnedOk || v.eod.outcome==EquityStageOutcome::Skipped)){require(eod["reads"],{"path","previous_equity_exact","previous_total_pnl_exact","previous_total_realized_pnl_exact","previous_total_transaction_costs_exact","initial_capital_exact"});forbid(eod["reads"],{});
}
if(eod["reads"]["path"]=="proved_desk_successor" && (v.eod.outcome==EquityStageOutcome::ReturnedOk || v.eod.outcome==EquityStageOutcome::Skipped)){require(eod["reads"],{"path","previous_equity_exact","previous_total_pnl_exact","previous_total_realized_pnl_exact","previous_total_transaction_costs_exact","initial_capital_exact"});forbid(eod["reads"],{});
if(prior["reads"].value("mode","")!="verified_desk_prior")throw std::invalid_argument("prior_mode_mismatch");
if(v.eod.outcome!=EquityStageOutcome::Skipped || v.eod.skip!=EquityStageSkip::ProvedDeskSuccessor)throw std::invalid_argument("successor_skip_mismatch");
}
}
const bool nontrading=v.primary.skip==EquityStageSkip::NonTradingDay;
if(nontrading || v.preparation.skip==EquityStageSkip::NonTradingDay || v.execution.skip==EquityStageSkip::NonTradingDay){for(const auto* s:{&preparation,&primary,&execution})if(s->at("outcome")!="skipped" || s->at("skip_reason")!="non_trading_day" || !s->at("reads").empty() || !s->at("symbols").empty() || !s->at("executions").empty())throw std::invalid_argument("nontrading_skip_mismatch");}
if(primary["reads"].contains("portfolio_invocation")) {
const auto& portfolio=primary["reads"]["portfolio_invocation"];
if(portfolio.at("available")!=true) {j["unavailable_reason"]=portfolio.at("unavailable_reason");return EquityRunProjection(std::move(j));}
for(const auto& pass:portfolio.at("passes")) {
if(!setup["reads"].contains("use_optimization") || !setup["reads"].contains("use_risk_management") ||
pass.at("reads").at("use_optimization")!=setup["reads"]["use_optimization"] ||
pass.at("reads").at("use_risk_management")!=setup["reads"]["use_risk_management"])
throw std::invalid_argument("portfolio_setup_flag_mismatch");
}
}
j["available"]=complete;j["complete"]=complete;j["unavailable_reason"]=complete?Json(nullptr):Json("stage_failed");
}catch(const std::length_error&){j["unavailable_reason"]="capacity_exceeded";}
catch(const std::exception&){j["unavailable_reason"]="invalid_observed_value";}
return EquityRunProjection(std::move(j));
}
} // namespace trade_ngin
