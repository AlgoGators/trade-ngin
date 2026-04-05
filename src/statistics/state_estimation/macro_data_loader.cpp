#include "trade_ngin/statistics/state_estimation/macro_data_loader.hpp"
#include "trade_ngin/core/logger.hpp"

#include <cmath>
#include <limits>
#include <sstream>

namespace trade_ngin {
namespace statistics {

MacroDataLoader::MacroDataLoader(PostgresDatabase& db)
    : db_(db) {}

// ============================================================================
// build_panel_query()
//
// Full-outer-joins all 6 macro_data tables on date, producing one row per
// date with every indicator as a column. COALESCE picks the first non-null
// date across tables so no rows are lost.
// ============================================================================

std::string MacroDataLoader::build_panel_query(const std::string& start_date,
                                                const std::string& end_date) const
{
    // We use a series of LEFT JOINs starting from a union of all dates,
    // which is equivalent to a full outer join but simpler in pqxx.

    std::ostringstream sql;
    sql << R"(
WITH all_dates AS (
    SELECT date FROM macro_data.inflation
    UNION
    SELECT date FROM macro_data.growth
    UNION
    SELECT date FROM macro_data.yield_curve
    UNION
    SELECT date FROM macro_data.credit_spreads
    UNION
    SELECT date FROM macro_data.liquidity
    UNION
    SELECT date FROM macro_data.market
)
SELECT
    d.date,
    -- growth (6 cols)
    g.nonfarm_payrolls,
    g.unemployment_rate,
    g.manufacturing_capacity_util,
    g.industrial_production,
    g.retail_sales,
    g.gdp,
    -- inflation (4 cols)
    i.cpi,
    i.core_cpi,
    i.core_pce,
    i.breakeven_5y,
    -- yield_curve (4 cols)
    yc.treasury_2y,
    yc.treasury_10y,
    yc.yield_spread_10y_2y,
    yc.fed_funds_rate,
    -- credit_spreads (2 cols)
    cs.ig_credit_spread,
    cs.high_yield_spread,
    -- liquidity (3 cols)
    lq.m2_money_supply,
    lq.ted_spread,
    lq.fed_balance_sheet,
    -- market (5 cols)
    mk.vix,
    mk.dxy,
    mk.tips_10y,
    mk.wti_crude,
    mk.gdp_nowcast
FROM all_dates d
LEFT JOIN macro_data.growth         g  ON g.date  = d.date
LEFT JOIN macro_data.inflation      i  ON i.date  = d.date
LEFT JOIN macro_data.yield_curve    yc ON yc.date = d.date
LEFT JOIN macro_data.credit_spreads cs ON cs.date = d.date
LEFT JOIN macro_data.liquidity      lq ON lq.date = d.date
LEFT JOIN macro_data.market         mk ON mk.date = d.date
)";

    // Optional date filters
    bool has_where = false;
    if (!start_date.empty()) {
        sql << "WHERE d.date >= '" << start_date << "' ";
        has_where = true;
    }
    if (!end_date.empty()) {
        sql << (has_where ? "AND " : "WHERE ") << "d.date <= '" << end_date << "' ";
    }

