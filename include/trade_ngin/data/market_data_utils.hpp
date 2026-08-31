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

}  // namespace trade_ngin::market_data_utils
