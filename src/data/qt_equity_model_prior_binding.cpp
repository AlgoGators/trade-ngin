#include "trade_ngin/data/qt_equity_model_prior_binding.hpp"
#include "trade_ngin/core/qt_sha256.hpp"
#include "trade_ngin/data/qt_desk_current_facts.hpp"
#include <regex>
#include <stdexcept>

namespace trade_ngin {
namespace {
using J=nlohmann::json;
void need(bool yes){if(!yes)throw std::invalid_argument("qt_equity_model_prior_binding_unavailable");}
std::string text(const J& j){need(j.is_string());auto s=j.get<std::string>();need(!s.empty()&&s.size()<=4096);return s;}
std::string uuid(const J& j){static const std::regex pattern("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$");
    const auto s=text(j);need(std::regex_match(s,pattern)&&s!="00000000-0000-0000-0000-000000000000");return s;}
std::string hex(const J& j){const auto s=text(j);need(s.size()==64&&s.find_first_not_of("0123456789abcdef")==std::string::npos);return s;}
std::string day(const J& j){static const std::regex pattern("^[0-9]{4}-[0-9]{2}-[0-9]{2}$");const auto s=text(j);need(std::regex_match(s,pattern));return s;}
// The engine's own canonical input wire (the one that already digests the
// action frame): sorted keys, no floats, bounded strings/arrays/objects.
std::string digest(const J& j){auto bytes=canonical_qt_desk_input_json(j);need(bytes.is_ok());
    auto hash=qt_sha256_hex(bytes.value());need(hash.is_ok());return hash.value();}
}

Result<QtEquityModelPriorBinding> derive_qt_equity_model_prior_binding(
    const std::string& publication_id,const std::string& book_id,
    const std::string& publication_day,const J& reference){try{
    need(reference.is_object());
    QtEquityModelPriorBinding row;
    row.publication_id=uuid(J(publication_id));row.book_id=text(J(book_id));row.source_day=day(J(publication_day));
    row.strategy_id="LIVE_EQUITY_MEAN_REVERSION";
    const auto schema=text(reference.at("schema_version"));
    const bool v2=schema=="qt-equity-model-prior/v2";
    need(v2||schema=="qt-equity-model-prior/v1");
    need(reference.at("mode")=="verified_desk_prior");
    need(reference.at("book_id")==book_id&&day(reference.at("valuation_day"))==row.source_day&&
         day(reference.at("source_day"))<row.source_day);
    row.decision_id=uuid(reference.at("decision_id"));
    row.finalization_id=uuid(reference.at("finalization_id"));
    row.finalization_digest=hex(reference.at("finalization_digest"));
    row.finalization_source_id=text(reference.at("finalization_source_id"));
    need(row.finalization_source_id=="qt-finalization/"+row.finalization_id);
    row.finalization_source_digest=hex(reference.at("finalization_source_digest"));
    if(v2){
        need(reference.at("action_admission")=="proved_action_adjusted_prior");
        const auto& frame=reference.at("action_frame");need(frame.is_object());
        need(hex(reference.at("action_frame_digest"))==digest(frame));
        const auto& source=frame.at("actions_source");need(source.is_object());
        need(source.at("purpose")=="actions"&&source.at("book_id")==book_id&&source.at("source_day")==row.source_day);
        row.actions_source_id=text(source.at("source_id"));
        row.actions_source_digest=hex(source.at("content_digest"));
    } else {
        need(reference.at("action_admission")=="action_free_only"&&
             !reference.contains("action_frame")&&!reference.contains("action_frame_digest"));
    }
    row.replay_reference=reference;
    row.replay_reference_digest=digest(reference);
    return row;
}catch(const std::exception&){return make_error<QtEquityModelPriorBinding>(ErrorCode::INVALID_DATA,
    "qt_equity_model_prior_binding_unavailable","qt_equity_model_prior_binding");}}

Result<void> record_qt_equity_model_prior_binding(pqxx::work& tx,const QtEquityModelPriorBinding& row){try{
    // Never trust a caller-assembled row: it must be exactly the derivation.
    auto derived=derive_qt_equity_model_prior_binding(row.publication_id,row.book_id,row.source_day,row.replay_reference);
    need(derived.is_ok());const auto& d=derived.value();
    need(d.strategy_id==row.strategy_id&&d.decision_id==row.decision_id&&d.finalization_id==row.finalization_id&&
         d.finalization_digest==row.finalization_digest&&d.finalization_source_id==row.finalization_source_id&&
         d.finalization_source_digest==row.finalization_source_digest&&d.actions_source_id==row.actions_source_id&&
         d.actions_source_digest==row.actions_source_digest&&d.replay_reference_digest==row.replay_reference_digest);
    // Storage capability qt_equity_model_prior_binding_v1 (migration 024), exactly once at version 1.
    const auto relations=tx.exec("SELECT to_regclass('trading.qt_equity_model_prior_bindings') IS NOT NULL "
        "AND to_regclass('trading.qt_storage_capabilities') IS NOT NULL");
    need(relations.size()==1&&relations[0][0].as<bool>());
    const auto capability=tx.exec("SELECT count(*),count(*) FILTER (WHERE capability_version=1) "
        "FROM trading.qt_storage_capabilities WHERE capability_name='qt_equity_model_prior_binding_v1'");
    need(capability.size()==1&&capability[0][0].as<long long>()==1&&capability[0][1].as<long long>()==1);
    const auto inserted=tx.exec("INSERT INTO trading.qt_equity_model_prior_bindings(publication_id,book_id,source_day,"
        "strategy_id,decision_id,finalization_id,finalization_digest,finalization_source_id,finalization_source_digest,"
        "actions_source_id,actions_source_digest,replay_reference,replay_reference_digest) VALUES($1::uuid,$2,$3::date,"
        "$4,$5::uuid,$6::uuid,$7,$8,$9,$10,$11,$12::jsonb,$13) RETURNING publication_id::text",
        pqxx::params{row.publication_id,row.book_id,row.source_day,row.strategy_id,row.decision_id,row.finalization_id,
            row.finalization_digest,row.finalization_source_id,row.finalization_source_digest,row.actions_source_id,
            row.actions_source_digest,row.replay_reference.dump(),row.replay_reference_digest});
    need(inserted.size()==1&&inserted[0][0].as<std::string>()==row.publication_id);
    return Result<void>();
}catch(const std::exception&){return make_error<void>(ErrorCode::DATABASE_ERROR,
    "qt_equity_model_prior_binding_write_refused","qt_equity_model_prior_binding");}}
}
