#include "trade_ngin/data/qt_desk_publication.hpp"
#include "trade_ngin/portfolio/qt_wire.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>

namespace trade_ngin {
namespace {
using Json = nlohmann::json;

[[noreturn]] void reject() { throw std::invalid_argument("qt_desk_decision_invalid"); }

std::set<std::string> names(const Json& value) {
    if (!value.is_object()) reject();
    std::set<std::string> out;
    for (auto it = value.begin(); it != value.end(); ++it) out.insert(it.key());
    return out;
}

void exact_names(const Json& value, std::initializer_list<std::string_view> expected) {
    std::set<std::string> wanted;
    for (auto name : expected) wanted.insert(std::string(name));
    if (names(value) != wanted) reject();
}

const std::string& string_at(const Json& value, std::string_view field) {
    const auto& item = value.at(std::string(field));
    if (!item.is_string()) reject();
    return item.get_ref<const std::string&>();
}

bool nonempty(const Json& value, std::string_view field) {
    return !string_at(value,field).empty();
}

void same(const Json& left, const Json& right, std::string_view field) {
    if (string_at(left,field) != string_at(right,field)) reject();
}

void lowercase_digest(const std::string& value) {
    if (value.size()!=64 || value.find_first_not_of("0123456789abcdef")!=std::string::npos)
        reject();
}

void uuid(const std::string& value) {
    if (value.size()!=36) reject();
    for (size_t i=0;i<value.size();++i) {
        if (i==8 || i==13 || i==18 || i==23) {
            if (value[i]!='-') reject();
        } else if (!((value[i]>='0'&&value[i]<='9') ||
                     (value[i]>='a'&&value[i]<='f'))) reject();
    }
}

void positive_integer(const Json& value, std::string_view field) {
    const auto& item=value.at(std::string(field));
    if (!item.is_number_integer()) reject();
    if (item.is_number_unsigned()) {
        const auto number=item.get<uint64_t>();
        if (number==0 || number>static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) reject();
    } else if (item.get<int64_t>()<=0) reject();
}

Quantity exact(const Json& value) {
    if (!value.is_string()) reject();
    auto parsed=parse_qt_quantity_exact(value.get_ref<const std::string&>());
    if (parsed.is_error()) reject();
    return parsed.value();
}

ComponentPositionKey key(const Json& value, std::string_view book, std::string_view day) {
    exact_names(value,{"portfolio_id","strategy_id","strategy_name","date","symbol","portfolio_type"});
    ComponentPositionKey out{string_at(value,"portfolio_id"),string_at(value,"strategy_id"),
                             string_at(value,"strategy_name"),string_at(value,"date"),
                             string_at(value,"symbol"),string_at(value,"portfolio_type")};
    if (out.portfolio_id!=book || out.date!=day || out.strategy_id.empty() ||
        out.strategy_name.empty() || out.symbol.empty() ||
        (out.portfolio_type!="qt_proposal" && out.portfolio_type!="qt")) reject();
    return out;
}

void check_display_row(const Json& row, const Quantity& quantity) {
    exact_names(row,{"key","quantity_exact","basis_status","average_price_exact",
                     "asset_type","editable","origin"});
    const auto& basis=string_at(row,"basis_status");
    if (basis!="preserved_source" && basis!="unfilled") reject();
    if (basis=="unfilled") {
        if (!row.at("average_price_exact").is_null()) reject();
    } else {
        exact(row.at("average_price_exact"));
    }
    const auto& asset=string_at(row,"asset_type");
    if (asset!="EQUITY" && asset!="FUTURE") reject();
    if (asset=="FUTURE" && quantity.raw_value()%Quantity(1).raw_value()!=0) reject();
    if (!row.at("editable").is_boolean()) reject();
    const auto& origin=string_at(row,"origin");
    if (origin!="verified_model_seed" && origin!="reconciled_legacy_draft" &&
        origin!="qt_draft" && origin!="verified_qt_decision" && origin!="immutable") reject();
}

void check_evidence(const Json& payload, const std::map<ComponentPositionKey,Quantity>& editable) {
    const auto& evaluation=payload.at("evaluation");
    const auto& risk=evaluation.at("selected_risk");
    const auto& costs=evaluation.at("selected_costs");
    if (string_at(risk,"status")!="evaluated" ||
        string_at(costs,"status")!="evaluated" ||
        string_at(risk,"evaluated_book_digest")!=string_at(payload,"selected_book_digest") ||
        string_at(costs,"evaluated_book_digest")!=string_at(payload,"selected_book_digest") ||
        !risk.at("passed").is_boolean() ||
        !nonempty(risk,"config_source_id") || !nonempty(risk,"market_snapshot_id")) reject();
    const bool passed=risk.at("passed").get<bool>();
    const bool override_required=payload.at("requires_override").get<bool>();
    if (passed==override_required || risk.at("breaches").empty()==override_required) reject();
    for (const auto& metric:risk.at("metrics"))
        if (metric.at("value_diagnostic").is_null() || !nonempty(metric,"source_id")) reject();
    for (const auto& breach:risk.at("breaches"))
        if (breach.at("limit_diagnostic").is_null() ||
            breach.at("actual_diagnostic").is_null()) reject();
    const auto& optimizer=evaluation.at("optimizer");
    if (string_at(optimizer,"status")=="evaluated" &&
        string_at(optimizer,"evaluated_book_digest")!=string_at(payload,"optimizer_book_digest")) reject();
    std::map<ComponentPositionKey,Quantity> costed;
    int64_t sum=0;
    for (const auto& row:costs.at("by_component")) {
        auto source_key=key(row.at("key"),string_at(payload,"book_id"),
                            string_at(payload,"source_day"));
        const auto chosen=exact(row.at("selected_quantity_exact"));
        if (!costed.emplace(source_key,chosen).second || !nonempty(row,"source_id")) reject();
        exact(row.at("prior_quantity_exact"));
        const auto cost=exact(row.at("cash_cost_exact")).raw_value();
        if (cost<0 || sum>std::numeric_limits<int64_t>::max()-cost) reject();
        sum+=cost;
    }
    if (costed!=editable || exact(costs.at("total_exact")).raw_value()!=sum) reject();
}
}  // namespace

Result<QtDeskSelectedBook> validate_qt_desk_decision_snapshot(
    const nlohmann::json& decision, const nlohmann::json& preview) {
    try {
        const auto& envelope=decision.at("payload");
        exact_names(envelope,{"schema_version","decision_id","preview_id","book_id",
                              "source_day","preview_payload_digest","selected_book_digest",
                              "read_set_digest"});
        if (string_at(envelope,"schema_version")!="qt-desk-decision/v1" ||
            string_at(decision,"status")!="confirmed_decision" ||
            string_at(preview,"state")!="confirmed_decision" ||
            string_at(preview,"availability")!="ready" ||
            !nonempty(preview,"evaluator_build") || !nonempty(preview,"policy_version") ||
            !preview.at("read_set_payload").is_object()) reject();
        uuid(string_at(decision,"decision_id"));
        uuid(string_at(decision,"preview_id"));
        uuid(string_at(decision,"draft_id"));
        if (!decision.at("model_publication_id").is_null())
            uuid(string_at(decision,"model_publication_id"));
        positive_integer(decision,"draft_revision");
        positive_integer(decision,"workflow_capability_version");
        positive_integer(decision,"submitter_grant_version");
        positive_integer(decision,"created_by");
        for (const auto* field:{"decision_id","preview_id","book_id","source_day",
                                "selected_book_digest","read_set_digest"})
            same(envelope,decision,field);
        for (const auto* field:{"preview_id","book_id","source_day","draft_id",
                                "selected_book_digest","read_set_digest","provenance_digest",
                                "policy_version"})
            same(decision,preview,field);
        if (decision.at("draft_revision")!=preview.at("draft_revision") ||
            string_at(envelope,"preview_payload_digest")!=string_at(preview,"payload_digest")) reject();
        for (const auto* field:{"preview_payload_digest","selected_book_digest",
                                "read_set_digest"}) lowercase_digest(string_at(envelope,field));
        const auto& payload=preview.at("payload");
        exact_names(payload,{"schema_version","book_id","source_day","preview_id","payload_digest",
                             "optimizer_book_digest","selected_book_digest","draft_id","draft_revision",
                             "source_digest","provenance_digest","read_set_digest","availability",
                             "confirmable","requires_override","selection_rows","unavailable_reasons",
                             "evaluation"});
        if (string_at(payload,"schema_version")!="qt-workflow/v1" ||
            string_at(payload,"availability")!="ready" ||
            !payload.at("confirmable").is_boolean() || !payload.at("confirmable").get<bool>() ||
            !payload.at("requires_override").is_boolean() ||
            !payload.at("unavailable_reasons").is_array() ||
            !payload.at("unavailable_reasons").empty()) reject();
        for (const auto* field:{"book_id","source_day","preview_id","draft_id",
                                "source_digest","provenance_digest","read_set_digest",
                                "selected_book_digest","payload_digest"}) same(payload,preview,field);
        if (payload.at("draft_revision")!=preview.at("draft_revision")) reject();
        Json unhashed=payload;
        unhashed.erase("payload_digest");
        auto actual_payload_digest=qt_digest_v1(unhashed);
        if (actual_payload_digest.is_error() ||
            actual_payload_digest.value()!=string_at(payload,"payload_digest")) reject();
        auto checked_payload=canonical_qt_bytes(payload);
        if (checked_payload.is_error()) reject();
        const auto& selection=payload.at("selection_rows");
        if (!selection.is_array()) reject();
        Json projected=Json::array();
        QtDeskSelectedBook book;
        std::set<ComponentPositionKey> seen_target;
        std::map<ComponentPositionKey,Quantity> editable;
        for (const auto& row:selection) {
            auto source_key=key(row.at("key"),string_at(payload,"book_id"),
                                string_at(payload,"source_day"));
            auto quantity=exact(row.at("quantity_exact"));
            check_display_row(row,quantity);
            if (row.at("editable").get<bool>() && !editable.emplace(source_key,quantity).second)
                reject();
            auto target_key=source_key;
            target_key.portfolio_type="qt";
            if (!seen_target.insert(target_key).second) reject();
            Json target=row.at("key");
            target["portfolio_type"]="qt";
            projected.push_back({{"key",std::move(target)},
                                 {"quantity_exact",quantity.to_string()}});
            book.selected_rows.push_back({std::move(target_key),quantity});
        }
        auto actual_selected_digest=qt_digest_v1({{"selection_rows",projected}});
        if (actual_selected_digest.is_error() ||
            actual_selected_digest.value()!=string_at(payload,"selected_book_digest")) reject();
        check_evidence(payload,editable);
        std::sort(book.selected_rows.begin(),book.selected_rows.end(),
                  [](const auto& a,const auto& b){return a.key<b.key;});
        book.selected_book_digest=actual_selected_digest.value();
        return book;
    } catch (const std::exception&) {
        return make_error<QtDeskSelectedBook>(ErrorCode::INVALID_DATA,
                                               "qt_desk_decision_invalid", "qt_desk_publication");
    }
}

}  // namespace trade_ngin
