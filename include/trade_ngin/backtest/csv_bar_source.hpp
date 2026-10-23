// include/trade_ngin/backtest/csv_bar_source.hpp
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/types.hpp"

/**
 * @brief Backtest-only bar source: {dir}/{SYMBOL}.csv with the equity data contract.
 *
 * File contract (one file per symbol, header required, dates ascending):
 *
 *     date,open,high,low,close,volume,div_cash,split_factor
 *
 *   date          YYYY-MM-DD session date, read as UTC midnight (core::parse_utc_date)
 *   open..close   RAW exchange prices; volume raw shares
 *   div_cash      cash dividend going ex on that bar, 0 if none
 *   split_factor  new/old shares taking effect on that bar, 1 if none (Tiingo orientation;
 *                 the recurrence divides by it)
 *
 * No vendor-adjusted column is accepted (adj_close / adjusted_close / closeadj make the
 * file invalid): docs/DATA_SOURCES_OF_TRUTH.md §6 rule 1. Adjustment is computed here with
 * market_data_utils::compute_backward_adjustment_factors, the C++ mirror of the SQL the
 * equities_data.ohlcv_1d path uses, anchored at 1 on the last bar <= the requested end so
 * a CSV run and a database run of the same window are on the same price scale.
 *
 * This header lives in trade_ngin::backtest and is included only by the backtest data
 * loader; the live apps never build a DataLoadConfig and cannot name DataSource::CSV.
 */
namespace trade_ngin::backtest {

struct CsvBarRow {
    Timestamp time;
    double open{0.0};
    double high{0.0};
    double low{0.0};
    double close{0.0};
    double volume{0.0};
    double div_cash{0.0};
    double split_factor{1.0};
};

/// Parse and validate a whole file. Never adjusts. Errors: DATA_NOT_FOUND (missing file),
/// INVALID_DATA (bad header, vendor-adjusted column, unsorted/duplicate dates, non-positive
/// close or split_factor, negative div_cash, unparseable row).
Result<std::vector<CsvBarRow>> read_csv_bar_file(const std::filesystem::path& file);

/// Keep rows with time <= end, compute the backward factors over that prefix (anchor = last
/// kept bar, factor 1), scale OHLC (volume stays raw), THEN drop rows with time < start.
/// DATA_NOT_FOUND when the window is empty. Bars come back in ascending time order.
Result<std::vector<Bar>> load_csv_bars(const std::string& symbol,
                                       const std::filesystem::path& dir,
                                       const Timestamp& start, const Timestamp& end);

/// Path convention shared by every caller: {dir}/{SYMBOL}.csv
std::filesystem::path csv_bar_path(const std::filesystem::path& dir, const std::string& symbol);

}  // namespace trade_ngin::backtest
