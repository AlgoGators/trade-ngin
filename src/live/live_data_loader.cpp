// src/live/live_data_loader.cpp
// Implementation of data loading component for live trading

#include "trade_ngin/live/live_data_loader.hpp"
#include <arrow/api.h>
#include <iomanip>
#include <sstream>
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/types.hpp"  // For Position struct

namespace trade_ngin {

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
        "WHERE strategy_id = '" +
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

    auto array = std::static_pointer_cast<arrow::DoubleArray>(table->column(0)->chunk(0));
    double value = array->Value(0);

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
        "WHERE strategy_id = '" +
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

    auto array = std::static_pointer_cast<arrow::DoubleArray>(table->column(0)->chunk(0));
    double value = array->Value(0);

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
        "active_positions, winning_trades, losing_trades "
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
    int col = 0;
    auto get_double = [&table, &col]() -> double {
        auto array = std::static_pointer_cast<arrow::DoubleArray>(table->column(col++)->chunk(0));
        return array->IsNull(0) ? 0.0 : array->Value(0);
    };

    auto get_int = [&table, &col]() -> int {
        auto array = std::static_pointer_cast<arrow::Int64Array>(table->column(col++)->chunk(0));
        return array->IsNull(0) ? 0 : static_cast<int>(array->Value(0));
    };

    row.daily_pnl = get_double();
    row.total_pnl = get_double();
    row.daily_realized_pnl = get_double();
    row.daily_unrealized_pnl = get_double();
    row.daily_return = get_double();
    row.total_cumulative_return = get_double();
    row.total_annualized_return = get_double();
    row.current_portfolio_value = get_double();
    row.portfolio_leverage = get_double();
    row.equity_to_margin_ratio = get_double();
    row.gross_notional = get_double();
    row.margin_posted = get_double();
    row.cash_available = get_double();
    row.daily_transaction_costs = get_double();
    row.sharpe_ratio = get_double();
    row.sortino_ratio = get_double();
    row.max_drawdown = get_double();
    row.volatility = get_double();
    row.active_positions = get_int();
    row.winning_trades = get_int();
    row.losing_trades = get_int();

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
        "WHERE strategy_id = '" +
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
    auto portfolio_array = std::static_pointer_cast<arrow::DoubleArray>(table->column(0)->chunk(0));
    auto total_pnl_array = std::static_pointer_cast<arrow::DoubleArray>(table->column(1)->chunk(0));
    auto daily_pnl_array = std::static_pointer_cast<arrow::DoubleArray>(table->column(2)->chunk(0));
    auto commissions_array =
        std::static_pointer_cast<arrow::DoubleArray>(table->column(3)->chunk(0));
    auto date_array = std::static_pointer_cast<arrow::TimestampArray>(table->column(4)->chunk(0));

    data.portfolio_value = portfolio_array->IsNull(0) ? 0.0 : portfolio_array->Value(0);
    data.total_pnl = total_pnl_array->IsNull(0) ? 0.0 : total_pnl_array->Value(0);
    data.daily_pnl = daily_pnl_array->IsNull(0) ? 0.0 : daily_pnl_array->Value(0);
    data.daily_transaction_costs = commissions_array->IsNull(0) ? 0.0 : commissions_array->Value(0);

    // Convert timestamp
    int64_t timestamp_us = date_array->Value(0);
    auto duration = std::chrono::microseconds(timestamp_us);
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

    auto time_t = std::chrono::system_clock::to_time_t(date);
    std::stringstream date_ss;
    date_ss << std::put_time(std::gmtime(&time_t), "%Y-%m-%d");

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

    auto array = std::static_pointer_cast<arrow::Int64Array>(table->column(0)->chunk(0));
    int64_t count = array->Value(0);

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

