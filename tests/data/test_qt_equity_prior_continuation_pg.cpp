// Lane N4: the continuation hookups against an owned, network-none PostgreSQL (QT_N4_TEST_DSN).
// The synthetic schema holds only the rows the :122 hookup and the wrapper read, as exact to_jsonb images of
// the shared vectors. trading.desk_run_results is deliberately absent, so a call that gets past the market
// conjunct of validate_qt_equity_finalized_anchor aborts the transaction in original(); a call refused at the
// conjunct leaves it usable. That is how each test proves where a differing market was stopped.
// No email path, no network, and no production configuration are touched.
#include <gtest/gtest.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <pqxx/pqxx>
#include "trade_ngin/apps/qt_equity_prior_continuation.hpp"
#include "trade_ngin/data/qt_equity_desk_finalization.hpp"
using namespace trade_ngin;
namespace {
using J=nlohmann::json;
std::filesystem::path tests_dir(){return std::filesystem::path(__FILE__).parent_path().parent_path();}
std::string slurp(const std::filesystem::path& p){std::ifstream f(p,std::ios::binary);if(!f.good())throw std::runtime_error("missing "+p.string());return std::string((std::istreambuf_iterator<char>(f)),std::istreambuf_iterator<char>());}
J inputs(const std::string& name){
 static const J doc=J::parse(slurp(tests_dir()/"contracts/qt-equity-prior-continuation-vectors-v1.json"));
 for(const auto& c:doc.at("cases"))if(c.at("name")==name){if(c.at("expected")!="accepted")throw std::runtime_error("not a positive vector");return c.at("inputs");}
 throw std::runtime_error("missing case "+name);
}
const char* kSchema=R"sql(
DROP SCHEMA IF EXISTS trading CASCADE; CREATE SCHEMA trading;
CREATE TABLE trading.qt_decisions(decision_id uuid PRIMARY KEY,book_id text NOT NULL,source_day date NOT NULL,model_publication_id uuid,preview_id uuid,status text);
CREATE TABLE trading.qt_desk_market_sources(source_id uuid PRIMARY KEY,book_id text,source_day date,model_publication_id uuid,producer_id text,policy_version text,policy_revision bigint,source_version text,as_of text,valid_until text,created_at text,content_digest text,payload jsonb);
CREATE TABLE trading.qt_desk_finalizations(finalization_id uuid PRIMARY KEY,decision_id uuid,market_source_id uuid,book_id text,source_day date,valuation_day date,producer_id text,policy_version text,source_version text,policy_revision bigint,input_digest text,output_digest text,content_digest text,created_at text,payload jsonb);
CREATE TABLE trading.qt_equity_desk_evidence_sources(source_id text PRIMARY KEY,purpose text,book_id text,source_day date,producer_id text,policy_version text,policy_revision bigint,source_version text,content_digest text,created_at text,payload jsonb);
CREATE TABLE trading.qt_equity_model_prior_bindings(publication_id uuid PRIMARY KEY,book_id text,source_day date,strategy_id text,decision_id uuid,finalization_id uuid,finalization_digest text,finalization_source_id text,finalization_source_digest text,actions_source_id text,actions_source_digest text,replay_reference jsonb,replay_reference_digest text,created_at text);
)sql";
// Review N4-1 finding 1: this suite drops and recreates the trading schema, so it only ever touches the owned,
// route-less database that run-postgres.py creates. Same rule as tests/integration/qt_equity_finalization_probe.cpp.
bool owned_dsn(const std::string& dsn){
 return dsn.rfind("host=/tmp/algolens-repair-pg-",0)==0&&dsn.find(" dbname=algolens_test_")!=std::string::npos&&
        dsn.find("hostaddr")==std::string::npos&&dsn.find("service=")==std::string::npos;
}
std::string dsn(){const char* v=std::getenv("QT_N4_TEST_DSN");if(v==nullptr||*v=='\0')throw std::runtime_error("QT_N4_TEST_DSN is required; this suite never runs without its owned database");
 if(!owned_dsn(v)){throw std::runtime_error("QT_N4_TEST_DSN is not an owned algolens-repair test database; refusing to touch it");}
 return v;}
