// include/trade_ngin/relative_value/contract_lookup.hpp
#pragma once

#include <arrow/api.h>

#include <memory>

#include "trade_ngin/core/error.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/relative_value/definition_validator.hpp"

namespace trade_ngin::relative_value {

/// The root of a leg symbol as `metadata.contract_metadata` names it: "ZS.v.0" -> "ZS", the way
/// the instrument registry maps a continuous symbol.
std::string contract_root(const std::string& symbol);

/// A contract lookup over a `metadata.contract_metadata` table ("Databento Symbol" and
/// "Contract Size" columns). A symbol with no row gives nullopt; a row whose size is null or
/// empty gives an empty size, so the validator can tell "no row" from "no size".
ValidationContext::ContractLookup make_contract_lookup(const std::shared_ptr<arrow::Table>& metadata);

/// The same, reading the table through `PostgresDatabase::get_contract_metadata()` (virtual, so a
/// test can supply its own).
Result<ValidationContext::ContractLookup> make_contract_lookup(const PostgresDatabase& db);

}  // namespace trade_ngin::relative_value
