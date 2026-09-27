#include "trade_ngin/portfolio/qt_wire.hpp"
#include "trade_ngin/portfolio/qt_evaluation.hpp"
#include "trade_ngin/git_version.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <set>
#include <stdexcept>
#include <string>

namespace trade_ngin {
namespace {
using Json = nlohmann::json;

[[noreturn]] void fail() { throw std::invalid_argument("invalid_qt_evaluation_result"); }

std::string diagnostic(double value) {
    if (!std::isfinite(value)) fail();
    char buffer[64];
    auto [end, error] = std::to_chars(buffer, buffer+sizeof(buffer), value, std::chars_format::general);
    if (error != std::errc{}) fail();
    return std::string(buffer,end);
}

Json component_key(const ComponentPositionKey& key, bool target_stream=false) {
    if (key.portfolio_id.empty()||key.strategy_id.empty()||key.strategy_name.empty()||
        key.date.empty()||key.symbol.empty()||
        (key.portfolio_type!="qt_proposal"&&key.portfolio_type!="qt")) fail();
    return {{"portfolio_id",key.portfolio_id},{"strategy_id",key.strategy_id},
            {"strategy_name",key.strategy_name},{"date",key.date},{"symbol",key.symbol},
            {"portfolio_type",target_stream ? "qt" : key.portfolio_type}};
}

Json instrument(const InstrumentIdentity& identity) {
    const char* type = nullptr;
    switch (identity.type) {
        case AssetType::FUTURE: type="FUTURE"; break;
        case AssetType::EQUITY: type="EQUITY"; break;
        case AssetType::OPTION: type="OPTION"; break;
        case AssetType::FOREX: type="FOREX"; break;
        case AssetType::CRYPTO: type="CRYPTO"; break;
        case AssetType::NONE: fail();
    }
    if (identity.symbol.empty()) fail();
    return {{"instrument_type",type},{"symbol",identity.symbol}};
}

std::string status(QtEvidenceStatus value) {
    switch (value) {
        case QtEvidenceStatus::Evaluated: return "evaluated";
        case QtEvidenceStatus::Disabled: return "disabled";
        case QtEvidenceStatus::Rejected: return "rejected";
        case QtEvidenceStatus::Unavailable: return "unavailable";
    }
    fail();
}

std::string operation(QtEvaluationOperation value) {
    switch (value) {
        case QtEvaluationOperation::DraftDiagnostic: return "draft_diagnostic";
        case QtEvaluationOperation::SelectedBook: return "selected_book";
    }
    fail();
}

std::string book_digest(const ComponentBookOverlay& book) {
    Json rows=Json::array();
    std::set<ComponentPositionKey> seen;
    for (const auto& component:book.components) {
        if (!seen.insert(component.key).second) fail();
        rows.push_back({{"key",component_key(component.key,true)},
                        {"quantity_exact",component.position.quantity.to_string()}});
    }
    auto digest=qt_digest_v1({{"selection_rows",std::move(rows)}});
    if (digest.is_error()) fail();
    return digest.value();
}

Json book_rows(const ComponentBookOverlay& book) {
    Json rows=Json::array();
    std::set<ComponentPositionKey> seen;
    for (const auto& component:book.components) {
        if (!seen.insert(component.key).second) fail();
        rows.push_back({{"key",component_key(component.key)},
                        {"instrument",instrument(component.instrument)},
                        {"editable",component.editable},
                        {"unfilled",component.unfilled},
                        {"quantity_exact",component.position.quantity.to_string()},
                        {"previous_quantity_exact",component.previous ?
                            Json(component.previous->quantity.to_string()) : Json(nullptr)},
                        {"average_price_exact",component.unfilled ? Json(nullptr) :
                            Json(component.position.average_price.to_string())}});
    }
    std::sort(rows.begin(),rows.end(),[](const Json& a,const Json& b) {
        return a.at("key").dump() < b.at("key").dump();
    });
    return rows;
}

Json weight_rows(const std::vector<ComponentOptimizerBinding>& bindings,
                 const std::vector<double>& values) {
    if (bindings.size()!=values.size()) fail();
    Json rows=Json::array();
    for (size_t i=0;i<values.size();++i) {
        auto row=instrument(bindings[i].instrument);
        row["weight_diagnostic"]=diagnostic(values[i]);
        rows.push_back(std::move(row));
    }
    return rows;
}

Json optional_diagnostics(const std::optional<std::vector<double>>& values) {
    if (!values) return nullptr;
    Json out=Json::array();
    for (double value:*values) out.push_back(diagnostic(value));
    return out;
}

Json optimizer_stage(const QtEvaluation& value) {
    Json result={{"status",status(value.optimizer_status)},
                 {"evaluated_book_digest",nullptr},
                 {"aggregate_bindings",Json::array()},
                 {"current_weights",Json::array()},
                 {"target_weights",Json::array()},
                 {"solved_weights",Json::array()},
                 {"trace",nullptr},
                 {"cost_penalty_diagnostic",nullptr},
                 {"tracking_error_diagnostic",nullptr},
                 {"actual_iterations",nullptr},
                 {"config_source_id",nullptr},
                 {"diagnostics",Json::array()}};
    if (value.optimizer_status!=QtEvidenceStatus::Evaluated) {
        if (value.optimizer) fail();
        return result;
    }
    if (!value.optimizer||value.optimizer_config_source_id.empty()) fail();
    const auto& evaluated=*value.optimizer;
    if (!evaluated.trace.solver_iterations || *evaluated.trace.solver_iterations < 0) fail();
    result["actual_iterations"]=*evaluated.trace.solver_iterations;
    result["evaluated_book_digest"]=book_digest(evaluated.evaluated_book);
    result["config_source_id"]=value.optimizer_config_source_id;
    for (const auto& binding:evaluated.prepared.bindings) {
        Json members=Json::array();
        for (const auto& key:binding.members) members.push_back(component_key(key));
        auto row=instrument(binding.instrument);
        row["component_keys"]=std::move(members);
        row["previous_net_quantity_exact"]=binding.previous_net_quantity.to_string();
        row["proposed_net_quantity_exact"]=binding.proposed_net_quantity.to_string();
        result["aggregate_bindings"].push_back(std::move(row));
    }
    result["current_weights"]=weight_rows(evaluated.prepared.bindings,evaluated.prepared.current_weights);
    result["target_weights"]=weight_rows(evaluated.prepared.bindings,evaluated.prepared.target_weights);
    result["solved_weights"]=weight_rows(evaluated.prepared.bindings,evaluated.optimization.positions);
    result["cost_penalty_diagnostic"]=diagnostic(evaluated.optimization.cost_penalty);
    result["tracking_error_diagnostic"]=diagnostic(evaluated.optimization.tracking_error);
    const auto& trace=evaluated.trace;
    const char* branch="not_reached";
    switch (trace.buffer_branch) {
        case OptimizationBufferBranch::NotReached: branch="not_reached"; break;
        case OptimizationBufferBranch::Disabled: branch="disabled"; break;
        case OptimizationBufferBranch::ReturnedPrior: branch="returned_prior"; break;
        case OptimizationBufferBranch::Applied: branch="applied"; break;
        case OptimizationBufferBranch::Failed: branch="failed"; break;
    }
    result["trace"]={{"buffer_branch",branch},
                     {"solver_positions",optional_diagnostics(trace.solver_positions)},
                     {"continuous_buffered_positions",optional_diagnostics(trace.continuous_buffered_positions)},
                     {"rounded_buffered_positions",optional_diagnostics(trace.rounded_buffered_positions)}};
    return result;
}

Json risk_metrics(const ComponentRiskEvaluation& evaluated) {
    const auto& risk = evaluated.risk;
    if (evaluated.evaluated_inputs.market_snapshot_id.empty()) fail();
    const std::pair<const char*,double> values[]={{"portfolio_var",risk.portfolio_var},
        {"jump_risk",risk.jump_risk},{"correlation_risk",risk.correlation_risk},
        {"gross_leverage",risk.gross_leverage},{"net_leverage",risk.net_leverage},
        {"max_portfolio_risk",risk.max_portfolio_risk},
        {"max_jump_risk",risk.max_jump_risk},{"max_leverage_risk",risk.max_leverage_risk},
        {"portfolio_multiplier",risk.portfolio_multiplier},
        {"jump_multiplier",risk.jump_multiplier},
        {"correlation_multiplier",risk.correlation_multiplier},
        {"leverage_multiplier",risk.leverage_multiplier},
        {"portfolio_var_gate",risk.portfolio_var_gate},
        {"recommended_scale",risk.recommended_scale}};
    Json out=Json::array();
    for (const auto& [code,number]:values)
        // Kernels return dimensionless return, correlation, leverage and
        // scaling ratios. Bind diagnostics to the snapshot actually evaluated;
        // the enclosing stage separately records its risk-policy source.
        out.push_back({{"code",code},{"value_diagnostic",diagnostic(number)},
                       {"unit","ratio"},
                       {"source_id",evaluated.evaluated_inputs.market_snapshot_id}});
    return out;
}

Json risk_breaches(const ComponentRiskEvaluation& evaluation) {
    const auto& risk=evaluation.risk;
    const auto& config=evaluation.evaluated_config;
    Json out=Json::array();
    auto add=[&out](std::string_view code,double actual,double limit) {
        out.push_back({{"code",code},{"actual_diagnostic",diagnostic(actual)},
                       {"limit_diagnostic",diagnostic(limit)}});
    };
    if (risk.portfolio_multiplier<1.0) add("portfolio_var",risk.portfolio_var_gate,config.var_limit);
    if (risk.jump_multiplier<1.0) add("jump_risk",risk.jump_risk,config.jump_risk_limit);
    if (risk.correlation_multiplier<1.0) add("correlation",risk.correlation_risk,config.max_correlation);
    if (risk.leverage_multiplier<1.0) {
        if (risk.gross_leverage>config.max_gross_leverage)
            add("gross_leverage",risk.gross_leverage,config.max_gross_leverage);
        if (risk.net_leverage>config.max_net_leverage)
            add("net_leverage",risk.net_leverage,config.max_net_leverage);
    }
    if (risk.risk_exceeded && out.empty()) fail();
    return out;
}

Json risk_stage(const QtEvaluation& value,const std::string& selected_digest) {
    Json result={{"status",status(value.risk_status)},
                 {"evaluated_book_digest",nullptr},{"passed",nullptr},
                 {"breaches",Json::array()},{"metrics",Json::array()},
                 {"config_source_id",nullptr},{"market_snapshot_id",nullptr},
                 {"diagnostics",Json::array()}};
    if (value.risk_status!=QtEvidenceStatus::Evaluated) {
        if (value.risk) fail();
        return result;
    }
    if (!value.risk||value.risk_config_source_id.empty()||
        book_digest(value.risk->evaluated_book)!=selected_digest) fail();
    result["evaluated_book_digest"]=selected_digest;
    result["passed"]=!value.risk->risk.risk_exceeded;
    result["breaches"]=risk_breaches(*value.risk);
    result["metrics"]=risk_metrics(*value.risk);
    result["config_source_id"]=value.risk_config_source_id;
    result["market_snapshot_id"]=value.risk->evaluated_inputs.market_snapshot_id;
    if(value.input_mode==QtEvaluationInputMode::EmptyOwnerV2) {
        // Only an actually completed explicit empty-book risk evaluation can
        // carry this explanation. Legacy/unavailable stages remain unchanged.
        const auto& risk=*value.risk;
        if(!risk.evaluated_book.components.empty() || !risk.bindings.empty() ||
           !risk.evaluated_inputs.valuations.empty() || !risk.evaluated_inputs.closes.empty() ||
           !risk.evaluated_inputs.expected_observation_times.empty() ||
           std::find(value.diagnostics.begin(),value.diagnostics.end(),
             "risk:empty_owner_no_instrument_observations;correlation_history_not_applicable")==value.diagnostics.end()) fail();
        result["diagnostics"].push_back(
            "This selection has no instruments; price and correlation history are not applicable.");
    }
    return result;
}

Json cost_stage(const QtEvaluation& value,const std::string& selected_digest) {
    Json result={{"status",status(value.cost_status)},
                 {"evaluated_book_digest",nullptr},{"by_component",Json::array()},
                 {"total_exact",nullptr},{"currency",nullptr},
                 {"diagnostics",Json::array()}};
    if (value.cost_status!=QtEvidenceStatus::Evaluated) {
        if (value.selected_costs) fail();
        return result;
    }
    if (!value.selected_costs||value.selected_costs->evaluated_book_digest!=selected_digest) fail();
    result["evaluated_book_digest"]=selected_digest;
    result["total_exact"]=value.selected_costs->total_cash_cost.to_string();
    result["currency"]=value.selected_costs->currency;
    std::set<ComponentPositionKey> seen;
    for (const auto& line:value.selected_costs->by_component) {
        if (!seen.insert(line.key).second) fail();
        result["by_component"].push_back({{"key",component_key(line.key)},
            {"prior_quantity_exact",line.previous_quantity.to_string()},
            {"selected_quantity_exact",line.selected_quantity.to_string()},
            {"cash_cost_exact",line.estimated_cash_cost.to_string()},
            {"source_id",line.source_id}});
    }
    return result;
}

}  // namespace

Result<std::string> serialize_qt_evaluation(const QtEvaluation& value) {
    try {
        if (value.input_mode!=QtEvaluationInputMode::LegacyV1 &&
            value.input_mode!=QtEvaluationInputMode::EmptyOwnerV2) fail();
        if (value.evaluator_build!=TRADE_NGIN_GIT_SHA || value.context_fingerprint.size()!=64 ||
            value.context_fingerprint.find_first_not_of("0123456789abcdef")!=std::string::npos) fail();
        const auto selected_digest=book_digest(value.evaluated_book);
        if (selected_digest!=value.evaluated_book_digest) fail();
        if (value.operation==QtEvaluationOperation::SelectedBook &&
            value.optimizer_status!=QtEvidenceStatus::Disabled) fail();
        Json result={{"schema",value.input_mode==QtEvaluationInputMode::EmptyOwnerV2 ?
                     "qt-eval-empty-owner/v2" : "qt-eval/v1"},
                     {"operation",operation(value.operation)},
                     {"evaluator_build",value.evaluator_build},
                     {"context_fingerprint",value.context_fingerprint},
                     {"evaluated_book_digest",selected_digest},
                     {"evaluated_book",book_rows(value.evaluated_book)},
                     {"optimizer",optimizer_stage(value)},
                     {"selected_risk",risk_stage(value,selected_digest)},
                     {"selected_costs",cost_stage(value,selected_digest)},
                     {"diagnostics",value.diagnostics}};
        const bool risk=value.risk_status==QtEvidenceStatus::Evaluated;
        const bool costs=value.cost_status==QtEvidenceStatus::Evaluated;
        const bool optimizer=value.optimizer_status==QtEvidenceStatus::Evaluated ||
            value.optimizer_status==QtEvidenceStatus::Disabled;
        result["completeness"]=risk&&costs&&optimizer ? "complete" :
            (risk||costs ? "partial" : "unavailable");
        auto bytes=result.dump(-1,' ',false,Json::error_handler_t::strict);
        if (bytes.size()>8*1024*1024) fail();
        return bytes;
    } catch (const std::exception& e) {
        return make_error<std::string>(ErrorCode::INVALID_DATA,e.what(),"qt_eval_wire");
    }
}

}  // namespace trade_ngin