// Second line of defence, inside the connected database and before any DDL: the database is an owned
// algolens_test_* database and its trading schema is absent or holds only this suite's synthetic tables.
void require_owned_database(pqxx::connection& c){
 pqxx::work tx(c);
 auto name=tx.exec("SELECT current_database()");if(name.size()!=1||name[0][0].as<std::string>().rfind("algolens_test_",0)!=0)throw std::runtime_error("not an owned algolens_test_ database");
 auto foreign=tx.exec("SELECT count(*) FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace WHERE n.nspname='trading' AND c.relkind IN ('r','p','v','m','f','S') AND c.relname NOT IN ('qt_decisions','qt_desk_market_sources','qt_desk_finalizations','qt_equity_desk_evidence_sources','qt_equity_model_prior_bindings')");
 if(foreign[0][0].as<long long>()!=0)throw std::runtime_error("trading schema holds relations this suite did not create; refusing to drop it");
 tx.commit();
}
void insert(pqxx::work& tx,const std::string& table,const J& row){tx.exec("INSERT INTO trading."+table+" SELECT * FROM jsonb_populate_record(NULL::trading."+table+","+tx.quote(row.dump())+"::jsonb)");}
J load(pqxx::work& tx,const std::string& sql){auto r=tx.exec(sql);if(r.size()!=1||r[0][0].is_null())throw std::runtime_error("expected one row");return J::parse(r[0][0].c_str());}
bool usable(pqxx::work& tx){try{tx.exec("SELECT 1");return true;}catch(const std::exception&){return false;}}

class QtEquityPriorContinuationPgTest:public ::testing::Test{
protected:
 std::unique_ptr<pqxx::connection> db;J in;
 void seed(const std::string& name){
  in=inputs(name);db=std::make_unique<pqxx::connection>(dsn());require_owned_database(*db);pqxx::work tx(*db);tx.exec(kSchema);
  insert(tx,"qt_decisions",in.at("decision"));insert(tx,"qt_decisions",in.at("prior_decision"));
  insert(tx,"qt_desk_market_sources",in.at("finalization_market"));insert(tx,"qt_desk_market_sources",in.at("input_market"));
  insert(tx,"qt_desk_finalizations",in.at("finalization"));insert(tx,"qt_equity_desk_evidence_sources",in.at("actions_row"));
  insert(tx,"qt_equity_model_prior_bindings",in.at("binding"));tx.commit();
 }
 void change(const std::string& sql){pqxx::work tx(*db);tx.exec(sql);tx.commit();}
 std::string id(const char* row,const char* field){return in.at(row).at(field).get<std::string>();}
 // :122 exactly as reconstructed() calls it: the D decision, the input market row A, and the anchor row.
 struct Outcome{bool ok;bool usable;};
 Outcome hook122(const J& market){pqxx::work tx(*db);auto r=validate_qt_equity_finalized_anchor(tx,in.at("decision"),market,in.at("anchor"));return {r.is_ok(),usable(tx)};}
 // The :90 continuation operand path, verbatim from the hunk: load A by the input payload's market_source_id,
 // require its digest, then the wrapper with F (nodes[index+1].finalization) and the anchor (r.at("prior")).
 bool hook90(const J& payload){
  pqxx::work tx(*db);
  try{auto a=load(tx,"SELECT to_jsonb(m) FROM trading.qt_desk_market_sources m WHERE source_id="+tx.quote(payload.at("market_source_id").get<std::string>())+"::uuid FOR SHARE");
   return a.at("content_digest")==payload.at("market_source_digest")&&verify_qt_equity_prior_continuation(tx,in.at("decision"),a,in.at("finalization"),in.at("anchor")).is_ok();}
  catch(const std::exception&){return false;}
 }
 J input_payload(){return {{"market_source_id",id("input_market","source_id")},{"market_source_digest",id("input_market","content_digest")}};}
};
}

