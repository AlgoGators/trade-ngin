#pragma once

#include <memory>
#include <vector>
#include <map>
#include <unordered_map>
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/data/postgres_database.hpp"

namespace trade_ngin {
namespace backtest {

/**
 * @brief Configuration for data loading
 */
struct DataLoadConfig {
    std::vector<std::string> symbols;
    Timestamp start_date;
    Timestamp end_date;
    AssetClass asset_class = AssetClass::FUTURES;
    DataFrequency data_freq = DataFrequency::DAILY;
    std::string data_type = "ohlcv";
    size_t batch_size = 5;  // Max symbols per batch query
};

/**
 * @brief Encapsulates PostgreSQL batch data loading for backtesting
 *
 * This class extracts the load_market_data() method from BacktestEngine
 * (lines 1909-2082). It provides:
 * - Batch loading of market data from database
 * - Data quality validation
 * - Grouping bars by timestamp
 *
 * Design principles:
 * - Stateless (only holds database reference)
 * - Returns Result types for error handling
 * - No modification of external state
 */
class BacktestDataLoader {
public:
    /**
     * @brief Constructor
     * @param db Database connection
     */
    explicit BacktestDataLoader(std::shared_ptr<PostgresDatabase> db);

    ~BacktestDataLoader() = default;

    /**
     * @brief Load market data for backtest
     *
     * Loads all market data for the configured symbols and date range,
     * handling batch loading and data conversion.
     *
     * @param config Data loading configuration
     * @return Vector of Bar objects or error
     */
    Result<std::vector<Bar>> load_market_data(const DataLoadConfig& config);

    /**
     * @brief Group bars by timestamp
     *
     * Organizes bars into a map where each timestamp contains
     * all bars for that time period (useful for day-by-day processing).
     *
     * @param bars Vector of bars to group
     * @return Map of timestamp to bars for that timestamp
     */
    std::map<Timestamp, std::vector<Bar>> group_bars_by_timestamp(
        const std::vector<Bar>& bars) const;

    /**
     * @brief Validate data quality
     *
     * Checks that loaded data has reasonable characteristics:
     * - At least some price movement
     * - No obvious data errors
     *
     * @param bars Vector of bars to validate
     * @return Success or error with validation message
     */
    Result<void> validate_data_quality(const std::vector<Bar>& bars) const;

    /**
     * @brief Get unique symbols from bars
     */
    std::vector<std::string> get_unique_symbols(const std::vector<Bar>& bars) const;

    /**
     * @brief Get date range from bars
     * @return Pair of (min_timestamp, max_timestamp)
     */
    std::pair<Timestamp, Timestamp> get_date_range(const std::vector<Bar>& bars) const;

    /**
     * @brief Get price statistics for a symbol
     * @return Map containing min_price, max_price, price_range_pct
     */
    std::unordered_map<std::string, double> get_price_statistics(
        const std::vector<Bar>& bars,
        const std::string& symbol) const;

private:
    std::shared_ptr<PostgresDatabase> db_;

    /**
     * @brief Load a batch of symbols
     */
    Result<std::vector<Bar>> load_symbol_batch(
        const std::vector<std::string>& symbols,
        const DataLoadConfig& config);

    /**
     * @brief Ensure database connection
     */
    Result<void> ensure_connection();
};

} // namespace backtest
} // namespace trade_ngin
