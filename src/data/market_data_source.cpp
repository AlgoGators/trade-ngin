// src/data/market_data_source.cpp

#include "trade_ngin/data/market_data_source.hpp"

#include <algorithm>

#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/data/conversion_utils.hpp"
#include "trade_ngin/data/postgres_database.hpp"

namespace trade_ngin {

PostgresMarketDataSource::PostgresMarketDataSource(std::shared_ptr<PostgresDatabase> db,
                                                   std::string data_type, size_t batch_size)
    : db_(std::move(db)),
      data_type_(std::move(data_type)),
      batch_size_(batch_size > 0 ? batch_size : 5) {}

Result<void> PostgresMarketDataSource::ensure_connection() {
    if (!db_) {
        return make_error<void>(ErrorCode::CONNECTION_ERROR, "Database interface is null",
                                "PostgresMarketDataSource");
    }

    if (!db_->is_connected()) {
        auto connect_result = db_->connect();
        if (connect_result.is_error()) {
            return make_error<void>(
                connect_result.error()->code(),
                "Failed to connect to database: " + std::string(connect_result.error()->what()),
                "PostgresMarketDataSource");
        }
    }

    return Result<void>();
}

Result<std::vector<Bar>> PostgresMarketDataSource::load_symbol_batch(
    const std::vector<std::string>& symbols, const Timestamp& start_date, const Timestamp& end_date,
    AssetClass asset_class, DataFrequency freq) {
    try {
        auto result =
            db_->get_market_data(symbols, start_date, end_date, asset_class, freq, data_type_);

        if (result.is_error()) {
            return make_error<std::vector<Bar>>(result.error()->code(), result.error()->what(),
                                                "PostgresMarketDataSource");
        }

        auto arrow_table = result.value();
        if (arrow_table->num_rows() == 0) {
            return make_error<std::vector<Bar>>(ErrorCode::DATA_NOT_FOUND,
                                                "Market data query returned an empty table",
                                                "PostgresMarketDataSource");
        }

        // Convert Arrow table to Bars
        auto conversion_result = DataConversionUtils::arrow_table_to_bars(arrow_table);
        if (conversion_result.is_error()) {
            return make_error<std::vector<Bar>>(conversion_result.error()->code(),
                                                conversion_result.error()->what(),
                                                "PostgresMarketDataSource");
        }

        return conversion_result;

    } catch (const std::exception& e) {
        return make_error<std::vector<Bar>>(
            ErrorCode::UNKNOWN_ERROR, std::string("Exception loading market data: ") + e.what(),
            "PostgresMarketDataSource");
    }
}

Result<std::vector<Bar>> PostgresMarketDataSource::load_bars(
    const std::vector<std::string>& symbols, const Timestamp& start_date, const Timestamp& end_date,
    AssetClass asset_class, DataFrequency freq) {
    auto conn_result = ensure_connection();
    if (conn_result.is_error()) {
        return make_error<std::vector<Bar>>(conn_result.error()->code(), conn_result.error()->what(),
                                            "PostgresMarketDataSource");
    }

    std::vector<Bar> all_bars;

    for (size_t i = 0; i < symbols.size(); i += batch_size_) {
        size_t end_idx = std::min(i + batch_size_, symbols.size());
        std::vector<std::string> symbol_batch(symbols.begin() + i, symbols.begin() + end_idx);

        auto batch_result =
            load_symbol_batch(symbol_batch, start_date, end_date, asset_class, freq);
        if (batch_result.is_error()) {
            WARN("Error loading data for symbols batch " + std::to_string(i) + "-" +
                 std::to_string(end_idx) + ": " + batch_result.error()->what() +
                 ". Continuing with other batches.");
            continue;
        }

        auto& batch_bars = batch_result.value();
        all_bars.insert(all_bars.end(), batch_bars.begin(), batch_bars.end());
    }

    return Result<std::vector<Bar>>(all_bars);
}

Result<std::vector<std::string>> PostgresMarketDataSource::get_symbols(AssetClass asset_class) {
    auto conn_result = ensure_connection();
    if (conn_result.is_error()) {
        return make_error<std::vector<std::string>>(
            conn_result.error()->code(), conn_result.error()->what(), "PostgresMarketDataSource");
    }

    return db_->get_symbols(asset_class);
}

}  // namespace trade_ngin
