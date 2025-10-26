#include "trade_ngin/live/live_price_manager.hpp"
#include "trade_ngin/core/logger.hpp"
#include <sstream>
#include <algorithm>

namespace trade_ngin {

Result<std::unordered_map<std::string, double>> LivePriceManager::load_close_prices(
    const std::vector<std::string>& symbols,
    const Timestamp& date) const {

    if (symbols.empty()) {
        return Result<std::unordered_map<std::string, double>>({});
    }

    // TODO: Implement Arrow Table parsing
    // For now, this is a placeholder that will be integrated with existing database patterns
    std::unordered_map<std::string, double> prices;

    INFO("LivePriceManager::load_close_prices called for " + std::to_string(symbols.size()) +
         " symbols");  // TODO: Add date formatting

    return Result<std::unordered_map<std::string, double>>(prices);
}

Result<void> LivePriceManager::load_previous_day_prices(
    const std::vector<std::string>& symbols,
    const Timestamp& current_date) {

    // Calculate T-1 date
    auto previous_date = current_date - std::chrono::hours(24);

    auto result = load_close_prices(symbols, previous_date);
    if (!result.is_ok()) {
        return make_error<void>(ErrorCode::DATABASE_ERROR, "Failed to load T-1 prices");
    }

    previous_day_prices_ = result.value();

    INFO("Loaded " + std::to_string(previous_day_prices_.size()) +
         " previous day (T-1) close prices");

    return Result<void>();
}

Result<void> LivePriceManager::load_two_days_ago_prices(
    const std::vector<std::string>& symbols,
    const Timestamp& current_date) {

    // Calculate T-2 date
    auto two_days_ago = current_date - std::chrono::hours(48);

    auto result = load_close_prices(symbols, two_days_ago);
    if (!result.is_ok()) {
        return make_error<void>(ErrorCode::DATABASE_ERROR, "Failed to load T-2 prices");
    }

    two_days_ago_prices_ = result.value();

    INFO("Loaded " + std::to_string(two_days_ago_prices_.size()) +
         " two days ago (T-2) close prices");

    return Result<void>();
}

Result<void> LivePriceManager::update_from_bars(const std::vector<Bar>& bars) {
    // Group bars by symbol and sort by timestamp to extract T-1 and T-2 prices
    std::unordered_map<std::string, std::vector<Bar>> bars_by_symbol;
    for (const auto& bar : bars) {
        bars_by_symbol[bar.symbol].push_back(bar);
    }

    // Clear previous prices before updating
    previous_day_prices_.clear();
    two_days_ago_prices_.clear();
    latest_prices_.clear();

    for (auto& [symbol, symbol_bars] : bars_by_symbol) {
        if (!symbol_bars.empty()) {
            // Sort by timestamp to ensure proper ordering
            std::sort(symbol_bars.begin(), symbol_bars.end(),
                     [](const Bar& a, const Bar& b) { return a.timestamp < b.timestamp; });

            // Last bar is Day T-1 (yesterday's close)
            double yesterday_close = static_cast<double>(symbol_bars.back().close);
            previous_day_prices_[symbol] = yesterday_close;
            latest_prices_[symbol] = yesterday_close;  // Also update latest prices

            auto last_bar_time = std::chrono::system_clock::to_time_t(symbol_bars.back().timestamp);
            DEBUG("Day T-1 close for " + symbol + ": " + std::to_string(yesterday_close) +
                  " (from " + std::to_string(last_bar_time) + ")");

            // Second-to-last bar is Day T-2 (two days ago close) - CRITICAL for PnL calculation
            if (symbol_bars.size() >= 2) {
                double two_days_ago_close = static_cast<double>(symbol_bars[symbol_bars.size() - 2].close);
                two_days_ago_prices_[symbol] = two_days_ago_close;
                auto second_last_bar_time = std::chrono::system_clock::to_time_t(symbol_bars[symbol_bars.size() - 2].timestamp);
                DEBUG("Day T-2 close for " + symbol + ": " + std::to_string(two_days_ago_close) +
                      " (from " + std::to_string(second_last_bar_time) + ")");
            } else {
                WARN("No T-2 bar available for " + symbol + " - only " +
                     std::to_string(symbol_bars.size()) + " bars");
            }
        } else {
            WARN("No bars data available for symbol: " + symbol);
        }
    }

    INFO("Updated prices from bars: " + std::to_string(previous_day_prices_.size()) + " Day T-1, " +
         std::to_string(two_days_ago_prices_.size()) + " Day T-2");
    INFO("Note: For weekends/holidays, Day T-1 automatically falls back to last available trading day");

    return Result<void>();
}

Result<double> LivePriceManager::get_settlement_price(
    const std::string& symbol,
    const Timestamp& date) const {

    // Check cache first
    // TODO: Create proper date string formatting
    // std::string cache_key = symbol + "_" + date_string;
    auto it = settlement_prices_.find(symbol);
    if (it != settlement_prices_.end()) {
        return Result<double>(it->second);
    }

    // TODO: Implement Arrow Table parsing for database query
    // For now, return error to indicate not found
    return make_error<double>(ErrorCode::DATA_NOT_FOUND,
        "Settlement price lookup not yet integrated with Arrow Table");
}

Result<double> LivePriceManager::get_latest_price(const std::string& symbol) const {
    auto it = latest_prices_.find(symbol);
    if (it != latest_prices_.end()) {
        return Result<double>(it->second);
    }

    // Fall back to previous day price if no latest
    return get_previous_day_price(symbol);
}

Result<double> LivePriceManager::get_previous_day_price(const std::string& symbol) const {
    auto it = previous_day_prices_.find(symbol);
    if (it != previous_day_prices_.end()) {
        return Result<double>(it->second);
    }

    return make_error<double>(ErrorCode::DATA_NOT_FOUND, "No previous day price found for " + symbol);
}

Result<double> LivePriceManager::get_two_days_ago_price(const std::string& symbol) const {
    auto it = two_days_ago_prices_.find(symbol);
    if (it != two_days_ago_prices_.end()) {
        return Result<double>(it->second);
    }

    return make_error<double>(ErrorCode::DATA_NOT_FOUND, "No T-2 price found for " + symbol);
}

Result<double> LivePriceManager::get_price(
    const std::string& symbol,
    const Timestamp& timestamp) const {

    // For live, we typically want the latest price
    return get_latest_price(symbol);
}

Result<std::unordered_map<std::string, double>> LivePriceManager::get_prices(
    const std::vector<std::string>& symbols,
    const Timestamp& timestamp) const {

    std::unordered_map<std::string, double> prices;

    for (const auto& symbol : symbols) {
        auto price_result = get_price(symbol, timestamp);
        if (price_result.is_ok()) {
            prices[symbol] = price_result.value();
        }
    }

    return Result<std::unordered_map<std::string, double>>(prices);
}

} // namespace trade_ngin