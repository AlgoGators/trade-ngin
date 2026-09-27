#include "trade_ngin/data/conversion_utils.hpp"
#include "trade_ngin/core/time_utils.hpp"
// src/live/live_data_loader.cpp
// Implementation of data loading component for live trading

#include "trade_ngin/live/live_data_loader.hpp"
#include <arrow/api.h>
#include <iomanip>
#include <sstream>
#include <cmath>
#include <limits>
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/types.hpp"  // For Position struct

namespace trade_ngin {
namespace {
// Generic PostgreSQL queries produce UTF8 arrays; test/data adapters may return
// numeric arrays. Scalar conversion handles both without reinterpreting buffers.
Result<std::vector<double>> numeric_values(const std::shared_ptr<arrow::Table>& table,
                                          int columns, int64_t row=0) {
    std::vector<double> values;
    if(!table || table->num_columns()<columns || row<0 || row>=table->num_rows())
        return make_error<std::vector<double>>(ErrorCode::INVALID_DATA,"missing_numeric_query_cell");
    try {
        for(int column=0;column<columns;++column) {
            auto scalar=table->column(column)->GetScalar(row);
            if(!scalar.ok()) throw std::invalid_argument("unreadable numeric cell");
            if(!scalar.ValueOrDie()->is_valid) {values.push_back(0);continue;}
            const auto text=scalar.ValueOrDie()->ToString();
            size_t consumed=0;
            const double value=std::stod(text,&consumed);
            if(consumed!=text.size() || !std::isfinite(value))
                throw std::invalid_argument("invalid numeric cell");
            values.push_back(value);
        }
        return Result<std::vector<double>>(values);
    } catch(const std::exception&) {
        return make_error<std::vector<double>>(ErrorCode::INVALID_DATA,"invalid_numeric_query_result");
    }
}
bool valid_count(double value) {
    return value>=0 && value<=std::numeric_limits<int>::max() && std::floor(value)==value;
}
}

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
    auto time_t = std::chrono::system_clock::to_time_t(date);
    std::stringstream date_ss;
    date_ss << std::put_time(std::gmtime(&time_t), "%Y-%m-%d");

    std::string actual_portfolio_id = portfolio_id.empty() ? "BASE_PORTFOLIO" : portfolio_id;

    std::string query =
        "SELECT COALESCE(current_portfolio_value, 0.0) "
        "FROM " +
        schema_ +
        ".live_results "
        "WHERE " + std::string(schema_ == "trading" ? "portfolio_type = 'system' AND " : "") + "strategy_id = '" +
        strategy_id +
        "' "
        "AND portfolio_id = '" +
        actual_portfolio_id +
        "' "
        "AND DATE(date) < '" +
        date_ss.str() +
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

    auto parsed=numeric_values(table,1);
    if(parsed.is_error()) return make_error<double>(ErrorCode::INVALID_DATA,parsed.error()->what());
    double value=parsed.value()[0];

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

    auto time_t = std::chrono::system_clock::to_time_t(date);
    std::stringstream date_ss;
    date_ss << std::put_time(std::gmtime(&time_t), "%Y-%m-%d");

    std::string actual_portfolio_id = portfolio_id.empty() ? "BASE_PORTFOLIO" : portfolio_id;

