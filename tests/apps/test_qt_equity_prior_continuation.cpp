#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string>
#include "trade_ngin/apps/qt_equity_prior_continuation.hpp"
#include "trade_ngin/core/qt_sha256.hpp"
#include "trade_ngin/data/qt_desk_current_facts.hpp"
using namespace trade_ngin;
namespace {
using J=nlohmann::json;
constexpr std::size_t kCases=114;
constexpr const char* kCode="qt_equity_prior_continuation_unavailable";
const J& vectors(){
 static const J doc=[]{
  std::ifstream f(std::filesystem::path(__FILE__).parent_path().parent_path()/"contracts/qt-equity-prior-continuation-vectors-v1.json",std::ios::binary);
  if(!f.good())throw std::runtime_error("missing qt-equity-prior-continuation-vectors-v1.json");
  const std::string bytes((std::istreambuf_iterator<char>(f)),std::istreambuf_iterator<char>());return J::parse(bytes);}();
 return doc;
}
J named(const std::string& name){for(const auto& c:vectors().at("cases"))if(c.at("name")==name)return c.at("inputs");throw std::runtime_error("missing case "+name);}
QtEquityPriorContinuationInputs from(const J& in){
 QtEquityPriorContinuationInputs out;out.decision=in.at("decision");out.prior_decision=in.at("prior_decision");out.finalization_market=in.at("finalization_market");
 out.input_market=in.at("input_market");out.finalization=in.at("finalization");out.anchor=in.at("anchor");out.binding=in.at("binding");out.actions_row=in.at("actions_row");
 for(const auto& id:in.at("candidate_action_sources"))out.candidate_action_sources.push_back(id.get<std::string>());return out;
}
std::string digest(const J& j){auto c=canonical_qt_desk_input_json(j);if(c.is_error())throw std::runtime_error("canonical");auto h=qt_sha256_hex(c.value());if(h.is_error())throw std::runtime_error("sha256");return h.value();}
J& instrument(J& in,const std::string& market,const std::string& symbol){for(auto& i:in[market]["payload"]["instruments"])if(i.at("symbol")==symbol)return i;throw std::runtime_error("missing instrument "+symbol);}
void reseal_a(J& in){in["input_market"]["content_digest"]=digest(in["input_market"]["payload"]);}
// The anchor's basis feeds the binding's replay reference (and frame) digests.
void reseal_anchor(J& in){
 auto& anchor=in["anchor"];anchor["content_digest"]=digest(anchor["payload"]);auto& b=in["binding"];auto& rr=b["replay_reference"];
 b["finalization_source_digest"]=anchor["content_digest"];rr["finalization_source_digest"]=anchor["content_digest"];rr["basis_positions"]=anchor["payload"]["previous_positions"];
 if(rr.contains("action_frame")){rr["action_frame"]["original_basis_positions"]=anchor["payload"]["previous_positions"];rr["action_frame_digest"]=digest(rr["action_frame"]);}
 b["replay_reference_digest"]=digest(rr);
}
bool accepted(const J& in){return validate_qt_equity_verified_prior_continuation(from(in)).is_ok();}
void set_price(J& in,const std::string& symbol,const std::string& value){for(auto f:{"reference","mark"})instrument(in,"input_market",symbol)[f]["price_model_number"]=value;reseal_a(in);}
}

TEST(QtEquityPriorContinuationTest, VectorFileDeclaresItsCaseCount){
 const auto& doc=vectors();ASSERT_EQ(doc.at("schema_version"),"qt-equity-prior-continuation-vectors/v1");
 ASSERT_EQ(doc.at("case_count").get<std::size_t>(),kCases);ASSERT_EQ(doc.at("cases").size(),kCases);EXPECT_EQ(doc.at("error_code"),kCode);
 std::set<std::string> names;std::size_t positive=0;
 for(const auto& c:doc.at("cases")){EXPECT_TRUE(names.insert(c.at("name").get<std::string>()).second);positive+=c.at("expected")=="accepted";EXPECT_TRUE(c.at("expected")=="accepted"||c.at("expected")=="refused");}
 EXPECT_EQ(positive,17u);
 for(auto name:{"accepted_no_action_v1","accepted_split","accepted_dividend","accepted_new_symbol","accepted_split_three","accepted_split_then_dividend","refused_ulp_dividend","accepted_created_at_other_offset","refused_created_at_other_instant"})EXPECT_TRUE(names.contains(name))<<name;
}

