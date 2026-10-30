// include/trade_ngin/relative_value/definition_validator.hpp
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "trade_ngin/relative_value/analysis_registry.hpp"
#include "trade_ngin/relative_value/trade_definition.hpp"

namespace trade_ngin::relative_value {

/// What the database knows about a leg symbol: the text of its "Contract Size" metadata.
struct ContractInfo {
    std::string contract_size;
};

/// The outside world the validator asks about. Everything is optional, so a test (or a caller
/// without a database) supplies only what it wants checked; a missing lookup skips its checks.
struct ValidationContext {
    const AnalysisRegistry* registry{nullptr};

    /// nullopt: the symbol has no metadata row (so it is not in the instrument registry). A row with
    /// an empty contract size is a leg the engine would silently size with 1.0.
    using ContractLookup = std::function<std::optional<ContractInfo>(const std::string& symbol)>;
    ContractLookup contract_lookup;

    /// Does `macro_data`-style table.column exist? (a `series` input)
    std::function<bool(const std::string& table, const std::string& key)> series_exists;

    /// The first date (ISO) on which every leg and input of the definition has data.
    std::function<std::optional<std::string>(const TradeDefinition&)> first_data_date;

    /// The other definitions in the same set: ids must be unique, and a `trade` input names one.
    std::vector<const TradeDefinition*> others;
};

/// The width of the `symbol` column of `analysis.signals`, which holds every key a definition
/// produces (section 9.2).
constexpr size_t kSignalKeyWidth = 50;
constexpr size_t kSignalNameWidth = 30;

/// Check one definition (section 6.1). An empty result means the definition is valid.
std::vector<ValidationIssue> validate_trade_definition(const TradeDefinition& definition,
                                                       const ValidationContext& context);

/// Check a set of definitions: each one, plus unique ids and no cycles among `trade` inputs.
/// Issue paths are prefixed with the definition id.
std::vector<ValidationIssue> validate_definition_set(const std::vector<TradeDefinition>& definitions,
                                                     const ValidationContext& context);

/// Every key the definition writes to `analysis.signals.symbol` (section 9.2), for the width check
/// and the catalog.
std::vector<std::string> signal_keys(const TradeDefinition& definition, const AnalysisRegistry& registry);

}  // namespace trade_ngin::relative_value
