// include/trade_ngin/data/market_data_utils.hpp

#pragma once

#include <string>
#include <vector>
#include "trade_ngin/core/types.hpp"

/**
 * @brief Pure utilities for market data queries and equity price adjustment
 *
 * This module provides testable column selection and adjustment logic:
 * - EQUITIES: PER-BAR-NATIVE adjustment. The loader reads RAW prices plus the
 *   per-bar corporate-action primitives (div_cash, split_factor) and computes
 *   the backward cumulative adjustment itself. The vendor's derived adj_*
 *   columns are deliberately NOT read: their refresh job can stall (it did on
 *   2026-08-06, leaving them stale), while the per-bar primitives are written
 *   once on the event date and never need restating. Computing from primitives
 *   reproduces the vendor's adjusted series exactly (validated to <1e-6
 *   relative against adjusted_close over clean windows) without depending on
 *   the vendor's restating pipeline.
 *
 * - All other asset classes (FUTURES, etc.): use unadjusted columns directly.
 *   Futures prices in this system are already back-adjusted per contract
 *   conventions, and adjustment concepts do not apply to other asset types.
 *
 * Adjustment convention (Tiingo, empirically validated on AAPL dividends and
 * GE spinoffs): walking backward from the newest bar (factor = 1),
 *
 *     f_i = f_{i+1} * ( close_{i+1} / (close_{i+1} + div_{i+1}) ) / split_{i+1}
 *
 * where close is the RAW close, div_cash the cash dividend going ex on that
 * bar, and split_factor the split ratio taking effect on that bar. Adjusted
 * price = raw price * f. The factor anchors at 1 on the last bar of the
 * queried window, so recent prices equal actual traded prices; returns are
 * anchor-invariant.
 *
 * Namespace: trade_ngin::market_data_utils
 */
