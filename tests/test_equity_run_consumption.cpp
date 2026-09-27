// Typed synthetic observations test projector semantics, not full-run certification.
#include <gtest/gtest.h>
#include <limits>
#include "trade_ngin/live/live_daily_cycle.hpp"
#include "trade_ngin/apps/equity_execution_consumption.hpp"
#include "trade_ngin/apps/equity_run_consumption.hpp"
using namespace trade_ngin;
namespace {
EquityRunConsumption complete_reference() {
    EquityRunConsumption run;
    run.portfolio_id="EQUITY_MR_PORTFOLIO";run.strategy_id="LIVE_EQUITY_MEAN_REVERSION";
    run.strategy_name="EQUITY_MEAN_REVERSION";run.date="2026-09-26";
    run.setup.outcome=run.market_input.outcome=run.cost_history.outcome=run.prior.outcome=
        run.corporate_actions.outcome=run.preparation.outcome=run.primary.outcome=
        run.execution.outcome=run.eod.outcome=run.result_assembly.outcome=EquityStageOutcome::ReturnedOk;
    run.setup.reads={100000.,2.,0.1,0.,false,false,true};
    run.market_input.reads={252,"EQUITY","DAILY","2025-09-26","2026-09-26",2};
    run.prior.reads.mode="system_reference";run.prior.reads.source_day="2026-09-25";
    run.corporate_actions.reads={"system_history","hold","hold",0};
    StrategyConsumptionTrace strategy;strategy.profile=StrategyConsumptionProfile::MeanReversion;
    run.primary.reads.strategy_invocation=strategy;
    PortfolioConsumptionTrace portfolio;portfolio.outcome=PortfolioCallOutcome::ReturnedOk;
    portfolio.skip_execution_generation=false;portfolio.pass_count=1;
    portfolio.passes[0].use_optimization=false;portfolio.passes[0].use_risk_management=false;
    run.primary.reads.portfolio_invocation=portfolio;
    run.eod.reads={"system_finalization",Decimal(100000),Decimal(0),Decimal(0),Decimal(0),Decimal(100000)};
    run.result_assembly.reads={"USD",Decimal(100000),Decimal(0),Decimal(0),Decimal(0)};
    return run;
}
nlohmann::json project(const EquityRunConsumption& run) {return project_equity_run_consumption(run).document();}
void unavailable(const EquityRunConsumption& run) {auto result=project(run);EXPECT_FALSE(result.at("available").get<bool>());EXPECT_FALSE(result.at("complete").get<bool>());}
}
TEST(EquityRunConsumption, CompleteTypedReferenceKeepsPartialPrimaryScope) {
    auto result=project(complete_reference());
    EXPECT_TRUE(result.at("available").get<bool>());EXPECT_TRUE(result.at("complete").get<bool>());
    EXPECT_EQ(result.at("stages").size(),10u);EXPECT_TRUE(result.at("unavailable_reason").is_null());
    const auto& nested=result.at("stages").at("primary").at("reads").at("strategy_invocation");
    EXPECT_EQ(nested.at("scope"),"strategy_invocation");EXPECT_FALSE(nested.at("full_run_certification").get<bool>());
    EXPECT_TRUE(result.at("stages").at("execution").at("executions").empty());
}
TEST(EquityRunConsumption, UnvisitedRequiredStageDoesNotBecomeComplete) {
    auto run=complete_reference();run.market_input={};unavailable(run);
    EXPECT_EQ(project(run).at("stages").at("market_input").at("reads"),nlohmann::json::object());
}
TEST(EquityRunConsumption, ActualFailureKeepsObservedReadWithoutDefaultFilling) {
    auto run=complete_reference();run.execution.outcome=EquityStageOutcome::ReturnedError;
    run.execution.reads.execution_price_max_staleness_days=3;
    auto result=project(run);unavailable(run);
    EXPECT_EQ(result.at("stages").at("execution").at("reads"),nlohmann::json({{"execution_price_max_staleness_days",3}}));
}
TEST(EquityRunConsumption, UnreachedReadIsContradictory) {
    auto run=complete_reference();run.cost_history.outcome=EquityStageOutcome::NotReached;
    run.cost_symbols["SYN"].adv_lookback_days=10;unavailable(run);
}
TEST(EquityRunConsumption, NonfiniteSetupRefused) {
    auto run=complete_reference();run.setup.reads.capital_allocation=std::numeric_limits<double>::infinity();unavailable(run);
}
TEST(EquityRunConsumption, VerifiedPriorRequiresEveryActualBinding) {
    auto run=complete_reference();run.prior.reads.mode="verified_desk_prior";
    run.prior.reads.valuation_day="2026-09-26";unavailable(run);
}
TEST(EquityRunConsumption, SystemReferenceNeverAcceptsDeskBindingFields) {
    auto run=complete_reference();run.prior.reads.decision_id="40000000-0000-4000-8000-000000000001";unavailable(run);
}
TEST(EquityRunConsumption, CorporateActionPolicyRequiredOnlyForActualSystemPath) {
    auto run=complete_reference();run.corporate_actions.reads.spinoff_child_policy_requested.reset();unavailable(run);
}
TEST(EquityRunConsumption, NontradingSkipsAreAllThreeReachedBranches) {
    auto run=complete_reference();run.preparation={};run.primary={};run.execution={};
    run.preparation.outcome=run.primary.outcome=run.execution.outcome=EquityStageOutcome::Skipped;
    run.preparation.skip=run.primary.skip=run.execution.skip=EquityStageSkip::NonTradingDay;
    EXPECT_TRUE(project(run).at("complete").get<bool>());
    run.preparation.skip=EquityStageSkip::None;unavailable(run);
}
TEST(EquityRunConsumption, NontradingDoesNotInventPrimaryReads) {
    auto run=complete_reference();run.primary.outcome=EquityStageOutcome::Skipped;
    run.primary.skip=EquityStageSkip::NonTradingDay;unavailable(run);
}
TEST(EquityRunConsumption, RiskSetupMustMatchActualPortfolioPass) {
    auto run=complete_reference();run.setup.reads.use_risk_management=true;unavailable(run);
    EXPECT_EQ(project(run).at("unavailable_reason"),"invalid_observed_value");
}
TEST(EquityRunConsumption, EnabledUnprojectedOptimizerIsExplicitlyUnavailable) {
    auto run=complete_reference();run.setup.reads.use_optimization=true;run.primary.reads.portfolio_invocation->passes[0].use_optimization=true;unavailable(run);
    EXPECT_EQ(project(run).at("unavailable_reason"),"unsupported_enabled_helper");
}
TEST(EquityRunConsumption, ForeignOwnerInExecutionIsRefusedBeforeReadAdmission) {
    auto run=complete_reference();EquityExecutionConsumption execution;
    execution.symbol="SYN";execution.portfolio_id="FOREIGN";execution.strategy_id=run.strategy_id;
    execution.strategy_name=run.strategy_name;execution.execution_id="actual-test-execution";
    run.executions.push_back(execution);unavailable(run);
}
TEST(EquityRunConsumption, SequenceCannotHideAReorderedExecution) {
    auto run=complete_reference();EquityExecutionConsumption execution;
    execution.symbol="SYN";execution.portfolio_id=run.portfolio_id;execution.strategy_id=run.strategy_id;
    execution.strategy_name=run.strategy_name;execution.execution_id="actual-test-execution";execution.index=1;
    run.executions.push_back(execution);unavailable(run);
}
TEST(EquityRunConsumption, MissingExecutionReadsRemainUnavailable) {
    auto run=complete_reference();EquityExecutionConsumption execution;
    execution.symbol="SYN";execution.portfolio_id=run.portfolio_id;execution.strategy_id=run.strategy_id;
    execution.strategy_name=run.strategy_name;execution.execution_id="actual-test-execution";
    run.executions.push_back(execution);unavailable(run);
}
TEST(EquityRunConsumption, MissingPerSymbolReadsRemainUnavailable) {
    auto run=complete_reference();run.preparation_symbols["SYN"].lookback_period=25;unavailable(run);
}

