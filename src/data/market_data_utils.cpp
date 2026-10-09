// src/data/market_data_utils.cpp

#include "trade_ngin/data/market_data_utils.hpp"

#include <cmath>
#include <sstream>

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

namespace {

std::string futures_window_predicate(bool with_symbol_filter) {
    std::string where = " WHERE time BETWEEN $1 AND $2";
    if (with_symbol_filter) {
        where += " AND symbol = ANY($3)";
    }
    return where;
}

std::string format_number(double v) {
    std::ostringstream os;
    os.precision(10);
    os << v;
    return os.str();
}

bool same_values(const FuturesBarCopy& a, const FuturesBarCopy& b) {
    return a.open == b.open && a.high == b.high && a.low == b.low && a.close == b.close &&
           a.volume == b.volume;
}

}  // namespace

std::string build_futures_bar_query(const std::string& full_table_name, bool with_symbol_filter) {
    const std::string cols = get_market_data_columns(AssetClass::FUTURES);
    return "SELECT " + cols + " FROM ("
           "SELECT DISTINCT ON (symbol, time) " + cols +
           " FROM " + full_table_name + futures_window_predicate(with_symbol_filter) +
           " ORDER BY symbol, time, " + kFuturesBarKeepOrder +
           ") AS one_bar_per_symbol_date ORDER BY time, symbol";
}

std::string build_futures_instrument_id_query(const std::string& full_table_name,
                                              bool with_symbol_filter) {
    return "SELECT k.symbol, k.time, r.instrument_id FROM ("
           "SELECT DISTINCT ON (symbol, time) symbol, time, open, high, low, close, volume"
           " FROM " + full_table_name + futures_window_predicate(with_symbol_filter) +
           " ORDER BY symbol, time, " + kFuturesBarKeepOrder +
           ") AS k JOIN " + kFuturesRawBarTable +
           " AS r ON r.symbol = k.symbol AND r.ts_event = k.time AND r.volume = k.volume"
           " AND r.open = k.open AND r.high = k.high AND r.low = k.low AND r.close = k.close"
           " ORDER BY k.symbol, k.time";
}

std::string build_futures_duplicate_copies_query(const std::string& full_table_name,
                                                 bool with_symbol_filter) {
    const std::string cols = get_market_data_columns(AssetClass::FUTURES);
    return "SELECT " + cols + " FROM ("
           "SELECT " + cols + ", count(*) OVER (PARTITION BY symbol, time) AS copies"
           " FROM " + full_table_name + futures_window_predicate(with_symbol_filter) +
           ") AS every_copy WHERE copies > 1 ORDER BY time, symbol, " + kFuturesBarKeepOrder;
}

FuturesBarDedupReport summarise_futures_bar_duplicates(
    const std::vector<FuturesBarCopy>& copies) {
    FuturesBarDedupReport report;
    size_t i = 0;
    while (i < copies.size()) {
        size_t j = i + 1;
        while (j < copies.size() && copies[j].symbol == copies[i].symbol &&
               copies[j].date == copies[i].date) {
            ++j;
        }
        const size_t n = j - i;
        if (n > 1) {
            report.rows_dropped += n - 1;
            report.symbol_dates += 1;
            const FuturesBarCopy& kept = copies[i];
            std::vector<const FuturesBarCopy*> differing;
            for (size_t k = i + 1; k < j; ++k) {
                if (same_values(copies[k], kept)) continue;
                bool seen = false;
                for (const auto* d : differing) {
                    if (same_values(*d, copies[k])) {
                        seen = true;
                        break;
                    }
                }
                if (!seen) differing.push_back(&copies[k]);
            }
            if (!differing.empty()) {
                std::string line = "FUTURES_BAR_DEDUP_CONFLICT symbol=" + kept.symbol +
                                   " date=" + kept.date + " copies=" + std::to_string(n) +
                                   " kept close=" + format_number(kept.close) +
                                   " volume=" + format_number(kept.volume);
                for (const auto* d : differing) {
                    line += " dropped close=" + format_number(d->close) +
                            " volume=" + format_number(d->volume);
                }
                report.conflicts.push_back(line);
            }
        }
        i = j;
    }
    return report;
}

std::string format_futures_bar_dedup_summary(size_t bars_returned,
                                             const FuturesBarDedupReport& report) {
    return "FUTURES_BAR_DEDUP rows_read=" + std::to_string(bars_returned + report.rows_dropped) +
           " rows_dropped=" + std::to_string(report.rows_dropped) +
           " symbol_dates=" + std::to_string(report.symbol_dates) +
           " conflicts=" + std::to_string(report.conflicts.size());
}

}  // namespace trade_ngin::market_data_utils
