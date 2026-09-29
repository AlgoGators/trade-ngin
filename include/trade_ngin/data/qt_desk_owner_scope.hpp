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
// N5 r2 (F2): an engine that appears only in carried (non-editable) rows of a desk selection is still
// written (its qt accounting rows). It must be registered for the book (primary book or membership) and
// every such registry row must be live and active: an incubating, retired, inactive or unregistered carried
// engine refuses. (Carried rows do not need the desk-editing membership that editable owners need.)
inline bool qt_desk_carried_engine_live(const nlohmann::json& registry,
        const nlohmann::json& memberships,std::string_view engine,std::string_view book) {
    const std::string engine_id(engine),book_id(book);
    std::set<std::string> book_members;
    for(const auto& m:memberships)
        if(m.at("portfolio_id")==book_id)book_members.insert(m.at("strategy_id").get<std::string>());
    bool registered=false;
    for(const auto& r:registry) {
        if(r.at("strategy_type")!=engine_id)continue;
        if(r.at("portfolio_id")!=book_id&&!book_members.contains(r.at("id").get<std::string>()))continue;
        if(r.at("is_active")!=true||r.at("lifecycle")!="live")return false;
        registered=true;
    }
    return registered;
}
// Every engine the selection names: editable engines keep qt_desk_owner_authorized (exactly one live,
// active, book-member owner); engines that only appear in carried rows need qt_desk_carried_engine_live.
inline bool qt_desk_selection_owners_authorized(const nlohmann::json& registry,
        const nlohmann::json& memberships,const nlohmann::json& selection_rows,std::string_view book) {
    std::set<std::string> editable,carried;
    for(const auto& row:selection_rows)
        (row.at("editable")==true?editable:carried).insert(row.at("key").at("strategy_id").get<std::string>());
    for(const auto& engine:editable)
        if(!qt_desk_owner_authorized(registry,memberships,engine,book))return false;
    for(const auto& engine:carried)
        if(!editable.contains(engine)&&!qt_desk_carried_engine_live(registry,memberships,engine,book))return false;
    return true;
}
// Empty-owner desk facts (qt-inputs-empty-owner/v2) admit only the publication's own registry row,
// unchanged since the publication (same engine and runtime_revision) and still live and active.
inline bool qt_desk_empty_owner_registry_eligible(const nlohmann::json& registry,
        const nlohmann::json& engine_id,const nlohmann::json& registry_revision) {
    return registry.at("strategy_type")==engine_id&&registry.at("runtime_revision")==registry_revision&&
        registry.at("lifecycle")=="live"&&registry.at("is_active")==true;
}
}
