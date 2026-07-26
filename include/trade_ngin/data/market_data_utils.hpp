// include/trade_ngin/data/market_data_utils.hpp

#pragma once

#include <string>
#include "trade_ngin/core/types.hpp"

namespace trade_ngin {

/**
 * @brief Pure utility for generating market data column lists
 *
 * This module provides testable column selection logic for querying market data.
 * Different asset classes require different columns:
 * - EQUITIES: uses adjusted columns (adj_open, adj_high, adj_low, adjusted_close, adj_volume)
 *   aliased to plain names (open, high, low, close, volume) for downstream transparency.
 *   This ensures splits and dividends are properly reflected in backtest returns.
 *
 * - All other asset classes (FUTURES, etc.): use unadjusted columns directly.
 *   Futures prices in this system are already back-adjusted per contract conventions,
 *   and adjustment concepts do not apply to other asset types.
 *
 * ADR-000 C-2 Contract Reference:
 * Both the column names and the table schema structure are defined by the
 * C-2 data contract. See data-ngin#39 for the mapping of live table shape
 * (currently Sharadar) to contract names. This code targets the contract,
 * not the current live schema, so failures will be loud (column does not exist)
 * rather than silent (wrong prices).
 */
namespace market_data_utils {

/**
 * @brief Generate the SELECT column list for market data queries
 *
 * Returns a SQL fragment for use in SELECT statements. Columns are selected
 * according to asset class and aliased to standard names where needed.
 *
 * @param asset_class The asset class determining column selection
 * @return SQL column list fragment (e.g., "time, symbol, open, high, low, close, volume")
 */
std::string get_market_data_columns(AssetClass asset_class);

}  // namespace market_data_utils

}  // namespace trade_ngin