namespace trade_ngin::market_data_utils {

/**
 * @brief Generate the SELECT column list for market data queries
 *
 * Returns a SQL fragment for use in SELECT statements. For EQUITIES this is
 * the raw per-bar list (including div_cash and split_factor) consumed by the
 * adjustment CTE in build_equity_adjusted_query(); for all other classes it is
 * the plain unadjusted list used directly.
 *
 * @param asset_class The asset class determining column selection
 * @return SQL column list fragment (e.g., "time, symbol, open, high, low, close, volume")
 */
std::string get_market_data_columns(AssetClass asset_class);

/**
 * @brief Build the full equity market-data query with per-bar backward adjustment
 *
 * Produces a CTE query that reads raw OHLCV + div_cash/split_factor and emits
 * time, symbol, open, high, low, close, volume with the OHLC scaled by the
 * backward cumulative adjustment factor (volume stays raw, matching the prior
 * closeadj-based contract). Timestamps bind as $1/$2; when with_symbol_filter
 * is true the symbol list binds as $3.
 *
 * @param full_table_name Validated schema-qualified table name
 * @param with_symbol_filter Whether to include the AND symbol = ANY($3) clause
 */
std::string build_equity_adjusted_query(const std::string& full_table_name,
                                        bool with_symbol_filter);

/**
 * @brief One bar's inputs to the backward adjustment recursion
 */
struct AdjustmentBar {
    double close{0.0};         ///< RAW close of this bar
    double div_cash{0.0};      ///< cash dividend going ex on this bar (0 if none)
    double split_factor{1.0};  ///< split ratio taking effect on this bar (1 if none)
};

/**
 * @brief Compute backward cumulative adjustment factors for a chronological bar series
 *
 * Pure C++ mirror of the SQL recursion in build_equity_adjusted_query(), used
 * for unit-test fixtures and by cash-accounting paths that need the factors
 * directly. bars must be in ascending time order; the returned vector has one
 * factor per bar, with the last bar's factor = 1. Degenerate inputs
 * (close <= 0, split_factor <= 0) contribute a neutral step.
 */
std::vector<double> compute_backward_adjustment_factors(
    const std::vector<AdjustmentBar>& bars);

/**
 * @brief Which copy of a futures bar is kept when a (symbol, time) is stored more than once
 *
 * futures_data.ohlcv_1d has no key, and holds 970 extra rows in 306 symbol-dates (all between
 * 2025-10-06 and 2026-02-05). The loader keeps ONE row per (symbol, time): the one first in
 * this order, i.e. the highest volume, then the lowest close, open, high, low. Identical
 * copies (305 of the 306 groups) make the tie-break irrelevant; it only makes the pick
 * independent of physical row order. The single disagreeing group (6A.v.0 2025-11-05) keeps
 * the 76,895-volume front-contract bar over the 196-volume wrong-instrument one.
 */
inline constexpr const char* kFuturesBarKeepOrder = "volume DESC, close, open, high, low";

/**
 * @brief The futures bar query: one row per (symbol, time), returned ORDER BY time, symbol
 *
 * DISTINCT ON (symbol, time) with kFuturesBarKeepOrder, wrapped so the rows come back in the
 * loader's historical order (time, symbol). A series with no repeated (symbol, time) comes
 * back exactly as the plain query returned it. Timestamps bind as $1/$2; with
 * with_symbol_filter the symbol list binds as $3.
 */
std::string build_futures_bar_query(const std::string& full_table_name, bool with_symbol_filter);

/**
 * @brief Companion query: every copy of every repeated (symbol, time) in the same window
 *
 * Rows ORDER BY time, symbol, then kFuturesBarKeepOrder, so the first row of each group is
 * the copy build_futures_bar_query() keeps. Same parameters as build_futures_bar_query().
 */
std::string build_futures_duplicate_copies_query(const std::string& full_table_name,
                                                 bool with_symbol_filter);

/**
 * @brief The vendor's raw futures table: one row per (symbol, ts_event), with instrument_id
 *
 * futures_data.ohlcv_1d (the loader's table) has no instrument_id column. The raw table holds
 * one row per (symbol, day) and is the same print as the loader's kept copy on all but three
 * rows of the stage-3 clone (6A.v.0 2025-11-05, where it holds the 196-lot copy the loader
 * drops, and ZM.v.0 / ZR.v.0 2025-10-10, where it holds a traded bar the loader's table does
 * not); build_futures_instrument_id_query() therefore reads an id only for the same print.
 */
inline constexpr const char* kFuturesRawBarTable = "futures_data.ohlcv_1d_raw";

/**
 * @brief The instrument id of each kept futures bar (T-7b-2 C10a, the classifier's instrument-id
 *        continuity limb): rows (symbol, time, instrument_id) ORDER BY symbol, time
 *
 * The kept copy is build_futures_bar_query()'s (DISTINCT ON (symbol, time) with
 * kFuturesBarKeepOrder, same window predicate); it is joined to kFuturesRawBarTable on
 * (symbol, time = ts_event) AND all five of volume, open, high, low, close, so a bar gets the id
 * of its own print or none. Same parameters as build_futures_bar_query().
 */
std::string build_futures_instrument_id_query(const std::string& full_table_name,
                                              bool with_symbol_filter);

/**
 * @brief One kept futures bar's vendor instrument id (build_futures_instrument_id_query)
 */
struct FuturesInstrumentId {
    std::string symbol;
    std::string date;  ///< the bar's UTC date, YYYY-MM-DD
    std::string instrument_id;
};

/**
 * @brief One stored copy of a futures bar, as read by the companion query
 */
struct FuturesBarCopy {
    std::string symbol;
    std::string date;  ///< the bar's UTC date, YYYY-MM-DD
    double open{0.0};
    double high{0.0};
    double low{0.0};
    double close{0.0};
    double volume{0.0};
};

/**
 * @brief What the one-bar-per-symbol-date rule dropped from one load
 */
struct FuturesBarDedupReport {
    size_t rows_dropped{0};              ///< copies beyond the first, summed over groups
    size_t symbol_dates{0};              ///< (symbol, date) groups with more than one copy
    std::vector<std::string> conflicts;  ///< one line per group whose copies disagree
};

/**
 * @brief Summarise the companion query's rows
 *
 * copies must be in the companion query's order: groups adjacent, the kept copy first. A
 * group whose copies are not all identical in (open, high, low, close, volume) yields one
 * line: "FUTURES_BAR_DEDUP_CONFLICT symbol=<s> date=<d> copies=<n> kept close=<c> volume=<v>"
 * followed by " dropped close=<c> volume=<v>" for each distinct dropped copy that differs
 * from the kept one. Numbers print with 10 significant digits.
 */
FuturesBarDedupReport summarise_futures_bar_duplicates(const std::vector<FuturesBarCopy>& copies);

/**
 * @brief The once-per-load summary line
 *
 * "FUTURES_BAR_DEDUP rows_read=<bars_returned + rows_dropped> rows_dropped=<n>
 * symbol_dates=<k> conflicts=<c>"
 */
std::string format_futures_bar_dedup_summary(size_t bars_returned,
                                             const FuturesBarDedupReport& report);

}  // namespace trade_ngin::market_data_utils
