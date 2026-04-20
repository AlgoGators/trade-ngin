// include/trade_ngin/data/csv_equity_loader.hpp
#pragma once

#include <string>
#include <vector>
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/types.hpp"

namespace trade_ngin {

/**
 * @brief Loads daily equity bars from a local CSV file, used as a fallback
 *        when the Postgres equity tables don't carry a given ticker.
 *
 * CSV format (header + rows):
 *   date,open,high,low,close,adj_close,volume
 *
 *   date       YYYY-MM-DD
 *   open/high/low/close  raw (unadjusted) prices
 *   adj_close  split/dividend-adjusted close
 *   volume     integer shares
 *
 * The loader applies the adjustment ratio (adj_close / close) to OHL so returned
 * bars match the semantics of equities_data.ohlcv_1d loaded via the SQL path.
 *
 * Files are expected at {dir}/{TICKER}.csv (e.g., data/equity_bars/QQQ.csv),
 * populated by scripts/seed_etfs_yfinance.py.
 */
class CSVEquityLoader {
public:
    /**
     * @brief Load bars for a ticker within [start, end] (inclusive on both ends).
     *        Dates are compared at daily granularity.
     * @param ticker   Upper-case ticker symbol, used to resolve the filename.
     * @param start    Earliest bar timestamp to include.
     * @param end      Latest bar timestamp to include.
     * @param dir      Directory containing {TICKER}.csv files. Defaults to "data/equity_bars".
     * @return Vector of Bars sorted by timestamp ascending, or error.
     */
    static Result<std::vector<Bar>> load(const std::string& ticker,
                                         const Timestamp& start,
                                         const Timestamp& end,
                                         const std::string& dir = "data/equity_bars");
};

}  // namespace trade_ngin
