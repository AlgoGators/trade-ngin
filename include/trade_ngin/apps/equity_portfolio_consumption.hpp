#pragma once
#include <nlohmann/json.hpp>
#include "trade_ngin/portfolio/portfolio_manager.hpp"
namespace trade_ngin {
// Actual invocation-scoped risk/helper and internal cost observations only.
// These charge vectors are not the runner's persisted execution batch.
nlohmann::json project_equity_portfolio_consumption(const PortfolioConsumptionTrace&);
}
