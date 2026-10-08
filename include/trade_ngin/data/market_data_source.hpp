// include/trade_ngin/data/market_data_source.hpp

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/types.hpp"

namespace trade_ngin {

class PostgresDatabase;

/**
 * @brief Abstract source of historical market data for backtesting
 *
 * This is the narrow seam that decouples the backtest engine from any
 * particular data provider. Implementations return plain Bar objects, so a
 * source can be backed by PostgreSQL, a CSV file, a pandas DataFrame, or any
 * other provider without the caller building Arrow tables.
 */
class MarketDataSource {
public:
    virtual ~MarketDataSource() = default;

    /**
     * @brief Load historical bars for the given symbols and date range
     * @param symbols Symbols to load
     * @param start_date Inclusive start of the range
     * @param end_date Inclusive end of the range
     * @param asset_class Asset class of the symbols
     * @param freq Bar frequency
     * @return Bars for all requested symbols, or an error
     */
    virtual Result<std::vector<Bar>> load_bars(const std::vector<std::string>& symbols,
                                               const Timestamp& start_date,
                                               const Timestamp& end_date, AssetClass asset_class,
                                               DataFrequency freq) = 0;

    /**
     * @brief List the symbols this source can provide for an asset class
     * @param asset_class Asset class to enumerate
     * @return Available symbols, or an error
     */
    virtual Result<std::vector<std::string>> get_symbols(AssetClass asset_class) = 0;
};

/**
 * @brief MarketDataSource backed by the trade-ngin PostgreSQL database
 *
 * Wraps an existing PostgresDatabase connection so that the historical
 * database path and any custom source share a single code path in
 * BacktestDataLoader. This preserves the original batching and Arrow
 * conversion behaviour that previously lived in
 * BacktestDataLoader::load_symbol_batch.
 */
class PostgresMarketDataSource : public MarketDataSource {
public:
    /**
     * @brief Constructor
     * @param db Database connection to read through
     * @param data_type Table/data type to query (defaults to "ohlcv")
     * @param batch_size Maximum symbols per query
     */
    explicit PostgresMarketDataSource(std::shared_ptr<PostgresDatabase> db,
                                      std::string data_type = "ohlcv", size_t batch_size = 5);

    Result<std::vector<Bar>> load_bars(const std::vector<std::string>& symbols,
                                       const Timestamp& start_date, const Timestamp& end_date,
                                       AssetClass asset_class, DataFrequency freq) override;

    Result<std::vector<std::string>> get_symbols(AssetClass asset_class) override;

    /**
     * @brief Access the underlying database connection
     */
    const std::shared_ptr<PostgresDatabase>& database() const {
        return db_;
    }

private:
    /**
     * @brief Connect the underlying database if it is not already connected
     */
    Result<void> ensure_connection();

    /**
     * @brief Load a single batch of symbols and convert the Arrow result to Bars
     */
    Result<std::vector<Bar>> load_symbol_batch(const std::vector<std::string>& symbols,
                                               const Timestamp& start_date,
                                               const Timestamp& end_date, AssetClass asset_class,
                                               DataFrequency freq);

    std::shared_ptr<PostgresDatabase> db_;
    std::string data_type_;
    size_t batch_size_;
};

}  // namespace trade_ngin