TEST(QtEquityPriorContinuationTest, EveryVectorMatchesItsExpectation){
 for(const auto& c:vectors().at("cases")){
  SCOPED_TRACE(c.at("name").get<std::string>());Result<void> result;
  ASSERT_NO_THROW(result=validate_qt_equity_verified_prior_continuation(from(c.at("inputs"))));
  if(c.at("expected")=="accepted")EXPECT_TRUE(result.is_ok())<<c.at("description").get<std::string>();
  else{ASSERT_TRUE(result.is_error())<<c.at("description").get<std::string>();EXPECT_STREQ(result.error()->what(),kCode);EXPECT_EQ(result.error()->code(),ErrorCode::INVALID_DATA);}
 }
}

TEST(QtEquityPriorContinuationTest, SplitRestatementIsTheModelArithmeticSpelledExactly){
 auto in=named("accepted_split");const auto& syn=instrument(in,"input_market","SYN");
 EXPECT_EQ(syn.at("mark").at("price_model_number"),"52");EXPECT_EQ(syn.at("mark").at("price_frame_id"),"adjusted-1/D-SPLIT");
 EXPECT_EQ(instrument(in,"input_market","AAA"),instrument(in,"finalization_market","AAA"));ASSERT_TRUE(accepted(in));
 auto spelled=in;set_price(spelled,"SYN","52.0");EXPECT_FALSE(accepted(spelled));
 auto off=in;set_price(off,"SYN","52.00000001");EXPECT_FALSE(accepted(off));
 auto back=in;set_price(back,"SYN","52");EXPECT_TRUE(accepted(back));
}

TEST(QtEquityPriorContinuationTest, StackedEventsFollowModelOrderWithDecimalScreening){
 auto in=named("accepted_split_then_dividend_order_sensitive");
 // 104.37 / 1.5 -> 69.58, screened to Decimal; then / (1 + 0.83/34.9).
 EXPECT_EQ(instrument(in,"input_market","SYN").at("mark").at("price_model_number"),"67.96367198");
 EXPECT_EQ(instrument(in,"input_market","AAA").at("mark").at("price_model_number"),"25.125");ASSERT_TRUE(accepted(in));
 auto dividend_first=in;set_price(dividend_first,"SYN","67.96367199");EXPECT_FALSE(accepted(dividend_first));
}

TEST(QtEquityPriorContinuationTest, DividendAndAdrSplitLiterals){
 auto dividend=named("accepted_dividend_awkward_close");EXPECT_EQ(instrument(dividend,"input_market","SYN").at("mark").at("price_model_number"),"102.68737864");EXPECT_TRUE(accepted(dividend));
 auto adr=named("accepted_adr_split");EXPECT_EQ(instrument(adr,"input_market","SYN").at("reference").at("price_model_number"),"208");EXPECT_TRUE(accepted(adr));
 auto two=named("accepted_two_symbols_split_and_dividend");
 EXPECT_EQ(instrument(two,"input_market","AAA").at("mark").at("price_model_number"),"6.25");
 EXPECT_EQ(instrument(two,"input_market","SYN").at("mark").at("price_model_number"),"101.96078431");EXPECT_TRUE(accepted(two));
}