    std::string query =
        "SELECT COALESCE(current_portfolio_value, 0.0) "
        "FROM " +
        schema_ +
        ".live_results "
        "WHERE " + std::string(schema_ == "trading" ? "portfolio_type = 'system' AND " : "") + "strategy_id = '" +
        strategy_id +
        "' "
        "AND portfolio_id = '" +
        actual_portfolio_id +
        "' "
        "AND DATE(date) = '" +
        date_ss.str() + "'";

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
                                  "No portfolio value found for date " + date_ss.str(),
                                  "LiveDataLoader");
    }

    auto parsed=numeric_values(table,1);
    if(parsed.is_error()) return make_error<double>(ErrorCode::INVALID_DATA,parsed.error()->what());
    double value=parsed.value()[0];

    return Result<double>(value);
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

    auto time_t = std::chrono::system_clock::to_time_t(date);
    std::stringstream date_ss;
    date_ss << std::put_time(std::gmtime(&time_t), "%Y-%m-%d");

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
        "WHERE " + std::string(schema_ == "trading" ? "portfolio_type = 'system' AND " : "") + "strategy_id = '" +
        strategy_id +
        "' "
        "AND portfolio_id = '" +
        actual_portfolio_id +
        "' "
        "AND DATE(date) = '" +
        date_ss.str() + "'";

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
                                          "No live results found for date " + date_ss.str(),
                                          "LiveDataLoader");
    }

    LiveResultsRow row;
    row.strategy_id = strategy_id;
    row.date = date;

    // Extract all fields from the result
    // NOTE: convert_generic_to_arrow() builds ALL columns as arrow::utf8() (strings),
    // so we must read via StringArray and convert to numeric types.
    //
    // TODO(ARROW-STRING-BUG): The same static_pointer_cast<DoubleArray> / <Int64Array> bug
    // exists in the following functions — they return 0 silently when the actual Arrow type
    // is StringArray. Fix these when they cause visible issues:
    //   - load_total_equity() line ~90
    //   - load_total_trades_count() lines ~143, ~414
    //   - load_previous_day_data() lines ~313-318
    //   - has_live_results() line ~377
    //   - load_equity_curve_history() line ~599
    //   - load_portfolio_positions() lines ~714-720
    //   - load_yesterday_metrics() line ~790
    //   - load_portfolio_value() line ~846
    //   - load_leverage_metrics() lines ~903-907
    //   - load_yesterday_daily_metrics() lines ~977-983
    auto parsed=numeric_values(table,31);
    if(parsed.is_error()) return make_error<LiveResultsRow>(ErrorCode::INVALID_DATA,parsed.error()->what());
    for(int index=27;index<31;++index)
        if(!valid_count(parsed.value()[index])) return make_error<LiveResultsRow>(ErrorCode::INVALID_DATA,"invalid_count");
    int col = 0;
    auto get_double = [&parsed, &col]() -> double {return parsed.value()[col++];};

    // Columns that are legitimately absent get a reader that can say so,
    // rather than one that turns NULL into 0.0 and loses the distinction.
    auto get_optional_double = [&table, &parsed, &col]() -> std::optional<double> {
        const auto index=col++;
        if(!table->column(index)->GetScalar(0).ValueOrDie()->is_valid) return std::nullopt;
        return parsed.value()[index];
    };

    // profit_factor additionally has a legacy value to reject: rows written
    // before the sentinel was removed still carry 999.99, which is not a profit
    // factor either. The threshold is specific to that column -- a Sharpe ratio
    // of 999 would be absurd but it would still be a Sharpe ratio.
    auto get_profit_factor = [&get_optional_double]() -> std::optional<double> {
        auto value = get_optional_double();
        if (value && *value >= 999.0) {
            return std::nullopt;
        }
        return value;
    };

    auto get_int = [&parsed, &col]() -> int {return static_cast<int>(parsed.value()[col++]);};

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
    row.sharpe_ratio = get_optional_double();
    row.sortino_ratio = get_optional_double();
    row.max_drawdown = get_double();
    row.volatility = get_double();
    row.win_rate = get_double();
    row.avg_win = get_double();
    row.avg_loss = get_double();
    row.profit_factor = get_profit_factor();
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

    INFO("Loaded live results for " + date_ss.str() + ": PnL=$" + std::to_string(row.daily_pnl) +
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

    auto time_t = std::chrono::system_clock::to_time_t(date);
    std::stringstream date_ss;
    date_ss << std::put_time(std::gmtime(&time_t), "%Y-%m-%d");

    std::string actual_portfolio_id = portfolio_id.empty() ? "BASE_PORTFOLIO" : portfolio_id;

    std::string query =
        "SELECT "
        "current_portfolio_value, total_pnl, daily_pnl, daily_transaction_costs, date "
        "FROM " +
        schema_ +
        ".live_results "
        "WHERE " + std::string(schema_ == "trading" ? "portfolio_type = 'system' AND " : "") + "strategy_id = '" +
        strategy_id +
        "' "
        "AND portfolio_id = '" +
        actual_portfolio_id +
        "' "
        "AND DATE(date) < '" +
        date_ss.str() +
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

    // Extract fields
    auto parsed=numeric_values(table,4);
    if(parsed.is_error()) return make_error<PreviousDayData>(ErrorCode::INVALID_DATA,parsed.error()->what());
    data.portfolio_value=parsed.value()[0];data.total_pnl=parsed.value()[1];
    data.daily_pnl=parsed.value()[2];data.daily_transaction_costs=parsed.value()[3];
    auto date_scalar=table->column(4)->GetScalar(0);
    if(!date_scalar.ok() || !date_scalar.ValueOrDie()->is_valid)
        return make_error<PreviousDayData>(ErrorCode::INVALID_DATA,"missing_previous_date");
    std::tm parsed_day{};
    std::istringstream date_text(date_scalar.ValueOrDie()->ToString());
    date_text>>std::get_time(&parsed_day,"%Y-%m-%d");
    const auto calendar=std::chrono::year{parsed_day.tm_year+1900}/
        std::chrono::month{static_cast<unsigned>(parsed_day.tm_mon+1)}/
        std::chrono::day{static_cast<unsigned>(parsed_day.tm_mday)};
    if(date_text.fail() || !calendar.ok())
        return make_error<PreviousDayData>(ErrorCode::INVALID_DATA,"invalid_previous_date");
    data.date=std::chrono::sys_days(calendar);

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

    auto time_t = std::chrono::system_clock::to_time_t(date);
    std::stringstream date_ss;
    date_ss << std::put_time(std::gmtime(&time_t), "%Y-%m-%d");

    std::string actual_portfolio_id = portfolio_id.empty() ? "BASE_PORTFOLIO" : portfolio_id;

    std::string query = "SELECT COUNT(*) FROM " + schema_ +
                        ".live_results "
                        "WHERE " + std::string(schema_ == "trading" ? "portfolio_type = 'system' AND " : "") + "strategy_id = '" +
                        strategy_id +
                        "' "
                        "AND portfolio_id = '" +
                        actual_portfolio_id +
                        "' "
                        "AND DATE(date) = '" +
                        date_ss.str() + "'";

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

    auto parsed=numeric_values(table,1);
    if(parsed.is_error() || !valid_count(parsed.value()[0])) return make_error<bool>(ErrorCode::INVALID_DATA,"invalid_count");
    int count=static_cast<int>(parsed.value()[0]);

    return Result<bool>(count > 0);
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
                        "WHERE " + std::string(schema_ == "trading" ? "portfolio_type = 'system' AND " : "") + "strategy_id = '" +
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

    auto parsed=numeric_values(table,1);
    if(parsed.is_error() || !valid_count(parsed.value()[0])) return make_error<int>(ErrorCode::INVALID_DATA,"invalid_count");
    int count=static_cast<int>(parsed.value()[0]);

    return Result<int>(count);
}