    auto array = std::static_pointer_cast<arrow::Int64Array>(table->column(0)->chunk(0));
    int count = static_cast<int>(array->Value(0));

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

// ========== Commission Methods ==========

Result<std::unordered_map<std::string, double>> LiveDataLoader::load_commissions_by_symbol(
    const std::string& portfolio_id, const Timestamp& date) {
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
        "SELECT symbol, COALESCE(SUM(commission), 0.0) as total_commission "
        "FROM " +
        schema_ +
        ".executions "
        "WHERE portfolio_id = '" +
        actual_portfolio_id + "' AND DATE(execution_time) = '" + date_ss.str() +
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
        INFO("No commissions found for " + date_ss.str());
        return Result<std::unordered_map<std::string, double>>(commissions);
    }

    for (int64_t i = 0; i < table->num_rows(); ++i) {
        auto symbol_array =
            std::static_pointer_cast<arrow::StringArray>(table->column(0)->chunk(0));
        auto commission_array =
            std::static_pointer_cast<arrow::DoubleArray>(table->column(1)->chunk(0));

        std::string symbol = symbol_array->GetString(i);
        double commission = commission_array->Value(i);
        commissions[symbol] = commission;
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

    auto time_t = std::chrono::system_clock::to_time_t(date);
    std::stringstream date_ss;
    date_ss << std::put_time(std::gmtime(&time_t), "%Y-%m-%d");

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

    auto array = std::static_pointer_cast<arrow::DoubleArray>(table->column(0)->chunk(0));
    double transaction_costs = array->IsNull(0) ? 0.0 : array->Value(0);

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
        "WHERE strategy_id = '" +
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

    auto leverage_array = std::static_pointer_cast<arrow::DoubleArray>(table->column(0)->chunk(0));
    auto equity_ratio_array =
        std::static_pointer_cast<arrow::DoubleArray>(table->column(1)->chunk(0));
    auto notional_array = std::static_pointer_cast<arrow::DoubleArray>(table->column(2)->chunk(0));
    auto margin_array = std::static_pointer_cast<arrow::DoubleArray>(table->column(3)->chunk(0));

    metrics.portfolio_leverage = leverage_array->IsNull(0) ? 0.0 : leverage_array->Value(0);
    metrics.equity_to_margin_ratio =
        equity_ratio_array->IsNull(0) ? 0.0 : equity_ratio_array->Value(0);
    metrics.gross_notional = notional_array->IsNull(0) ? 0.0 : notional_array->Value(0);
    metrics.margin_posted = margin_array->IsNull(0) ? 0.0 : margin_array->Value(0);

    // Calculate margin cushion
    if (metrics.margin_posted > 0) {
        metrics.margin_cushion = (metrics.equity_to_margin_ratio - 1.0) * 100.0;
    }

    metrics.valid = true;

    INFO("Loaded margin metrics: Leverage=" + std::to_string(metrics.portfolio_leverage) +
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
        "WHERE strategy_id = '" +
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

    auto daily_return = std::static_pointer_cast<arrow::DoubleArray>(table->column(0)->chunk(0));
    auto daily_unrealized =
        std::static_pointer_cast<arrow::DoubleArray>(table->column(1)->chunk(0));
    auto daily_realized = std::static_pointer_cast<arrow::DoubleArray>(table->column(2)->chunk(0));
    auto daily_total = std::static_pointer_cast<arrow::DoubleArray>(table->column(3)->chunk(0));
    auto daily_transaction_costs =
        std::static_pointer_cast<arrow::DoubleArray>(table->column(4)->chunk(0));

    metrics["Daily Return"] = daily_return->IsNull(0) ? 0.0 : daily_return->Value(0);
    metrics["Daily Unrealized PnL"] =
        daily_unrealized->IsNull(0) ? 0.0 : daily_unrealized->Value(0);
    metrics["Daily Realized PnL"] = daily_realized->IsNull(0) ? 0.0 : daily_realized->Value(0);
    metrics["Daily Total PnL"] = daily_total->IsNull(0) ? 0.0 : daily_total->Value(0);
    metrics["Daily Transaction Costs"] = daily_transaction_costs->IsNull(0) ? 0.0 : daily_transaction_costs->Value(0);

    INFO("Loaded email metrics: Return=" + std::to_string(metrics["Daily Return"]) + "%");

    return Result<std::unordered_map<std::string, double>>(metrics);
}

}  // namespace trade_ngin