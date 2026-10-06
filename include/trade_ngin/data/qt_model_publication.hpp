#pragma once
#include <pqxx/pqxx>
#include "trade_ngin/data/qt_empty_model_owner_reference.hpp"
namespace trade_ngin {
enum class QtModelPublicationKind { LegacyV1, EmptyOwnerV2 };
struct QtModelPublicationRecord {
    QtModelPublicationKind kind;
    nlohmann::json row,reference;
};
// Immutable archive only; current physical/authority/lease readiness is separate.
// Caller owns transaction. No nested commits/current-clock checks.
Result<QtModelPublicationRecord> load_qt_model_publication_record(pqxx::work&,const std::string& publication_id);
// Complete immutable book/day union inventory and head only. Current metadata,
// registry/member/physical/QT continuation readiness remains caller-owned.
Result<nlohmann::json> load_qt_model_publication_scope(pqxx::work&,const std::string& book,const std::string& source_day);
} // namespace trade_ngin