// ========== Historical Series Methods ==========

Result<std::vector<double>> LiveDataLoader::load_daily_returns_history(
    const std::string& strategy_id, const std::string& portfolio_id, const Timestamp& as_of_date) {
    auto validation = validate_connection();
    if (validation.is_error()) {
        return make_error<std::vector<double>>(ErrorCode::DATABASE_ERROR,
                                               validation.error()->what(), "LiveDataLoader");
    }

    auto time_t = std::chrono::system_clock::to_time_t(as_of_date);
    std::stringstream date_ss;
    date_ss << std::put_time(std::gmtime(&time_t), "%Y-%m-%d");

    std::string actual_portfolio_id = portfolio_id.empty() ? "BASE_PORTFOLIO" : portfolio_id;

    std::string query =
        "SELECT daily_return::double precision as daily_return "
        "FROM " +
        schema_ +
        ".live_results "
        "WHERE " + std::string(schema_ == "trading" ? "portfolio_type = 'system' AND " : "") + "strategy_id = '" +
        strategy_id +
        "' "
        "AND portfolio_id = '" +
        actual_portfolio_id +
        "' "
        "AND DATE(date) <= '" +
        date_ss.str() +
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

    returns.reserve(static_cast<size_t>(table->num_rows()));
    for (int64_t i = 0; i < table->num_rows(); ++i) {
        auto parsed=numeric_values(table,1,i);
        if(parsed.is_error()) return make_error<std::vector<double>>(ErrorCode::INVALID_DATA,parsed.error()->what());
        returns.push_back(parsed.value()[0]);
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

    auto time_t = std::chrono::system_clock::to_time_t(as_of_date);
    std::stringstream date_ss;
    date_ss << std::put_time(std::gmtime(&time_t), "%Y-%m-%d");

    std::string actual_portfolio_id = portfolio_id.empty() ? "BASE_PORTFOLIO" : portfolio_id;

    std::string query =
        "SELECT daily_pnl::double precision as daily_pnl "
        "FROM " +
        schema_ +
        ".live_results "
        "WHERE " + std::string(schema_ == "trading" ? "portfolio_type = 'system' AND " : "") + "strategy_id = '" +
        strategy_id +
        "' "
        "AND portfolio_id = '" +
        actual_portfolio_id +
        "' "
        "AND DATE(date) <= '" +
        date_ss.str() +
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

    pnls.reserve(static_cast<size_t>(table->num_rows()));
    for (int64_t i = 0; i < table->num_rows(); ++i) {
        auto parsed=numeric_values(table,1,i);
        if(parsed.is_error()) return make_error<std::vector<double>>(ErrorCode::INVALID_DATA,parsed.error()->what());
        pnls.push_back(parsed.value()[0]);
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

    auto time_t = std::chrono::system_clock::to_time_t(as_of_date);
    std::stringstream date_ss;
    date_ss << std::put_time(std::gmtime(&time_t), "%Y-%m-%d");

    std::string actual_portfolio_id = portfolio_id.empty() ? "BASE_PORTFOLIO" : portfolio_id;

    std::string query =
        "SELECT equity "
        "FROM " +
        schema_ +
        ".equity_curve "
        "WHERE " + std::string(schema_ == "trading" ? "portfolio_type = 'system' AND " : "") + "strategy_id = '" +
        strategy_id +
        "' "
        "AND portfolio_id = '" +
        actual_portfolio_id +
        "' "
        "AND DATE(timestamp) <= '" +
        date_ss.str() +
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

    equity.reserve(static_cast<size_t>(table->num_rows()));
    for (int64_t i = 0; i < table->num_rows(); ++i) {
        auto parsed=numeric_values(table,1,i);
        if(parsed.is_error()) return make_error<std::vector<double>>(ErrorCode::INVALID_DATA,parsed.error()->what());
        equity.push_back(parsed.value()[0]);
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

    auto time_t = std::chrono::system_clock::to_time_t(as_of_date);
    std::stringstream date_ss;
    date_ss << std::put_time(std::gmtime(&time_t), "%Y-%m-%d");

    std::string actual_portfolio_id = portfolio_id.empty() ? "BASE_PORTFOLIO" : portfolio_id;

    std::string query =
        "SELECT COUNT(*) "
        "FROM " +
        schema_ +
        ".executions "
        "WHERE " + std::string(schema_ == "trading" ? "portfolio_type = 'system' AND " : "") + "strategy_id = '" +
        strategy_id +
        "' "
        "AND portfolio_id = '" +
        actual_portfolio_id +
        "' "
        "AND DATE(execution_time) <= '" +
        date_ss.str() + "'";

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

    auto parsed=numeric_values(table,1);
    if(parsed.is_error() || !valid_count(parsed.value()[0])) return make_error<int>(ErrorCode::INVALID_DATA,"invalid_count");
    int count=static_cast<int>(parsed.value()[0]);

    return Result<int>(count);
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

    auto time_t = std::chrono::system_clock::to_time_t(date);
    std::stringstream date_ss;
    date_ss << std::put_time(std::gmtime(&time_t), "%Y-%m-%d");

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
        date_ss.str() +
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
        INFO("No positions found for " + date_ss.str());
        return Result<std::vector<Position>>(positions);
    }

    // Extract positions from result
    for (int64_t i = 0; i < table->num_rows(); ++i) {
        Position pos;

        auto symbol_array =
            std::static_pointer_cast<arrow::StringArray>(table->column(0)->chunk(0));
        auto qty_array = std::static_pointer_cast<arrow::DoubleArray>(table->column(1)->chunk(0));
        auto price_array = std::static_pointer_cast<arrow::DoubleArray>(table->column(2)->chunk(0));
        auto realized_array =
            std::static_pointer_cast<arrow::DoubleArray>(table->column(3)->chunk(0));
        auto unrealized_array =
            std::static_pointer_cast<arrow::DoubleArray>(table->column(4)->chunk(0));

        pos.symbol = symbol_array->GetString(i);
        pos.quantity = Decimal(qty_array->Value(i));
        pos.average_price = Decimal(price_array->Value(i));
        pos.realized_pnl = Decimal(realized_array->IsNull(i) ? 0.0 : realized_array->Value(i));
        pos.unrealized_pnl =
            Decimal(unrealized_array->IsNull(i) ? 0.0 : unrealized_array->Value(i));

        positions.push_back(pos);
    }

    INFO("Loaded " + std::to_string(positions.size()) + " positions for " + date_ss.str());
    return Result<std::vector<Position>>(positions);
}

Result<std::vector<Position>> LiveDataLoader::load_positions_for_export(
    const std::string& strategy_id, const std::string& portfolio_id, const Timestamp& date) {
    // For now, same as load_positions
    // Could be customized later for specific export requirements
    return load_positions(strategy_id, portfolio_id, date);
}

// Commission method (load_commissions_by_symbol) was deleted as dead code.
// See header comment for rationale.


Result<double> LiveDataLoader::load_daily_transaction_costs(const std::string& strategy_id,
                                                            const std::string& portfolio_id,
                                                            const Timestamp& date) {
    auto validation = validate_connection();
    if (validation.is_error()) {
        return make_error<double>(ErrorCode::DATABASE_ERROR, validation.error()->what(),
                                  "LiveDataLoader");
    }

    auto time_t = std::chrono::system_clock::to_time_t(date);
    std::stringstream date_ss;
    date_ss << std::put_time(std::gmtime(&time_t), "%Y-%m-%d");

    std::string actual_portfolio_id = portfolio_id.empty() ? "BASE_PORTFOLIO" : portfolio_id;

    std::string query =
        "SELECT COALESCE(daily_transaction_costs, 0.0) "
        "FROM " +
        schema_ +
        ".live_results "
        "WHERE " + std::string(schema_ == "trading" ? "portfolio_type = 'system' AND " : "") + "strategy_id = '" +
        strategy_id +
        "' "
        "AND portfolio_id = '" +
        actual_portfolio_id +
        "' "
        "AND DATE(date) = '" +
        date_ss.str() + "'";

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
        INFO("No transaction cost data found for " + date_ss.str());
        return Result<double>(0.0);
    }

    auto parsed=numeric_values(table,1);
    if(parsed.is_error()) return make_error<double>(ErrorCode::INVALID_DATA,parsed.error()->what());
    double transaction_costs=parsed.value()[0];

    return Result<double>(transaction_costs);
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

    auto time_t = std::chrono::system_clock::to_time_t(date);
    std::stringstream date_ss;
    date_ss << std::put_time(std::gmtime(&time_t), "%Y-%m-%d");

    std::string actual_portfolio_id = portfolio_id.empty() ? "BASE_PORTFOLIO" : portfolio_id;

    std::string query =
        "SELECT portfolio_leverage, equity_to_margin_ratio, "
        "gross_notional, margin_posted "
        "FROM " +
        schema_ +
        ".live_results "
        "WHERE " + std::string(schema_ == "trading" ? "portfolio_type = 'system' AND " : "") + "strategy_id = '" +
        strategy_id +
        "' "
        "AND portfolio_id = '" +
        actual_portfolio_id +
        "' "
        "AND DATE(date) = '" +
        date_ss.str() + "'";

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
        INFO("No margin metrics found for " + date_ss.str());
        return Result<MarginMetrics>(metrics);
    }

    auto parsed=numeric_values(table,4);
    if(parsed.is_error()) return make_error<MarginMetrics>(ErrorCode::INVALID_DATA,parsed.error()->what());
    metrics.gross_leverage=parsed.value()[0];metrics.equity_to_margin_ratio=parsed.value()[1];
    metrics.gross_notional=parsed.value()[2];metrics.margin_posted=parsed.value()[3];

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

    auto time_t = std::chrono::system_clock::to_time_t(date);
    std::stringstream date_ss;
    date_ss << std::put_time(std::gmtime(&time_t), "%Y-%m-%d");

    std::string actual_portfolio_id = portfolio_id.empty() ? "BASE_PORTFOLIO" : portfolio_id;

    std::string query =
        "SELECT daily_return, daily_unrealized_pnl, daily_realized_pnl, daily_pnl, "
        "daily_transaction_costs "
        "FROM " +
        schema_ +
        ".live_results "
        "WHERE " + std::string(schema_ == "trading" ? "portfolio_type = 'system' AND " : "") + "strategy_id = '" +
        strategy_id +
        "' "
        "AND portfolio_id = '" +
        actual_portfolio_id +
        "' "
        "AND DATE(date) = '" +
        date_ss.str() + "'";

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
        INFO("No daily metrics found for " + date_ss.str());
        return Result<std::unordered_map<std::string, double>>(metrics);
    }

    auto parsed=numeric_values(table,5);
    if(parsed.is_error()) return make_error<std::unordered_map<std::string,double>>(ErrorCode::INVALID_DATA,parsed.error()->what());
    metrics["Daily Return"]=parsed.value()[0];metrics["Daily Unrealized PnL"]=parsed.value()[1];
    metrics["Daily Realized PnL"]=parsed.value()[2];metrics["Daily Total PnL"]=parsed.value()[3];
    metrics["Daily Transaction Costs"]=parsed.value()[4];

    INFO("Loaded email metrics: Return=" + std::to_string(metrics["Daily Return"]) + "%");

    return Result<std::unordered_map<std::string, double>>(metrics);
}

