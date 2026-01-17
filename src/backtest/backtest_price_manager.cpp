#include "trade_ngin/backtest/backtest_price_manager.hpp"

namespace trade_ngin {
namespace backtest {

void BacktestPriceManager::update_from_bars(const std::vector<Bar>& bars) {
    // Shift existing prices before updating with new ones
    // T-1 becomes T-2, current becomes T-1
    for (const auto& bar : bars) {
        const std::string& symbol = bar.symbol;
        double current_close = static_cast<double>(bar.close);

        // Shift: previous -> two_days_ago
        auto prev_it = previous_day_prices_.find(symbol);
        if (prev_it != previous_day_prices_.end()) {
            two_days_ago_prices_[symbol] = prev_it->second;
        }

        // Shift: current -> previous
        auto curr_it = current_prices_.find(symbol);
        if (curr_it != current_prices_.end()) {
            previous_day_prices_[symbol] = curr_it->second;
            has_previous_prices_ = true;
        }

        // Update current price
        current_prices_[symbol] = current_close;

        // Add to price history
        price_history_[symbol].push_back(current_close);
    }
}

void BacktestPriceManager::shift_prices() {
    // Shift without adding new prices
    // Used when we want to advance the day without new bar data

    // T-1 becomes T-2
    two_days_ago_prices_ = previous_day_prices_;

    // Current becomes T-1
    previous_day_prices_ = current_prices_;

    if (!current_prices_.empty()) {
        has_previous_prices_ = true;
    }

    // Clear current prices (will be populated by next update_from_bars call)
    current_prices_.clear();
}

Result<double> BacktestPriceManager::get_current_price(const std::string& symbol) const {
    auto it = current_prices_.find(symbol);
    if (it == current_prices_.end()) {
        return make_error<double>(ErrorCode::DATA_NOT_FOUND,
            "Current price not found for symbol: " + symbol);
    }
    return it->second;
}

Result<double> BacktestPriceManager::get_previous_day_price(const std::string& symbol) const {
    auto it = previous_day_prices_.find(symbol);
    if (it == previous_day_prices_.end()) {
        return make_error<double>(ErrorCode::DATA_NOT_FOUND,
            "Previous day price not found for symbol: " + symbol);
    }
    return it->second;
}

Result<double> BacktestPriceManager::get_two_days_ago_price(const std::string& symbol) const {
    auto it = two_days_ago_prices_.find(symbol);
    if (it == two_days_ago_prices_.end()) {
        return make_error<double>(ErrorCode::DATA_NOT_FOUND,
            "Two days ago price not found for symbol: " + symbol);
    }
    return it->second;
}

const std::vector<double>* BacktestPriceManager::get_price_history(const std::string& symbol) const {
    auto it = price_history_.find(symbol);
    if (it == price_history_.end()) {
        return nullptr;
    }
    return &it->second;
}

size_t BacktestPriceManager::get_price_history_length(const std::string& symbol) const {
    auto it = price_history_.find(symbol);
    if (it == price_history_.end()) {
        return 0;
    }
    return it->second.size();
}

void BacktestPriceManager::reset() {
    current_prices_.clear();
    previous_day_prices_.clear();
    two_days_ago_prices_.clear();
    price_history_.clear();
    has_previous_prices_ = false;
}

// PriceManagerBase interface implementation

Result<double> BacktestPriceManager::get_price(
    const std::string& symbol,
    const Timestamp& /* timestamp */) const {
    // In backtest mode, we ignore timestamp and return the current cached price
    // Prices are updated sequentially via update_from_bars()
    return get_current_price(symbol);
}

Result<std::unordered_map<std::string, double>> BacktestPriceManager::get_prices(
    const std::vector<std::string>& symbols,
    const Timestamp& /* timestamp */) const {
    std::unordered_map<std::string, double> result;

    for (const auto& symbol : symbols) {
        auto it = current_prices_.find(symbol);
        if (it != current_prices_.end()) {
            result[symbol] = it->second;
        }
        // Skip symbols not found rather than failing
        // This matches the behavior in the original BacktestEngine
    }

    return result;
}

} // namespace backtest
} // namespace trade_ngin
