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

namespace {
EquityRunConsumption adjusted_reference(){auto r=complete_reference();r.prior.reads.mode="verified_desk_prior";r.prior.reads.valuation_day=r.date;r.prior.reads.decision_id="10000000-0000-4000-8000-000000000001";r.prior.reads.finalization_id="20000000-0000-4000-8000-000000000001";for(auto p:{&r.prior.reads.finalization_digest,&r.prior.reads.finalization_source_digest,&r.prior.reads.accounting_input_digest,&r.prior.reads.observation_digest,&r.prior.reads.results_digest})*p=std::string(64,'a');r.eod.outcome=EquityStageOutcome::Skipped;r.eod.skip=EquityStageSkip::ProvedDeskSuccessor;r.eod.reads.path="proved_desk_successor";r.corporate_actions.reads={};r.corporate_actions.reads.path="proved_action_adjusted_prior";r.corporate_actions.reads.effective_event_count=0;r.corporate_actions.reads.original_action_count=1;r.corporate_actions.reads.successor_action_count=0;for(auto p:{&r.corporate_actions.reads.original_action_digest,&r.corporate_actions.reads.successor_action_digest,&r.corporate_actions.reads.basis_frame_digest})*p=std::string(64,'b');return r;}
}
TEST(EquityActionRunConsumption, OldSystemReferenceBytesStayExactlyV1){auto r=project(complete_reference());EXPECT_EQ(r["schema_version"],"qt-equity-run-consumption/v1");EXPECT_EQ(r["catalog_version"],"qt-equity-main08b15c-run/v1");EXPECT_EQ(r["stages"]["corporate_actions"]["reads"].size(),4);EXPECT_TRUE(r["available"].get<bool>());}
TEST(EquityActionRunConsumption, OriginalActionOnlyHasClosedSevenReadV2){auto r=project(adjusted_reference());EXPECT_EQ(r["schema_version"],"qt-equity-run-consumption/v2");EXPECT_EQ(r["catalog_version"],"qt-equity-main08b15c-run/v2");EXPECT_EQ(r["stages"]["corporate_actions"]["reads"].size(),7);EXPECT_TRUE(r["complete"].get<bool>());}
TEST(EquityActionRunConsumption, DerivedActionOnlyRemainsActualAvailable){auto run=adjusted_reference();run.corporate_actions.reads.original_action_count=0;run.corporate_actions.reads.successor_action_count=1;EXPECT_TRUE(project(run)["available"].get<bool>());}
TEST(EquityActionRunConsumption, PositiveCountSumUsesWideIntermediate){auto run=adjusted_reference();run.corporate_actions.reads.original_action_count=INT32_MAX;run.corporate_actions.reads.successor_action_count=INT32_MAX;EXPECT_TRUE(project(run)["available"].get<bool>());}
TEST(EquityActionRunConsumption, EmptyFrameRefuses){auto run=adjusted_reference();run.corporate_actions.reads.original_action_count=0;unavailable(run);}
TEST(EquityActionRunConsumption, ReplayClaimRefuses){auto run=adjusted_reference();run.corporate_actions.reads.effective_event_count=1;unavailable(run);}
TEST(EquityActionRunConsumption, MissingReadRefuses){auto run=adjusted_reference();run.corporate_actions.reads.basis_frame_digest.reset();unavailable(run);}
TEST(EquityActionRunConsumption, NegativeCountRefuses){auto run=adjusted_reference();run.corporate_actions.reads.successor_action_count=-1;unavailable(run);}
TEST(EquityActionRunConsumption, SpinoffPolicyCannotEnterAdjustedPath){auto run=adjusted_reference();run.corporate_actions.reads.spinoff_child_policy_effective="hold";unavailable(run);}
TEST(EquityActionRunConsumption, BadDigestRefuses){auto run=adjusted_reference();run.corporate_actions.reads.basis_frame_digest="bad";unavailable(run);}
TEST(EquityActionRunConsumption, AdjustedPartialCannotClaimSystemPrior){auto run=adjusted_reference();run.corporate_actions.outcome=EquityStageOutcome::ReturnedError;run.prior.reads.mode="system_reference";auto r=project(run);EXPECT_EQ(r["schema_version"],"qt-equity-run-consumption/v2");EXPECT_EQ(r["unavailable_reason"],"invalid_observed_value");}
TEST(EquityActionRunConsumption, GenuineAdjustedPartialKeepsObservedReadsUnavailable){auto run=adjusted_reference();run.corporate_actions.outcome=EquityStageOutcome::ReturnedError;run.corporate_actions.reads.basis_frame_digest.reset();auto r=project(run);EXPECT_EQ(r["schema_version"],"qt-equity-run-consumption/v2");EXPECT_EQ(r["unavailable_reason"],"stage_failed");EXPECT_FALSE(r["complete"].get<bool>());}
TEST(EquityActionRunConsumption, NewReadsCannotLeakToOldPath){auto run=adjusted_reference();run.corporate_actions.reads.path="proved_action_free_prior";unavailable(run);}
