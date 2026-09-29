#include "trade_ngin/apps/qt_equity_prior_continuation.hpp"
#include <cstdint>
#include <stdexcept>
#include <string_view>
// Borrows the caller's book-fenced transaction: loads the continuation's
// operands FOR SHARE and hands them to the pure validator. Writes nothing.
namespace trade_ngin { namespace {
using J=nlohmann::json;
void need(bool ok){if(!ok)throw std::invalid_argument("qt_equity_prior_continuation_unavailable");}
std::string text(const J& j){need(j.is_string());auto s=j.get<std::string>();need(!s.empty()&&s.size()<=4096);return s;}
J rows(pqxx::work& tx,const std::string& sql){J a=J::array();for(auto r:tx.exec(sql)){need(!r[0].is_null()&&std::string_view(r[0].c_str()).size()<=2u*1024*1024);a.push_back(J::parse(r[0].c_str()));}need(a.size()<=4096);return a;}
J one(pqxx::work& tx,const std::string& sql){auto a=rows(tx,sql);need(a.size()==1);return a[0];}
}
Result<void> verify_qt_equity_prior_continuation(pqxx::work& tx,const nlohmann::json& decision,
    const nlohmann::json& input_market,const nlohmann::json& finalization_row,const nlohmann::json& anchor_row){try{
 QtEquityPriorContinuationInputs in;in.decision=decision;in.input_market=input_market;in.finalization=finalization_row;in.anchor=anchor_row;
 const auto& p=input_market.at("payload");
 in.finalization_market=one(tx,"SELECT to_jsonb(m) FROM trading.qt_desk_market_sources m WHERE source_id="+tx.quote(text(finalization_row.at("market_source_id")))+"::uuid FOR SHARE");
 in.prior_decision=one(tx,"SELECT to_jsonb(d) FROM trading.qt_decisions d WHERE decision_id="+tx.quote(text(finalization_row.at("decision_id")))+"::uuid FOR SHARE");
 // Without migration 024 there is no binding and so no continuation; probe
 // first so the caller's transaction is never aborted by a missing relation.
 auto present=tx.exec("SELECT to_regclass('trading.qt_equity_model_prior_bindings') IS NOT NULL");need(present.size()==1&&!present[0][0].is_null()&&present[0][0].as<bool>());
 in.binding=one(tx,"SELECT to_jsonb(b) FROM trading.qt_equity_model_prior_bindings b WHERE publication_id="+tx.quote(text(decision.at("model_publication_id")))+"::uuid FOR SHARE");
 in.actions_row=one(tx,"SELECT to_jsonb(e) FROM trading.qt_equity_desk_evidence_sources e WHERE source_id="+tx.quote(text(p.at("actions_source_id")))+" FOR SHARE");
 // A copy of the MODEL's own candidate query (equity_model_action_source.cpp:44):
 // D rows for this book with previous_day S and non-empty events, in A's governed
 // producer/policy/revision role. M's own empty D actions row is never a candidate.
 need(input_market.at("policy_revision").is_number_integer());
 for(auto r:tx.exec("SELECT source_id FROM trading.qt_equity_desk_evidence_sources e WHERE book_id="+tx.quote(text(input_market.at("book_id")))+
     " AND source_day="+tx.quote(text(input_market.at("source_day")))+"::date AND purpose='actions' AND payload->>'previous_day'="+tx.quote(text(p.at("previous_day")))+
     " AND producer_id="+tx.quote(text(input_market.at("producer_id")))+" AND policy_version="+tx.quote(text(input_market.at("policy_version")))+
     " AND policy_revision="+tx.quote(input_market.at("policy_revision").get<int64_t>())+
     " AND jsonb_array_length(payload->'events')>0 ORDER BY source_id LIMIT 4097 FOR SHARE")){
  need(!r[0].is_null());in.candidate_action_sources.push_back(r[0].as<std::string>());
 }
 need(in.candidate_action_sources.size()<=4096);
 return validate_qt_equity_verified_prior_continuation(in);
}catch(const std::exception&){return make_error<void>(ErrorCode::INVALID_DATA,"qt_equity_prior_continuation_unavailable","qt_equity_prior_continuation");}}
}