TEST_F(QtEquityPriorContinuationPgTest, SyntheticTablesReproduceTheVectorRowImages){
 seed("accepted_split");pqxx::work tx(*db);
 EXPECT_EQ(load(tx,"SELECT to_jsonb(m) FROM trading.qt_desk_market_sources m WHERE source_id="+tx.quote(id("input_market","source_id"))+"::uuid"),in.at("input_market"));
 EXPECT_EQ(load(tx,"SELECT to_jsonb(m) FROM trading.qt_desk_market_sources m WHERE source_id="+tx.quote(id("finalization_market","source_id"))+"::uuid"),in.at("finalization_market"));
 EXPECT_EQ(load(tx,"SELECT to_jsonb(f) FROM trading.qt_desk_finalizations f"),in.at("finalization"));
 EXPECT_EQ(load(tx,"SELECT to_jsonb(e) FROM trading.qt_equity_desk_evidence_sources e"),in.at("actions_row"));
 EXPECT_EQ(load(tx,"SELECT to_jsonb(b) FROM trading.qt_equity_model_prior_bindings b"),in.at("binding"));
 EXPECT_EQ(load(tx,"SELECT to_jsonb(d) FROM trading.qt_decisions d WHERE decision_id="+tx.quote(id("prior_decision","decision_id"))+"::uuid"),in.at("prior_decision"));
}

TEST_F(QtEquityPriorContinuationPgTest, WrapperProvesTheBoundContinuation){
 for(auto name:{"accepted_split","accepted_no_action_v1","accepted_split_then_dividend","accepted_new_symbol","accepted_created_at_other_offset"}){
  SCOPED_TRACE(name);seed(name);pqxx::work tx(*db);
  EXPECT_TRUE(verify_qt_equity_prior_continuation(tx,in.at("decision"),in.at("input_market"),in.at("finalization"),in.at("anchor")).is_ok());EXPECT_TRUE(usable(tx));
 }
}

TEST_F(QtEquityPriorContinuationPgTest, Hook122PassesTheMarketConjunctOnlyWithTheBinding){
 for(auto name:{"accepted_split","accepted_no_action_v1"}){
  SCOPED_TRACE(name);seed(name);
  const auto bound=hook122(in.at("input_market"));EXPECT_FALSE(bound.ok);EXPECT_FALSE(bound.usable)<<"a proven continuation must get past :122's market conjunct";
  change("DELETE FROM trading.qt_equity_model_prior_bindings");
  const auto unbound=hook122(in.at("input_market"));EXPECT_FALSE(unbound.ok);EXPECT_TRUE(unbound.usable)<<"a differing market without a binding must stop at :122";
 }
}

TEST_F(QtEquityPriorContinuationPgTest, Hook122RefusesADifferingMarketWithoutABindingRow){
 seed("accepted_split");change("DELETE FROM trading.qt_equity_model_prior_bindings");
 const auto r=hook122(in.at("input_market"));EXPECT_FALSE(r.ok);EXPECT_TRUE(r.usable);
}

TEST_F(QtEquityPriorContinuationPgTest, Hook122RefusesABindingRowForAnotherFinalization){
 seed("accepted_split");
 change("UPDATE trading.qt_equity_model_prior_bindings SET finalization_id='a4000000-0000-4000-8000-0000000000ee',replay_reference=jsonb_set(replay_reference,'{finalization_id}','\"a4000000-0000-4000-8000-0000000000ee\"')");
 const auto r=hook122(in.at("input_market"));EXPECT_FALSE(r.ok);EXPECT_TRUE(r.usable);
}

TEST_F(QtEquityPriorContinuationPgTest, Hook122RefusesABindingRowForAnotherPublication){
 seed("accepted_split");change("UPDATE trading.qt_equity_model_prior_bindings SET publication_id='a4000000-0000-4000-8000-0000000000ef'");
 const auto r=hook122(in.at("input_market"));EXPECT_FALSE(r.ok);EXPECT_TRUE(r.usable);
}