Result<std::unordered_map<std::string,double>> LiveDataLoader::load_commissions_by_symbol(
    const std::string& strategy_id,const std::string& strategy_name,
    const std::string& portfolio_id,const Timestamp& date) {
    if(schema_!="trading" || strategy_id.empty() || strategy_name.empty() || portfolio_id.empty())
        return make_error<std::unordered_map<std::string,double>>(ErrorCode::INVALID_ARGUMENT,"equity_commission_owner_required");
    auto connection=validate_connection();
    if(connection.is_error())return make_error<std::unordered_map<std::string,double>>(
        ErrorCode::DATABASE_ERROR,connection.error()->what());
    const auto quote=[](const std::string& value) {
        std::string result="'";for(char character:value) {if(character=='\'') result+='\'';result+=character;}return result+"'";
    };
    const auto day=quote(core::format_utc_date(date));
    const std::string sql="SELECT symbol,SUM(commissions_fees) AS total_commission FROM trading.executions WHERE "
        "strategy_id="+quote(strategy_id)+" AND strategy_name="+quote(strategy_name)+
        " AND portfolio_id="+quote(portfolio_id)+" AND portfolio_type='system' AND execution_time >= "+day+
        "::date AT TIME ZONE 'UTC' AND execution_time < ("+day+
        "::date + INTERVAL '1 day') AT TIME ZONE 'UTC' GROUP BY symbol ORDER BY symbol";
    auto queried=db_->execute_query(sql);
    if(queried.is_error())return make_error<std::unordered_map<std::string,double>>(
        ErrorCode::DATABASE_ERROR,queried.error()->what());
    const auto table=queried.value();
    if(!table || table->num_columns()!=2)
        return make_error<std::unordered_map<std::string,double>>(ErrorCode::INVALID_DATA,"invalid_commission_result");
    std::unordered_map<std::string,double> values;
    for(int64_t row=0;row<table->num_rows();++row) {
        auto symbol=DataConversionUtils::safe_get_string(table->column(0),row,"symbol");
        auto value=DataConversionUtils::safe_get_double(table->column(1),row,"total_commission");
        if(symbol.is_error() || value.is_error() || symbol.value().empty() || value.value()<0 || values.count(symbol.value()))
            return make_error<std::unordered_map<std::string,double>>(ErrorCode::INVALID_DATA,"invalid_commission_result");
        values.emplace(symbol.value(),value.value());
    }
    return Result<std::unordered_map<std::string,double>>(values);
}

}  // namespace trade_ngin
