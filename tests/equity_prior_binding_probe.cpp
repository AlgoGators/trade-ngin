// Lane N3 copy of equity_prior_publication_probe.cpp (equity day 2 binding write).
// Actual prior proof + actual native publisher. The new MODEL payload is synthetic.
// This probe certifies atomic protocol behavior, never a strategy/full-run invocation.
// Additions: the prior trace follows the captured reference (v1 action-free or v2
// action frame); "publish_any" reports commit or refusal without judging it;
// "system_reference_publish" is a non-verified (PublicationPriorRequirement::None) publish.
// r2 (review F7.1): "publish_empty_owner" is a VerifiedEquity publish whose D MODEL run ends flat
// (explicit empty member batch + complete verified trace), i.e. an empty-owner v2 publication.
// N5: "model_only_uncontrolled" is "system_reference_publish" without runtime control (revision-0 path).
#include <cstdlib>
#include <iostream>
#include <set>
#include <unordered_map>
#include "trade_ngin/apps/equity_model_prior.hpp"
#include "trade_ngin/apps/equity_run_consumption.hpp"
#include "trade_ngin/apps/live_portfolio_helpers.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/portfolio/qt_wire.hpp"
using namespace trade_ngin;
namespace {
constexpr auto strategy="LIVE_EQUITY_MEAN_REVERSION";
constexpr auto name="EQUITY_MEAN_REVERSION";
Decimal decimal(const nlohmann::json& node) {
    auto parsed=parse_qt_quantity_exact(node.get<std::string>());
    if(parsed.is_error())throw std::runtime_error("invalid_exact_probe_operand");
    return parsed.value();
}
EquityRunProjection prior_only_projection(const VerifiedEquityModelPrior& capture,
    const EquityModelPriorOwner& owner,const std::string& mode) {
    EquityRunConsumption run;run.portfolio_id=owner.portfolio_id;run.strategy_id=owner.strategy_id;
    run.strategy_name=owner.strategy_name;run.date=owner.valuation_day;
    run.prior.outcome=EquityStageOutcome::ReturnedOk;
    const auto& ref=capture.replay_reference;
    auto& p=run.prior.reads;
    p.mode=ref.at("mode").get<std::string>();p.source_day=ref.at("source_day").get<std::string>();p.valuation_day=ref.at("valuation_day").get<std::string>();
    p.decision_id=ref.at("decision_id").get<std::string>();p.finalization_id=ref.at("finalization_id").get<std::string>();
    p.finalization_digest=ref.at("finalization_digest").get<std::string>();p.finalization_source_digest=ref.at("finalization_source_digest").get<std::string>();
    p.accounting_input_digest=ref.at("accounting_input_digest").get<std::string>();p.observation_digest=ref.at("observation_digest").get<std::string>();
    p.results_digest=ref.at("results_digest").get<std::string>();
    if(mode=="trace_mismatch")p.results_digest=std::string(64,'f');
    run.corporate_actions.outcome=EquityStageOutcome::ReturnedOk;
    run.corporate_actions.reads.path="proved_action_free_prior";
    run.corporate_actions.reads.effective_event_count=0;
    if(ref.at("schema_version")=="qt-equity-model-prior/v2") {
        const auto& frame=ref.at("action_frame");auto& a=run.corporate_actions.reads;
        a.path="proved_action_adjusted_prior";
        a.original_action_count=frame.at("original_action_count").get<int>();
        a.successor_action_count=frame.at("successor_action_count").get<int>();
        a.original_action_digest=frame.at("original_action_digest").get<std::string>();
        a.successor_action_digest=frame.at("successor_action_digest").get<std::string>();
        a.basis_frame_digest=ref.at("action_frame_digest").get<std::string>();
    }
    run.eod.outcome=EquityStageOutcome::Skipped;run.eod.skip=EquityStageSkip::ProvedDeskSuccessor;
    const auto& row=capture.financial.at("live_results").at(0);
    run.eod.reads={"proved_desk_successor",decimal(row.at("current_portfolio_value_exact")),
        decimal(row.at("total_pnl_exact")),decimal(row.at("total_realized_pnl_exact")),
        decimal(row.at("total_transaction_costs_exact")),decimal(row.at("initial_capital_exact"))};
    if(mode=="eod_equity")run.eod.reads.previous_equity_exact=Decimal(999);
    if(mode=="eod_pnl")run.eod.reads.previous_total_pnl_exact=Decimal(999);
    if(mode=="eod_realized")run.eod.reads.previous_total_realized_pnl_exact=Decimal(999);
    if(mode=="eod_cost")run.eod.reads.previous_total_transaction_costs_exact=Decimal(999);
    if(mode=="eod_capital")run.eod.reads.initial_capital_exact=Decimal(999);
    return project_equity_run_consumption(run);
}
// r2: a COMPLETE, available verified-prior run trace (every stage observed), as an
// empty-owner publication requires. Stage reads mirror the installed
// apps/tools/qt_empty_owner_publication_probe.cpp observed(); prior, corporate
// actions and EOD are the verified-prior reads of prior_only_projection.
EquityRunProjection complete_verified_projection(const VerifiedEquityModelPrior& capture,
    const EquityModelPriorOwner& owner,double capital) {
    EquityRunConsumption r;r.portfolio_id=owner.portfolio_id;r.strategy_id=owner.strategy_id;
    r.strategy_name=owner.strategy_name;r.date=owner.valuation_day;
    r.setup.outcome=r.market_input.outcome=r.cost_history.outcome=r.prior.outcome=r.corporate_actions.outcome=
    r.preparation.outcome=r.primary.outcome=r.execution.outcome=r.result_assembly.outcome=EquityStageOutcome::ReturnedOk;
    r.setup.reads={capital,2.,0.1,0.,false,false,true};
    r.market_input.reads={252,"EQUITY","DAILY","2025-09-26",owner.valuation_day,2};
    StrategyConsumptionTrace strategy_trace;strategy_trace.profile=StrategyConsumptionProfile::MeanReversion;
    r.primary.reads.strategy_invocation=strategy_trace;
    PortfolioConsumptionTrace portfolio;portfolio.outcome=PortfolioCallOutcome::ReturnedOk;portfolio.skip_execution_generation=false;
    portfolio.pass_count=1;portfolio.passes[0].use_optimization=false;portfolio.passes[0].use_risk_management=false;
    r.primary.reads.portfolio_invocation=portfolio;
    r.result_assembly.reads={"USD",Decimal(capital),Decimal(0),Decimal(0),Decimal(0)};
    const auto& ref=capture.replay_reference;auto& p=r.prior.reads;
    p.mode=ref.at("mode").get<std::string>();p.source_day=ref.at("source_day").get<std::string>();p.valuation_day=ref.at("valuation_day").get<std::string>();
    p.decision_id=ref.at("decision_id").get<std::string>();p.finalization_id=ref.at("finalization_id").get<std::string>();
    p.finalization_digest=ref.at("finalization_digest").get<std::string>();p.finalization_source_digest=ref.at("finalization_source_digest").get<std::string>();
    p.accounting_input_digest=ref.at("accounting_input_digest").get<std::string>();p.observation_digest=ref.at("observation_digest").get<std::string>();
    p.results_digest=ref.at("results_digest").get<std::string>();
    r.corporate_actions.reads.path="proved_action_free_prior";r.corporate_actions.reads.effective_event_count=0;
    r.eod.outcome=EquityStageOutcome::Skipped;r.eod.skip=EquityStageSkip::ProvedDeskSuccessor;
    const auto& row=capture.financial.at("live_results").at(0);
    r.eod.reads={"proved_desk_successor",decimal(row.at("current_portfolio_value_exact")),
        decimal(row.at("total_pnl_exact")),decimal(row.at("total_realized_pnl_exact")),
        decimal(row.at("total_transaction_costs_exact")),decimal(row.at("initial_capital_exact"))};
    return project_equity_run_consumption(r);
}
}
int main(int argc,char** argv) {try {
    if(argc!=7)return 64;
    const std::string mode=argv[1],book=argv[4],source_day=argv[5],day=argv[6];
    const std::set<std::string> modes{"snapshot","publish","missing_capture","bad_capture", "duplicate_capture",
        "wrong_owner","missing_replay","replay_mismatch","trace_mismatch","wrong_token","duplicate_trace",
        "changed_prior","incomplete","legacy_evidence","callback_failure","uow_historical","uow_wrong_owner",
        "uow_wrong_book","uow_current","uow_future","uow_failed_pending","uow_verified_prior",
        "eod_equity","eod_pnl","eod_realized","eod_cost","eod_capital","publish_any","system_reference_publish","publish_empty_owner",
        "model_only_uncontrolled"};
    if(!modes.contains(mode))return 64;
    AppConfig synthetic;synthetic.portfolio_id=book;
    synthetic.strategies_config={{name,{{"enabled_live",true},{"default_allocation",1.0},
        {"type","MeanReversionStrategy"}}}};
    auto snapshot=build_runtime_trading_snapshot(synthetic);if(snapshot.is_error())return 65;
    if(mode=="snapshot"){std::cout<<snapshot.value().dump()<<'\n';return 0;}
    const char* raw=std::getenv("ALGOLENS_TEST_DB");const std::string dsn=raw?raw:"";
    if(dsn.rfind("host=/tmp/algolens-repair-pg-",0)!=0 || dsn.find(" dbname=algolens_test_")==std::string::npos ||
        dsn.find("hostaddr")!=std::string::npos || dsn.find("service=")!=std::string::npos)return 66;
    Timestamp date;if(!core::parse_utc_date(day,date))return 67;
    PostgresDatabase database(dsn);if(database.connect().is_error())return 68;
    PublicationEvidenceToken token;
    const bool uow_mode=mode.starts_with("uow_");
    // N5: "model_only_uncontrolled" is the scheduled model-only run (no approved intent: the legacy
    // revision-0 path) with the same non-verified system-reference publication.
    const bool uncontrolled=mode=="model_only_uncontrolled";
    const bool system_reference=mode=="system_reference_publish" || uncontrolled;
    auto begun=database.begin_live_publication(strategy,book,date,snapshot.value(),!uncontrolled,"synthetic-mr-bridge-test",
        mode=="legacy_evidence"?PublicationEvidenceRequirement::LegacyNotCollected:
            PublicationEvidenceRequirement::RequiredFinalObservations,&token,
        (uow_mode && mode!="uow_verified_prior") || system_reference?
            PublicationPriorRequirement::None:PublicationPriorRequirement::VerifiedEquity);
    if(mode=="legacy_evidence"){
        if(!begun.is_error() || token.valid())return 80;
        std::cout<<"EQ_LEGACY_PRIOR_REFUSED=1\n";return 0;
    }
    if(begun.is_error() || begun.value() || !token.valid())return 69;
    EquityModelPriorOwner owner{book,strategy,name,source_day,day};
    EquityModelPriorSelection selection;selection.mode=EquityModelPriorMode::VerifiedDeskPrior;
    selection.decision_id=argv[2];selection.finalization_id=argv[3];
    if(uow_mode) {
        if(mode=="uow_failed_pending") {
            EquityRunConsumption unobserved;unobserved.portfolio_id=book;unobserved.strategy_id=strategy;
            unobserved.strategy_name=name;unobserved.date=day;
            if(database.attach_equity_run_consumption(PublicationEvidenceToken{},
                project_equity_run_consumption(unobserved)).is_ok())return 85;
        }
        Timestamp historical;if(!core::parse_utc_date(source_day,historical))return 86;
        if(mode=="uow_current")historical=date;
        if(mode=="uow_future")historical=date+std::chrono::hours(24);
        const auto write_owner=mode=="uow_wrong_owner"?"FOREIGN":strategy;
        const auto write_book=mode=="uow_wrong_book"?"FOREIGN":book;
        bool stored_ok=false;
        {
            // The Result owns the UOW; scope exit rolls back a refused transaction
            // before abandoning the pending publication (value() is const).
            auto tx=database.begin_unit_of_work();if(tx.is_error())return 87;
            Position prior{"SYN",Quantity(99),Price(100),Decimal(0),Decimal(0),historical};
            auto stored=database.store_positions(*tx.value(),{prior},write_owner,name,write_book,"trading.positions");
            stored_ok=stored.is_ok();
            if(stored_ok!=(mode=="uow_historical"))return 88;
            if(stored_ok && tx.value()->commit().is_error())return 89;
        }
        database.abandon_live_publication();
        std::cout<<(stored_ok?"EQ_HISTORICAL_UOW_COMMITTED=1\n":"EQ_HISTORICAL_UOW_REFUSED=1\n");return 0;
    }
    if(mode=="bad_capture")selection.decision_id="40000000-0000-4000-8000-000000000099";
    if(mode=="wrong_owner")owner.portfolio_id="FOREIGN";
    std::optional<VerifiedEquityModelPrior> captured;
    if(mode!="missing_capture" && !system_reference) {
        auto result=database.capture_equity_model_prior(selection,owner);
        const bool expected_refusal=mode=="bad_capture" || mode=="wrong_owner";
        if(result.is_error()!=expected_refusal)return 70;
        if(result.is_ok())captured=result.value();
        if(expected_refusal) {
            if(database.publish_live_publication().is_ok())return 71;
            std::cout<<"EQ_CAPTURE_REFUSED=1\n";return 0;
        }
        if(mode=="duplicate_capture") {
            if(database.capture_equity_model_prior(selection,owner).is_ok() ||
                database.publish_live_publication().is_ok())return 72;
            std::cout<<"EQ_CAPTURE_REFUSED=1\n";return 0;
        }
    }
    if(mode=="changed_prior") {
        std::cout<<"EQ_PRIOR_CAPTURED=1"<<std::endl;std::string command;
        if(!std::getline(std::cin,command) || command!="publish")return 73;
    }
    const bool empty_owner=mode=="publish_empty_owner";
    const double capital=100000.;
    std::vector<Position> fresh{{"SYN",Quantity(12),Price(100),Decimal(0),Decimal(0),date}};
    if(database.clear_equity_model_current_positions(book,date).is_error())return 90;
    if(empty_owner) {
        // The D MODEL run ends flat: the one configured member returns an explicit empty batch,
        // so the publication is an empty-owner v2 row (created_at = clock_timestamp()).
        QtModelPositionBatch flat{book,strategy,name,day,{}};
        if(database.store_model_position_batch(flat).is_error())return 92;
    } else if(database.store_positions(fresh,strategy,name,book,"trading.positions").is_error())return 74;
    if(mode!="incomplete" && database.store_risk_limits(strategy,book,{{"max_gross_leverage",2.}}).is_error())return 75;
    const std::unordered_map<std::string,double> flat_totals{{"daily_pnl",0.},{"daily_realized_pnl",0.},
        {"daily_unrealized_pnl",0.},{"daily_transaction_costs",0.},{"total_pnl",0.},{"total_realized_pnl",0.},
        {"total_unrealized_pnl",0.},{"total_transaction_costs",0.},{"current_portfolio_value",capital}};
    if(database.store_live_results_complete(strategy,date,empty_owner?flat_totals:
        std::unordered_map<std::string,double>{{mode=="callback_failure"?"does_not_exist":"total_pnl",123}}, {},{},book).is_error())return 76;
    const nlohmann::json raw_capture={{"capture_schema_version",2},{"captured_at",day+"T12:00:00.000000Z"}};
    if(database.store_live_run_metadata(date,strategy,book,{{name,1.}},
        {{"total_capital",capital},{"config_inspection",raw_capture}},{}).is_error())return 77;
    if(database.store_trading_equity_curve(strategy,date,empty_owner?capital:100123,book).is_error() ||
        (empty_owner && database.seed_qt_proposal_positions_from_system(strategy,name,book,day).is_error()) ||
        database.seed_qt_positions_from_system(strategy,name,book,day).is_error())return 78;
    nlohmann::json flags=nlohmann::json::object();
    if(captured)flags["equity_model_prior"]=captured->replay_reference;
    if(mode=="missing_replay")flags.erase("equity_model_prior");
    if(mode=="replay_mismatch")flags["equity_model_prior"]["results_digest"]=std::string(64,'f');
    auto stored=database.store_live_run_inputs(strategy,book,date,{{"trade_ngin_sha","synthetic-mr-bridge-test"},
        {"config_snapshot",snapshot.value()},{"universe",{"SYN"}},{"data_window",{}},{"engine_flags",flags}});
    const bool bad_replay=mode=="missing_replay" || mode=="replay_mismatch" || mode=="missing_capture";
    if(stored.is_error()!=bad_replay)return 79;
    if(system_reference) {
        // Non-verified MODEL publish: the prior stage is the system reference.
        EquityRunConsumption run;run.portfolio_id=book;run.strategy_id=strategy;run.strategy_name=name;run.date=day;
        run.prior.outcome=EquityStageOutcome::ReturnedOk;run.prior.reads.mode="system_reference";
        if(database.attach_equity_run_consumption(token,project_equity_run_consumption(run)).is_error())return 91;
    }
    if(captured && mode!="duplicate_capture") {
        auto projection=empty_owner?complete_verified_projection(*captured,owner,capital):prior_only_projection(*captured,owner,mode);
        if(empty_owner && (projection.document().at("available")!=true || projection.document().at("complete")!=true))return 93;
        const auto attached=database.attach_equity_run_consumption(mode=="wrong_token"?PublicationEvidenceToken{}:token,projection);
        const bool bad_attach=bad_replay || mode=="trace_mismatch" || mode=="wrong_token" || mode.starts_with("eod_");
        if(attached.is_error()!=bad_attach)return 81;
        if(mode=="duplicate_trace" && database.attach_equity_run_consumption(token,projection).is_ok())return 82;
    }
    if(mode!="incomplete" && !bad_replay)std::cout<<"EQ_ALL_PARTS_QUEUED=1\n";
    auto published=database.publish_live_publication();
    if(empty_owner)std::cout<<"EQ_EMPTY_OWNER_BATCH=1\n";
    if(mode!="publish_any" && !empty_owner && published.is_ok()!=(mode=="publish" || system_reference))return 83;
    std::cout<<(published.is_ok()?"EQ_PUBLICATION_COMMITTED=1\n":"EQ_PUBLICATION_REFUSED=1\n");return 0;
}catch(const std::exception&){std::cerr<<"EQ_BRIDGE_SETUP_OR_PROTOCOL_FAILURE\n";return 84;}}
