#include "trade_ngin/backtest/backtest_data_loader.hpp"
#include "trade_ngin/data/conversion_utils.hpp"
#include "trade_ngin/core/logger.hpp"
#include <algorithm>
#include <set>

namespace trade_ngin {
namespace backtest {

BacktestDataLoader::BacktestDataLoader(std::shared_ptr<PostgresDatabase> db)
    : source_(std::make_shared<PostgresMarketDataSource>(std::move(db))) {}

BacktestDataLoader::BacktestDataLoader(std::shared_ptr<MarketDataSource> source)
    : source_(std::move(source)) {}

Result<std::vector<Bar>> BacktestDataLoader::load_market_data(const DataLoadConfig& config) {
    if (!source_) {
        return make_error<std::vector<Bar>>(
            ErrorCode::NOT_INITIALIZED,
            "No market data source configured",
            "BacktestDataLoader");
    }

    // Validate symbols list
    if (config.symbols.empty()) {
        return make_error<std::vector<Bar>>(
            ErrorCode::INVALID_ARGUMENT,
            "Empty symbols list provided for backtest",
            "BacktestDataLoader");
    }

    // Delegate loading to the configured source. Batching and any provider
    // specific conversion is the source's responsibility.
    auto load_result = source_->load_bars(
        config.symbols, config.start_date, config.end_date,
        config.asset_class, config.data_freq);

    if (load_result.is_error()) {
        return make_error<std::vector<Bar>>(
            load_result.error()->code(),
            load_result.error()->what(),
            "BacktestDataLoader");
    }

    auto& all_bars = load_result.value();

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

} // namespace backtest
} // namespace trade_ngin