TEST(EquityRunConsumption, ActualCalendarRejectsImpossibleDates) {
    for(const auto& date:{"2026-02-29","2026-04-31","0000-01-01","2026-13-01"}){
        auto run=complete_reference();run.date=date;unavailable(run);
    }
    auto run=complete_reference();run.date="2028-02-29";
    EXPECT_TRUE(project(run).at("available").get<bool>());
}
TEST(EquityRunConsumption, PriorAndMarketDatesHaveActualRunBounds) {
    auto run=complete_reference();run.prior.reads.source_day=run.date;unavailable(run);
    run=complete_reference();run.market_input.reads.start_day="2026-09-27";unavailable(run);
    run=complete_reference();run.market_input.reads.end_day="2026-09-27";unavailable(run);
    run=complete_reference();run.market_input.reads.start_day="2025-02-29";unavailable(run);
    run=complete_reference();run.prior.reads.source_day="2026-09-31";unavailable(run);
}
TEST(EquityRunConsumption, DecimalTransportUsesCanonicalCheckedInt64Extremes) {
    auto run=complete_reference();
    run.result_assembly.reads.current_portfolio_value_exact=Decimal::from_raw(std::numeric_limits<int64_t>::max());
    run.result_assembly.reads.total_realized_pnl_exact=Decimal::from_raw(std::numeric_limits<int64_t>::min());
    const auto result=project(run);EXPECT_TRUE(result.at("available").get<bool>());
    const auto& reads=result.at("stages").at("result_assembly").at("reads");
    EXPECT_EQ(reads.at("current_portfolio_value_exact"),"92233720368.54775807");
    EXPECT_EQ(reads.at("total_realized_pnl_exact"),"-92233720368.54775808");
    EXPECT_EQ(reads.at("total_unrealized_pnl_exact"),"0");
}
TEST(EquityRunConsumption, InvalidStrategyReadCannotBeCertifiedByValidPortfolio) {
    auto run=complete_reference();
    run.primary.reads.strategy_invocation->mean_reversion.symbols["SYN"].entry_threshold=std::numeric_limits<double>::infinity();
    unavailable(run);
}
TEST(EquityRunConsumption, RiskCannotBorrowOptimizerSkipReason) {
    auto run=complete_reference();run.setup.reads.use_risk_management=true;
    auto& pass=run.primary.reads.portfolio_invocation->passes[0];pass.use_risk_management=true;
    pass.risk_helper=PortfolioCallOutcome::ReturnedOk;pass.risk.skip=PortfolioHelperSkip::AbsentOptimizer;
    pass.risk.source=PortfolioRiskManagerSource::Absent;unavailable(run);
}
TEST(EquityRunConsumption, ActualEnabledRiskNoPositionsBranchIsRepresented) {
    auto run=complete_reference();run.setup.reads.use_risk_management=true;
    auto& pass=run.primary.reads.portfolio_invocation->passes[0];pass.use_risk_management=true;
    pass.risk_helper=PortfolioCallOutcome::ReturnedOk;pass.risk.skip=PortfolioHelperSkip::NoPositions;
    pass.risk.source=PortfolioRiskManagerSource::Internal;pass.risk.lookback_period=60;
    EXPECT_TRUE(project(run).at("available").get<bool>());
}