TEST_F(QtEquityPriorContinuationPgTest, Hook122WithoutMigration024FailsClosedAndKeepsTheTransaction){
 seed("accepted_split");change("DROP TABLE trading.qt_equity_model_prior_bindings");
 const auto r=hook122(in.at("input_market"));EXPECT_FALSE(r.ok);EXPECT_TRUE(r.usable);
}

TEST_F(QtEquityPriorContinuationPgTest, Hook122SameMarketKeepsTheOldPathWithoutConsultingTheBinding){
 seed("accepted_split");change("DROP TABLE trading.qt_equity_model_prior_bindings");
 const auto r=hook122(in.at("finalization_market"));EXPECT_FALSE(r.ok);EXPECT_FALSE(r.usable)<<"F's own market passes the conjunct as before";
}

TEST_F(QtEquityPriorContinuationPgTest, Hook90OperandPathRequiresTheBindingForThisFinalization){
 seed("accepted_split");EXPECT_TRUE(hook90(input_payload()));
 auto wrong=input_payload();wrong["market_source_digest"]=std::string(64,'0');EXPECT_FALSE(hook90(wrong));
 change("UPDATE trading.qt_equity_model_prior_bindings SET finalization_id='a4000000-0000-4000-8000-0000000000ee'");EXPECT_FALSE(hook90(input_payload()));
 change("DELETE FROM trading.qt_equity_model_prior_bindings");EXPECT_FALSE(hook90(input_payload()));
 change("DROP TABLE trading.qt_equity_model_prior_bindings");EXPECT_FALSE(hook90(input_payload()));
}

// The candidate query is a copy of the MODEL's (equity_model_action_source.cpp:44): M's own empty D actions
// row, other empty rows and rows outside A's governed role are never candidates; any other non-empty D row is.
TEST_F(QtEquityPriorContinuationPgTest, CandidateQueryIsTheModelsQuery){
 const auto verified=[&]{pqxx::work tx(*db);return verify_qt_equity_prior_continuation(tx,in.at("decision"),in.at("input_market"),in.at("finalization"),in.at("anchor")).is_ok();};
 const auto add=[&](const std::string& id,const J& events,const std::string& previous_day,const std::string& producer,std::int64_t revision){
  J row=in.at("actions_row");row["source_id"]=id;row["source_version"]=id;row["payload"]["events"]=events;row["payload"]["previous_day"]=previous_day;
  row["producer_id"]=producer;row["policy_revision"]=revision;pqxx::work tx(*db);insert(tx,"qt_equity_desk_evidence_sources",row);tx.commit();};
 for(auto name:{"accepted_no_action_v1","accepted_split"}){
  SCOPED_TRACE(name);seed(name);const bool acted=!in.at("actions_row").at("payload").at("events").empty();
  const auto& a=in.at("input_market");const auto previous=a.at("payload").at("previous_day").get<std::string>();
  const auto producer=a.at("producer_id").get<std::string>();const auto revision=a.at("policy_revision").get<std::int64_t>();
  J late=J::array({acted?in.at("actions_row").at("payload").at("events")[0]:J::parse(R"({"key":{"symbol":"SYN"}})")});
  if(!acted){J m;pqxx::work tx(*db);m=load(tx,"SELECT to_jsonb(e) FROM trading.qt_equity_desk_evidence_sources e");tx.commit();
   EXPECT_TRUE(m.at("payload").at("events").empty())<<"M's own empty D actions row is present";}
  EXPECT_TRUE(verified());
  add("qt-actions/n4-empty-same-role",J::array(),previous,producer,revision);EXPECT_TRUE(verified())<<"an empty row is never a candidate";
  add("qt-actions/n4-other-previous-day",late,"2026-09-23",producer,revision);EXPECT_TRUE(verified())<<"another previous_day is outside the query";
  add("qt-actions/n4-other-producer",late,previous,"other-execution",revision);EXPECT_TRUE(verified())<<"another producer is outside the query";
  add("qt-actions/n4-other-revision",late,previous,producer,revision+1);EXPECT_TRUE(verified())<<"another revision is outside the query";
  add("qt-actions/n4-late-same-role",late,previous,producer,revision);EXPECT_FALSE(verified())<<"a second (or late) non-empty D row fails closed";
 }
}

