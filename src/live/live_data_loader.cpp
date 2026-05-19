// src/live/live_data_loader.cpp
// Implementation of data loading component for live trading
//
// Phase 5 §1.17a + §5c + §5d: numeric columns are decoded via
// DataConversionUtils::safe_get_* (which handles the convert_generic_to_arrow
// utf8 storage convention transparently); date-string keys are produced via
// trade_ngin::core::format_utc_date. Direct static_pointer_cast<DoubleArray>
// + chunk(0) and direct std::gmtime use are forbidden in this file.

#include "trade_ngin/live/live_data_loader.hpp"
#include <arrow/api.h>
#include <iomanip>
#include <sstream>
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/core/types.hpp"  // For Position struct
#include "trade_ngin/data/conversion_utils.hpp"

namespace trade_ngin {

namespace {

// Read a double cell preserving the historical loader semantic of "SQL NULL
// is treated as 0.0" (many queries here aggregate values that legitimately
// have no row; the caller's struct has a separate `exists` flag for "no
// data" -- nulls in the cell are NOT a corruption signal).
//
// Phase 5 §1.17a: type-mismatch errors (CONVERSION_ERROR) and out-of-range
// errors (INVALID_ARGUMENT) are still propagated -- those are the silent
// 0.0 footguns the audit flagged. Only INVALID_DATA from safe_get_*
// (specifically "null cell") gets demoted to 0.0.
Result<double> read_double_or_zero_on_null(
    const std::shared_ptr<arrow::ChunkedArray>& col, int64_t row,
    const std::string& col_name) {
    auto r = DataConversionUtils::safe_get_double(col, row, col_name);
    if (r.is_ok()) return r;
    if (r.error()->code() == ErrorCode::INVALID_DATA) {
        return Result<double>(0.0);
    }
    return r;
}

}  // namespace

LiveDataLoader::LiveDataLoader(std::shared_ptr<PostgresDatabase> db, const std::string& schema)
    : db_(std::move(db)), schema_(schema) {
    if (!db_) {
        throw std::invalid_argument("LiveDataLoader: Database connection cannot be null");
    }

    INFO("LiveDataLoader initialized with schema: " + schema_);
}

Result<void> LiveDataLoader::validate_connection() const {
    if (!db_) {
        return make_error<void>(ErrorCode::DATABASE_ERROR, "Database connection is null",
                                "LiveDataLoader");
    }

    if (!db_->is_connected()) {
        return make_error<void>(ErrorCode::DATABASE_ERROR, "Database is not connected",
                                "LiveDataLoader");
    }

    return Result<void>();
}

bool LiveDataLoader::is_connected() const {
    return db_ && db_->is_connected();
}

// ========== Portfolio Value Methods ==========

Result<double> LiveDataLoader::load_previous_portfolio_value(const std::string& strategy_id,
                                                             const std::string& portfolio_id,
                                                             const Timestamp& date) {
    auto validation = validate_connection();
    if (validation.is_error()) {
        return make_error<double>(ErrorCode::DATABASE_ERROR, validation.error()->what(),
                                  "LiveDataLoader");
    }

    // Convert date to string for SQL query
    // Phase 5 §5c: UTC date-string contract -- format_utc_date is the only
    // approved primitive for date keys; std::gmtime is not thread-safe.
    const std::string date_str = core::format_utc_date(date);

    std::string actual_portfolio_id = portfolio_id.empty() ? "BASE_PORTFOLIO" : portfolio_id;

    std::string query =
        "SELECT COALESCE(current_portfolio_value, 0.0) "
        "FROM " +
        schema_ +
        ".live_results "
        "WHERE strategy_id = '" +
        strategy_id +
        "' "
        "AND portfolio_id = '" +
        actual_portfolio_id +
        "' "
        "AND DATE(date) < '" +
        date_str +
        "' "
        "ORDER BY date DESC LIMIT 1";

    DEBUG("Loading previous portfolio value: " + query);

    auto result = db_->execute_query(query);
    if (result.is_error()) {
        return make_error<double>(
            ErrorCode::DATABASE_ERROR,
            "Failed to load previous portfolio value: " + std::string(result.error()->what()),
            "LiveDataLoader");
    }

    auto table = result.value();
    if (!table || table->num_rows() == 0) {
        DEBUG("No previous portfolio value found for " + strategy_id);
        return Result<double>(0.0);  // Return 0 if no previous data
    }

    auto val_r = DataConversionUtils::safe_get_double(
        table->column(0), 0, "current_portfolio_value");
    if (val_r.is_error()) {
        return make_error<double>(val_r.error()->code(), val_r.error()->what(),
                                  "LiveDataLoader");
    }
    const double value = val_r.value();

    INFO("Loaded previous portfolio value: $" + std::to_string(value));
    return Result<double>(value);
}

Result<double> LiveDataLoader::load_portfolio_value(const std::string& strategy_id,
                                                    const std::string& portfolio_id,
                                                    const Timestamp& date) {
    auto validation = validate_connection();
    if (validation.is_error()) {
        return make_error<double>(ErrorCode::DATABASE_ERROR, validation.error()->what(),
                                  "LiveDataLoader");
    }

    // Phase 5 §5c: UTC date-string contract -- format_utc_date is the only
    // approved primitive for date keys; std::gmtime is not thread-safe.
    const std::string date_str = core::format_utc_date(date);

    std::string actual_portfolio_id = portfolio_id.empty() ? "BASE_PORTFOLIO" : portfolio_id;

    std::string query =
        "SELECT COALESCE(current_portfolio_value, 0.0) "
        "FROM " +
        schema_ +
        ".live_results "
        "WHERE strategy_id = '" +
        strategy_id +
        "' "
        "AND portfolio_id = '" +
        actual_portfolio_id +
        "' "
        "AND DATE(date) = '" +
        date_str + "'";

    DEBUG("Loading portfolio value: " + query);

    auto result = db_->execute_query(query);
    if (result.is_error()) {
        return make_error<double>(
            ErrorCode::DATABASE_ERROR,
            "Failed to load portfolio value: " + std::string(result.error()->what()),
            "LiveDataLoader");
    }

    auto table = result.value();
    if (!table || table->num_rows() == 0) {
        return make_error<double>(ErrorCode::INVALID_ARGUMENT,
                                  "No portfolio value found for date " + date_str,
                                  "LiveDataLoader");
    }

    auto val_r = DataConversionUtils::safe_get_double(
        table->column(0), 0, "current_portfolio_value");
    if (val_r.is_error()) {
        return make_error<double>(val_r.error()->code(), val_r.error()->what(),
                                  "LiveDataLoader");
    }
    return Result<double>(val_r.value());
}

// ========== Live Results Methods ==========

Result<LiveResultsRow> LiveDataLoader::load_live_results(const std::string& strategy_id,
                                                         const std::string& portfolio_id,
                                                         const Timestamp& date) {
    auto validation = validate_connection();
    if (validation.is_error()) {
        return make_error<LiveResultsRow>(ErrorCode::DATABASE_ERROR, validation.error()->what(),
                                          "LiveDataLoader");
    }

    // Phase 5 §5c: UTC date-string contract -- format_utc_date is the only
    // approved primitive for date keys; std::gmtime is not thread-safe.
    const std::string date_str = core::format_utc_date(date);

    std::string actual_portfolio_id = portfolio_id.empty() ? "BASE_PORTFOLIO" : portfolio_id;

    std::string query =
        "SELECT "
        "daily_pnl, total_pnl, daily_realized_pnl, daily_unrealized_pnl, "
        "daily_return, total_cumulative_return, total_annualized_return, current_portfolio_value, "
        "portfolio_leverage, equity_to_margin_ratio, gross_notional, "
        "margin_posted, cash_available, daily_transaction_costs, "
        "sharpe_ratio, sortino_ratio, max_drawdown, volatility, "
        "win_rate, avg_win, avg_loss, profit_factor, best_day, worst_day, downside_deviation, "
        "gross_profit, gross_loss, "
        "active_positions, winning_days, "
        "losing_days, total_days "
        "FROM " +
        schema_ +
        ".live_results "
        "WHERE strategy_id = '" +
        strategy_id +
        "' "
        "AND portfolio_id = '" +
        actual_portfolio_id +
        "' "
        "AND DATE(date) = '" +
        date_str + "'";

    DEBUG("Loading live results: " + query);

    auto result = db_->execute_query(query);
    if (result.is_error()) {
        return make_error<LiveResultsRow>(
            ErrorCode::DATABASE_ERROR,
            "Failed to load live results: " + std::string(result.error()->what()),
            "LiveDataLoader");
    }

    auto table = result.value();
    if (!table || table->num_rows() == 0) {
        return make_error<LiveResultsRow>(ErrorCode::INVALID_ARGUMENT,
                                          "No live results found for date " + date_str,
                                          "LiveDataLoader");
    }

    LiveResultsRow row;
    row.strategy_id = strategy_id;
    row.date = date;

    // Extract all fields from the result.
    //
    // Phase 5 §1.17a + §5d: numeric columns are decoded via
    // DataConversionUtils::safe_get_* which dispatches on the actual Arrow
    // type (DOUBLE / INT64 / STRING fallback) and logs WARN on parse
    // failures instead of returning a silent 0.0. The sequential `col++`
    // pattern below is preserved so the column ordering of the SELECT keeps
    // driving the row fields directly.
    int col = 0;
    auto get_double = [&table, &col]() -> double {
        const int this_col = col++;
        auto r = read_double_or_zero_on_null(
            table->column(this_col), 0, "col[" + std::to_string(this_col) + "]");
        return r.is_ok() ? r.value() : 0.0;
    };

    auto get_int = [&table, &col]() -> int {
        const int this_col = col++;
        auto r = DataConversionUtils::safe_get_int64(
            table->column(this_col), 0, "col[" + std::to_string(this_col) + "]");
        if (r.is_ok()) return static_cast<int>(r.value());
        // null cell -> 0 (legacy semantic for this loader); type mismatch
        // already logged WARN inside safe_get_int64.
        return 0;
    };

    row.daily_pnl = get_double();
    row.total_pnl = get_double();
    row.daily_realized_pnl = get_double();
    row.daily_unrealized_pnl = get_double();
    row.daily_return = get_double();
    row.total_cumulative_return = get_double();
    row.total_annualized_return = get_double();
    row.current_portfolio_value = get_double();
    row.gross_leverage = get_double();
    row.equity_to_margin_ratio = get_double();
    row.gross_notional = get_double();
    row.margin_posted = get_double();
    row.cash_available = get_double();
    row.daily_transaction_costs = get_double();
    row.sharpe_ratio = get_double();
    row.sortino_ratio = get_double();
    row.max_drawdown = get_double();
    row.volatility = get_double();
    row.win_rate = get_double();
    row.avg_win = get_double();
    row.avg_loss = get_double();
    row.profit_factor = get_double();
    row.best_day = get_double();
    row.worst_day = get_double();
    row.downside_deviation = get_double();
    row.gross_profit = get_double();
    row.gross_loss = get_double();
    row.active_positions = get_int();
    // Removed total_trades - column dropped from database
    row.winning_days = get_int();
    row.losing_days = get_int();
    row.total_days = get_int();

    INFO("Loaded live results for " + date_str + ": PnL=$" + std::to_string(row.daily_pnl) +
         ", Portfolio=$" + std::to_string(row.current_portfolio_value));

    return Result<LiveResultsRow>(row);
}

Result<PreviousDayData> LiveDataLoader::load_previous_day_data(const std::string& strategy_id,
                                                               const std::string& portfolio_id,
                                                               const Timestamp& date) {
    auto validation = validate_connection();
    if (validation.is_error()) {
        return make_error<PreviousDayData>(ErrorCode::DATABASE_ERROR, validation.error()->what(),
                                           "LiveDataLoader");
    }

    // Phase 5 §5c: UTC date-string contract -- format_utc_date is the only
    // approved primitive for date keys; std::gmtime is not thread-safe.
    const std::string date_str = core::format_utc_date(date);

    std::string actual_portfolio_id = portfolio_id.empty() ? "BASE_PORTFOLIO" : portfolio_id;

    std::string query =
        "SELECT "
        "current_portfolio_value, total_pnl, daily_pnl, daily_transaction_costs, date "
        "FROM " +
        schema_ +
        ".live_results "
        "WHERE strategy_id = '" +
        strategy_id +
        "' "
        "AND portfolio_id = '" +
        actual_portfolio_id +
        "' "
        "AND DATE(date) < '" +
        date_str +
        "' "
        "ORDER BY date DESC LIMIT 1";

    DEBUG("Loading previous day data: " + query);

    auto result = db_->execute_query(query);
    if (result.is_error()) {
        return make_error<PreviousDayData>(
            ErrorCode::DATABASE_ERROR,
            "Failed to load previous day data: " + std::string(result.error()->what()),
            "LiveDataLoader");
    }

    auto table = result.value();
    PreviousDayData data;

    if (!table || table->num_rows() == 0) {
        data.exists = false;
        INFO("No previous day data found for " + strategy_id);
        return Result<PreviousDayData>(data);
    }

    // Extract fields (Phase 5 §1.17a -- via safe_get_double, null-as-zero
    // semantics preserved; type mismatches now propagate as errors).
    auto pv_r  = read_double_or_zero_on_null(table->column(0), 0, "portfolio_value");
    auto tp_r  = read_double_or_zero_on_null(table->column(1), 0, "total_pnl");
    auto dp_r  = read_double_or_zero_on_null(table->column(2), 0, "daily_pnl");
    auto com_r = read_double_or_zero_on_null(table->column(3), 0, "daily_transaction_costs");
    if (pv_r.is_error() || tp_r.is_error() || dp_r.is_error() || com_r.is_error()) {
        return make_error<PreviousDayData>(
            ErrorCode::CONVERSION_ERROR,
            "load_previous_day_data: column type mismatch",
            "LiveDataLoader");
    }
    data.portfolio_value = pv_r.value();
    data.total_pnl = tp_r.value();
    data.daily_pnl = dp_r.value();
    data.daily_transaction_costs = com_r.value();

    // Ultrareview follow-up: timestamp column is `arrow::TimestampArray` in
    // the canonical schema, but `convert_generic_to_arrow` may surface it as
    // utf8 / int64 depending on the source. Route through safe_get_int64 so
    // both shapes work and a type mismatch logs a clear WARN instead of
    // crashing the cast.
    auto ts_r = DataConversionUtils::safe_get_int64(table->column(4), 0, "date");
    if (ts_r.is_error()) {
        return make_error<PreviousDayData>(
            ErrorCode::CONVERSION_ERROR,
            "load_previous_day_data: bad timestamp column (" +
                std::string(ts_r.error()->what()) + ")",
            "LiveDataLoader");
    }
    auto duration = std::chrono::microseconds(ts_r.value());
    data.date = std::chrono::system_clock::time_point(duration);

    data.exists = true;

    INFO("Loaded previous day data: Portfolio=$" + std::to_string(data.portfolio_value) +
         ", Total PnL=$" + std::to_string(data.total_pnl));

    return Result<PreviousDayData>(data);
}

Result<bool> LiveDataLoader::has_live_results(const std::string& strategy_id,
                                              const std::string& portfolio_id,
                                              const Timestamp& date) {
    auto validation = validate_connection();
    if (validation.is_error()) {
        return make_error<bool>(ErrorCode::DATABASE_ERROR, validation.error()->what(),
                                "LiveDataLoader");
    }

    // Phase 5 §5c: UTC date-string contract -- format_utc_date is the only
    // approved primitive for date keys; std::gmtime is not thread-safe.
    const std::string date_str = core::format_utc_date(date);

    std::string actual_portfolio_id = portfolio_id.empty() ? "BASE_PORTFOLIO" : portfolio_id;

    std::string query = "SELECT COUNT(*) FROM " + schema_ +
                        ".live_results "
                        "WHERE strategy_id = '" +
                        strategy_id +
                        "' "
                        "AND portfolio_id = '" +
                        actual_portfolio_id +
                        "' "
                        "AND DATE(date) = '" +
                        date_str + "'";

    auto result = db_->execute_query(query);
    if (result.is_error()) {
        return make_error<bool>(
            ErrorCode::DATABASE_ERROR,
            "Failed to check live results existence: " + std::string(result.error()->what()),
            "LiveDataLoader");
    }

    auto table = result.value();
    if (!table || table->num_rows() == 0) {
        return Result<bool>(false);
    }

    // Ultrareview follow-up: Phase 5 retrofit covered DoubleArray but missed
    // Int64Array sites. safe_get_int64 dispatches on actual type.
    auto cnt_r = DataConversionUtils::safe_get_int64(table->column(0), 0, "count");
    if (cnt_r.is_error()) {
        return make_error<bool>(cnt_r.error()->code(), cnt_r.error()->what(),
                                "LiveDataLoader");
    }
    return Result<bool>(cnt_r.value() > 0);
}

Result<int> LiveDataLoader::get_live_results_count(const std::string& strategy_id,
                                                   const std::string& portfolio_id) {
    auto validation = validate_connection();
    if (validation.is_error()) {
        return make_error<int>(ErrorCode::DATABASE_ERROR, validation.error()->what(),
                               "LiveDataLoader");
    }

    std::string actual_portfolio_id = portfolio_id.empty() ? "BASE_PORTFOLIO" : portfolio_id;

    std::string query = "SELECT COUNT(*) FROM " + schema_ +
                        ".live_results "
                        "WHERE strategy_id = '" +
                        strategy_id +
                        "' "
                        "AND portfolio_id = '" +
                        actual_portfolio_id + "'";

    auto result = db_->execute_query(query);
    if (result.is_error()) {
        return make_error<int>(
            ErrorCode::DATABASE_ERROR,
            "Failed to get live results count: " + std::string(result.error()->what()),
            "LiveDataLoader");
    }

    auto table = result.value();
    if (!table || table->num_rows() == 0) {
        return Result<int>(0);
    }

    // Ultrareview follow-up: safe_get_int64 covers utf8-stored count.
    auto cnt_r = DataConversionUtils::safe_get_int64(table->column(0), 0, "count");
    if (cnt_r.is_error()) {
        return make_error<int>(cnt_r.error()->code(), cnt_r.error()->what(),
                               "LiveDataLoader");
    }
    return Result<int>(static_cast<int>(cnt_r.value()));
}

// ========== Historical Series Methods ==========

Result<std::vector<double>> LiveDataLoader::load_daily_returns_history(
    const std::string& strategy_id, const std::string& portfolio_id, const Timestamp& as_of_date) {
    auto validation = validate_connection();
    if (validation.is_error()) {
        return make_error<std::vector<double>>(ErrorCode::DATABASE_ERROR,
                                               validation.error()->what(), "LiveDataLoader");
    }

    // Phase 5 §5c: UTC date-string contract -- see format_utc_date docs.
    const std::string date_str = core::format_utc_date(as_of_date);

    std::string actual_portfolio_id = portfolio_id.empty() ? "BASE_PORTFOLIO" : portfolio_id;

    std::string query =
        "SELECT daily_return::double precision as daily_return "
        "FROM " +
        schema_ +
        ".live_results "
        "WHERE strategy_id = '" +
        strategy_id +
        "' "
        "AND portfolio_id = '" +
        actual_portfolio_id +
        "' "
        "AND DATE(date) <= '" +
        date_str +
        "' "
        "ORDER BY date ASC";

    DEBUG("Loading daily returns history: " + query);

    auto result = db_->execute_query(query);
    if (result.is_error()) {
        return make_error<std::vector<double>>(
            ErrorCode::DATABASE_ERROR,
            "Failed to load daily returns history: " + std::string(result.error()->what()),
            "LiveDataLoader");
    }

    std::vector<double> returns;
    auto table = result.value();
    if (!table || table->num_rows() == 0) {
        return Result<std::vector<double>>(returns);
    }

    // convert_generic_to_arrow builds ALL columns as arrow::utf8() (strings)
    // so we must read via StringArray and convert to double
    auto array = std::static_pointer_cast<arrow::StringArray>(table->column(0)->chunk(0));
    returns.reserve(static_cast<size_t>(table->num_rows()));
    for (int64_t i = 0; i < table->num_rows(); ++i) {
        if (array->IsNull(i)) {
            returns.push_back(0.0);
        } else {
            try {
                returns.push_back(std::stod(array->GetString(i)));
            } catch (const std::exception&) {
                returns.push_back(0.0);
            }
        }
    }

    return Result<std::vector<double>>(returns);
}

Result<std::vector<double>> LiveDataLoader::load_daily_pnl_history(const std::string& strategy_id,
                                                                   const std::string& portfolio_id,
                                                                   const Timestamp& as_of_date) {
    auto validation = validate_connection();
    if (validation.is_error()) {
        return make_error<std::vector<double>>(ErrorCode::DATABASE_ERROR,
                                               validation.error()->what(), "LiveDataLoader");
    }

    // Phase 5 §5c: UTC date-string contract -- see format_utc_date docs.
    const std::string date_str = core::format_utc_date(as_of_date);

    std::string actual_portfolio_id = portfolio_id.empty() ? "BASE_PORTFOLIO" : portfolio_id;

    std::string query =
        "SELECT daily_pnl::double precision as daily_pnl "
        "FROM " +
        schema_ +
        ".live_results "
        "WHERE strategy_id = '" +
        strategy_id +
        "' "
        "AND portfolio_id = '" +
        actual_portfolio_id +
        "' "
        "AND DATE(date) <= '" +
        date_str +
        "' "
        "ORDER BY date ASC";

    DEBUG("Loading daily PnL history: " + query);

    auto result = db_->execute_query(query);
    if (result.is_error()) {
        return make_error<std::vector<double>>(
            ErrorCode::DATABASE_ERROR,
            "Failed to load daily PnL history: " + std::string(result.error()->what()),
            "LiveDataLoader");
    }

    std::vector<double> pnls;
    auto table = result.value();
    if (!table || table->num_rows() == 0) {
        return Result<std::vector<double>>(pnls);
    }

    // convert_generic_to_arrow builds ALL columns as arrow::utf8() (strings)
    // so we must read via StringArray and convert to double
    auto array = std::static_pointer_cast<arrow::StringArray>(table->column(0)->chunk(0));
    pnls.reserve(static_cast<size_t>(table->num_rows()));
    for (int64_t i = 0; i < table->num_rows(); ++i) {
        if (array->IsNull(i)) {
            pnls.push_back(0.0);
        } else {
            try {
                pnls.push_back(std::stod(array->GetString(i)));
            } catch (const std::exception&) {
                pnls.push_back(0.0);
            }
        }
    }

    return Result<std::vector<double>>(pnls);
}

Result<std::vector<double>> LiveDataLoader::load_equity_curve_history(
    const std::string& strategy_id, const std::string& portfolio_id, const Timestamp& as_of_date) {
    auto validation = validate_connection();
    if (validation.is_error()) {
        return make_error<std::vector<double>>(ErrorCode::DATABASE_ERROR,
                                               validation.error()->what(), "LiveDataLoader");
    }

    // Phase 5 §5c: UTC date-string contract -- see format_utc_date docs.
    const std::string date_str = core::format_utc_date(as_of_date);

    std::string actual_portfolio_id = portfolio_id.empty() ? "BASE_PORTFOLIO" : portfolio_id;

    std::string query =
        "SELECT equity "
        "FROM " +
        schema_ +
        ".equity_curve "
        "WHERE strategy_id = '" +
        strategy_id +
        "' "
        "AND portfolio_id = '" +
        actual_portfolio_id +
        "' "
        "AND DATE(timestamp) <= '" +
        date_str +
        "' "
        "ORDER BY timestamp ASC";

    DEBUG("Loading equity curve history: " + query);

    auto result = db_->execute_query(query);
    if (result.is_error()) {
        return make_error<std::vector<double>>(
            ErrorCode::DATABASE_ERROR,
            "Failed to load equity curve history: " + std::string(result.error()->what()),
            "LiveDataLoader");
    }

    std::vector<double> equity;
    auto table = result.value();
    if (!table || table->num_rows() == 0) {
        return Result<std::vector<double>>(equity);
    }

    // convert_generic_to_arrow builds ALL columns as arrow::utf8() (strings)
    auto array = std::static_pointer_cast<arrow::StringArray>(table->column(0)->chunk(0));
    equity.reserve(static_cast<size_t>(table->num_rows()));
    for (int64_t i = 0; i < table->num_rows(); ++i) {
        if (array->IsNull(i)) {
            equity.push_back(0.0);
        } else {
            try {
                equity.push_back(std::stod(array->GetString(i)));
            } catch (const std::exception&) {
                equity.push_back(0.0);
            }
        }
    }

    return Result<std::vector<double>>(equity);
}

Result<int> LiveDataLoader::load_total_trades_count(const std::string& strategy_id,
                                                    const std::string& portfolio_id,
                                                    const Timestamp& as_of_date) {
    auto validation = validate_connection();
    if (validation.is_error()) {
        return make_error<int>(ErrorCode::DATABASE_ERROR, validation.error()->what(),
                               "LiveDataLoader");
    }

    // Phase 5 §5c: UTC date-string contract -- see format_utc_date docs.
    const std::string date_str = core::format_utc_date(as_of_date);

    std::string actual_portfolio_id = portfolio_id.empty() ? "BASE_PORTFOLIO" : portfolio_id;

    std::string query =
        "SELECT COUNT(*) "
        "FROM " +
        schema_ +
        ".executions "
        "WHERE strategy_id = '" +
        strategy_id +
        "' "
        "AND portfolio_id = '" +
        actual_portfolio_id +
        "' "
        "AND DATE(execution_time) <= '" +
        date_str + "'";

    DEBUG("Loading total trades count: " + query);

    auto result = db_->execute_query(query);
    if (result.is_error()) {
        return make_error<int>(
            ErrorCode::DATABASE_ERROR,
            "Failed to load total trades count: " + std::string(result.error()->what()),
            "LiveDataLoader");
    }

    auto table = result.value();
    if (!table || table->num_rows() == 0) {
        return Result<int>(0);
    }

    // Ultrareview follow-up: safe_get_int64 with null-as-zero fallback
    // (preserves the legacy "no trades = 0 count" semantic).
    auto cnt_r = DataConversionUtils::safe_get_int64(table->column(0), 0, "trades_count");
    if (cnt_r.is_error()) {
        if (cnt_r.error()->code() == ErrorCode::INVALID_DATA) {
            return Result<int>(0);  // null cell -> 0 trades
        }
        return make_error<int>(cnt_r.error()->code(), cnt_r.error()->what(),
                               "LiveDataLoader");
    }
    return Result<int>(static_cast<int>(cnt_r.value()));
}

// ========== Position Methods ==========

Result<std::vector<Position>> LiveDataLoader::load_positions(const std::string& strategy_id,
                                                             const std::string& portfolio_id,
                                                             const Timestamp& date) {
    auto validation = validate_connection();
    if (validation.is_error()) {
        return make_error<std::vector<Position>>(ErrorCode::DATABASE_ERROR,
                                                 validation.error()->what(), "LiveDataLoader");
    }

    // Phase 5 §5c: UTC date-string contract -- format_utc_date is the only
    // approved primitive for date keys; std::gmtime is not thread-safe.
    const std::string date_str = core::format_utc_date(date);

    std::string actual_portfolio_id = portfolio_id.empty() ? "BASE_PORTFOLIO" : portfolio_id;

    std::string query =
        "SELECT symbol, quantity, average_price, "
        "daily_realized_pnl, daily_unrealized_pnl, last_update "
        "FROM " +
        schema_ +
        ".positions "
        "WHERE strategy_id = '" +
        strategy_id +
        "' "
        "AND portfolio_id = '" +
        actual_portfolio_id +
        "' "
        "AND DATE(last_update) = '" +
        date_str +
        "' "
        "ORDER BY symbol";

    DEBUG("Loading positions: " + query);

    auto result = db_->execute_query(query);
    if (result.is_error()) {
        return make_error<std::vector<Position>>(
            ErrorCode::DATABASE_ERROR,
            "Failed to load positions: " + std::string(result.error()->what()), "LiveDataLoader");
    }

    auto table = result.value();
    std::vector<Position> positions;

    if (!table || table->num_rows() == 0) {
        INFO("No positions found for " + date_str);
        return Result<std::vector<Position>>(positions);
    }

    // Cache column references once (Phase 5 §1.17a -- safe_get_double walks
    // chunks internally, so no chunk(0) assumption).
    auto symbol_col     = table->column(0);
    auto qty_col        = table->column(1);
    auto price_col      = table->column(2);
    auto realized_col   = table->column(3);
    auto unrealized_col = table->column(4);

    for (int64_t i = 0; i < table->num_rows(); ++i) {
        Position pos;

        auto sym_r = DataConversionUtils::safe_get_string(symbol_col, i, "symbol");
        auto qty_r = DataConversionUtils::safe_get_double(qty_col, i, "quantity");
        auto px_r  = DataConversionUtils::safe_get_double(price_col, i, "average_price");
        auto rp_r  = read_double_or_zero_on_null(realized_col, i, "daily_realized_pnl");
        auto up_r  = read_double_or_zero_on_null(unrealized_col, i, "daily_unrealized_pnl");
        if (sym_r.is_error() || qty_r.is_error() || px_r.is_error() ||
            rp_r.is_error() || up_r.is_error()) {
            WARN("load_positions: skipping row " + std::to_string(i) +
                 " due to column read error");
            continue;
        }
        pos.symbol = sym_r.value();
        pos.quantity = Decimal(qty_r.value());
        pos.average_price = Decimal(px_r.value());
        pos.realized_pnl = Decimal(rp_r.value());
        pos.unrealized_pnl = Decimal(up_r.value());

        positions.push_back(pos);
    }

    INFO("Loaded " + std::to_string(positions.size()) + " positions for " + date_str);
    return Result<std::vector<Position>>(positions);
}

Result<std::vector<Position>> LiveDataLoader::load_positions_for_export(
    const std::string& strategy_id, const std::string& portfolio_id, const Timestamp& date) {
    // For now, same as load_positions
    // Could be customized later for specific export requirements
    return load_positions(strategy_id, portfolio_id, date);
}

// ========== Commission Methods ==========

Result<std::unordered_map<std::string, double>> LiveDataLoader::load_commissions_by_symbol(
    const std::string& portfolio_id, const Timestamp& date) {
    auto validation = validate_connection();
    if (validation.is_error()) {
        return make_error<std::unordered_map<std::string, double>>(
            ErrorCode::DATABASE_ERROR, validation.error()->what(), "LiveDataLoader");
    }

    // Phase 5 §5c: UTC date-string contract -- format_utc_date is the only
    // approved primitive for date keys; std::gmtime is not thread-safe.
    const std::string date_str = core::format_utc_date(date);

    std::string actual_portfolio_id = portfolio_id.empty() ? "BASE_PORTFOLIO" : portfolio_id;

    std::string query =
        "SELECT symbol, COALESCE(SUM(commission), 0.0) as total_commission "
        "FROM " +
        schema_ +
        ".executions "
        "WHERE portfolio_id = '" +
        actual_portfolio_id + "' AND DATE(execution_time) = '" + date_str +
        "' "
        "GROUP BY symbol";

    DEBUG("Loading commissions by symbol: " + query);

    auto result = db_->execute_query(query);
    if (result.is_error()) {
        return make_error<std::unordered_map<std::string, double>>(
            ErrorCode::DATABASE_ERROR,
            "Failed to load commissions: " + std::string(result.error()->what()), "LiveDataLoader");
    }

    std::unordered_map<std::string, double> commissions;
    auto table = result.value();

    if (!table || table->num_rows() == 0) {
        INFO("No commissions found for " + date_str);
        return Result<std::unordered_map<std::string, double>>(commissions);
    }

    auto symbol_col = table->column(0);
    auto commission_col = table->column(1);
    for (int64_t i = 0; i < table->num_rows(); ++i) {
        auto sym_r = DataConversionUtils::safe_get_string(symbol_col, i, "symbol");
        auto com_r = DataConversionUtils::safe_get_double(commission_col, i, "total_commission");
        if (sym_r.is_error() || com_r.is_error()) {
            WARN("load_commissions_by_symbol: skipping row " + std::to_string(i));
            continue;
        }
        commissions[sym_r.value()] = com_r.value();
    }

    INFO("Loaded commissions for " + std::to_string(commissions.size()) + " symbols");
    return Result<std::unordered_map<std::string, double>>(commissions);
}

Result<double> LiveDataLoader::load_daily_transaction_costs(const std::string& strategy_id,
                                                            const std::string& portfolio_id,
                                                            const Timestamp& date) {
    auto validation = validate_connection();
    if (validation.is_error()) {
        return make_error<double>(ErrorCode::DATABASE_ERROR, validation.error()->what(),
                                  "LiveDataLoader");
    }

    // Phase 5 §5c: UTC date-string contract -- format_utc_date is the only
    // approved primitive for date keys; std::gmtime is not thread-safe.
    const std::string date_str = core::format_utc_date(date);

    std::string actual_portfolio_id = portfolio_id.empty() ? "BASE_PORTFOLIO" : portfolio_id;

    std::string query =
        "SELECT COALESCE(daily_transaction_costs, 0.0) "
        "FROM " +
        schema_ +
        ".live_results "
        "WHERE strategy_id = '" +
        strategy_id +
        "' "
        "AND portfolio_id = '" +
        actual_portfolio_id +
        "' "
        "AND DATE(date) = '" +
        date_str + "'";

    DEBUG("Loading daily transaction costs: " + query);

    auto result = db_->execute_query(query);
    if (result.is_error()) {
        return make_error<double>(
            ErrorCode::DATABASE_ERROR,
            "Failed to load daily transaction costs: " + std::string(result.error()->what()),
            "LiveDataLoader");
    }

    auto table = result.value();
    if (!table || table->num_rows() == 0) {
        INFO("No transaction cost data found for " + date_str);
        return Result<double>(0.0);
    }

    auto tc_r = read_double_or_zero_on_null(table->column(0), 0, "transaction_costs");
    if (tc_r.is_error()) {
        return make_error<double>(tc_r.error()->code(), tc_r.error()->what(),
                                  "LiveDataLoader");
    }
    return Result<double>(tc_r.value());
}

// ========== Margin and Risk Methods ==========

Result<MarginMetrics> LiveDataLoader::load_margin_metrics(const std::string& strategy_id,
                                                          const std::string& portfolio_id,
                                                          const Timestamp& date) {
    auto validation = validate_connection();
    if (validation.is_error()) {
        return make_error<MarginMetrics>(ErrorCode::DATABASE_ERROR, validation.error()->what(),
                                         "LiveDataLoader");
    }

    // Phase 5 §5c: UTC date-string contract -- format_utc_date is the only
    // approved primitive for date keys; std::gmtime is not thread-safe.
    const std::string date_str = core::format_utc_date(date);

    std::string actual_portfolio_id = portfolio_id.empty() ? "BASE_PORTFOLIO" : portfolio_id;

    std::string query =
        "SELECT portfolio_leverage, equity_to_margin_ratio, "
        "gross_notional, margin_posted "
        "FROM " +
        schema_ +
        ".live_results "
        "WHERE strategy_id = '" +
        strategy_id +
        "' "
        "AND portfolio_id = '" +
        actual_portfolio_id +
        "' "
        "AND DATE(date) = '" +
        date_str + "'";

    DEBUG("Loading margin metrics: " + query);

    auto result = db_->execute_query(query);
    if (result.is_error()) {
        return make_error<MarginMetrics>(
            ErrorCode::DATABASE_ERROR,
            "Failed to load margin metrics: " + std::string(result.error()->what()),
            "LiveDataLoader");
    }

    MarginMetrics metrics;
    auto table = result.value();

    if (!table || table->num_rows() == 0) {
        metrics.valid = false;
        INFO("No margin metrics found for " + date_str);
        return Result<MarginMetrics>(metrics);
    }

    auto lev_r  = read_double_or_zero_on_null(table->column(0), 0, "gross_leverage");
    auto er_r   = read_double_or_zero_on_null(table->column(1), 0, "equity_to_margin_ratio");
    auto not_r  = read_double_or_zero_on_null(table->column(2), 0, "gross_notional");
    auto mar_r  = read_double_or_zero_on_null(table->column(3), 0, "margin_posted");
    if (lev_r.is_error() || er_r.is_error() || not_r.is_error() || mar_r.is_error()) {
        return make_error<MarginMetrics>(
            ErrorCode::CONVERSION_ERROR,
            "load_margin_metrics: column type mismatch",
            "LiveDataLoader");
    }
    metrics.gross_leverage = lev_r.value();
    metrics.equity_to_margin_ratio = er_r.value();
    metrics.gross_notional = not_r.value();
    metrics.margin_posted = mar_r.value();

    // Calculate margin cushion
    if (metrics.margin_posted > 0) {
        metrics.margin_cushion = (metrics.equity_to_margin_ratio - 1.0) * 100.0;
    }

    metrics.valid = true;

    INFO("Loaded margin metrics: Gross Leverage=" + std::to_string(metrics.gross_leverage) +
         ", Equity/Margin=" + std::to_string(metrics.equity_to_margin_ratio));

    return Result<MarginMetrics>(metrics);
}

// ========== Email/Reporting Methods ==========

Result<std::unordered_map<std::string, double>> LiveDataLoader::load_daily_metrics_for_email(
    const std::string& strategy_id, const std::string& portfolio_id, const Timestamp& date) {
    auto validation = validate_connection();
    if (validation.is_error()) {
        return make_error<std::unordered_map<std::string, double>>(
            ErrorCode::DATABASE_ERROR, validation.error()->what(), "LiveDataLoader");
    }

    // Phase 5 §5c: UTC date-string contract -- format_utc_date is the only
    // approved primitive for date keys; std::gmtime is not thread-safe.
    const std::string date_str = core::format_utc_date(date);

    std::string actual_portfolio_id = portfolio_id.empty() ? "BASE_PORTFOLIO" : portfolio_id;

    std::string query =
        "SELECT daily_return, daily_unrealized_pnl, daily_realized_pnl, daily_pnl, "
        "daily_transaction_costs "
        "FROM " +
        schema_ +
        ".live_results "
        "WHERE strategy_id = '" +
        strategy_id +
        "' "
        "AND portfolio_id = '" +
        actual_portfolio_id +
        "' "
        "AND DATE(date) = '" +
        date_str + "'";

    DEBUG("Loading daily metrics for email: " + query);

    auto result = db_->execute_query(query);
    if (result.is_error()) {
        return make_error<std::unordered_map<std::string, double>>(
            ErrorCode::DATABASE_ERROR,
            "Failed to load daily metrics: " + std::string(result.error()->what()),
            "LiveDataLoader");
    }

    std::unordered_map<std::string, double> metrics;
    auto table = result.value();

    if (!table || table->num_rows() == 0) {
        INFO("No daily metrics found for " + date_str);
        return Result<std::unordered_map<std::string, double>>(metrics);
    }

    auto dr_r  = read_double_or_zero_on_null(table->column(0), 0, "daily_return");
    auto du_r  = read_double_or_zero_on_null(table->column(1), 0, "daily_unrealized_pnl");
    auto drl_r = read_double_or_zero_on_null(table->column(2), 0, "daily_realized_pnl");
    auto dt_r  = read_double_or_zero_on_null(table->column(3), 0, "daily_pnl");
    auto dtc_r = read_double_or_zero_on_null(table->column(4), 0, "daily_transaction_costs");
    if (dr_r.is_error() || du_r.is_error() || drl_r.is_error() ||
        dt_r.is_error() || dtc_r.is_error()) {
        return make_error<std::unordered_map<std::string, double>>(
            ErrorCode::CONVERSION_ERROR,
            "load_daily_metrics_for_email: column type mismatch", "LiveDataLoader");
    }
    metrics["Daily Return"] = dr_r.value();
    metrics["Daily Unrealized PnL"] = du_r.value();
    metrics["Daily Realized PnL"] = drl_r.value();
    metrics["Daily Total PnL"] = dt_r.value();
    metrics["Daily Transaction Costs"] = dtc_r.value();

    INFO("Loaded email metrics: Return=" + std::to_string(metrics["Daily Return"]) + "%");

    return Result<std::unordered_map<std::string, double>>(metrics);
}

}  // namespace trade_ngin