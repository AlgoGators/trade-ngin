#pragma once
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <string_view>

namespace trade_ngin {
// Eligibility over already admitted qt-read-set registry/membership rows.
// SQL selection, locks and immutable digest binding remain with the caller.
inline bool qt_desk_owner_authorized(const nlohmann::json& registry,
        const nlohmann::json& memberships,std::string_view engine,std::string_view book) {
    const std::string engine_id(engine),book_id(book);
    std::set<std::string> book_members;
    for(const auto& m:memberships)
        if(m.at("portfolio_id")==book_id)book_members.insert(m.at("strategy_id").get<std::string>());
    const nlohmann::json* owner=nullptr;
    for(const auto& r:registry) {
        if(r.at("strategy_type")!=engine_id||r.at("is_active")!=true||r.at("lifecycle")!="live")continue;
        const auto& id=r.at("id").get_ref<const std::string&>();
        if(r.at("portfolio_id")!=book_id&&!book_members.contains(id))continue;
        if(owner!=nullptr)return false;
        owner=&r;
    }
    return owner!=nullptr&&book_members.contains(owner->at("id").get_ref<const std::string&>());
}
}
