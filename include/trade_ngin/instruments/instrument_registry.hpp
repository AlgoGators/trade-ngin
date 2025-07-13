// include/trade_ngin/instruments/instrument_registry.hpp
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/instruments/equity.hpp"
#include "trade_ngin/instruments/futures.hpp"
#include "trade_ngin/instruments/instrument.hpp"
#include "trade_ngin/instruments/option.hpp"

namespace trade_ngin {

/**
 * @brief Central registry for trading instruments
 */
class InstrumentRegistry {
public:
    /**
     * @brief Get singleton instance of the registry
     */
    static InstrumentRegistry& instance() {
        static InstrumentRegistry instance;
        return instance;
    }

    /**
     * @brief Initialize the registry with a database connection
     * @param db Postgres database to use for loading instruments
     */
    Result<void> initialize(std::shared_ptr<PostgresDatabase> db);

    /**
     * @brief Get instrument by symbol
     * @param symbol Instrument symbol
     * @return Shared pointer to the instrument, or nullptr if not found
     */
    std::shared_ptr<Instrument> get_instrument(const std::string& symbol) const;

    /**
     * @brief Get futures instrument by symbol
     * @param symbol Futures symbol
     * @return Shared pointer to the futures instrument, or nullptr if not found or not a futures
     */
    std::shared_ptr<FuturesInstrument> get_futures_instrument(const std::string& symbol) const;

    /**
     * @brief Get equity instrument by symbol
     * @param symbol Equity symbol
     * @return Shared pointer to the equity instrument, or nullptr if not found or not an equity
     */
    std::shared_ptr<EquityInstrument> get_equity_instrument(const std::string& symbol) const;

    /**
     * @brief Get option instrument by symbol
     * @param symbol Option symbol
     * @return Shared pointer to the option instrument, or nullptr if not found or not an option
     */
    std::shared_ptr<OptionInstrument> get_option_instrument(const std::string& symbol) const;

    /**
     * @brief Load all instruments from the database
     * @return Result indicating success or failure
     */
    Result<void> load_instruments();

    /**
     * @brief Get all loaded instruments
     * @return Map of symbols to instruments
     */
    std::unordered_map<std::string, std::shared_ptr<Instrument>> get_all_instruments() const;

    /**
     * @brief Get all instruments of a specific asset class
     * @param asset_class Asset class to filter by
     * @return Vector of instruments of the specified asset class
     */
    std::vector<std::shared_ptr<Instrument>> get_instruments_by_asset_class(
        AssetClass asset_class) const;

    /**
     * @brief Check if an instrument is loaded
     * @param symbol Instrument symbol
     * @return True if the instrument is loaded
     */
    bool has_instrument(const std::string& symbol) const;

private:
    InstrumentRegistry() = default;  // Private constructor for singleton pattern
    InstrumentRegistry(const InstrumentRegistry&) = delete;
    InstrumentRegistry& operator=(const InstrumentRegistry&) = delete;
    InstrumentRegistry(InstrumentRegistry&&) = delete;
    InstrumentRegistry& operator=(InstrumentRegistry&&) = delete;

    /**
     * @brief Create an instrument from database data
     * @param row Database result row
     * @return Shared pointer to the created instrument
     */
    std::shared_ptr<Instrument> create_instrument_from_db(
        const std::shared_ptr<arrow::Table>& table, int64_t row);

    /**
     * @brief Convert asset type string to AssetType enum
     * @param asset_type_str Asset type string from database
     * @return AssetType enum value
     */
    AssetType string_to_asset_type(const std::string& asset_type_str) const;

    std::shared_ptr<PostgresDatabase> db_;
    std::unordered_map<std::string, std::shared_ptr<Instrument>> instruments_;
    mutable std::mutex mutex_;
    bool initialized_{false};
};

}  // namespace trade_ngin