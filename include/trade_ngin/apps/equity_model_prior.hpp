#pragma once
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/types.hpp"
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <string>
#include <unordered_map>
#include <vector>

namespace trade_ngin {
enum class EquityModelPriorMode { SystemReference, VerifiedDeskPrior };
struct EquityModelPriorSelection {
    EquityModelPriorMode mode{EquityModelPriorMode::SystemReference};
    std::string decision_id;
    std::string finalization_id;
    // Date/email arguments retain the runner's existing parsing and semantics.
    std::vector<std::string> runner_arguments;
};
struct EquityModelPriorOwner {
    std::string portfolio_id;
    std::string strategy_id;
    std::string strategy_name;
    std::string source_day;
    std::string valuation_day;
};
struct VerifiedEquityModelPrior {
    std::unordered_map<std::string,Position> positions;
    nlohmann::json replay_reference;
    // Original/successor financial records and governed basis frames remain
    // exact evidence. No inference from MODEL numbers, releases or system CA ledger.
    nlohmann::json financial;
    nlohmann::json basis_positions;
};
Result<EquityModelPriorSelection> parse_equity_model_prior_arguments(
    const std::vector<std::string>& arguments);
// Caller owns this short book-fenced transaction. Release it before runner
// compute; central publisher revalidates the captured exact documents in its
// own transaction/book fence before writes. Never hold it through other DB calls.
// Read-only proof admission: no commits, authority changes or MODEL writes.
Result<VerifiedEquityModelPrior> load_verified_equity_model_prior(pqxx::work&,
    const EquityModelPriorSelection&,const EquityModelPriorOwner&);
}