// Static audit of the hooked file compiled into this binary's library: the wrapper is the only way a market
// other than F's is admitted, at exactly the two contract sites.
TEST(QtEquityPriorContinuationHookupAudit, OnlyTheWrapperAdmitsADifferingMarket){
 const auto text=slurp(tests_dir().parent_path()/"src/data/qt_equity_desk_finalization.cpp");
 auto count=[&](const std::string& needle){std::size_t n=0;for(auto at=text.find(needle);at!=std::string::npos;at=text.find(needle,at+1))++n;return n;};
 EXPECT_EQ(count("verify_qt_equity_prior_continuation("),2u);
 EXPECT_EQ(count("need((f.at(\"market_source_id\")==m.at(\"source_id\")||verify_qt_equity_prior_continuation(tx,d,m,f,final).is_ok())&&f.at(\"book_id\")==d.at(\"book_id\")"),1u);
 EXPECT_EQ(count("need((preceding.at(\"market_source_id\")==in.at(\"market_source_id\")&&preceding.at(\"market_source_digest\")==in.at(\"market_source_digest\"))||[&]{auto a=one(tx,\"SELECT to_jsonb(m) FROM trading.qt_desk_market_sources m WHERE source_id=\"+tx.quote(text(in.at(\"market_source_id\")))+\"::uuid FOR SHARE\");return a.at(\"content_digest\")==in.at(\"market_source_digest\")&&verify_qt_equity_prior_continuation(tx,d,a,nodes[index+1].finalization,r.at(\"prior\")).is_ok();}());"),1u);
 EXPECT_EQ(count("f.at(\"market_source_id\")==m.at(\"source_id\")"),1u);
 EXPECT_EQ(count("preceding.at(\"market_source_id\")==in.at(\"market_source_id\")"),1u);
}

// Review N4-1 finding 1: the DSN guard refuses every database this suite does not own, before connecting.
TEST(QtEquityPriorContinuationPgGuard, RefusesANonOwnedDsnBeforeConnecting){
 const std::string owned="host=/tmp/algolens-repair-pg-abc123/socket port=5432 dbname=algolens_test_0123456789ab user=postgres password=synthetic-test-only connect_timeout=2";
 EXPECT_TRUE(owned_dsn(owned));
 for(const std::string& bad:{std::string("host=localhost port=5432 dbname=algolens_test_x user=postgres"),
     std::string("host=127.0.0.1 dbname=new_algo_data user=algolens"),std::string("postgresql://algolens@localhost/algolens_demo"),
     std::string("host=/tmp/algolens-repair-pg-abc/socket dbname=algolens user=postgres"),
     std::string("host=/tmp/algolens-repair-pg-abc/socket dbname=postgres user=postgres"),
     std::string("host=/var/run/postgresql dbname=algolens_test_x user=postgres"),
     std::string("host=/tmp/algolens-repair-pg-abc/socket hostaddr=10.0.0.5 dbname=algolens_test_x"),
     std::string("host=/tmp/algolens-repair-pg-abc/socket dbname=algolens_test_x service=prod"),
     std::string(" host=/tmp/algolens-repair-pg-abc/socket dbname=algolens_test_x"),std::string("")}){
  SCOPED_TRACE(bad);EXPECT_FALSE(owned_dsn(bad));
 }
 const char* current=std::getenv("QT_N4_TEST_DSN");const std::string saved=current?current:"";
 if(current){EXPECT_TRUE(owned_dsn(saved))<<"the runner must hand this suite its owned database";}
 ASSERT_EQ(setenv("QT_N4_TEST_DSN","host=127.0.0.1 dbname=new_algo_data user=algolens",1),0);
 EXPECT_THROW(dsn(),std::runtime_error);
 ASSERT_EQ(unsetenv("QT_N4_TEST_DSN"),0);EXPECT_THROW(dsn(),std::runtime_error);
 if(current){ASSERT_EQ(setenv("QT_N4_TEST_DSN",saved.c_str(),1),0);}
}
