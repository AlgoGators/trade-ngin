#include "trade_ngin/backtest/backtest_data_loader.hpp"
#include "trade_ngin/data/conversion_utils.hpp"
#include "trade_ngin/core/logger.hpp"
#include <algorithm>
#include <set>

namespace trade_ngin {
namespace backtest {

/**
 * @brief Resolve table name based on data source
 *
 * Selects the appropriate schema and constructs the full table name:
 * - REAL: delegates to global build_table_name() to use asset-class schemas
 * - SYNTHETIC: returns table in synthetic schema (asset class does not change the schema)
 *
 * This function stays in the backtest namespace and is NOT exported to the live path.
 * The live path never calls build_table_name_with_source; it calls get_market_data with
 * AssetClass only, which has no way to express SYNTHETIC.
 *
 * @param asset_class Asset class (only used for REAL; ignored for SYNTHETIC)
 * @param data_type Base table name (e.g., "ohlcv")
 * @param freq Data frequency
 * @param source Data source (REAL or SYNTHETIC)
 * @return Full table name (e.g., "synthetic.ohlcv_1d" or "futures_data.ohlcv_1d")
 */
static std::string build_table_name_with_source(
    AssetClass asset_class,
    const std::string& data_type,
    DataFrequency freq,
    DataSource source) {
    if (source == DataSource::SYNTHETIC) {
        // All asset classes use the same synthetic schema; asset_class parameter is ignored
        return "synthetic." + data_type + "_" + get_table_suffix(freq);
    }
    // REAL: delegate to global function that uses asset_class to pick the schema
    return build_table_name(asset_class, data_type, freq);
}

BacktestDataLoader::BacktestDataLoader(std::shared_ptr<PostgresDatabase> db)
    : db_(std::move(db)) {}

Result<std::vector<Bar>> BacktestDataLoader::load_market_data(const DataLoadConfig& config) {
    // Reject options upfront: data-ngin has zero options ingestion (no fetcher, no schema).
    // The options_data schema does not exist and will not for the foreseeable future.
    // This is the honest failure: loud and actionable, not a silent SQL error.
    if (config.asset_class == AssetClass::OPTIONS) {
        return make_error<std::vector<Bar>>(
            ErrorCode::MARKET_DATA_ERROR,
            "Options market data is not yet ingested by data-ngin. "
            "No fetcher, schema, or DAG exists for options. "
            "See data-ngin#39 for ingest roadmap.",
            "BacktestDataLoader");
    }

    // Ensure database connection
    auto conn_result = ensure_connection();
    if (conn_result.is_error()) {
        return make_error<std::vector<Bar>>(
            conn_result.error()->code(),
            conn_result.error()->what(),
            "BacktestDataLoader");
    }

    // Validate symbols list
    if (config.symbols.empty()) {
        return make_error<std::vector<Bar>>(
            ErrorCode::INVALID_ARGUMENT,
            "Empty symbols list provided for backtest",
            "BacktestDataLoader");
    }

    // Load market data in batches
    std::vector<Bar> all_bars;
    size_t batch_size = config.batch_size > 0 ? config.batch_size : 5;

    for (size_t i = 0; i < config.symbols.size(); i += batch_size) {
        // Create a batch of symbols
        size_t end_idx = std::min(i + batch_size, config.symbols.size());
        std::vector<std::string> symbol_batch(
            config.symbols.begin() + i,
            config.symbols.begin() + end_idx);

        // Load this batch
        auto batch_result = load_symbol_batch(symbol_batch, config);
        if (batch_result.is_error()) {
            WARN("Error loading data for symbols batch " + std::to_string(i) + "-" +
                 std::to_string(end_idx) + ": " + batch_result.error()->what() +
                 ". Continuing with other batches.");
            continue;
        }

        auto& batch_bars = batch_result.value();
        all_bars.insert(all_bars.end(), batch_bars.begin(), batch_bars.end());
    }

    // Check for empty data
    if (all_bars.empty()) {
        return make_error<std::vector<Bar>>(
            ErrorCode::MARKET_DATA_ERROR,
            "No market data loaded for backtest",
            "BacktestDataLoader");
    }

    // Validate data quality
    auto validation_result = validate_data_quality(all_bars);
    if (validation_result.is_error()) {
        WARN(validation_result.error()->what());
        // Don't fail, just warn
    }

    INFO("Loaded a total of " + std::to_string(all_bars.size()) + " bars for " +
         std::to_string(config.symbols.size()) + " symbols");

    return Result<std::vector<Bar>>(all_bars);
}

std::map<Timestamp, std::vector<Bar>> BacktestDataLoader::group_bars_by_timestamp(
    const std::vector<Bar>& bars) const {
    std::map<Timestamp, std::vector<Bar>> grouped;

    for (const auto& bar : bars) {
        grouped[bar.timestamp].push_back(bar);
    }

    return grouped;
}

Result<void> BacktestDataLoader::validate_data_quality(const std::vector<Bar>& bars) const {
    if (bars.empty()) {
        return make_error<void>(
            ErrorCode::INVALID_DATA,
            "Empty bars vector provided for validation",
            "BacktestDataLoader");
    }

    // Check price movement for each symbol
    std::unordered_map<std::string, double> min_prices, max_prices;

    for (const auto& bar : bars) {
        double close = static_cast<double>(bar.close);
        if (min_prices.find(bar.symbol) == min_prices.end()) {
            min_prices[bar.symbol] = close;
            max_prices[bar.symbol] = close;
        } else {
            min_prices[bar.symbol] = std::min(min_prices[bar.symbol], close);
            max_prices[bar.symbol] = std::max(max_prices[bar.symbol], close);
        }
    }

    bool has_price_movement = false;
    for (const auto& [symbol, min_price] : min_prices) {
        double max_price = max_prices[symbol];
        double price_range_pct = min_price > 0 ?
            (max_price - min_price) / min_price * 100.0 : 0.0;

        if (price_range_pct > 1.0) {  // At least 1% price movement
            has_price_movement = true;
            break;
        }
    }

    if (!has_price_movement) {
        return make_error<void>(
            ErrorCode::INVALID_DATA,
            "No significant price movement detected in market data. "
            "Strategy may not generate signals.",
            "BacktestDataLoader");
    }

    return Result<void>();
}

std::vector<std::string> BacktestDataLoader::get_unique_symbols(const std::vector<Bar>& bars) const {
    std::set<std::string> symbol_set;
    for (const auto& bar : bars) {
        symbol_set.insert(bar.symbol);
    }
    return std::vector<std::string>(symbol_set.begin(), symbol_set.end());
}

std::pair<Timestamp, Timestamp> BacktestDataLoader::get_date_range(
    const std::vector<Bar>& bars) const {
    if (bars.empty()) {
        return {Timestamp{}, Timestamp{}};
    }

    auto [min_it, max_it] = std::minmax_element(bars.begin(), bars.end(),
        [](const Bar& a, const Bar& b) { return a.timestamp < b.timestamp; });

    return {min_it->timestamp, max_it->timestamp};
}

std::unordered_map<std::string, double> BacktestDataLoader::get_price_statistics(
    const std::vector<Bar>& bars,
    const std::string& symbol) const {
    std::unordered_map<std::string, double> stats;
    stats["min_price"] = 0.0;
    stats["max_price"] = 0.0;
    stats["price_range_pct"] = 0.0;

    bool first = true;
    for (const auto& bar : bars) {
        if (bar.symbol != symbol) continue;

        double close = static_cast<double>(bar.close);
        if (first) {
            stats["min_price"] = close;
            stats["max_price"] = close;
            first = false;
        } else {
            stats["min_price"] = std::min(stats["min_price"], close);
            stats["max_price"] = std::max(stats["max_price"], close);
        }
    }

    if (stats["min_price"] > 0) {
        stats["price_range_pct"] =
            (stats["max_price"] - stats["min_price"]) / stats["min_price"] * 100.0;
    }

    return stats;
}

Result<std::vector<Bar>> BacktestDataLoader::load_symbol_batch(
    const std::vector<std::string>& symbols,
    const DataLoadConfig& config) {
    // Route to synthetic path if requested. This ensures SYNTHETIC data stays confined
    // to the backtest namespace and is never accessible to the live trading path.
    if (config.data_source == DataSource::SYNTHETIC) {
        return load_symbol_batch_synthetic(symbols, config);
    }

    // REAL data: use the standard database path
    try {
        auto result = db_->get_market_data(
            symbols, config.start_date, config.end_date,
            config.asset_class, config.data_freq, config.data_type);

        if (result.is_error()) {
            return make_error<std::vector<Bar>>(
                result.error()->code(),
                result.error()->what(),
                "BacktestDataLoader");
        }

        auto arrow_table = result.value();
        if (arrow_table->num_rows() == 0) {
            return make_error<std::vector<Bar>>(
                ErrorCode::DATA_NOT_FOUND,
                "Market data query returned an empty table",
                "BacktestDataLoader");
        }

        // Convert Arrow table to Bars
        auto conversion_result = DataConversionUtils::arrow_table_to_bars(arrow_table);
        if (conversion_result.is_error()) {
            return make_error<std::vector<Bar>>(
                conversion_result.error()->code(),
                conversion_result.error()->what(),
                "BacktestDataLoader");
        }

        return conversion_result;

    } catch (const std::exception& e) {
        return make_error<std::vector<Bar>>(
            ErrorCode::UNKNOWN_ERROR,
            std::string("Exception loading market data: ") + e.what(),
            "BacktestDataLoader");
    }
}

Result<std::vector<Bar>> BacktestDataLoader::load_symbol_batch_synthetic(
    const std::vector<std::string>& symbols,
    const DataLoadConfig& config) {
    try {
        // Construct the table name pointing to the synthetic schema
        std::string table_name = build_table_name_with_source(
            config.asset_class, config.data_type, config.data_freq, DataSource::SYNTHETIC);

        // Call the low-level method that accepts an explicit table name.
        // This keeps the synthetic schema confined to the backtest namespace.
        auto result = db_->get_market_data_from_table(
            symbols, config.start_date, config.end_date,
            table_name, config.asset_class, config.data_freq);

        if (result.is_error()) {
            return make_error<std::vector<Bar>>(
                result.error()->code(),
                result.error()->what(),
                "BacktestDataLoader");
        }

        auto arrow_table = result.value();
        if (arrow_table->num_rows() == 0) {
            return make_error<std::vector<Bar>>(
                ErrorCode::DATA_NOT_FOUND,
                "Synthetic market data query returned an empty table",
                "BacktestDataLoader");
        }

        // Convert Arrow table to Bars
        auto conversion_result = DataConversionUtils::arrow_table_to_bars(arrow_table);
        if (conversion_result.is_error()) {
            return make_error<std::vector<Bar>>(
                conversion_result.error()->code(),
                conversion_result.error()->what(),
                "BacktestDataLoader");
        }

        return conversion_result;

    } catch (const std::exception& e) {
        return make_error<std::vector<Bar>>(
            ErrorCode::UNKNOWN_ERROR,
            std::string("Exception loading synthetic market data: ") + e.what(),
            "BacktestDataLoader");
    }
}

Result<void> BacktestDataLoader::ensure_connection() {
    if (!db_) {
        return make_error<void>(
            ErrorCode::CONNECTION_ERROR,
            "Database interface is null",
            "BacktestDataLoader");
    }

    if (!db_->is_connected()) {
        auto connect_result = db_->connect();
        if (connect_result.is_error()) {
            return make_error<void>(
                connect_result.error()->code(),
                "Failed to connect to database: " + std::string(connect_result.error()->what()),
                "BacktestDataLoader");
        }
    }

    return Result<void>();
}

} // namespace backtest
} // namespace trade_ngin