TEST(QtEquityPriorContinuationTest, NewSymbolIsAnOpenOnly){
 auto in=named("accepted_no_action_v1");auto fresh=instrument(in,"input_market","AAA");fresh["symbol"]="OPEN";
 for(auto f:{"reference","mark"})fresh[f]["price_frame_id"]="fresh-OPEN";
 in["input_market"]["payload"]["instruments"].push_back(fresh);reseal_a(in);ASSERT_TRUE(accepted(in));
 auto held=in;auto row=held["anchor"]["payload"]["previous_positions"][0];row["key"]["symbol"]="OPEN";row["quantity_exact"]="0";
 held["anchor"]["payload"]["previous_positions"].push_back(row);reseal_anchor(held);EXPECT_FALSE(accepted(held));
 auto dated=in;instrument(dated,"input_market","OPEN")["mark"]["date"]=dated["decision"]["source_day"];reseal_a(dated);EXPECT_FALSE(accepted(dated));
}

TEST(QtEquityPriorContinuationTest, CandidatesMustBeExactlyTheAppliedRow){
 auto in=named("accepted_split");const auto applied=in.at("actions_row").at("source_id").get<std::string>();ASSERT_TRUE(accepted(in));
 auto none=in;none["candidate_action_sources"]=J::array();EXPECT_FALSE(accepted(none));
 auto late=in;late["candidate_action_sources"]=J::array({applied,"qt-actions/late"});EXPECT_FALSE(accepted(late));
 auto quiet=named("accepted_no_action_v1");quiet["candidate_action_sources"]=J::array({"qt-actions/late"});EXPECT_FALSE(accepted(quiet));
}

TEST(QtEquityPriorContinuationTest, SameMarketIsNeverAContinuation){
 auto in=named("accepted_no_action_v1");in["input_market"]=in["finalization_market"];EXPECT_FALSE(accepted(in));
 auto renamed=named("accepted_no_action_v1");renamed["input_market"]["source_id"]=renamed["finalization_market"]["source_id"];EXPECT_FALSE(accepted(renamed));
}

TEST(QtEquityPriorContinuationTest, MalformedInputsFailClosedWithoutThrowing){
 auto base=named("accepted_split");
 for(auto field:{"decision","prior_decision","finalization_market","input_market","finalization","anchor","binding","actions_row"}){
  for(const J& bad:{J(nullptr),J::array(),J("text"),J::object()}){
   auto in=base;in[field]=bad;Result<void> result;ASSERT_NO_THROW(result=validate_qt_equity_verified_prior_continuation(from(in)))<<field;
   ASSERT_TRUE(result.is_error())<<field;EXPECT_STREQ(result.error()->what(),kCode);
  }
 }
 auto shaped=base;shaped["input_market"]["payload"]["instruments"]="SYN";reseal_a(shaped);EXPECT_FALSE(accepted(shaped));
 auto oversized=base;oversized["candidate_action_sources"]=J::array();for(int i=0;i<4097;++i)oversized["candidate_action_sources"].push_back("qt-actions/"+std::to_string(i));EXPECT_FALSE(accepted(oversized));
}

// Contract v2 (plan Task 4): the MODEL-mirrored rounding on non-terminating factors, pinned by literals.
TEST(QtEquityPriorContinuationTest, RestatementMirrorsTheModelArithmetic){
 const std::pair<const char*,const char*> literals[]={{"accepted_split","52"},{"accepted_split_three","34.66666667"},
  {"accepted_dividend","102.97029703"},{"accepted_split_then_dividend","51.48514851"}};
 for(const auto& [name,literal]:literals){SCOPED_TRACE(name);auto in=named(name);
  EXPECT_EQ(instrument(in,"input_market","SYN").at("mark").at("price_model_number"),literal);
  EXPECT_EQ(instrument(in,"input_market","SYN").at("reference").at("price_model_number"),literal);EXPECT_TRUE(accepted(in));}
 auto ulp=named("refused_ulp_dividend");EXPECT_EQ(instrument(ulp,"input_market","SYN").at("mark").at("price_model_number"),"102.97029702");EXPECT_FALSE(accepted(ulp));
 auto up=named("accepted_dividend");set_price(up,"SYN","102.97029704");EXPECT_FALSE(accepted(up));
}

