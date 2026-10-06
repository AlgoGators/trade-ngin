#pragma once
#include <pqxx/pqxx>
namespace trade_ngin {
// False only when BOTH the reserved v2 relation and capability are absent.
// A partial, changed or unsupported installation throws and cannot widen v1.
bool require_qt_empty_owner_storage_capability(pqxx::work&);
}