namespace {
EquityRunConsumption actual_cost_run(bool warm,bool sell,bool percent,bool fallback=false) {
    auto run=complete_reference();ExecutionManager manager;
    auto asset=transaction_cost::AssetCostConfigRegistry::get_equity_default_config();
    asset.symbol="SYN";asset.apply_regulatory_fees=true;
    asset.commission_per_unit=fallback?-1.:0.005;asset.max_commission_pct=percent?0.01:-1.;asset.max_commission_per_order=10.;
    auto& cost=manager.get_transaction_cost_manager();cost.register_asset_config(asset);
    Timestamp date;core::parse_utc_date(run.date,date);
    const std::vector<Bar> bars={Bar(date-std::chrono::hours(72),100.,101.,99.,100.,100000.,"SYN"),
        Bar(date-std::chrono::hours(48),101.,102.,100.,101.,100000.,"SYN"),
        Bar(date-std::chrono::hours(24),102.,103.,101.,102.,100000.,"SYN")};
    std::vector<Bar> feed=warm?bars:std::vector<Bar>{bars.front()};
    std::unordered_map<std::string,LiveDailyCycle::EquityCostFeedSymbol> consumed;
    LiveDailyCycle::feed_cost_model(cost,{"SYN"},{{"SYN",feed}},21,&consumed);
    const auto& observed=consumed.at("SYN");auto& history=run.cost_symbols["SYN"];
    if(observed.market_data.volume.adv_lookback_days)history.adv_lookback_days=static_cast<int>(*observed.market_data.volume.adv_lookback_days);
    if(observed.market_data.log_returns.lookback_days)history.log_return_lookback_days=static_cast<int>(*observed.market_data.log_returns.lookback_days);
    history.previous_close_forwarded=observed.previous_close_forwarded;history.previous_close_source=warm?"stored_previous_close":"no_previous_close";
    std::unordered_map<std::string,Position> current,previous;
    Position one{"SYN",Quantity(1),Price(100),Decimal(0),Decimal(0),date};
    (sell?previous:current)["SYN"]=one;
    DailyExecutionObservation calls;auto result=manager.generate_daily_executions(current,previous,{{"SYN",102.}},date,PricingPolicy::STRICT,nullptr,&calls);
    if(result.is_error() || result.value().size()!=1 || calls.attempts.size()!=1)throw std::runtime_error("actual_cost_setup");
    auto entry=equity_execution_consumption(result.value().front(),calls.attempts.front(),run.portfolio_id,run.strategy_id,run.strategy_name,0);
    if(entry.is_error())throw std::runtime_error("actual_cost_projection");run.executions.push_back(entry.value());return run;
}
}
TEST(EquityRunConsumption, ActualColdBuyPercentageCeilingOmitsUncalledReads) {
    auto run=actual_cost_run(false,false,true);const auto result=project(run);EXPECT_TRUE(result.at("complete").get<bool>());
    const auto& reads=result.at("stages").at("execution").at("executions").at(0).at("reads");
    EXPECT_EQ(reads.at("volatility_calculation_reached"),false);EXPECT_FALSE(reads.contains("volatility_lambda"));
    EXPECT_FALSE(reads.contains("max_commission_per_order"));EXPECT_FALSE(reads.contains("sec_fee_per_million"));
    EXPECT_FALSE(result.at("stages").at("cost_history").at("symbols").at("SYN").contains("log_return_lookback_days"));
}
TEST(EquityRunConsumption, ActualWarmFeeSellOrderCeilingIncludesReachedReads) {
    auto run=actual_cost_run(true,true,false);auto result=project(run);EXPECT_TRUE(result.at("complete").get<bool>());
    const auto& reads=result.at("stages").at("execution").at("executions").at(0).at("reads");
    EXPECT_EQ(reads.at("volatility_calculation_reached"),true);EXPECT_LT(reads.at("quantity").get<double>(),0);
    EXPECT_TRUE(reads.contains("max_commission_per_order"));EXPECT_TRUE(reads.contains("sec_fee_per_million"));
    EXPECT_TRUE(reads.contains("volatility_lambda"));
}
TEST(EquityRunConsumption, ActualCommissionFallbackHasOnlyActualFeeRead) {
    const auto result=project(actual_cost_run(false,false,false,true));EXPECT_TRUE(result.at("complete").get<bool>());
    const auto& reads=result.at("stages").at("execution").at("executions").at(0).at("reads");
    EXPECT_TRUE(reads.contains("explicit_fee_per_contract"));EXPECT_FALSE(reads.contains("min_commission_per_order"));
    EXPECT_FALSE(reads.contains("max_commission_pct"));EXPECT_FALSE(reads.contains("max_commission_per_order"));
}
TEST(EquityRunConsumption, ActualUnchangedBookReturnsNoExecutionEvidence) {
    auto run=complete_reference();ExecutionManager manager;Timestamp date;core::parse_utc_date(run.date,date);
    std::unordered_map<std::string,Position> book{{"SYN",Position{"SYN",Quantity(1),Price(100),Decimal(0),Decimal(0),date}}};
    DailyExecutionObservation observations;auto result=manager.generate_daily_executions(book,book,{{"SYN",100.}},date,PricingPolicy::STRICT,nullptr,&observations);
    ASSERT_TRUE(result.is_ok());EXPECT_TRUE(result.value().empty());EXPECT_TRUE(observations.attempts.empty());
    EXPECT_EQ(observations.state,DailyExecutionState::returned);EXPECT_TRUE(project(run).at("complete").get<bool>());
}
TEST(EquityRunConsumption, RiskNoPositionsRequiresActuallyReadLookback) {
    auto run=complete_reference();run.setup.reads.use_risk_management=true;auto& pass=run.primary.reads.portfolio_invocation->passes[0];
    pass.use_risk_management=true;pass.risk_helper=PortfolioCallOutcome::ReturnedOk;pass.risk.skip=PortfolioHelperSkip::NoPositions;
    pass.risk.source=PortfolioRiskManagerSource::Internal;unavailable(run);
}
TEST(EquityRunConsumption, AbsentRiskManagerCannotInventLookback) {
    auto run=complete_reference();run.setup.reads.use_risk_management=true;auto& pass=run.primary.reads.portfolio_invocation->passes[0];
    pass.use_risk_management=true;pass.risk_helper=PortfolioCallOutcome::ReturnedOk;pass.risk.skip=PortfolioHelperSkip::AbsentRiskManager;
    pass.risk.source=PortfolioRiskManagerSource::Absent;pass.risk.lookback_period=60;unavailable(run);
    pass.risk.lookback_period.reset();EXPECT_TRUE(project(run).at("available").get<bool>());
}


