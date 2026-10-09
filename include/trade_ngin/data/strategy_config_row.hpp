// include/trade_ngin/data/strategy_config_row.hpp
#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace trade_ngin {

/**
 * @brief One trading.strategy_config row (migration 022, QT plan E2): a desk change to a
 *        portfolio's settings. `overrides` is deep-merged over the portfolio's config files at
 *        the start of a live run (ConfigLoader::load with an overlay).
 */
struct StrategyConfigRow {
    std::string portfolio_id;
    int version{0};
    nlohmann::json overrides;
    std::string reason;
    std::string created_by;
};

}  // namespace trade_ngin
