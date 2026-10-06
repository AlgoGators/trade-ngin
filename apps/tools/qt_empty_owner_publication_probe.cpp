// Owned PostgreSQL transaction probe. Native synthetic observations only;
// never a MODEL/strategy runner or a financial correctness certification.
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/apps/live_portfolio_helpers.hpp"
#include "trade_ngin/apps/equity_run_consumption.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/portfolio/qt_wire.hpp"
#include <cctype>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <set>
#include <new>
#include <dlfcn.h>
namespace { thread_local bool fail_next_staging_allocation=false; }
// Test binary only. Ordinary allocation remains unchanged when unarmed.
void* operator new(std::size_t size){
 if(fail_next_staging_allocation){fail_next_staging_allocation=false;throw std::bad_alloc();}
 if(auto p=std::malloc(size?size:1))return p;throw std::bad_alloc();
}
void* operator new[](std::size_t size){return ::operator new(size);}
void operator delete(void* p)noexcept{std::free(p);}
void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}
void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
using namespace trade_ngin;
namespace {
constexpr auto engine="LIVE_EQUITY_MEAN_REVERSION";
constexpr auto member="EQUITY_MEAN_REVERSION";
// Test-only read admission for the exact reviewed owned SQL clock. It never
// installs/replaces a provider or changes the OS/monotonic/builtin clocks.
bool historical_provider(pqxx::work& tx) {
 const auto relation=tx.exec(R"SQL(
 SELECT c.relkind='r' AND c.relpersistence='p' AND NOT c.relrowsecurity
 AND NOT c.relforcerowsecurity AND NOT c.relispartition AND NOT c.relhasrules
 AND pg_catalog.pg_get_userbyid(c.relowner)='postgres'
 AND (SELECT array_agg(a.attname::text||':'||pg_catalog.format_type(a.atttypid,a.atttypmod)||':'||a.attnotnull::text ORDER BY a.attnum)
      FROM pg_catalog.pg_attribute a WHERE a.attrelid=c.oid AND a.attnum>0 AND NOT a.attisdropped)
   =ARRAY['singleton:boolean:true','fixture_timestamp:timestamp with time zone:true']
 AND (SELECT count(*) FROM pg_catalog.pg_constraint x WHERE x.conrelid=c.oid)=3
 AND EXISTS(SELECT 1 FROM pg_catalog.pg_constraint x WHERE x.conrelid=c.oid AND x.contype='p' AND x.conkey=ARRAY[1]::smallint[])
 AND EXISTS(SELECT 1 FROM pg_catalog.pg_constraint x WHERE x.conrelid=c.oid AND x.contype='c' AND x.convalidated AND pg_catalog.pg_get_expr(x.conbin,x.conrelid)='singleton')
 AND EXISTS(SELECT 1 FROM pg_catalog.pg_constraint x WHERE x.conrelid=c.oid AND x.contype='c' AND x.convalidated
  AND pg_catalog.pg_get_expr(x.conbin,x.conrelid)='(fixture_timestamp = ANY (ARRAY[''2026-09-24 12:00:00+00''::timestamp with time zone, ''2026-09-25 12:00:00+00''::timestamp with time zone]))')
 FROM pg_catalog.pg_class c JOIN pg_catalog.pg_namespace n ON n.oid=c.relnamespace
 WHERE n.nspname='trading' AND c.relname='qt_owned_equity_eod_clock'
 )SQL");
 if(relation.size()!=1||relation[0][0].is_null()||!relation[0][0].as<bool>())return false;
 const auto functions=tx.exec(R"SQL(
 SELECT count(*)=2 AND bool_and(l.lanname='sql' AND p.pronargs=0
  AND p.prorettype='pg_catalog.timestamptz'::pg_catalog.regtype AND p.provolatile='v'
  AND p.proparallel='u' AND NOT p.prosecdef AND p.proconfig IS NULL
  AND pg_catalog.pg_get_userbyid(p.proowner)='postgres'
  AND p.prosrc='SELECT fixture_timestamp FROM trading.qt_owned_equity_eod_clock WHERE singleton=true')
 FROM pg_catalog.pg_proc p JOIN pg_catalog.pg_namespace n ON n.oid=p.pronamespace
 JOIN pg_catalog.pg_language l ON l.oid=p.prolang
 WHERE n.nspname='trading' AND p.proname IN('clock_timestamp','now')
 )SQL");
 if(functions.size()!=1||functions[0][0].is_null()||!functions[0][0].as<bool>())return false;
 const auto builtins=tx.exec(R"SQL(
 SELECT count(*)=2 AND bool_and(l.lanname='internal' AND p.prosrc=p.proname
  AND p.pronargs=0 AND p.prorettype='pg_catalog.timestamptz'::pg_catalog.regtype)
 FROM pg_catalog.pg_proc p JOIN pg_catalog.pg_namespace n ON n.oid=p.pronamespace
 JOIN pg_catalog.pg_language l ON l.oid=p.prolang
 WHERE n.nspname='pg_catalog' AND p.proname IN('clock_timestamp','now') AND p.pronargs=0
 )SQL");
 if(builtins.size()!=1||builtins[0][0].is_null()||!builtins[0][0].as<bool>())return false;
 const auto resolution=tx.exec(R"SQL(
 SELECT 'clock_timestamp()'::pg_catalog.regprocedure::oid='trading.clock_timestamp()'::pg_catalog.regprocedure::oid
 AND 'now()'::pg_catalog.regprocedure::oid='trading.now()'::pg_catalog.regprocedure::oid
 AND replace(current_setting('search_path'),' ','')='trading,pg_catalog,public'
 AND inet_server_addr() IS NULL AND current_user='postgres'
 )SQL");
 return resolution.size()==1&&!resolution[0][0].is_null()&&resolution[0][0].as<bool>();
}

EquityRunProjection observed(const std::string& day,bool unavailable,const std::string& book,double capital){
 EquityRunConsumption r;r.portfolio_id=book;r.strategy_id=engine;r.strategy_name=member;r.date=day;
 r.setup.outcome=r.market_input.outcome=r.cost_history.outcome=r.prior.outcome=r.corporate_actions.outcome=
 r.preparation.outcome=r.primary.outcome=r.execution.outcome=r.eod.outcome=r.result_assembly.outcome=EquityStageOutcome::ReturnedOk;
 r.setup.reads={capital,2.,0.1,0.,false,false,true};
 r.market_input.reads={252,"EQUITY","DAILY","2025-09-26",day,2};
 r.prior.reads.mode="system_reference";Timestamp current;if(!core::parse_utc_date(day,current))throw std::invalid_argument("invalid_explicit_day");r.prior.reads.source_day=core::format_utc_date(current-std::chrono::hours(24));
 r.corporate_actions.reads={"system_history","hold","hold",0};
 StrategyConsumptionTrace strategy;strategy.profile=StrategyConsumptionProfile::MeanReversion;r.primary.reads.strategy_invocation=strategy;
 PortfolioConsumptionTrace portfolio;portfolio.outcome=PortfolioCallOutcome::ReturnedOk;portfolio.skip_execution_generation=false;
 portfolio.pass_count=1;portfolio.passes[0].use_optimization=false;portfolio.passes[0].use_risk_management=false;r.primary.reads.portfolio_invocation=portfolio;
 r.eod.reads={"system_finalization",Decimal(capital),Decimal(0),Decimal(0),Decimal(0),Decimal(capital)};
 r.result_assembly.reads={"USD",Decimal(capital),Decimal(0),Decimal(0),Decimal(0)};
 if(unavailable)r.market_input={};
 return project_equity_run_consumption(r);
}
}
int main(int argc,char** argv){try{
 auto loaded=reinterpret_cast<int(*)()>(dlsym(RTLD_DEFAULT,"qt_no_delivery_guard_loaded"));if(!loaded||loaded()!=1)return 63;
 if(argc!=3&&argc!=6)return 64;
 const std::string supplied_mode=argv[1],day=argv[2];
 const bool scoped=supplied_mode=="snapshot_scoped"||supplied_mode=="controlled_scoped";
 if((scoped&&argc!=6)||(!scoped&&argc!=3))return 64;
 const std::string mode=scoped?(supplied_mode=="snapshot_scoped"?"snapshot":"controlled"):supplied_mode;
 const std::string book=scoped?argv[3]:"EQ_BOOK";
 const std::string clock_mode=scoped?argv[5]:"current_utc";
 double capital=100000.;
 if(scoped){
  if(book.empty()||book.size()>256||std::all_of(book.begin(),book.end(),[](unsigned char c){return std::isspace(c);})||
     std::any_of(book.begin(),book.end(),[](unsigned char c){return std::iscntrl(c);})||
     (clock_mode!="current_utc"&&clock_mode!="historical_sql"))return 64;
  const auto parsed=parse_qt_quantity_exact(argv[4]);if(parsed.is_error()||!parsed.value().is_positive())return 64;
  capital=parsed.value().to_double();if(!std::isfinite(capital)||capital<=0.)return 64;
  try{if(Decimal(capital)!=parsed.value())return 64;}catch(const std::exception&){return 64;}
 }
 const std::set<std::string> modes{"snapshot","snapshot_second","controlled","uncontrolled","duplicate_batch","missing_batch","callback_failure","unavailable_capture","staging_allocation_failure","unobserved_second_owner"};
 if(!modes.contains(mode))return 64;
 AppConfig config;config.portfolio_id=book;config.initial_capital=capital;config.strategies_config={{member,{{"enabled_live",true},{"default_allocation",1.0},{"type","MeanReversionStrategy"}}}};
 if(mode=="unobserved_second_owner"||mode=="snapshot_second")config.strategies_config["UNOBSERVED_MEMBER"]=config.strategies_config.at(member);
 auto snapshot=build_runtime_trading_snapshot(config);if(snapshot.is_error())return 65;
 if(!scoped&&(mode=="snapshot"||mode=="snapshot_second")){std::cout<<"EMPTY_SNAPSHOT="<<snapshot.value().dump()<<'\n';return 0;}
 const char* raw=std::getenv("ALGOLENS_TEST_DB");std::string dsn=raw?raw:"";
 if(dsn.rfind("host=/tmp/algolens-repair-pg-",0)!=0 || dsn.find(" dbname=algolens_test_")==std::string::npos || dsn.find("hostaddr")!=std::string::npos || dsn.find("service=")!=std::string::npos)return 66;
 Timestamp date;if(!core::parse_utc_date(day,date))return 67;
 std::string captured_at;
 {pqxx::connection c(dsn);pqxx::work tx(c);tx.exec("SET LOCAL TIME ZONE 'UTC'");
  if(clock_mode=="historical_sql"&&!historical_provider(tx))return 67;
  const auto clock=tx.exec(clock_mode=="historical_sql"?
   "SELECT (clock_timestamp() AT TIME ZONE 'UTC')::date::text,to_char(clock_timestamp() AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"'),clock_timestamp()=now(),(SELECT count(*) FROM trading.qt_owned_equity_eod_clock WHERE singleton=true)=1":
   "SELECT (pg_catalog.clock_timestamp() AT TIME ZONE 'UTC')::date::text,to_char(pg_catalog.clock_timestamp() AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"'),true,true");
  if(clock.size()!=1||clock[0][0].as<std::string>()!=day||!clock[0][2].as<bool>()||!clock[0][3].as<bool>())return 67;
  captured_at=clock[0][1].as<std::string>();}
 if(scoped&&mode=="snapshot"){std::cout<<"EMPTY_SNAPSHOT="<<snapshot.value().dump()<<'\n';return 0;}

 PostgresDatabase db(dsn);if(db.connect().is_error())return 68;
 PublicationEvidenceToken token;
 auto begin=db.begin_live_publication(engine,book,date,snapshot.value(),mode!="uncontrolled","synthetic-empty-probe",
     PublicationEvidenceRequirement::RequiredFinalObservations,&token);
 if(begin.is_error()||begin.value()||!token.valid())return 69;
 auto refuse=[&]{db.abandon_live_publication("synthetic_empty_refusal");std::cout<<"EMPTY_PUBLICATION_REFUSED=1\n";return 0;};
 if(mode!="missing_batch"){
   QtModelPositionBatch completed{book,engine,member,day,{}};
   if(mode=="staging_allocation_failure")fail_next_staging_allocation=true;
   if(db.store_model_position_batch(completed).is_error())return refuse();
   if(mode=="staging_allocation_failure")return 78;
   if(mode=="duplicate_batch"){
     if(db.store_model_position_batch(completed).is_ok())return 70;
     return refuse();
   }
 }
 if(mode=="unobserved_second_owner"){QtModelPositionBatch second{book,engine,"UNOBSERVED_MEMBER",day,{}};if(db.store_model_position_batch(second).is_error())return refuse();}
 if(db.store_risk_limits(engine,book,{{"max_gross_leverage",2.}}).is_error())return 71;
 // Explicit synthetic governed capital and actual stored zero-flow totals.
 std::unordered_map<std::string,double> totals{{"daily_pnl",0.},{"daily_realized_pnl",0.},
   {"daily_unrealized_pnl",0.},{"daily_transaction_costs",0.},{"total_pnl",0.},
   {"total_realized_pnl",0.},{"total_unrealized_pnl",0.},{"total_transaction_costs",0.},
   {"current_portfolio_value",capital}};
 if(mode=="callback_failure")totals["does_not_exist"]=0.;
 if(db.store_live_results_complete(engine,date,totals, {},{},book).is_error())return 72;
 if(db.store_live_run_metadata(date,engine,book,{{member,1.}},{{"config_inspection",{{"capture_schema_version",2},{"captured_at",captured_at}}}},{}).is_error())return 73;
 if(db.store_trading_equity_curve(engine,date,capital,book).is_error())return 74;
 if(mode=="missing_batch")return refuse();
 if(db.seed_qt_proposal_positions_from_system(engine,member,book,day).is_error() || db.seed_qt_positions_from_system(engine,member,book,day).is_error())return 75;
 if(db.store_live_run_inputs(engine,book,date,{{"trade_ngin_sha","synthetic-empty-probe"},{"config_snapshot",snapshot.value()},{"universe",nlohmann::json::array()},{"data_window",{}},{"engine_flags",nlohmann::json::object()}}).is_error())return 76;
 if(db.attach_equity_run_consumption(token,observed(day,mode=="unavailable_capture",book,capital)).is_error())return refuse();
 const auto outcome=db.publish_live_publication();
 std::cout<<(outcome.is_ok()?"EMPTY_PUBLICATION_COMMITTED=1\n":"EMPTY_PUBLICATION_REFUSED=1\n");return 0;
}catch(const std::exception&){std::cerr<<"EMPTY_PROBE_SETUP_OR_PROTOCOL_FAILURE\n";return 77;}}
