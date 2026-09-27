#include "trade_ngin/data/qt_seed_publication.hpp"
#include "trade_ngin/core/qt_sha256.hpp"
#include "trade_ngin/portfolio/qt_wire.hpp"

#include <algorithm>
#include <array>
#include <set>

namespace trade_ngin {
namespace {
bool complete_key(const ComponentPositionKey& key) {
    return !key.portfolio_id.empty() && !key.strategy_id.empty() &&
           !key.strategy_name.empty() && !key.date.empty() &&
           !key.symbol.empty() && key.portfolio_type == "system";
}

bool uuid_text(const std::string& value) {
    if (value.size()!=36) return false;
    for (size_t i=0; i<value.size(); ++i) {
        if (i==8 || i==13 || i==18 || i==23) {
            if (value[i]!='-') return false;
        } else if (!((value[i]>='0' && value[i]<='9') ||
                     (value[i]>='a' && value[i]<='f'))) return false;
    }
    return true;
}

bool fields(const nlohmann::json& object, const std::set<std::string>& expected) {
    if (!object.is_object() || object.size()!=expected.size()) return false;
    for (auto it=object.begin(); it!=object.end(); ++it)
        if (!expected.contains(it.key())) return false;
    return true;
}

constexpr size_t kMaxManifestBytes = 1'048'576;
}

Result<nlohmann::json> qt_model_seed_document(const QtModelSeedPublication& publication) {
    if (publication.publication_id.empty() || publication.portfolio_id.empty() ||
        publication.strategy_id.empty() || publication.source_day.empty() ||
        publication.system_components.empty() || publication.producer_version.empty())
        return make_error<nlohmann::json>(ErrorCode::INVALID_ARGUMENT,
                                          "qt_model_seed_scope_incomplete");

    auto rows = publication.system_components;
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
        return a.key < b.key;
    });
    nlohmann::json seed_rows = nlohmann::json::array();
    for (size_t i = 0; i < rows.size(); ++i) {
        const auto& row = rows[i];
        if (!complete_key(row.key) || row.key.portfolio_id != publication.portfolio_id ||
            row.key.strategy_id != publication.strategy_id ||
            row.key.date != publication.source_day ||
            (i && row.key == rows[i-1].key))
            return make_error<nlohmann::json>(ErrorCode::INVALID_ARGUMENT,
                                              "qt_model_seed_key_invalid");
        seed_rows.push_back({
            {"key", {{"portfolio_id", row.key.portfolio_id},
                     {"strategy_id", row.key.strategy_id},
                     {"strategy_name", row.key.strategy_name},
                     {"date", row.key.date},
                     {"symbol", row.key.symbol},
                     {"portfolio_type", row.key.portfolio_type}}},
            {"quantity_exact", row.quantity.to_string()},
            {"average_price_exact", row.average_price.to_string()}
        });
    }
    return nlohmann::json{{"seed_rows", std::move(seed_rows)}};
}

Result<std::string> qt_model_seed_digest(const QtModelSeedPublication& publication) {
    auto document = qt_model_seed_document(publication);
    if (document.is_error())
        return make_error<std::string>(document.error()->code(), document.error()->what());
    return qt_digest_v1(document.value());
}

