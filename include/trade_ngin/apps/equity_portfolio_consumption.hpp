#pragma once
#include <nlohmann/json.hpp>
#include <set>
#include "trade_ngin/portfolio/portfolio_manager.hpp"
namespace trade_ngin {
// Actual invocation-scoped risk/helper and internal cost observations only.
// These charge vectors are not the runner's persisted execution batch.
nlohmann::json project_equity_portfolio_consumption(const PortfolioConsumptionTrace&);
// Composite invocation identity is supplied from the validated source owner map.
nlohmann::json project_equity_portfolio_consumption(const PortfolioConsumptionTrace&,
    const std::set<std::string>& strategy_owners);
}
