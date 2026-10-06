#pragma once
#include <nlohmann/json.hpp>
namespace trade_ngin {
struct PortfolioConsumptionTrace;
// Closed invocation-only envelope; never certifies the legacy full run or netted costs.
nlohmann::json project_equity_multi_consumption(const PortfolioConsumptionTrace&, bool non_trading,
    const nlohmann::json& snapshot,const std::string& engine,const std::string& book,
    const std::string& date,const std::string& effective_sha256);
bool validate_equity_multi_consumption(const nlohmann::json&,const nlohmann::json& snapshot,
    const std::string& engine,const std::string& book,const std::string& date,
    const std::string& effective_sha256);
}
