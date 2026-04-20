#include "trade_ngin/backtest/backtest_data_loader.hpp"
#include "trade_ngin/data/conversion_utils.hpp"
#include "trade_ngin/data/csv_equity_loader.hpp"
#include "trade_ngin/core/logger.hpp"
#include <algorithm>
#include <iterator>
#include <map>
#include <set>
#include <sstream>

namespace trade_ngin {
namespace backtest {

BacktestDataLoader::BacktestDataLoader(std::shared_ptr<PostgresDatabase> db)
    : db_(std::move(db)) {}

Result<std::vector<Bar>> BacktestDataLoader::load_market_data(const DataLoadConfig& config) {
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
    std::vector<std::string> failed_symbols;

    auto join_symbols = [](const std::vector<std::string>& syms) {
        std::string joined;
        for (size_t j = 0; j < syms.size(); ++j) {
            if (j > 0) joined += ", ";
            joined += syms[j];
        }
        return joined;
    };

    for (size_t i = 0; i < config.symbols.size(); i += batch_size) {
        // Create a batch of symbols
        size_t end_idx = std::min(i + batch_size, config.symbols.size());
        std::vector<std::string> symbol_batch(
            config.symbols.begin() + i,
            config.symbols.begin() + end_idx);

        // Load this batch
        auto batch_result = load_symbol_batch(symbol_batch, config);
        if (batch_result.is_error()) {
            ERROR("Failed to load data for symbols batch " + std::to_string(i) + "-" +
                  std::to_string(end_idx) + " [" + join_symbols(symbol_batch) + "]: " +
                  batch_result.error()->what());
            for (const auto& sym : symbol_batch) failed_symbols.push_back(sym);
            continue;
        }

        auto& batch_bars = batch_result.value();
        INFO("Batch " + std::to_string(i / batch_size) + " [" + join_symbols(symbol_batch) +
             "]: loaded " + std::to_string(batch_bars.size()) + " bars");
        all_bars.insert(all_bars.end(), batch_bars.begin(), batch_bars.end());
    }

    // Fail fast on any missing symbol batch. Continuing with partial data produces
    // misleading results: the strategy thinks it has a 10-symbol universe but is
    // silently sizing 5 of them to zero.
    if (!failed_symbols.empty()) {
        return make_error<std::vector<Bar>>(
            ErrorCode::MARKET_DATA_ERROR,
            "Missing market data for symbols: [" + join_symbols(failed_symbols) +
                "]. Aborting backtest — partial-load results are not trustworthy.",
            "BacktestDataLoader");
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
    try {
        // 1. Try the DB first.
        std::vector<Bar> bars;
        auto result = db_->get_market_data(
            symbols, config.start_date, config.end_date,
            config.asset_class, config.data_freq, config.data_type);

        if (result.is_ok()) {
            auto arrow_table = result.value();
            if (arrow_table && arrow_table->num_rows() > 0) {
                auto conversion_result = DataConversionUtils::arrow_table_to_bars(arrow_table);
                if (conversion_result.is_error()) {
                    return make_error<std::vector<Bar>>(
                        conversion_result.error()->code(),
                        conversion_result.error()->what(),
                        "BacktestDataLoader");
                }
                bars = std::move(conversion_result.value());
            }
        }
        // Don't short-circuit on DB error/empty — fall through to CSV fallback.

        // 2. For EQUITIES only, try a local CSV fallback for any symbols the DB didn't cover.
        //    This is how we support tickers (yfinance-sourced ETFs) that deliberately aren't
        //    written to Postgres — the DB stays a single-source-of-truth for Databento rows.
        if (config.asset_class == AssetClass::EQUITIES) {
            std::set<std::string> covered;
            for (const auto& bar : bars) covered.insert(bar.symbol);

            std::map<std::string, size_t> csv_counts;
            size_t csv_total = 0;
            for (const auto& sym : symbols) {
                if (covered.count(sym)) continue;

                auto csv_result = CSVEquityLoader::load(sym, config.start_date, config.end_date);
                if (csv_result.is_error()) {
                    // CSV miss is not fatal here — the batch-level check in load_market_data
                    // will still abort with a clear error if this symbol ends up with zero bars.
                    continue;
                }
                auto& csv_bars = csv_result.value();
                csv_counts[sym] = csv_bars.size();
                csv_total += csv_bars.size();
                bars.insert(bars.end(),
                            std::make_move_iterator(csv_bars.begin()),
                            std::make_move_iterator(csv_bars.end()));
            }

            if (csv_total > 0) {
                std::ostringstream msg;
                msg << "CSV fallback loaded " << csv_total << " bars (";
                bool first = true;
                for (const auto& [sym, n] : csv_counts) {
                    if (!first) msg << ", ";
                    first = false;
                    msg << sym << ":" << n;
                }
                msg << ")";
                INFO(msg.str());
            }
        }

        if (bars.empty()) {
            // Surface the original DB error if we had one, otherwise a generic miss.
            std::string why = result.is_error()
                ? std::string(result.error()->what())
                : std::string("Market data query returned an empty table (DB + CSV fallback)");
            return make_error<std::vector<Bar>>(
                ErrorCode::DATA_NOT_FOUND, why, "BacktestDataLoader");
        }

        return Result<std::vector<Bar>>(std::move(bars));

    } catch (const std::exception& e) {
        return make_error<std::vector<Bar>>(
            ErrorCode::UNKNOWN_ERROR,
            std::string("Exception loading market data: ") + e.what(),
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
