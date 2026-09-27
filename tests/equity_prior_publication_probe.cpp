// Actual prior proof + actual native publisher. The new MODEL payload is synthetic.
// This probe certifies atomic protocol behavior, never a strategy/full-run invocation.
#include <cstdlib>
#include <iostream>
#include <set>
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
}
int main(int argc,char** argv) {try {
    if(argc!=7)return 64;
    const std::string mode=argv[1],book=argv[4],source_day=argv[5],day=argv[6];
    const std::set<std::string> modes{"snapshot","publish","missing_capture","bad_capture", "duplicate_capture",
        "wrong_owner","missing_replay","replay_mismatch","trace_mismatch","wrong_token","duplicate_trace",
        "changed_prior","incomplete","legacy_evidence","callback_failure","uow_historical","uow_wrong_owner",
        "uow_wrong_book","uow_current","uow_future","uow_failed_pending","uow_verified_prior",
        "eod_equity","eod_pnl","eod_realized","eod_cost","eod_capital"};
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
    auto begun=database.begin_live_publication(strategy,book,date,snapshot.value(),true,"synthetic-mr-bridge-test",
        mode=="legacy_evidence"?PublicationEvidenceRequirement::LegacyNotCollected:
            PublicationEvidenceRequirement::RequiredFinalObservations,&token,
        uow_mode && mode!="uow_verified_prior"?PublicationPriorRequirement::None:PublicationPriorRequirement::VerifiedEquity);
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
    if(mode!="missing_capture") {
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
    std::vector<Position> fresh{{"SYN",Quantity(12),Price(100),Decimal(0),Decimal(0),date}};
    if(database.clear_equity_model_current_positions(book,date).is_error())return 90;
    if(database.store_positions(fresh,strategy,name,book,"trading.positions").is_error())return 74;
    if(mode!="incomplete" && database.store_risk_limits(strategy,book,{{"max_gross_leverage",2.}}).is_error())return 75;
    if(database.store_live_results_complete(strategy,date,{{mode=="callback_failure"?"does_not_exist":"total_pnl",123}}, {},{},book).is_error())return 76;
    const nlohmann::json raw_capture={{"capture_schema_version",2},{"captured_at",day+"T12:00:00.000000Z"}};
    if(database.store_live_run_metadata(date,strategy,book,{{name,1.}},
        {{"total_capital",100000.},{"config_inspection",raw_capture}},{}).is_error())return 77;
    if(database.store_trading_equity_curve(strategy,date,100123,book).is_error() ||
        database.seed_qt_positions_from_system(strategy,name,book,day).is_error())return 78;
    nlohmann::json flags=nlohmann::json::object();
    if(captured)flags["equity_model_prior"]=captured->replay_reference;
    if(mode=="missing_replay")flags.erase("equity_model_prior");
    if(mode=="replay_mismatch")flags["equity_model_prior"]["results_digest"]=std::string(64,'f');
    auto stored=database.store_live_run_inputs(strategy,book,date,{{"trade_ngin_sha","synthetic-mr-bridge-test"},
        {"config_snapshot",snapshot.value()},{"universe",{"SYN"}},{"data_window",{}},{"engine_flags",flags}});
    const bool bad_replay=mode=="missing_replay" || mode=="replay_mismatch" || mode=="missing_capture";
    if(stored.is_error()!=bad_replay)return 79;
    if(captured && mode!="duplicate_capture") {
        auto projection=prior_only_projection(*captured,owner,mode);
        const auto attached=database.attach_equity_run_consumption(mode=="wrong_token"?PublicationEvidenceToken{}:token,projection);
        const bool bad_attach=bad_replay || mode=="trace_mismatch" || mode=="wrong_token" || mode.starts_with("eod_");
        if(attached.is_error()!=bad_attach)return 81;
        if(mode=="duplicate_trace" && database.attach_equity_run_consumption(token,projection).is_ok())return 82;
    }
    if(mode!="incomplete" && !bad_replay)std::cout<<"EQ_ALL_PARTS_QUEUED=1\n";
    auto published=database.publish_live_publication();
    if(published.is_ok()!=(mode=="publish"))return 83;
    std::cout<<(published.is_ok()?"EQ_PUBLICATION_COMMITTED=1\n":"EQ_PUBLICATION_REFUSED=1\n");return 0;
}catch(const std::exception&){std::cerr<<"EQ_BRIDGE_SETUP_OR_PROTOCOL_FAILURE\n";return 84;}}
