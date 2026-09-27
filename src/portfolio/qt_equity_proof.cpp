#include "trade_ngin/portfolio/qt_equity_proof.hpp"
#include "trade_ngin/apps/qt_equity_accounting_output.hpp"
#include "trade_ngin/apps/qt_equity_prior_finalization.hpp"
#include "trade_ngin/data/qt_desk_current_facts.hpp"
#include "trade_ngin/core/qt_sha256.hpp"
#include "trade_ngin/portfolio/qt_wire.hpp"
#include "trade_ngin/git_version.hpp"
#include <set>
#include <stdexcept>
#include <vector>
namespace trade_ngin { namespace {
using J=nlohmann::json;
void need(bool ok) { if(!ok) throw std::invalid_argument("invalid_qt_offline_request"); }
}
Result<J> parse_qt_offline_envelope(std::string_view bytes) {
    try {
        need(!bytes.empty() && bytes.size()<=8u*1024*1024);
        std::vector<std::set<std::string>> keys;
        std::size_t events=0;
        auto callback=[&](int depth,J::parse_event_t event,J& parsed) {
            need(depth<=32);++events;
            if(event==J::parse_event_t::object_start) keys.emplace_back();
            else if(event==J::parse_event_t::object_end) { need(!keys.empty()); keys.pop_back(); }
            else if(event==J::parse_event_t::key) {
                need(!keys.empty() && keys.back().insert(parsed.get<std::string>()).second);
            }
            return true;
        };
        auto value=J::parse(bytes.begin(),bytes.end(),callback);
        need(value.is_object() && value.contains("schema") && value.at("schema").is_string());
        // Preserve the existing qt-eval/v1 capacity contract. The byte/depth
        // bounds still apply to every request; this tighter event limit belongs
        // to the new equity proof operation and unknown envelopes only.
        need(value.at("schema")=="qt-eval/v1" || events<=400000);
        return value;
    } catch(const std::exception&) {
        return make_error<J>(ErrorCode::INVALID_ARGUMENT,"invalid_qt_offline_request","qt_offline_wire");
    }
}
namespace {
void shape(const J& value,std::initializer_list<const char*> fields) {
    need(value.is_object() && value.size()==fields.size());
    for(auto field:fields) need(value.contains(field));
}
std::string hash(const J& value,bool output=false) {
    auto bytes=output?canonical_qt_desk_source_json(value):canonical_qt_desk_input_json(value);
    need(bytes.is_ok());auto digest=qt_sha256_hex(bytes.value());need(digest.is_ok());return digest.value();
}
void selection_shape(const J& rows,bool empty_owner) {
    need(rows.is_array() && rows.size()<=2048);
    need(empty_owner ? rows.empty() : !rows.empty());
    for(const auto& row:rows) {
        shape(row,{"key","quantity_exact","average_price_exact","basis_status","asset_type","editable","origin"});
        shape(row.at("key"),{"portfolio_id","strategy_id","strategy_name","date","symbol","portfolio_type"});
        need(row.at("asset_type")=="EQUITY" && row.at("editable").is_boolean());
        need(row.at("basis_status")=="preserved_source" || row.at("basis_status")=="unfilled");
        need((row.at("basis_status")=="unfilled")==row.at("average_price_exact").is_null());
        const std::set<J> origins={"verified_model_seed","reconciled_legacy_draft","verified_qt_decision","qt_draft","immutable"};
        need(origins.contains(row.at("origin")));
    }
    // Reuse public exact decimal/key validation without replacing, sorting or
    // reserializing the immutable selection supplied to the actual kernel.
    need(canonical_qt_bytes(J{{"selection_rows",rows}}).is_ok());
}
}
Result<J> recompute_qt_equity_proof(const J& request) {
    try {
        // Bounds and float rejection precede any kernel call. Every request
        // operand is covered by this fingerprint; expected output is absent.
        need(canonical_qt_desk_input_json(request).is_ok());
        shape(request,{"schema","operation","evaluator_build","context_fingerprint",
            "decision","selection_rows","accounting_input","producer_authority"});
        need(request.at("schema")=="qt-equity-proof/v1" && request.at("operation")=="recompute_equity_accounting");
        need(request.at("evaluator_build")==std::string(TRADE_NGIN_GIT_SHA));
        need(request.at("context_fingerprint").is_string());
        auto operands=request;operands.erase("context_fingerprint");
        need(request.at("context_fingerprint")==hash(operands));
        const auto& decision=request.at("decision");
        shape(decision,{"decision_id","book_id","source_day"});
        const auto& input=request.at("accounting_input");
        const bool empty_owner=input.is_object() && input.contains("schema_version") &&
            input.at("schema_version")=="qt-equity-accounting-input-empty-owner/v2";
        const auto& selection=request.at("selection_rows");selection_shape(selection,empty_owner);
        const auto& authority=request.at("producer_authority");
        // The shared helper validates closed financial input/authority and
        // runs the actual equity position/cost kernels with their actual trace.
        // SQL authority and executable-bundle identity remain caller duties.
        auto computed=recompute_qt_equity_accounting_output(decision,selection,input,authority);
        need(computed.is_ok());const auto& output=computed.value();
        auto financial=output;financial.erase("consumption");
        return J{{"schema","qt-equity-proof/v1"},{"operation","recompute_equity_accounting"},
            {"evaluator_build",TRADE_NGIN_GIT_SHA},{"context_fingerprint",request.at("context_fingerprint")},
            {"calculation_version","qt-equity-main08b15c/v1"},{"input_digest",hash(input)},
            {"selection_digest",hash(selection)},{"financial_output_digest",hash(financial)},
            {"output_digest",hash(output,true)}};
    } catch(const std::exception&) {
        return make_error<J>(ErrorCode::INVALID_ARGUMENT,"qt_equity_recomputation_unavailable","qt_equity_proof");
    }
}
Result<J> recompute_qt_equity_finalization_proof(const J& request) {
    try {
        shape(request,{"schema","operation","evaluator_build","context_fingerprint",
            "decision","original_input","original_output_json","market_payload",
            "actions_payload","before_financial","provenance","finalizer_authority"});
        need(request.at("schema")=="qt-equity-finalization-proof/v1" &&
             request.at("operation")=="recompute_equity_finalization");
        need(request.at("evaluator_build")==std::string(TRADE_NGIN_GIT_SHA));
        need(request.at("original_output_json").is_string());
        const auto& original_bytes=request.at("original_output_json").get_ref<const std::string&>();
        need(!original_bytes.empty() && original_bytes.size()<=1024u*1024);

        // Only the nested, already source-canonical output may contain the
        // original native consumption trace's floating point observations.
        // Keep the normal exact input limits for all other operands.
        auto bounded=request;bounded["original_output_json"]="";
        need(canonical_qt_desk_input_json(bounded).is_ok());
        auto operands=request;operands.erase("context_fingerprint");
        need(request.at("context_fingerprint").is_string() &&
             request.at("context_fingerprint")==hash(operands,true));
        auto parsed=parse_qt_offline_envelope(
            std::string("{\"schema\":\"qt-equity-original-output/v1\",\"output\":")+original_bytes+"}");
        need(parsed.is_ok());const auto& wrapper=parsed.value();
        shape(wrapper,{"schema","output"});
        need(wrapper.at("schema")=="qt-equity-original-output/v1" && wrapper.at("output").is_object());
        const auto& original=wrapper.at("output");
        auto canonical=canonical_qt_desk_source_json(original);
        need(canonical.is_ok() && canonical.value()==original_bytes);
        shape(original,{"schema_version","calculation_version","input_digest","selection_policy",
            "producer_authority","observation","executions","live_results","equity_curve",
            "distance","layers_applied","corporate_action_adjustments","consumption"});
        auto original_financial=original;original_financial.erase("consumption");
        need(canonical_qt_desk_input_json(original_financial).is_ok());

        const auto& decision=request.at("decision");
        shape(decision,{"decision_id","book_id","source_day","model_publication_id"});
        const auto& authority=request.at("finalizer_authority");
        need(authority==original.at("producer_authority") &&
             authority==request.at("provenance").at("finalizer_authority"));
        // This proves the mark successor using the retained original producer.
        // The caller must separately prove original accounting and SQL lineage;
        // source IDs and digests supplied here do not confer authority.
        auto computed=produce_qt_equity_prior_finalization(decision,request.at("original_input"),
            original,request.at("market_payload"),request.at("actions_payload"),
            request.at("before_financial"),request.at("provenance"));
        need(computed.is_ok());
        return J{{"schema","qt-equity-finalization-proof/v1"},{"operation","recompute_equity_finalization"},
            {"evaluator_build",TRADE_NGIN_GIT_SHA},{"context_fingerprint",request.at("context_fingerprint")},
            {"calculation_version","equity-prior-close-mark/v1"},{"original_output_digest",hash(original,true)},
            {"market_digest",hash(request.at("market_payload"))},{"actions_digest",hash(request.at("actions_payload"))},
            {"before_financial_digest",hash(request.at("before_financial"))},
            {"provenance_digest",hash(request.at("provenance"))},{"successor_digest",hash(computed.value())}};
    } catch(const std::exception&) {
        return make_error<J>(ErrorCode::INVALID_ARGUMENT,"qt_equity_finalization_proof_unavailable","qt_equity_proof");
    }
}
}
