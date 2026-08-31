// src/data/market_data_utils.cpp

#include "trade_ngin/data/market_data_utils.hpp"

#include <cmath>

namespace trade_ngin::market_data_utils {

std::string get_market_data_columns(AssetClass asset_class) {
    using enum AssetClass;
    switch (asset_class) {
        case EQUITIES:
            // Per-bar-native: raw prices plus the corporate-action primitives.
            // The adjustment itself is computed in build_equity_adjusted_query()
            // (and mirrored in compute_backward_adjustment_factors()); the
            // vendor's derived adj_* columns are intentionally not read.
            return "time, symbol, open, high, low, close, volume, div_cash, split_factor";

        case FUTURES:
        case FIXED_INCOME:
        case CURRENCIES:
        case COMMODITIES:
        case CRYPTO:
            // These asset classes use unadjusted prices directly.
            // Futures are already back-adjusted per contract conventions.
            // Other asset classes do not have adjustment concepts (splits, dividends).
            return "time, symbol, open, high, low, close, volume";

        default:
            // Defensive: unknown asset classes default to unadjusted.
            return "time, symbol, open, high, low, close, volume";
    }
}

std::string build_equity_adjusted_query(const std::string& full_table_name,
                                        bool with_symbol_filter) {
    // Backward cumulative product via EXP(SUM(LN(step))): each bar's step is
    //   ln(close / (close + div_cash)) - ln(split_factor)
    // computed from that bar's OWN values; a bar's factor is the product of the
    // steps of every LATER bar (ROWS BETWEEN 1 FOLLOWING AND UNBOUNDED
    // FOLLOWING), so the newest bar's factor is exactly 1.
    std::string query =
        "WITH raw AS ("
        "SELECT " + get_market_data_columns(AssetClass::EQUITIES) +
        " FROM " + full_table_name +
        " WHERE time BETWEEN $1 AND $2";
    if (with_symbol_filter) {
        query += " AND symbol = ANY($3)";
    }
    query +=
        "), fac AS ("
        "SELECT time, symbol, open, high, low, close, volume, "
        "EXP(COALESCE(SUM("
        "CASE WHEN close > 0 THEN "
        "LN(close / (close + COALESCE(div_cash, 0))) - "
        "LN(COALESCE(NULLIF(split_factor, 0), 1.0)) "
        "ELSE 0 END"
        ") OVER (PARTITION BY symbol ORDER BY time "
        "ROWS BETWEEN 1 FOLLOWING AND UNBOUNDED FOLLOWING), 0)) AS f "
        "FROM raw) "
        "SELECT time, symbol, "
        "open * f AS open, high * f AS high, low * f AS low, close * f AS close, "
        "volume "
        "FROM fac ORDER BY time, symbol";
    return query;
}

std::vector<double> compute_backward_adjustment_factors(
    const std::vector<AdjustmentBar>& bars) {
    std::vector<double> factors(bars.size(), 1.0);
    if (bars.empty()) {
        return factors;
    }
    for (size_t idx = bars.size() - 1; idx-- > 0;) {
        const auto& next = bars[idx + 1];
        double step = 1.0;
        if (next.close > 0.0) {
            double split = (next.split_factor > 0.0) ? next.split_factor : 1.0;
            step = (next.close / (next.close + next.div_cash)) / split;
        }
        factors[idx] = factors[idx + 1] * step;
    }
    return factors;
}

}  // namespace trade_ngin::market_data_utils