Result<nlohmann::json> qt_validate_proposal_manifest_json(
    const nlohmann::json& rows, const std::string& portfolio_id,
    const std::string& strategy_id, const std::string& source_day) {
    static const std::set<std::string> entry_fields = {
        "action","average_price_exact","key","origin_publication_id",
        "position_revision","quantity_exact"};
    static const std::set<std::string> key_fields = {
        "date","portfolio_id","portfolio_type","strategy_id","strategy_name","symbol"};
    if (!rows.is_array() || rows.size()>4096 || portfolio_id.empty() ||
        strategy_id.empty() || source_day.empty())
        return make_error<nlohmann::json>(ErrorCode::INVALID_ARGUMENT,
                                          "qt_proposal_manifest_scope_invalid");
    std::optional<ComponentPositionKey> previous;
    try {
        for (const auto& row : rows) {
            if (!fields(row,entry_fields) || !fields(row.at("key"),key_fields) ||
                !row.at("action").is_string() ||
                !row.at("quantity_exact").is_string() ||
                !row.at("average_price_exact").is_string())
                throw std::invalid_argument("qt_proposal_manifest_shape_invalid");
            const auto& key = row.at("key");
            for (const auto& name : key_fields)
                if (!key.at(name).is_string())
                    throw std::invalid_argument("qt_proposal_manifest_key_invalid");
            ComponentPositionKey physical{
                key.at("portfolio_id").get<std::string>(),
                key.at("strategy_id").get<std::string>(),
                key.at("strategy_name").get<std::string>(),
                key.at("date").get<std::string>(),
                key.at("symbol").get<std::string>(),
                key.at("portfolio_type").get<std::string>()};
            if (physical.portfolio_id!=portfolio_id ||
                physical.strategy_id!=strategy_id || physical.date!=source_day ||
                physical.strategy_name.empty() || physical.symbol.empty() ||
                physical.portfolio_type!="qt_proposal" ||
                (previous && !(previous.value()<physical)))
                throw std::invalid_argument("qt_proposal_manifest_key_invalid");
            previous = physical;
            const auto quantity = row.at("quantity_exact").get<std::string>();
            const auto basis = row.at("average_price_exact").get<std::string>();
            if (parse_qt_quantity_exact(quantity).is_error() ||
                parse_qt_quantity_exact(basis).is_error())
                throw std::invalid_argument("qt_proposal_manifest_decimal_invalid");
            const auto action = row.at("action").get<std::string>();
            if (action!="inserted" && action!="preserved")
                throw std::invalid_argument("qt_proposal_manifest_action_invalid");
            for (const auto& name : {"position_revision","origin_publication_id"})
                if (!row.at(name).is_null() &&
                    (!row.at(name).is_string() ||
                     !uuid_text(row.at(name).get<std::string>())))
                    throw std::invalid_argument("qt_proposal_manifest_uuid_invalid");
            if ((action=="inserted" &&
                 (row.at("position_revision").is_null() ||
                  row.at("origin_publication_id").is_null())) ||
                (row.at("position_revision").is_null() &&
                 !row.at("origin_publication_id").is_null()))
                throw std::invalid_argument("qt_proposal_manifest_origin_invalid");
        }
        nlohmann::json document{{"proposal_rows",rows}};
        if (document.dump(-1,' ',false,nlohmann::json::error_handler_t::strict).size()
                >kMaxManifestBytes)
            throw std::invalid_argument("qt_proposal_manifest_oversized");
        return document;
    } catch (const std::exception& e) {
        return make_error<nlohmann::json>(ErrorCode::INVALID_ARGUMENT,e.what());
    }
}

Result<nlohmann::json> qt_proposal_manifest_document(
    const QtModelSeedPublication& publication) {
    auto rows = publication.proposal_components;
    std::sort(rows.begin(),rows.end(),[](const auto& a,const auto& b) {
        return a.key<b.key;
    });
    nlohmann::json entries=nlohmann::json::array();
    for (const auto& row : rows) {
        entries.push_back({
            {"action",row.action},
            {"average_price_exact",row.average_price.to_string()},
            {"key",{{"portfolio_id",row.key.portfolio_id},
                    {"strategy_id",row.key.strategy_id},
                    {"strategy_name",row.key.strategy_name},
                    {"date",row.key.date},
                    {"symbol",row.key.symbol},
                    {"portfolio_type",row.key.portfolio_type}}},
            {"origin_publication_id",row.origin_publication_id
                 ? nlohmann::json(*row.origin_publication_id) : nlohmann::json(nullptr)},
            {"position_revision",row.position_revision
                 ? nlohmann::json(*row.position_revision) : nlohmann::json(nullptr)},
            {"quantity_exact",row.quantity.to_string()}});
    }
    return qt_validate_proposal_manifest_json(entries,publication.portfolio_id,
                                               publication.strategy_id,
                                               publication.source_day);
}

Result<std::string> qt_proposal_manifest_digest(
    const QtModelSeedPublication& publication) {
    auto document=qt_proposal_manifest_document(publication);
    if (document.is_error())
        return make_error<std::string>(document.error()->code(),document.error()->what());
    return qt_sha256_hex(document.value().dump(-1,' ',false,
                                              nlohmann::json::error_handler_t::strict));
}

}  // namespace trade_ngin