    sql << "ORDER BY d.date ASC";
    return sql.str();
}

// ============================================================================
// load()  — full panel
// ============================================================================

Result<MacroPanel> MacroDataLoader::load(const std::string& start_date,
                                          const std::string& end_date) const
{
    if (!db_.is_connected()) {
        return make_error<MacroPanel>(ErrorCode::CONNECTION_ERROR,
            "Database not connected", "MacroDataLoader");
    }

    std::string sql = build_panel_query(start_date, end_date);
    DEBUG("[MacroDataLoader] executing panel query");

    auto result = db_.execute_query(sql);
    if (result.is_error()) {
        return make_error<MacroPanel>(ErrorCode::DATABASE_ERROR,
            "Query failed: " + std::string(result.error()->what()),
            "MacroDataLoader");
    }

    auto table = result.value();
    const int64_t num_rows = table->num_rows();
    // First column is date, rest are indicators
    const int num_cols = table->num_columns() - 1;

    if (num_rows == 0) {
        return make_error<MacroPanel>(ErrorCode::INVALID_ARGUMENT,
            "No macro data returned from query", "MacroDataLoader");
    }

    MacroPanel panel;
    panel.T = static_cast<int>(num_rows);
    panel.N = num_cols;
    panel.data = Eigen::MatrixXd::Constant(panel.T, panel.N,
        std::numeric_limits<double>::quiet_NaN());

    // Extract column names (skip column 0 = "date")
    for (int c = 1; c <= num_cols; ++c) {
        panel.column_names.push_back(table->field(c)->name());
    }

    // Extract dates from column 0
    auto date_col = table->column(0);
    auto date_chunks = date_col->chunks();
    int row_idx = 0;
    for (const auto& chunk : date_chunks) {
        // Dates come back as string or date32 — handle both
        if (auto str_arr = std::dynamic_pointer_cast<arrow::StringArray>(chunk)) {
            for (int64_t i = 0; i < str_arr->length(); ++i) {
                panel.dates.push_back(str_arr->GetString(i));
                ++row_idx;
            }
        } else if (auto date_arr = std::dynamic_pointer_cast<arrow::Date32Array>(chunk)) {
            for (int64_t i = 0; i < date_arr->length(); ++i) {
                if (date_arr->IsNull(i)) {
                    panel.dates.push_back("");
                } else {
                    // Date32 stores days since epoch
                    int32_t days = date_arr->Value(i);
                    // Convert to YYYY-MM-DD
                    std::time_t epoch_seconds = static_cast<std::time_t>(days) * 86400;
                    std::tm* tm = std::gmtime(&epoch_seconds);
                    char buf[11];
                    std::strftime(buf, sizeof(buf), "%Y-%m-%d", tm);
                    panel.dates.push_back(buf);
                }
                ++row_idx;
            }
        } else {
            // Fallback: try to get as string via ToString
            for (int64_t i = 0; i < chunk->length(); ++i) {
                auto scalar = chunk->GetScalar(i);
                panel.dates.push_back(scalar.ok() ? scalar.ValueOrDie()->ToString() : "");
                ++row_idx;
            }
        }
    }

    // Extract numeric data for each indicator column
    for (int c = 0; c < num_cols; ++c) {
        auto col = table->column(c + 1);  // skip date column
        row_idx = 0;
        for (const auto& chunk : col->chunks()) {
            if (auto dbl_arr = std::dynamic_pointer_cast<arrow::DoubleArray>(chunk)) {
                for (int64_t i = 0; i < dbl_arr->length(); ++i) {
                    if (!dbl_arr->IsNull(i)) {
                        panel.data(row_idx, c) = dbl_arr->Value(i);
                    }
                    // else stays NaN
                    ++row_idx;
                }
            } else if (auto flt_arr = std::dynamic_pointer_cast<arrow::FloatArray>(chunk)) {
                for (int64_t i = 0; i < flt_arr->length(); ++i) {
                    if (!flt_arr->IsNull(i)) {
                        panel.data(row_idx, c) = static_cast<double>(flt_arr->Value(i));
                    }
                    ++row_idx;
                }
            } else if (auto int_arr = std::dynamic_pointer_cast<arrow::Int64Array>(chunk)) {
                for (int64_t i = 0; i < int_arr->length(); ++i) {
                    if (!int_arr->IsNull(i)) {
                        panel.data(row_idx, c) = static_cast<double>(int_arr->Value(i));
                    }
                    ++row_idx;
                }
            } else if (auto str_arr = std::dynamic_pointer_cast<arrow::StringArray>(chunk)) {
                // convert_generic_to_arrow stores everything as strings —
                // parse them to doubles here
                for (int64_t i = 0; i < str_arr->length(); ++i) {
                    if (!str_arr->IsNull(i)) {
                        std::string val = str_arr->GetString(i);
                        if (!val.empty()) {
                            try {
                                panel.data(row_idx, c) = std::stod(val);
                            } catch (...) {
                                // unparseable — stays NaN
                            }
                        }
                    }
                    ++row_idx;
                }
            } else {
                // Truly unknown type — leave as NaN
                row_idx += chunk->length();
            }
        }
    }

    // Drop columns that are entirely NaN (e.g. wti_crude, gdp_nowcast
    // when the DB hasn't been populated yet)
    std::vector<int> keep_cols;
    for (int c = 0; c < panel.N; ++c) {
        bool all_nan = true;
        for (int t = 0; t < panel.T; ++t) {
            if (std::isfinite(panel.data(t, c))) { all_nan = false; break; }
        }
        if (!all_nan) {
            keep_cols.push_back(c);
        } else {
            WARN("[MacroDataLoader] dropping all-NaN column: " << panel.column_names[c]);
        }
    }

    if (static_cast<int>(keep_cols.size()) < panel.N) {
        Eigen::MatrixXd trimmed(panel.T, static_cast<int>(keep_cols.size()));
        std::vector<std::string> trimmed_names;
        for (int i = 0; i < static_cast<int>(keep_cols.size()); ++i) {
            trimmed.col(i) = panel.data.col(keep_cols[i]);
            trimmed_names.push_back(panel.column_names[keep_cols[i]]);
        }
        panel.data = std::move(trimmed);
        panel.column_names = std::move(trimmed_names);
        panel.N = static_cast<int>(keep_cols.size());
    }

    // Forward-fill: carry last known value forward for each column.
    // Macro indicators release at different frequencies (daily/monthly/quarterly),
    // so between releases the "current" value is the last reported one.
    for (int c = 0; c < panel.N; ++c) {
        double last_valid = std::numeric_limits<double>::quiet_NaN();
        for (int t = 0; t < panel.T; ++t) {
            if (std::isfinite(panel.data(t, c))) {
                last_valid = panel.data(t, c);
            } else if (std::isfinite(last_valid)) {
                panel.data(t, c) = last_valid;
            }
            // else: still NaN (no observation yet for this series)
        }
    }

    // Count non-NaN entries for diagnostics
    int non_nan = 0;
    for (int t = 0; t < panel.T; ++t)
        for (int c = 0; c < panel.N; ++c)
            if (std::isfinite(panel.data(t, c))) ++non_nan;

    double fill_rate = 100.0 * non_nan / (panel.T * panel.N);
    INFO("[MacroDataLoader] loaded panel: T=" << panel.T
         << " N=" << panel.N
         << " fill_rate=" << fill_rate << "%");

    return Result<MacroPanel>(std::move(panel));
}

// ============================================================================
// load_single()  — one observation for online update
// ============================================================================

Result<std::pair<Eigen::VectorXd, std::vector<std::string>>>
MacroDataLoader::load_single(const std::string& date) const
{
    // Reuse the panel query with a single-date filter
    auto panel_result = load(date, date);
    if (panel_result.is_error()) {
        return make_error<std::pair<Eigen::VectorXd, std::vector<std::string>>>(
            panel_result.error()->code(),
            std::string(panel_result.error()->what()),
            "MacroDataLoader");
    }

    auto& panel = panel_result.value();
    if (panel.T == 0) {
        return make_error<std::pair<Eigen::VectorXd, std::vector<std::string>>>(
            ErrorCode::INVALID_ARGUMENT,
            "No data for date " + date, "MacroDataLoader");
    }

    // Return first (and only) row
    Eigen::VectorXd row = panel.data.row(0).transpose();
    return Result<std::pair<Eigen::VectorXd, std::vector<std::string>>>(
        {std::move(row), std::move(panel.column_names)});
}

} // namespace statistics
} // namespace trade_ngin