TEST(EquityRunConsumption, PartialMarketChronologyIsStillActualEvidence) {
    for(const auto state:{EquityStageOutcome::ReturnedError,EquityStageOutcome::Threw}) {
        auto run=complete_reference();run.market_input.outcome=state;
        run.market_input.reads.start_day="2026-09-27";
        EXPECT_EQ(project(run).at("unavailable_reason"),"invalid_observed_value");
        run=complete_reference();run.market_input.outcome=state;
        run.market_input.reads.end_day="2026-09-27";
        EXPECT_EQ(project(run).at("unavailable_reason"),"invalid_observed_value");
    }
}
TEST(EquityRunConsumption, PartialPriorChronologyIsStillActualEvidence) {
    for(const auto state:{EquityStageOutcome::ReturnedError,EquityStageOutcome::Threw}) {
        auto run=complete_reference();run.prior.outcome=state;run.prior.reads.source_day=run.date;
        EXPECT_EQ(project(run).at("unavailable_reason"),"invalid_observed_value");
        run=complete_reference();run.prior.outcome=state;run.prior.reads.mode="verified_desk_prior";
        run.prior.reads.valuation_day="2026-09-27";
        EXPECT_EQ(project(run).at("unavailable_reason"),"invalid_observed_value");
    }
}
TEST(EquityRunConsumption, UnavailablePortfolioCannotContradictActualSetupFlags) {
    auto run=complete_reference();run.primary.outcome=EquityStageOutcome::ReturnedError;
    auto& portfolio=*run.primary.reads.portfolio_invocation;portfolio.outcome=PortfolioCallOutcome::ReturnedError;
    portfolio.passes[0].use_risk_management=true;portfolio.passes[0].risk_helper=PortfolioCallOutcome::ReturnedError;
    EXPECT_EQ(project(run).at("unavailable_reason"),"invalid_observed_value");
}
TEST(EquityRunConsumption, MatchingPartialPortfolioPreservesObservedFailure) {
    auto run=complete_reference();run.primary.outcome=EquityStageOutcome::ReturnedError;
    run.primary.reads.portfolio_invocation->outcome=PortfolioCallOutcome::ReturnedError;
    auto result=project(run);EXPECT_FALSE(result.at("available").get<bool>());
    EXPECT_EQ(result.at("unavailable_reason"),"stage_failed");
    EXPECT_EQ(result.at("stages").at("primary").at("reads").at("portfolio_invocation").at("outcome"),"returned_error");
}