TEST(QtEquityPriorContinuationTest, CreatedAtIsComparedAsAnInstant){
 EXPECT_TRUE(accepted(named("accepted_created_at_other_offset")));EXPECT_TRUE(accepted(named("accepted_created_at_z_spelling")));
 EXPECT_FALSE(accepted(named("refused_created_at_other_instant")));
 auto in=named("accepted_split");const auto original=in.at("actions_row").at("created_at").get<std::string>();
 for(const auto& [spelling,same]:std::initializer_list<std::pair<const char*,bool>>{{"2026-09-25T09:30:02.5+09:30",true},{"2026-09-25T00:00:02.5+0000",true},
     {"2026-09-25T00:00:02.5+00",true},{"2026-09-25 00:00:02.5Z",true},{"2026-09-25T00:00:02.500001Z",false},{"2026-09-25T00:00:02.5",false},{"not-a-time",false}}){
  SCOPED_TRACE(spelling);auto changed=in;changed["actions_row"]["created_at"]=spelling;EXPECT_EQ(accepted(changed),same)<<original;}
}

TEST(QtEquityPriorContinuationTest, InputsAreNotMutated){
 const auto in=from(named("accepted_split_then_dividend"));
 const auto before=J::array({in.decision,in.prior_decision,in.finalization_market,in.input_market,in.finalization,in.anchor,in.binding,in.actions_row});
 (void)validate_qt_equity_verified_prior_continuation(in);
 EXPECT_EQ(before,J::array({in.decision,in.prior_decision,in.finalization_market,in.input_market,in.finalization,in.anchor,in.binding,in.actions_row}));
}

TEST(QtEquityPriorContinuationTest, NewSymbolSharesMsSingleSourceNamespace){
 auto in=named("accepted_new_symbol");ASSERT_TRUE(accepted(in));
 for(auto f:{"reference","mark"})EXPECT_EQ(instrument(in,"input_market","NEW").at(f).at("source_id"),"owned-S-close/NEW");
 auto other=in;for(auto f:{"reference","mark"})instrument(other,"input_market","NEW")[f]["source_id"]="owned-S-close-2/NEW";reseal_a(other);EXPECT_FALSE(accepted(other));
 auto bare=in;for(auto f:{"reference","mark"})instrument(bare,"input_market","NEW")[f]["source_id"]="NEW";reseal_a(bare);EXPECT_FALSE(accepted(bare));
 EXPECT_FALSE(accepted(named("refused_new_symbol_m_has_two_namespaces")));EXPECT_TRUE(accepted(named("accepted_no_new_symbol_m_two_namespaces")));
}

// r3 (A1 review finding 1): M's quote ids need no "/" unless a new-on-D symbol must be matched to M's namespace.
TEST(QtEquityPriorContinuationTest, SlashlessQuoteIdsContinueWhenNoSymbolIsNew){
 for(auto name:{"accepted_no_action_m_quote_ids_without_namespace","accepted_split_m_quote_ids_without_namespace"}){
  SCOPED_TRACE(name);auto in=named(name);
  for(const auto& inst:in.at("finalization_market").at("payload").at("instruments"))for(auto f:{"reference","mark"})
   EXPECT_EQ(inst.at(f).at("source_id").get<std::string>().find('/'),std::string::npos);
  EXPECT_TRUE(accepted(in));
  for(const std::string& id:{std::string("owned-S-close/OPEN"),std::string("owned-S-close")}){
   SCOPED_TRACE(id);auto open=in;auto fresh=instrument(open,"input_market","AAA");fresh["symbol"]="OPEN";
   for(auto f:{"reference","mark"}){fresh[f]["price_frame_id"]="fresh-OPEN";fresh[f]["source_id"]=id;}
   open["input_market"]["payload"]["instruments"].push_back(fresh);reseal_a(open);EXPECT_FALSE(accepted(open))<<"an open needs a governed namespace";
  }
 }
 EXPECT_FALSE(accepted(named("refused_new_symbol_m_quote_ids_without_namespace")));
 auto split=named("accepted_split_m_quote_ids_without_namespace");
 EXPECT_EQ(instrument(split,"input_market","SYN").at("mark").at("source_id"),"owned-S-close");
 EXPECT_EQ(instrument(split,"input_market","SYN").at("mark").at("price_model_number"),"52");
}
